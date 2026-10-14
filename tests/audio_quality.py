#!/usr/bin/env python3
"""Plays a pure tone, captures it with ssb-capture's audio pipeline and looks for glitches."""
import importlib.util, os, struct, subprocess, sys, time, array, math
import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib

spec = importlib.util.spec_from_file_location("cap", os.path.join(os.path.dirname(__file__), "..", "bridge", "ssb-capture.py"))
cap = importlib.util.module_from_spec(spec); spec.loader.exec_module(cap)
Gst.init(None)

d = os.environ.get("AQ_DIR", "/tmp/claude-1000/aq"); os.makedirs(d, exist_ok=True)
pub = cap.AudioPublisher(d + "/audio.shm")
import re
src = open(os.path.join(os.path.dirname(__file__), "..", "bridge", "ssb-capture.py")).read()
# same pipeline as production, but pointed at an isolated null sink so other desktop audio can't interfere
mod = subprocess.run(["pactl", "load-module", "module-null-sink", "sink_name=ssbtest", "channel_map=stereo", "rate=48000"], capture_output=True, text=True).stdout.strip()
pl = Gst.parse_launch(
    'pipewiresrc stream-properties="props,stream.capture.sink=true,node.name=SmashSodaTest" target-object=ssbtest ! '
    "audioconvert ! audioresample ! audio/x-raw,format=S16LE,rate=48000,channels=2,layout=interleaved ! "
    "appsink name=asink emit-signals=true max-buffers=16 drop=false sync=false")
def on_sample(s):
    smp = s.emit("pull-sample"); b = smp.get_buffer(); ok, mi = b.map(Gst.MapFlags.READ)
    if ok:
        pub.publish(bytes(mi.data)); b.unmap(mi)
    return Gst.FlowReturn.OK
pl.get_by_name("asink").connect("new-sample", on_sample)
tone = subprocess.Popen(["ffmpeg", "-loglevel", "quiet", "-re", "-f", "lavfi", "-i", "sine=frequency=440:duration=45",
                         "-af", "volume=0.2", "-f", "pulse", "-device", "ssbtest", "aq-test"])
loop = GLib.MainLoop()
pl.set_state(Gst.State.PLAYING)
GLib.timeout_add(int(float(os.environ.get("AQ_SECS","8.5"))*1000), loop.quit)
t0 = time.time()
loop.run()
pl.set_state(Gst.State.NULL); tone.terminate(); subprocess.run(["pactl", "unload-module", mod])
secs = time.time() - t0

frames = pub.pos
print(f"captured {frames} frames in {secs:.2f}s -> {frames/secs:.0f} frames/s (expect 48000)")
a = array.array("h"); a.frombytes(pub.mm[64:64 + min(frames, 65536) * 4])
n = len(a) // 2
left = a[0::2]
# analyse the last ~1 s that is still in the ring: tone should be a clean 440 Hz sine
seg = left[-48000:] if len(left) > 48000 else left
peak = max(abs(x) for x in seg)
zeros = sum(1 for i in range(1, len(seg)) if seg[i] == 0 and seg[i-1] == 0)
# discontinuity metric: deviation of second difference from that of an ideal sine at the observed amplitude
w = 2 * math.pi * 440 / 48000
maxd2 = max(abs(seg[i+1] - 2*seg[i] + seg[i-1]) for i in range(1, len(seg)-1))
expect = peak * (2 - 2*math.cos(w))
print(f"peak {peak}, consecutive zero pairs {zeros}, max 2nd difference {maxd2} (clean sine <= ~{expect*1.1:.0f})")
big = [i for i in range(1, len(seg)-1) if abs(seg[i+1] - 2*seg[i] + seg[i-1]) > expect*3]
print("glitch points in last second:", len(big), big[:10])
