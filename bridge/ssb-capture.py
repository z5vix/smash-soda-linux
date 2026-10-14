#!/usr/bin/env python3
"""
ssb-capture - native Linux half of the Smash Soda screen bridge.

Captures a monitor through the XDG ScreenCast portal (PipeWire), converts to
BGRA and publishes frames into a shared-memory ring that the Windows build of
Smash Soda (running under Proton) uploads to Parsec.

The portal asks once which screen to share; the choice is remembered in a
restore token so later runs start silently.
"""
import argparse
import mmap
import os
import signal
import struct
import sys
import time

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstApp", "1.0")
from gi.repository import Gio, GLib, Gst, GstApp  # noqa: E402

# ---- must match ssb_proto.h ------------------------------------------------
VIDEO_MAGIC = 0x53534256
VIDEO_VERSION = 1
SLOTS = 3
MAX_W, MAX_H = 3840, 2160
SLOT_BYTES = MAX_W * MAX_H * 4
HDR_BYTES = 4096
FILE_BYTES = HDR_BYTES + SLOTS * SLOT_BYTES
# header: magic, version, heartbeat, latest, info[3]{w,h}, fps
OFF_HEARTBEAT = 8
OFF_LATEST = 12
OFF_INFO = 16
OFF_FPS = OFF_INFO + SLOTS * 8
OFF_GEOM = OFF_FPS + 4  # mon x,y,w,h, desk x,y,w,h (int32)
OFF_WANT = OFF_GEOM + 32
GPU_OFF = 256            # struct ssb_gpu inside the video header (see ssb_proto.h)
GPU_SLOTS = 3
OFF_STATE = OFF_WANT + 4
OFF_REFRESH = OFF_STATE + 4   # refresh_mhz
OFF_WANT_W = OFF_REFRESH + 4  # requested stream size (0 = native)
OFF_WANT_H = OFF_WANT_W + 4
OFF_CAP_MODE = OFF_WANT_H + 4    # capture_mode, source_types, remember (uint32 each)
OFF_CHANGE = OFF_CAP_MODE + 12   # change_request counter
CAP_IDLE, CAP_WAITING, CAP_STREAMING, CAP_FAILED = 0, 1, 2, 3

AUDIO_MAGIC = 0x53534241
AUDIO_VERSION = 1
AUDIO_RATE = 48000
AUDIO_RING_FRAMES = 65536
AUDIO_HDR = 64
AUDIO_FILE_BYTES = AUDIO_HDR + AUDIO_RING_FRAMES * 4

EXIT_NO_SCREEN = 3   # picker cancelled / portal error: do not retry on our own
EXIT_STREAM_LOST = 4  # stream dropped (screen locked, monitor change...): reconnect

PORTAL = "org.freedesktop.portal.Desktop"
PORTAL_PATH = "/org/freedesktop/portal/desktop"
SC_IFACE = "org.freedesktop.portal.ScreenCast"


LOGFILE = None


def log(*a):
    msg = " ".join(str(x) for x in a)
    print("ssb-capture:", msg, file=sys.stderr, flush=True)
    if LOGFILE:
        try:
            t = time.time()
            stamp = time.strftime("%H:%M:%S", time.localtime(t)) + ".%03d" % int((t % 1) * 1000)
            with open(LOGFILE, "a") as f:
                f.write(f"{stamp} ssb-capture: {msg}\n")
        except OSError:
            pass


