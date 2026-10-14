import mmap, os, struct, subprocess, sys, time
T = "/tmp/claude-1000/cs2"; os.makedirs(T, exist_ok=True)
for f in os.listdir(T): os.remove(os.path.join(T, f))
log = open(T + "/h.log", "w")
here = os.path.dirname(os.path.abspath(__file__))
sup = subprocess.Popen([sys.executable, os.path.join(here, "..", "bridge", "ssb-capture.py"), "--dir", T, "--no-audio", "--remember", "--capture-mode", "gpu",
                        "--token", os.path.expanduser("~/.local/share/smashsoda-linux/portal-token")], stderr=log)
time.sleep(2)
fh = open(T + "/video.shm", "r+b"); m = mmap.mmap(fh.fileno(), 0)
state = lambda: struct.unpack_from("I", m, 80)[0]
struct.pack_into("I", m, 76, 1)
t = time.time()
while state() != 2 and time.time() - t < 25: time.sleep(0.2)
kids = lambda: subprocess.run(["pgrep", "-P", str(sup.pid)], capture_output=True, text=True).stdout.split()
first = kids(); print("1) streaming, state", state(), "session pid", first)
struct.pack_into("I", m, 108, struct.unpack_from("I", m, 108)[0] + 1)      # the app's "Change Screen" button
time.sleep(1.5)
second = kids()
print("2) after Change Screen: state", state(), "(1 = waiting for the picker) | new session pid", second, "| force-pick marker consumed:", not os.path.exists(T + "/force-pick"))
t = time.time()
while state() != 2 and time.time() - t < 12: time.sleep(0.3)
print("3) picker answered within 12 s?", "yes -> streaming again (state %d)" % state() if state() == 2 else "no (nobody picked) - that is expected in an unattended test")
struct.pack_into("I", m, 76, 0); time.sleep(1.5); sup.terminate(); sup.wait(8)
print("helper log:", [l.strip().split("ssb-capture: ")[-1] for l in open(T + "/h.log") if "switching" in l or "capture mode" in l or "waiting for the screen" in l])
