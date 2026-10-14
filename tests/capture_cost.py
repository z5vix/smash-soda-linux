#!/usr/bin/env python3
"""Measures what KWin pays for different ways of receiving the screen (CPU + GPU), 10 s each."""
import importlib.util, os, subprocess, sys, time
import gi
gi.require_version("Gst", "1.0"); gi.require_version("GstApp", "1.0")
from gi.repository import Gst, GLib
spec = importlib.util.spec_from_file_location("cap", os.path.join(os.path.dirname(__file__), "..", "bridge", "ssb-capture.py"))
cap = importlib.util.module_from_spec(spec); spec.loader.exec_module(cap)
Gst.init(None)

kw = int(subprocess.run(["pgrep", "-x", "kwin_wayland"], capture_output=True, text=True).stdout.split()[0])
def jiffies():
    f = open(f"/proc/{kw}/stat").read().rsplit(")", 1)[1].split()
    return int(f[11]) + int(f[12])
HZ = os.sysconf("SC_CLK_TCK")

VARIANTS = {
 "V0 current (CPU memory, drop after)": "pipewiresrc fd={fd} path={node} ! videorate drop-only=true max-rate=120 ! videoconvert ! video/x-raw,format={{BGRx,BGRA}} ! appsink name=s emit-signals=false sync=false max-buffers=2 drop=true",
 "V1 CPU memory, max-framerate=60 negotiated": "pipewiresrc fd={fd} path={node} ! video/x-raw,max-framerate=60/1 ! videoconvert ! video/x-raw,format={{BGRx,BGRA}} ! appsink name=s sync=false max-buffers=2 drop=true",
 "V2 DMA-BUF straight (no readback), 60": "pipewiresrc fd={fd} path={node} ! video/x-raw(memory:DMABuf),max-framerate=60/1 ! appsink name=s sync=false max-buffers=2 drop=true",
 "V4 DMA-BUF -> videorate(120) -> GL -> download":"pipewiresrc fd={fd} path={node} ! video/x-raw(memory:DMABuf) ! videorate drop-only=true max-rate=120 ! glupload ! glcolorconvert ! gldownload ! videoscale ! video/x-raw,format={{BGRx,BGRA}},width=2560,height=1080 ! appsink name=s sync=false max-buffers=2 drop=true",
 "V3 DMA-BUF -> GL -> download, 60": "pipewiresrc fd={fd} path={node} ! video/x-raw(memory:DMABuf),max-framerate=60/1 ! glupload ! glcolorconvert ! gldownload ! video/x-raw,format={{BGRx,BGRA}} ! appsink name=s sync=false max-buffers=2 drop=true",
}
only = sys.argv[1:]  # optional variant prefixes e.g. V2 V3
tok = os.path.expanduser("~/.local/share/smashsoda-linux/portal-token")
print(f"kwin pid {kw}; baseline idle sample first", flush=True)

def sample_gpu(seconds):
    p = subprocess.Popen(["nvidia-smi", "pmon", "-c", str(seconds), "-s", "u"], stdout=subprocess.PIPE, text=True)
    return p

def run(name, pipe):
    loop = GLib.MainLoop()
    portal = cap.Portal(loop, tok, True)
    node, size, pos = portal.start()
    fd = portal.open_pipewire_remote()
    try:
        pl = Gst.parse_launch(pipe.format(fd=fd, node=node))
    except GLib.Error as e:
        print(f"{name}: pipeline error {e.message}"); return
    sink = pl.get_by_name("s")
    frames = [0]
    # count frames without touching the pixels
    sink.set_property("emit-signals", True)
    def on(s):
        smp = s.emit("pull-sample"); frames[0] += 1 if smp else 0; return Gst.FlowReturn.OK
    sink.connect("new-sample", on)
    err = []
    bus = pl.get_bus(); bus.add_signal_watch()
    bus.connect("message::error", lambda b, m: (err.append(m.parse_error()[0].message), loop.quit()))
    pl.set_state(Gst.State.PLAYING)
    GLib.timeout_add(3000, loop.quit); loop.run()           # warm up
    if err: print(f"{name}: ERROR {err[0]}"); pl.set_state(Gst.State.NULL); return
    f0 = frames[0]; j0 = jiffies(); t0 = time.monotonic(); g = sample_gpu(10)
    GLib.timeout_add(10000, loop.quit); loop.run()
    dt = time.monotonic() - t0; j1 = jiffies(); fr = frames[0] - f0
    out = g.communicate(timeout=20)[0]
    sm = [int(l.split()[3]) for l in out.splitlines() if l.strip() and not l.startswith("#") and l.split()[1] == str(kw) and l.split()[0] == "0" and l.split()[3].isdigit()]
    smv = sum(sm) / 10.0
    pl.set_state(Gst.State.NULL)
    print(f"{name:46s} {fr/dt:5.1f} fps | kwin CPU {(j1-j0)/HZ/dt*100:5.1f}% | kwin GPU SM {smv:5.1f}%", flush=True)

for name, pipe in VARIANTS.items():
    if only and not any(name.startswith(o) for o in only): continue
    try: run(name, pipe)
    except Exception as e: print(f"{name}: failed: {e}")
    time.sleep(2)