class Portal:
    """Minimal synchronous-ish ScreenCast portal client."""

    def __init__(self, loop, token_file, remember, source_types=1, ignore_token=False):
        self.ignore_token = ignore_token
        self.source_types = source_types
        self.remember = remember
        self.loop = loop
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.sender = self.bus.get_unique_name()[1:].replace(".", "_")
        self.counter = 0
        self.token_file = token_file
        self.session = None

    def _token(self):
        self.counter += 1
        return f"ssb{os.getpid()}_{self.counter}"

    def _request(self, method, build_params, options):
        """Calls a portal method that answers through a Request object."""
        token = self._token()
        options["handle_token"] = GLib.Variant("s", token)
        path = f"/org/freedesktop/portal/desktop/request/{self.sender}/{token}"
        result = {}

        def on_response(_c, _s, _p, _i, _sig, params):
            result["code"], result["data"] = params.unpack()
            self.loop.quit()

        sub = self.bus.signal_subscribe(
            PORTAL, "org.freedesktop.portal.Request", "Response", path,
            None, Gio.DBusSignalFlags.NONE, on_response)
        self.bus.call_sync(
            PORTAL, PORTAL_PATH, SC_IFACE, method,
            build_params(GLib.Variant("a{sv}", options)),
            None, Gio.DBusCallFlags.NONE, -1, None)
        self.loop.run()
        self.bus.signal_unsubscribe(sub)
        return result["code"], result["data"]

    def start(self):
        code, data = self._request(
            "CreateSession", lambda o: GLib.Variant.new_tuple(o),
            {"session_handle_token": GLib.Variant("s", self._token())})
        if code != 0:
            raise RuntimeError("CreateSession failed")
        self.session = data["session_handle"]

        opts = {
            "types": GLib.Variant("u", self.source_types),   # 1 monitor, 2 window, 3 either
            "multiple": GLib.Variant("b", False),
            "cursor_mode": GLib.Variant("u", 2),    # cursor embedded in frames
        }
        # Always ask the portal to persist: a reconnect inside one hosting run (e.g. after the
        # screen was locked) then restores the same source without showing the picker again.
        opts["persist_mode"] = GLib.Variant("u", 2)
        try:
            tok = "" if self.ignore_token else open(self.token_file).read().strip()
            if tok:
                opts["restore_token"] = GLib.Variant("s", tok)
        except OSError:
            pass
        code, _ = self._request(
            "SelectSources",
            lambda o: GLib.Variant.new_tuple(GLib.Variant("o", self.session), o), opts)
        if code != 0:
            raise RuntimeError("SelectSources failed")

        log("waiting for the screen-share confirmation (first run only)...")
        code, data = self._request(
            "Start",
            lambda o: GLib.Variant.new_tuple(
                GLib.Variant("o", self.session), GLib.Variant("s", ""), o), {})
        if code != 0:
            raise RuntimeError("screen sharing was cancelled")
        if "restore_token" in data:
            os.makedirs(os.path.dirname(self.token_file), exist_ok=True)
            with open(self.token_file, "w") as f:
                f.write(data["restore_token"])
        streams = data["streams"]
        node_id, props = streams[0]
        size = props.get("size")
        return node_id, size, props.get("position")

    def open_pipewire_remote(self):
        res, fdlist = self.bus.call_with_unix_fd_list_sync(
            PORTAL, PORTAL_PATH, SC_IFACE, "OpenPipeWireRemote",
            GLib.Variant.new_tuple(GLib.Variant("o", self.session),
                                   GLib.Variant("a{sv}", {})),
            GLib.VariantType("(h)"), Gio.DBusCallFlags.NONE, -1, None, None)
        idx = res.unpack()[0]
        return fdlist.get(idx)


