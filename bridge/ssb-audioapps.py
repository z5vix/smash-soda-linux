#!/usr/bin/env python3
"""
ssb-audioapps - per-application audio mixer for the Smash Soda Linux build.

Publishes every app that currently has an audio stream (PulseAudio/PipeWire "sink inputs") into shared memory and
applies the commands the app sends: per-app volume, mute and "share with guests".

"Share with guests" off keeps that app out of the audio guests receive while you still hear it (the Linux
counterpart of Windows' "exclude from shared audio"). While any app is unshared, every other app is routed through a
"share" sink that guests capture (and that is played on your speakers through a short loopback); unshared apps stay
directly on your speakers, so they are never in the guests' capture. With nothing unshared there is no routing at all.
"""
import json
import mmap
import os
import signal
import struct
import subprocess
import sys
import time

APPS_MAGIC = 0x53534150
APPS_VERSION = 1
MAX_APPS = 16
HDR = 64
ENTRY = 64
FILE_BYTES = HDR + MAX_APPS * ENTRY
SHARE_SINK = "smashsoda_share"
LEGACY_SINK = "smashsoda_private"

# header field offsets
O_HEARTBEAT, O_COUNT, O_CMD_SEQ, O_CMD_ACK, O_CMD_TYPE, O_CMD_ID, O_CMD_VALUE = 8, 12, 16, 20, 24, 28, 32


def log(*a):
    print("ssb-audioapps:", *a, file=sys.stderr, flush=True)


