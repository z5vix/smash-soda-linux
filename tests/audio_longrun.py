#!/usr/bin/env python3
"""Runs the GStreamer capture and a parec capture side by side and prints each one's fill rate over time."""
import importlib.util, os, subprocess, sys, threading, time
import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib
spec = importlib.util.spec_from_file_location("cap", os.path.join(os.path.dirname(__file__), "..", "bridge", "ssb-capture.py"))
cap = importlib.util.module_from_spec(spec); spec.loader.exec_module(cap)
Gst.init(None)
d = "/tmp/claude-1000/alr"; os.makedirs(d, exist_ok=True)
pub = cap.AudioPublisher(d + "/audio.shm")
pl = cap.build_audio_pipeline(pub)

parec_frames = [0]
def parec_reader():
    p = subprocess.Popen(["parec", "--device=@DEFAULT_MONITOR@", "--format=s16le", "--rate=48000", "--channels=2",
                          "--latency-msec=20", "--raw"], stdout=subprocess.PIPE)
    while True:
        b = os.read(p.stdout.fileno(), 3840)
        if not b: break
        parec_frames[0] += len(b) // 4
threading.Thread(target=parec_reader, daemon=True).start()

loop = GLib.MainLoop(); pl.set_state(Gst.State.PLAYING)
total = float(sys.argv[1]); step = 15.0
last = [time.monotonic(), pub.pos, parec_frames[0]]
t_start = time.monotonic()
def report():
    now = time.monotonic(); dt = now - last[0]
    g = (pub.pos - last[1]) / dt; p = (parec_frames[0] - last[2]) / dt
    print("t=%3.0fs  gstreamer %6.0f f/s   parec %6.0f f/s" % (now - t_start, g, p), flush=True)
    last[:] = [now, pub.pos, parec_frames[0]]
    return True
GLib.timeout_add(int(step * 1000), report)
GLib.timeout_add(int(total * 1000), loop.quit)
loop.run(); pl.set_state(Gst.State.NULL)