class Publisher:
    def __init__(self, path, fresh):
        fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
        os.ftruncate(fd, FILE_BYTES)
        self.mm = mmap.mmap(fd, FILE_BYTES, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        os.close(fd)
        if fresh:
            self.mm[0:HDR_BYTES] = b"\0" * HDR_BYTES
            struct.pack_into("II", self.mm, 0, VIDEO_MAGIC, VIDEO_VERSION)
        else:
            struct.pack_into("I", self.mm, OFF_LATEST, 0)  # new session: restart frame numbering
        self.seq = 0
        self.frames = 0
        self.t0 = time.monotonic()

    def mark_frame(self, w, h):
        """Frame delivered without pixel data (GPU mode): only bump the sequence the app waits on."""
        self.seq += 1
        slot = self.seq % SLOTS
        struct.pack_into("II", self.mm, OFF_INFO + slot * 8, w, h)
        struct.pack_into("I", self.mm, OFF_LATEST, self.seq)
        self.frames += 1
        now = time.monotonic()
        if now - self.t0 >= 1.0:
            struct.pack_into("I", self.mm, OFF_FPS, int(self.frames / (now - self.t0)))
            self.frames = 0
            self.t0 = now

    def set_refresh(self, hz):
        struct.pack_into("I", self.mm, OFF_REFRESH, int(hz * 1000))

    def app_choices(self):
        """(capture_mode, source_types, remember) as set by the app's Video settings; 0 = use the defaults."""
        return struct.unpack_from("III", self.mm, OFF_CAP_MODE)

    def change_request(self):
        return struct.unpack_from("I", self.mm, OFF_CHANGE)[0]

    def wanted_size(self):
        return struct.unpack_from("II", self.mm, OFF_WANT_W)

    def want(self):
        return struct.unpack_from("I", self.mm, OFF_WANT)[0] != 0

    def set_state(self, state):
        struct.pack_into("I", self.mm, OFF_STATE, state)

    def set_geometry(self, mon, desk):
        struct.pack_into("8i", self.mm, OFF_GEOM, *mon, *desk)

    def heartbeat(self):
        hb = struct.unpack_from("I", self.mm, OFF_HEARTBEAT)[0]
        struct.pack_into("I", self.mm, OFF_HEARTBEAT, (hb + 1) & 0xFFFFFFFF)

    def publish(self, data, w, h):
        self.seq += 1
        slot = self.seq % SLOTS
        off = HDR_BYTES + slot * SLOT_BYTES
        self.mm[off:off + len(data)] = data
        struct.pack_into("II", self.mm, OFF_INFO + slot * 8, w, h)
        struct.pack_into("I", self.mm, OFF_LATEST, self.seq)  # publish last
        self.frames += 1
        now = time.monotonic()
        if now - self.t0 >= 1.0:
            struct.pack_into("I", self.mm, OFF_FPS, int(self.frames / (now - self.t0)))
            self.frames = 0
            self.t0 = now


class AudioPublisher:
    def __init__(self, path):
        fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
        os.ftruncate(fd, AUDIO_FILE_BYTES)
        self.mm = mmap.mmap(fd, AUDIO_FILE_BYTES, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        os.close(fd)
        self.mm[0:AUDIO_HDR] = b"\0" * AUDIO_HDR
        struct.pack_into("IIII", self.mm, 0, AUDIO_MAGIC, AUDIO_VERSION, 0, AUDIO_RATE)
        self.pos = 0

    def heartbeat(self):
        hb = struct.unpack_from("I", self.mm, 8)[0]
        struct.pack_into("I", self.mm, 8, (hb + 1) & 0xFFFFFFFF)

    def publish(self, data):
        frames = len(data) // 4
        if frames == 0:
            return
        start = self.pos % AUDIO_RING_FRAMES
        first = min(frames, AUDIO_RING_FRAMES - start)
        self.mm[AUDIO_HDR + start * 4: AUDIO_HDR + (start + first) * 4] = data[:first * 4]
        if first < frames:
            self.mm[AUDIO_HDR: AUDIO_HDR + (frames - first) * 4] = data[first * 4: frames * 4]
        self.pos += frames
        struct.pack_into("Q", self.mm, 16, self.pos)  # publish last


def build_audio_pipeline(pub):
    """Default sink monitor -> S16LE stereo 48k -> ring. Returns the pipeline or None."""
    try:
        pl = Gst.parse_launch(
            "pipewiresrc stream-properties=\"props,stream.capture.sink=true,node.name=SmashSodaCapture\" ! "
            "audioconvert ! audioresample ! "
            f"audio/x-raw,format=S16LE,rate={AUDIO_RATE},channels=2,layout=interleaved ! "
            "appsink name=asink emit-signals=true max-buffers=16 drop=false sync=false")
    except GLib.Error as exc:
        log("audio pipeline unavailable:", exc.message)
        return None
    sink = pl.get_by_name("asink")
    last = [0.0]
    gaps = [0]

    def on_sample(s):
        sample = s.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.OK
        now = time.monotonic()
        if last[0] and now - last[0] > 0.080 and gaps[0] < 20:
            gaps[0] += 1
            log(f"STALL: desktop audio block arrived {1000 * (now - last[0]):.0f} ms after the previous one")
        last[0] = now
        buf = sample.get_buffer()
        ok, mi = buf.map(Gst.MapFlags.READ)
        if ok:
            try:
                pub.publish(bytes(mi.data))
            finally:
                buf.unmap(mi)
        return Gst.FlowReturn.OK

    sink.connect("new-sample", on_sample)
    return pl


def monitor_geometry(position, w, h):
    """(monitor rect, desktop bounds, refresh Hz of the shared monitor) in logical compositor coordinates."""
    px, py = position if position else (0, 0)
    mon = (px, py, w, h)
    refresh = 0.0
    try:
        gi.require_version("Gdk", "3.0")
        from gi.repository import Gdk
        display = Gdk.Display.get_default()
        mons = [display.get_monitor(i) for i in range(display.get_n_monitors())]
        rects = [m.get_geometry() for m in mons]
        if rects:
            x0 = min(r.x for r in rects)
            y0 = min(r.y for r in rects)
            x1 = max(r.x + r.width for r in rects)
            y1 = max(r.y + r.height for r in rects)
            matched = False
            for m, r in zip(mons, rects):
                if (r.x, r.y) == (px, py):
                    mon = (r.x, r.y, r.width, r.height)
                    refresh = m.get_refresh_rate() / 1000.0
                    matched = True
            if not matched:   # a window (or unknown monitor): assume the fastest display
                refresh = max(m.get_refresh_rate() for m in mons) / 1000.0
            return mon, (x0, y0, x1 - x0, y1 - y0), refresh
    except Exception as exc:  # noqa: BLE001
        log("could not read monitor layout:", exc)
    return mon, mon, refresh


def fit(w, h, max_h):
    if h > max_h:
        w = int(w * max_h / h)
        h = max_h
    return max(2, w & ~1), max(2, h & ~1)


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "smashsoda"))
    ap.add_argument("--max-height", type=int, default=0, help="optional: never stream taller than this (0 = off)")
    ap.add_argument("--fps", type=int, default=0, help="screen capture rate cap (0 = the display's refresh rate)")
    ap.add_argument("--no-audio", action="store_true", help="do not publish desktop audio")
    ap.add_argument("--remember", action="store_true", help="remember the chosen screen instead of asking every time")
    ap.add_argument("--token", default=os.path.expanduser("~/.local/share/smashsoda-linux/portal-token"))
    ap.add_argument("--gpu-ipc", type=int, default=int(os.environ.get("SSB_GPU_IPC", "0")),
                    help="1 if the Windows side can open CUDA IPC buffers (probed by the launcher)")
    ap.add_argument("--capture-mode", choices=["auto", "gpu", "gl", "shm"], default=os.environ.get("SSB_CAPTURE", "auto"),
                    help="gpu: frames stay in GPU memory end to end (zero copies; needs CUDA IPC); "
                         "gl: receive GPU buffers and download them on the GPU (light on the compositor); "
                         "shm: ask the compositor for CPU memory (heavy: it reads every frame back); auto: gl, falling back to shm")
    ap.add_argument("--source", choices=["monitor", "window", "any"], default=os.environ.get("SSB_SOURCE", "monitor"),
                    help="what the screen-share picker offers")
    ap.add_argument("--session", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--audio-only", action="store_true", help=argparse.SUPPRESS)
    return ap.parse_args()


def supervisor(args):
    """Idles until Smash Soda starts hosting, then runs a capture session (which opens the picker)."""
    import subprocess
    os.makedirs(args.dir, mode=0o700, exist_ok=True)
    pub = Publisher(os.path.join(args.dir, "video.shm"), fresh=True)
    try:
        os.remove(os.path.join(args.dir, "no-gpu"))     # a launch starts by trusting the GPU path again
    except OSError:
        pass
    log("ready; waiting for Smash Soda to start hosting")
    child = None
    stop = []
    signal.signal(signal.SIGINT, lambda *_: stop.append(1))
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    gave_up = False   # picker cancelled / portal error: wait for the next Host press
    retry_at = 0.0    # stream lost while hosting: reconnect at this time
    was_wanted = False
    last_change = pub.change_request()
    token = os.path.join(args.dir, "session-token")
    while not stop:
        want = pub.want()
        if want and not was_wanted and not (args.remember or pub.app_choices()[2]):
            try:
                os.remove(token)          # a new Host press always shows the picker
            except OSError:
                pass
        was_wanted = want
        change = pub.change_request()
        if change != last_change:
            last_change = change
            if want:   # "Change Screen": restart the capture with a fresh picker; the room (and its guests) stays up
                log("switching to another screen/window")
                try:
                    open(os.path.join(args.dir, "force-pick"), "w").close()
                except OSError:
                    pass
                if child is not None:
                    child.terminate()
                    try:
                        child.wait(5)
                    except subprocess.TimeoutExpired:
                        child.kill()
                    child = None
                gave_up = False
                retry_at = 0.0
                pub.set_state(CAP_WAITING)
        if child is not None and child.poll() is not None:
            code = child.returncode
            child = None
            if want:
                if code == EXIT_NO_SCREEN:
                    gave_up = True
                else:
                    log("screen stream lost (screen locked?); reconnecting...")
                    pub.set_state(CAP_WAITING)
                    retry_at = time.monotonic() + 3.0
        if want and child is None and not gave_up and time.monotonic() >= retry_at:
            child = subprocess.Popen([sys.executable, os.path.abspath(__file__), "--session"] + sys.argv[1:])
        if not want:
            gave_up = False
            retry_at = 0.0
            if child is not None:
                child.terminate()
                try:
                    child.wait(5)
                except subprocess.TimeoutExpired:
                    child.kill()
                child = None
            pub.set_state(CAP_IDLE)
        time.sleep(0.2)
    if child is not None:
        child.terminate()
        child.wait()


def audio_process(args):
    """Desktop audio capture in its own process, using parec (PulseAudio protocol on PipeWire).

    Records the source the audio-app mixer selects (normally the default output's monitor; the "share" sink's monitor
    while some app is switched to "not shared") as raw 48 kHz S16 stereo and publishes it to the ring.
    GStreamer's pipewiresrc was tried first but progressively dropped buffers (48k -> 18k frames/s over five
    minutes, while parec held 48k for the whole run), which sounded like ever-worsening crackle."""
    import select
    import subprocess
    pub = AudioPublisher(os.path.join(args.dir, "audio.shm"))
    device_file = os.path.join(args.dir, "audio-device")

    def wanted_device():
        try:
            dev = open(device_file).read().strip()
        except OSError:
            dev = ""
        return dev or "@DEFAULT_MONITOR@"

    def start(dev):
        cmd = ["parec", f"--device={dev}", "--format=s16le", "--rate=48000", "--channels=2",
               "--latency-msec=20", "--raw", "--client-name=Smash Soda", "--stream-name=Desktop audio capture"]
        return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    device = wanted_device()
    try:
        proc = start(device)
    except OSError as exc:
        log("cannot start parec:", exc)
        return
    parent = os.getppid()
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    signal.signal(signal.SIGINT, lambda *_: stop.append(1))
    log(f"publishing desktop audio (parec, own process, source {device})")

    last_beat = last_read = last_check = time.monotonic()
    gaps = 0
    pending = b""
    while not stop and os.getppid() == parent:
        ready, _, _ = select.select([proc.stdout], [], [], 0.25)
        now = time.monotonic()
        if now - last_check >= 0.5:
            last_check = now
            dev = wanted_device()
            if dev != device:                     # the mixer re-routed what guests should hear
                log(f"switching the audio source to {dev}")
                proc.terminate()
                try:
                    proc.wait(2)
                except subprocess.TimeoutExpired:
                    proc.kill()
                device = dev
                proc = start(device)
                pending = b""
                last_read = now
                continue
        if now - last_beat >= 0.25:
            pub.heartbeat()
            last_beat = now
        if not ready:
            continue
        data = os.read(proc.stdout.fileno(), 3840)       # ~20 ms of audio
        if not data:
            log("parec ended")
            break
        if now - last_read > 0.080 and gaps < 20:
            gaps += 1
            log(f"STALL: desktop audio block arrived {1000 * (now - last_read):.0f} ms after the previous one")
        last_read = now
        pending += data
        whole = len(pending) - (len(pending) % 4)   # keep whole stereo frames
        if whole:
            pub.publish(pending[:whole])
            pending = pending[whole:]
    proc.terminate()
    try:
        proc.wait(3)
    except subprocess.TimeoutExpired:
        proc.kill()


class GpuRing:
    """CUDA ring buffers exported over CUDA IPC (see the `gpu` mode)."""

    def __init__(self, video_mm):
        import ctypes
        self.ct = ctypes
        self.mm = video_mm
        self.cu = ctypes.CDLL("libcuda.so.1")
        self.gst = ctypes.CDLL("libgstreamer-1.0.so.0")
        self.gst.gst_buffer_peek_memory.restype = ctypes.c_void_p
        self.gst.gst_buffer_peek_memory.argtypes = [ctypes.c_void_p, ctypes.c_uint]

        class MapInfo(ctypes.Structure):
            _fields_ = [("memory", ctypes.c_void_p), ("flags", ctypes.c_int), ("data", ctypes.c_void_p),
                        ("size", ctypes.c_size_t), ("maxsize", ctypes.c_size_t),
                        ("user_data", ctypes.c_void_p * 4), ("reserved", ctypes.c_void_p * 4)]

        class Memcpy2D(ctypes.Structure):
            _fields_ = [("srcXInBytes", ctypes.c_size_t), ("srcY", ctypes.c_size_t), ("srcMemoryType", ctypes.c_int),
                        ("srcHost", ctypes.c_void_p), ("srcDevice", ctypes.c_ulonglong), ("srcArray", ctypes.c_void_p),
                        ("srcPitch", ctypes.c_size_t), ("dstXInBytes", ctypes.c_size_t), ("dstY", ctypes.c_size_t),
                        ("dstMemoryType", ctypes.c_int), ("dstHost", ctypes.c_void_p), ("dstDevice", ctypes.c_ulonglong),
                        ("dstArray", ctypes.c_void_p), ("dstPitch", ctypes.c_size_t),
                        ("WidthInBytes", ctypes.c_size_t), ("Height", ctypes.c_size_t)]
        self.MapInfo, self.Memcpy2D = MapInfo, Memcpy2D
        self.gst.gst_memory_map.restype = ctypes.c_int
        self.gst.gst_memory_map.argtypes = [ctypes.c_void_p, ctypes.POINTER(MapInfo), ctypes.c_int]
        self.gst.gst_memory_unmap.argtypes = [ctypes.c_void_p, ctypes.POINTER(MapInfo)]

        if self.cu.cuInit(0):
            raise RuntimeError("cuInit failed")
        dev = ctypes.c_int(0)
        self.cu.cuDeviceGet(ctypes.byref(dev), 0)
        self.dev = dev.value
        self.ctx = ctypes.c_void_p()   # the primary context: the same one gst-cuda allocates frames in
        if self.cu.cuDevicePrimaryCtxRetain(ctypes.byref(self.ctx), dev):
            raise RuntimeError("cannot retain the CUDA primary context")
        self.ptrs = []
        self.pitch = 0
        self.w = self.h = 0
        self.seq = 0
        self.stride = 0

    def alloc(self, w, h):
        ct, cu = self.ct, self.cu
        cu.cuCtxPushCurrent_v2(self.ctx)
        try:
            self.ptrs = []
            pitch0 = None
            for i in range(GPU_SLOTS):
                ptr, pitch = ct.c_ulonglong(), ct.c_size_t()
                if cu.cuMemAllocPitch_v2(ct.byref(ptr), ct.byref(pitch), w * 4, h, 16):
                    raise RuntimeError("cuMemAllocPitch failed")
                handle = (ct.c_char * 64)()
                if cu.cuIpcGetMemHandle(handle, ptr):
                    raise RuntimeError("cuIpcGetMemHandle failed")
                self.ptrs.append(ptr.value)
                pitch0 = pitch.value if pitch0 is None else pitch0
                self.mm[GPU_OFF + 40 + i * 64: GPU_OFF + 40 + (i + 1) * 64] = bytes(handle)
            self.pitch, self.w, self.h = pitch0, w, h
        finally:
            cu.cuCtxPopCurrent_v2(None)
        gen = struct.unpack_from("I", self.mm, GPU_OFF + 4)[0]
        struct.pack_into("IIIII", self.mm, GPU_OFF + 8, GPU_SLOTS, w, h, pitch0, 0)   # slots, w, h, pitch, latest_slot
        struct.pack_into("I", self.mm, GPU_OFF + 36, self.dev)
        struct.pack_into("I", self.mm, GPU_OFF + 32, 0)                                # consumer_state := unknown
        struct.pack_into("I", self.mm, GPU_OFF + 4, (gen + 1) & 0xFFFFFFFF)            # generation
        struct.pack_into("I", self.mm, GPU_OFF + 0, 1)                                 # enabled
        log(f"GPU ring: {GPU_SLOTS} CUDA buffers {w}x{h}, pitch {pitch0}, device {self.dev} (exported over CUDA IPC)")

    def disable(self):
        struct.pack_into("I", self.mm, GPU_OFF + 0, 0)

    def consumer_failed(self):
        return struct.unpack_from("I", self.mm, GPU_OFF + 32)[0] == 2

    def push_frame(self, gst_buffer, w, h, stride):
        """Copies one GStreamer CUDA frame into the next ring slot and publishes it."""
        ct, cu = self.ct, self.cu
        bptr = ct.c_void_p.from_address(id(gst_buffer) + 16).value    # PyGBoxed: PyObject_HEAD, then the boxed pointer
        mem = self.gst.gst_buffer_peek_memory(bptr, 0)
        info = self.MapInfo()
        if not self.gst.gst_memory_map(mem, ct.byref(info), 1 | (1 << 17)):   # GST_MAP_READ | GST_MAP_CUDA
            return False
        try:
            if (w, h) != (self.w, self.h):
                return False
            slot = (self.seq + 1) % GPU_SLOTS
            c = self.Memcpy2D()
            c.srcMemoryType = 2; c.srcDevice = info.data; c.srcPitch = stride
            c.dstMemoryType = 2; c.dstDevice = self.ptrs[slot]; c.dstPitch = self.pitch
            c.WidthInBytes = w * 4; c.Height = h
            cu.cuCtxPushCurrent_v2(self.ctx)
            rc = cu.cuMemcpy2D_v2(ct.byref(c))
            cu.cuCtxPopCurrent_v2(None)
            if rc:
                return False
        finally:
            self.gst.gst_memory_unmap(mem, ct.byref(info))
        self.seq += 1
        struct.pack_into("I", self.mm, GPU_OFF + 24, slot)                          # latest_slot
        struct.pack_into("I", self.mm, GPU_OFF + 28, self.seq & 0xFFFFFFFF)        # latest_seq (published last)
        return True


def session(args):
    os.makedirs(args.dir, mode=0o700, exist_ok=True)
    pub = Publisher(os.path.join(args.dir, "video.shm"), fresh=False)
    pub.set_state(CAP_WAITING)
    Gst.init(None)
    loop = GLib.MainLoop()
    app_mode, app_source, app_remember = pub.app_choices()
    if app_mode in (1, 2, 3):
        args.capture_mode = {1: "gpu", 2: "gl", 3: "shm"}[app_mode]
    if app_source in (1, 2, 3):
        args.source = {1: "monitor", 2: "window", 3: "any"}[app_source]
    if app_remember:
        args.remember = True
    token_file = args.token if args.remember else os.path.join(args.dir, "session-token")
    force_marker = os.path.join(args.dir, "force-pick")
    force_pick = os.path.exists(force_marker)       # "Change Screen": always show the picker, never restore
    if force_pick:
        try:
            os.remove(force_marker)
        except OSError:
            pass
    try:
        portal = Portal(loop, token_file, args.remember, {"monitor": 1, "window": 2, "any": 3}[args.source], ignore_token=force_pick)
        node_id, size, position = portal.start()
        fd = portal.open_pipewire_remote()
    except Exception as exc:  # noqa: BLE001 - cancelled picker or portal error
        log("no screen shared:", exc)
        pub.set_state(CAP_FAILED)
        sys.exit(EXIT_NO_SCREEN)
    src_w, src_h = size if size else (1920, 1080)
    mon_rect, desk_rect, refresh_hz = monitor_geometry(position, src_w, src_h)
    pub.set_geometry(mon_rect, desk_rect)
    pub.set_refresh(refresh_hz)
    # Output size: what the app asked for (its Resolution setting), never larger than the source; otherwise native.
    want_w, want_h = pub.wanted_size()
    if want_w > 0 and want_h > 0 and want_w <= src_w and want_h <= src_h:
        out_w, out_h = max(2, want_w & ~1), max(2, want_h & ~1)
    elif args.max_height > 0:
        out_w, out_h = fit(src_w, src_h, min(args.max_height, MAX_H))
    else:
        out_w, out_h = max(2, src_w & ~1), max(2, src_h & ~1)
    # Capture rate cap: auto = the display's own refresh rate (at least 60).
    if args.fps <= 0:
        args.fps = min(120, max(60, int(round(refresh_hz)))) if refresh_hz else 120   # the stream tops out at 60: more is waste
    log(f"streaming node {node_id}: {src_w}x{src_h} -> {out_w}x{out_h}, display {refresh_hz:.0f} Hz, capture cap {args.fps} fps")

    last_frame = [0.0]
    stall_logs = [0]
    got_frame = [False]
    failed = []
    probing = [True]
    probe_error = []

    ring = [None]

    def make_pipeline(mode, remote_fd):
        head = f"pipewiresrc fd={remote_fd} path={node_id} do-timestamp=true always-copy=false ! "
        if mode == "gpu":
            # DMA-BUF -> GL -> CUDA memory. Nothing is read back to system memory; every frame is copied
            # GPU-to-GPU into the exported ring (see GpuRing).
            if (src_w, src_h) == (out_w, out_h):
                gl = "glupload ! glcolorconvert ! video/x-raw(memory:GLMemory),format=BGRx ! "
            else:   # GL's scaler wants RGBA: scale there, then convert back
                gl = ("glupload ! glcolorconvert ! video/x-raw(memory:GLMemory),format=RGBA ! glcolorscale ! "
                      f"video/x-raw(memory:GLMemory),format=RGBA,width={out_w},height={out_h} ! "
                      "glcolorconvert ! video/x-raw(memory:GLMemory),format=BGRx ! ")
            return (head + "video/x-raw(memory:DMABuf) ! " + f"videorate drop-only=true max-rate={args.fps} ! " + gl +
                    "cudaupload ! video/x-raw(memory:CUDAMemory),format=BGRx ! "
                    "appsink name=sink emit-signals=true max-buffers=2 drop=true sync=false")
        tail = (f"videoscale ! video/x-raw,format={{BGRx,BGRA}},width={out_w},height={out_h} ! "
                f"appsink name=sink emit-signals=true max-buffers=2 drop=true sync=false")
        if mode == "gl":
            # Frames stay in GPU memory (DMA-BUF) all the way into GL, so the compositor does not read every
            # frame back to the CPU; the download happens here on the GPU's copy engine instead.
            return (head + "video/x-raw(memory:DMABuf) ! " + f"videorate drop-only=true max-rate={args.fps} ! "
                    "glupload ! glcolorconvert ! gldownload ! " + tail)
        return head + f"videorate drop-only=true max-rate={args.fps} ! videoconvert ! " + tail

    def on_sample(s):
        sample = s.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.OK
        got_frame[0] = True
        now = time.monotonic()
        if last_frame[0] and now - last_frame[0] > 0.040 and stall_logs[0] < 20:
            stall_logs[0] += 1
            log(f"STALL: screen frame arrived {1000 * (now - last_frame[0]):.0f} ms after the previous one")
        last_frame[0] = now
        buf = sample.get_buffer()
        caps = sample.get_caps().get_structure(0)
        w, h = caps.get_value("width"), caps.get_value("height")
        ok, mi = buf.map(Gst.MapFlags.READ)
        if ok:
            try:
                stride = w * 4
                data = mi.data
                if len(data) >= stride * h:
                    pub.publish(data[:stride * h], w, h)
            finally:
                buf.unmap(mi)
        return Gst.FlowReturn.OK

    gpu_stride = [0]
    gpu_vid = [None]

    def on_sample_gpu(s):
        sample = s.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.OK
        now = time.monotonic()
        if last_frame[0] and now - last_frame[0] > 0.040 and stall_logs[0] < 20:
            stall_logs[0] += 1
            log(f"STALL: screen frame arrived {1000 * (now - last_frame[0]):.0f} ms after the previous one")
        last_frame[0] = now
        buf = sample.get_buffer()
        caps = sample.get_caps().get_structure(0)
        w, h = caps.get_value("width"), caps.get_value("height")
        if gpu_vid[0] is None:
            gi.require_version("GstVideo", "1.0")
            from gi.repository import GstVideo
            gpu_vid[0] = GstVideo
        meta = gpu_vid[0].buffer_get_video_meta(buf)
        stride = meta.stride[0] if meta else w * 4
        if ring[0].w == 0:
            ring[0].alloc(w, h)
        if ring[0].push_frame(buf, w, h, stride):
            got_frame[0] = True
            pub.mark_frame(w, h)
        return Gst.FlowReturn.OK

    def on_bus(_bus, msg):
        if msg.type == Gst.MessageType.ERROR:
            err, dbg = msg.parse_error()
            if probing[0]:
                probe_error.append(err.message)
                return
            log("pipeline error:", err.message, dbg or "")
            failed.append(1)
            loop.quit()
        elif msg.type == Gst.MessageType.EOS and not probing[0]:
            log("stream ended")
            failed.append(1)
            loop.quit()

    use_gpu = bool(args.gpu_ipc) and not os.path.exists(os.path.join(args.dir, "no-gpu"))
    if args.capture_mode == "auto":
        modes = ["gpu", "gl", "shm"] if use_gpu else ["gl", "shm"]
    else:   # a chosen method is tried first, then the lighter-to-heavier ones below it, so there is always a picture
        modes = {"gpu": ["gpu", "gl", "shm"], "gl": ["gl", "shm"], "shm": ["shm"]}[args.capture_mode]
        if args.capture_mode == "gpu" and os.path.exists(os.path.join(args.dir, "no-gpu")):
            modes = ["gl", "shm"]
    pipeline = None
    chosen = None
    ctx = GLib.MainContext.default()
    for i, mode in enumerate(modes):
        remote = fd if i == 0 else portal.open_pipewire_remote()
        probing[0] = True; probe_error.clear(); got_frame[0] = False
        try:
            candidate = Gst.parse_launch(make_pipeline(mode, remote))
        except GLib.Error as exc:
            log(f"capture mode '{mode}' unavailable: {exc.message}")
            continue
        if mode == "gpu":
            try:
                ring[0] = ring[0] or GpuRing(pub.mm)
            except Exception as exc:  # noqa: BLE001
                log(f"capture mode 'gpu' unavailable: {exc}")
                continue
        candidate.get_by_name("sink").connect("new-sample", on_sample_gpu if mode == "gpu" else on_sample)
        cbus = candidate.get_bus()
        cbus.add_signal_watch()
        cbus.connect("message", on_bus)
        candidate.set_state(Gst.State.PLAYING)
        t0 = time.monotonic()
        while not got_frame[0] and not probe_error and time.monotonic() - t0 < 5.0:
            ctx.iteration(False)
            time.sleep(0.01)
        if got_frame[0]:
            pipeline, chosen = candidate, mode
            break
        log(f"capture mode '{mode}' produced no frames ({probe_error[0] if probe_error else 'timeout'})"
            + ("; falling back" if i + 1 < len(modes) else ""))
        candidate.set_state(Gst.State.NULL)
        if ring[0] is not None:
            ring[0].disable()
    probing[0] = False
    if pipeline is None:
        log("no capture mode worked")
        pub.set_state(CAP_FAILED)
        sys.exit(EXIT_NO_SCREEN)
    log(f"capture mode: {chosen}")

    import subprocess
    audio_proc = None
    if not args.no_audio:
        audio_proc = subprocess.Popen([sys.executable, os.path.abspath(__file__), "--audio-only"] +
                                      [a for a in sys.argv[1:] if a not in ("--session", "--audio-only")])

    def beat():
        pub.heartbeat()
        return True

    GLib.timeout_add(250, beat)
    signal.signal(signal.SIGINT, lambda *_: loop.quit())
    signal.signal(signal.SIGTERM, lambda *_: loop.quit())

    def check_want():
        if not pub.want():
            loop.quit()
            return False
        if chosen == "gpu" and ring[0] is not None and ring[0].consumer_failed():
            log("the Windows side could not open the GPU buffers; falling back to the CPU capture path")
            try:
                open(os.path.join(args.dir, "no-gpu"), "w").close()
            except OSError:
                pass
            failed.append(1)
            loop.quit()
            return False
        return True

    GLib.timeout_add(250, check_want)
    pub.set_state(CAP_STREAMING)
    log("capturing")
    loop.run()
    pipeline.set_state(Gst.State.NULL)
    if ring[0] is not None:
        ring[0].disable()
    if audio_proc is not None:
        audio_proc.terminate()
        try:
            audio_proc.wait(3)
        except subprocess.TimeoutExpired:
            audio_proc.kill()
    if failed:
        pub.set_state(CAP_WAITING)
        sys.exit(EXIT_STREAM_LOST)
    pub.set_state(CAP_IDLE)


def main():
    global LOGFILE
    args = parse_args()
    LOGFILE = os.path.join(args.dir, "helpers.log")
    if args.audio_only:
        audio_process(args)
    elif args.session:
        session(args)
    else:
        supervisor(args)


if __name__ == "__main__":
    main()