def pactl(*args, check=False):
    try:
        r = subprocess.run(["pactl", *args], capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if r.returncode != 0:
        return None
    return r.stdout


def pactl_json(*args):
    out = pactl("--format=json", *args)
    if out is None:
        return []
    try:
        return json.loads(out)
    except ValueError:
        return []


class Mixer:
    def __init__(self, path, bridge_dir):
        fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
        os.ftruncate(fd, FILE_BYTES)
        self.mm = mmap.mmap(fd, FILE_BYTES, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        os.close(fd)
        self.mm[0:FILE_BYTES] = b"\0" * FILE_BYTES
        struct.pack_into("II", self.mm, 0, APPS_MAGIC, APPS_VERSION)
        self.dir = bridge_dir
        self.modules = []        # module ids we loaded
        self.share_ready = False
        self.real_sink = ""
        self.excluded = set()    # sink-input indexes the user switched to "not shared"
        self.cleanup_stale()
        self.set_capture_device("")

    # ------------------------------------------------------------ what the guests' audio capture listens to
    def set_capture_device(self, device):
        """Tells the audio capture process which source to record ('' = the default output's monitor)."""
        path = os.path.join(self.dir, "audio-device")
        tmp = path + ".tmp"
        try:
            with open(tmp, "w") as f:
                f.write(device)
            os.replace(tmp, path)
        except OSError:
            pass

    # ------------------------------------------------------------ routing through the share sink
    def cleanup_stale(self):
        """Unload leftovers of a previous run that did not shut down cleanly."""
        out = pactl("list", "short", "modules") or ""
        for line in out.splitlines():
            parts = line.split("\t")
            if len(parts) >= 3 and (SHARE_SINK in line or LEGACY_SINK in line) and parts[1] in ("module-null-sink", "module-loopback"):
                pactl("unload-module", parts[0])

    def sink_index(self, name):
        for s in pactl_json("list", "sinks"):
            if s.get("name") == name:
                return s.get("index")
        return None

    def ensure_share(self):
        if self.share_ready:
            return True
        default = (pactl("get-default-sink") or "").strip()
        if not default or default == SHARE_SINK:
            return False
        self.real_sink = default
        a = pactl("load-module", "module-null-sink", f"sink_name={SHARE_SINK}", "channels=2", "rate=48000",
                  "sink_properties=device.description=Smash\\ Soda\\ (shared\\ audio)")
        b = pactl("load-module", "module-loopback", f"source={SHARE_SINK}.monitor", f"sink={default}", "latency_msec=20")
        for m in (a, b):
            if m and m.strip().isdigit():
                self.modules.append(m.strip())
        self.share_ready = a is not None and b is not None
        if self.share_ready:
            self.set_capture_device(f"{SHARE_SINK}.monitor")
            log("routing: shared apps go through the share sink; unshared apps stay on your speakers")
        return self.share_ready

    def teardown_share(self):
        """Back to no routing: every stream returns to the normal output and the share sink is removed."""
        if self.share_ready:
            sidx = self.sink_index(SHARE_SINK)
            real = self.real_sink or (pactl("get-default-sink") or "").strip() or "@DEFAULT_SINK@"
            for s in pactl_json("list", "sink-inputs"):
                if s.get("sink") == sidx and not self.is_plumbing(s):
                    pactl("move-sink-input", str(s["index"]), real)
        self.set_capture_device("")
        for m in reversed(self.modules):
            pactl("unload-module", m)
        self.modules = []
        self.share_ready = False

    def route(self, raw):
        """Keeps every stream on the right side of the share sink (called on every scan)."""
        alive = {int(s["index"]) for s in raw}
        self.excluded &= alive                      # forget apps that closed
        if not self.excluded:
            if self.share_ready:
                self.teardown_share()
            return
        if not self.ensure_share():
            return
        sidx = self.sink_index(SHARE_SINK)
        for s in raw:
            if self.is_plumbing(s):
                continue
            idx = int(s["index"])
            on_share = s.get("sink") == sidx
            if idx in self.excluded and on_share:
                pactl("move-sink-input", str(idx), self.real_sink)
            elif idx not in self.excluded and not on_share:
                pactl("move-sink-input", str(idx), SHARE_SINK)

    def cleanup(self):
        self.excluded = set()
        self.teardown_share()

    @staticmethod
    def is_plumbing(s):
        """Our own loopback, other modules' streams, and Smash Soda's own sounds are not apps to mix."""
        p = s.get("properties", {})
        if "loopback" in str(s.get("driver", "")).lower() or s.get("owner_module") not in (None, "n/a"):
            return True
        app = (p.get("application.name") or "") + (p.get("application.process.binary") or "")
        return "smashsoda" in app.lower().replace(" ", "")

    # ------------------------------------------------------------ listing
    @staticmethod
    def app_name(s):
        p = s.get("properties", {})
        app = p.get("application.name") or p.get("application.process.binary") or "Unknown app"
        media = p.get("media.name") or ""
        generic = {"", "audio stream #1", "playback", "Playback", "AudioStream", "audio stream"}
        name = app if media in generic or media == app else f"{app} - {media}"
        return name[:47]

    def snapshot(self, raw):
        entries = []
        for s in raw:
            if self.is_plumbing(s):
                continue
            vols = [v.get("value_percent", "100%") for v in s.get("volume", {}).values()]
            try:
                vol = int(round(max(int(v.rstrip("%")) for v in vols))) if vols else 100
            except ValueError:
                vol = 100
            entries.append({
                "id": int(s["index"]),
                "volume": max(0, min(100, vol)),
                "muted": 1 if s.get("mute") else 0,
                "shared": 0 if int(s["index"]) in self.excluded else 1,
                "playing": 0 if s.get("corked") else 1,
                "name": self.app_name(s),
            })
        entries.sort(key=lambda e: (-e["playing"], e["name"].lower(), e["id"]))
        return entries[:MAX_APPS]

    def publish(self, entries):
        for i in range(MAX_APPS):
            off = HDR + i * ENTRY
            if i < len(entries):
                e = entries[i]
                name = e["name"].encode("utf-8", "replace")[:47]
                struct.pack_into("I4B8x48s", self.mm, off, e["id"], e["volume"], e["muted"], e["shared"], e["playing"], name)
            else:
                self.mm[off:off + ENTRY] = b"\0" * ENTRY
        struct.pack_into("I", self.mm, O_COUNT, len(entries))   # published last

    # ------------------------------------------------------------ commands from the app
    def handle_command(self):
        seq, ack = struct.unpack_from("II", self.mm, O_CMD_SEQ)
        if seq == ack:
            return
        ctype, cid = struct.unpack_from("II", self.mm, O_CMD_TYPE)
        value = struct.unpack_from("i", self.mm, O_CMD_VALUE)[0]
        cid = int(cid)
        if ctype == 1:      # volume
            pactl("set-sink-input-volume", str(cid), f"{max(0, min(100, value))}%")
        elif ctype == 2:    # mute
            pactl("set-sink-input-mute", str(cid), "1" if value else "0")
        elif ctype == 3:    # share with guests
            if value:
                self.excluded.discard(cid)
            else:
                self.excluded.add(cid)
            self.route(pactl_json("list", "sink-inputs"))
        struct.pack_into("I", self.mm, O_CMD_ACK, seq)

    def beat(self):
        hb = struct.unpack_from("I", self.mm, O_HEARTBEAT)[0]
        struct.pack_into("I", self.mm, O_HEARTBEAT, (hb + 1) & 0xFFFFFFFF)


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "smashsoda")
    os.makedirs(d, mode=0o700, exist_ok=True)
    mixer = Mixer(os.path.join(d, "audioapps.shm"), d)
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    signal.signal(signal.SIGINT, lambda *_: stop.append(1))
    log("ready")
    last_scan = 0.0
    try:
        while not stop:
            now = time.monotonic()
            mixer.handle_command()
            if now - last_scan >= 0.8:
                last_scan = now
                raw = pactl_json("list", "sink-inputs")
                mixer.route(raw)
                mixer.publish(mixer.snapshot(raw))
                mixer.beat()
            time.sleep(0.05)
    finally:
        mixer.cleanup()
        log("stopped; streams restored")


if __name__ == "__main__":
    main()
