#!/usr/bin/env python3
"""Runs the real capture helper in a given mode and reports kwin + helper CPU/GPU cost."""
import mmap, os, struct, subprocess, sys, time
mode = sys.argv[1]
T = f"/tmp/claude-1000/hc_{mode}"; os.makedirs(T, exist_ok=True)
for f in os.listdir(T): os.remove(os.path.join(T, f))
here = os.path.dirname(os.path.abspath(__file__))
sup = subprocess.Popen([sys.executable, os.path.join(here, "..", "bridge", "ssb-capture.py"), "--dir", T, "--remember", "--no-audio", "--capture-mode", mode],
                       stderr=open("/tmp/claude-1000/hc_err.log","w"))
time.sleep(2)
f = open(T + "/video.shm", "r+b"); m = mmap.mmap(f.fileno(), 0)
W = 16 + 24 + 4 + 32; S = W + 4
struct.pack_into("I", m, W, 1)
t = time.time()
while struct.unpack_from("I", m, S)[0] != 2 and time.time() - t < 30: time.sleep(0.2)
time.sleep(4)
kw = int(subprocess.run(["pgrep", "-x", "kwin_wayland"], capture_output=True, text=True).stdout.split()[0])
kids = subprocess.run(["pgrep", "-P", str(sup.pid)], capture_output=True, text=True).stdout.split()
helper = int(kids[0])
HZ = os.sysconf("SC_CLK_TCK")
def jf(pid):
    x = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split(); return int(x[11]) + int(x[12])
j0 = (jf(kw), jf(helper)); t0 = time.monotonic()
g = subprocess.Popen(["nvidia-smi", "pmon", "-c", "10", "-s", "u"], stdout=subprocess.PIPE, text=True)
l0 = struct.unpack_from("I", m, 12)[0]
time.sleep(10)
dt = time.monotonic() - t0; j1 = (jf(kw), jf(helper)); l1 = struct.unpack_from("I", m, 12)[0]
out = g.communicate()[0]
def sm(pid):
    v = [int(l.split()[3]) for l in out.splitlines() if l.strip() and not l.startswith("#") and l.split()[1] == str(pid) and l.split()[3].isdigit()]
    return sum(v) / 10.0
print(f"{mode:4s}: {(l1-l0)/dt:5.1f} fps | kwin CPU {(j1[0]-j0[0])/HZ/dt*100:5.1f}% GPU {sm(kw):4.1f}% | helper CPU {(j1[1]-j0[1])/HZ/dt*100:5.1f}% GPU {sm(helper):4.1f}%")
struct.pack_into("I", m, W, 0); time.sleep(1); sup.terminate()
