#!/usr/bin/env python3
"""Runs ONLY the audio capture pipeline (default sink monitor) and reports the real fill rate."""
import importlib.util, os, struct, time, sys
import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib
spec = importlib.util.spec_from_file_location("cap", os.path.join(os.path.dirname(__file__), "..", "bridge", "ssb-capture.py"))
cap = importlib.util.module_from_spec(spec); spec.loader.exec_module(cap)
Gst.init(None)
d = "/tmp/claude-1000/ar"; os.makedirs(d, exist_ok=True)
pub = cap.AudioPublisher(d + "/audio.shm")
pl = cap.build_audio_pipeline(pub)
loop = GLib.MainLoop(); pl.set_state(Gst.State.PLAYING)
marks = []
def mark():
    marks.append((time.monotonic(), pub.pos)); return True
GLib.timeout_add(500, mark)
GLib.timeout_add(int(float(sys.argv[1] if len(sys.argv) > 1 else 8) * 1000), loop.quit)
loop.run(); pl.set_state(Gst.State.NULL)
(t0, p0), (t1, p1) = marks[2], marks[-1]
print("audio-only capture rate: %.0f frames/s (48000 = real time)" % ((p1 - p0) / (t1 - t0)))
