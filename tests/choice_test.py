import mmap, os, struct, subprocess, sys, time
mode, source = int(sys.argv[1]), int(sys.argv[2])
T = f"/tmp/claude-1000/ch_{mode}"; os.makedirs(T, exist_ok=True)
for f in os.listdir(T): os.remove(os.path.join(T, f))
log = open(T + "/h.log", "w")
sup = subprocess.Popen([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bridge", "ssb-capture.py"),
                        "--dir", T, "--no-audio", "--token", os.path.expanduser("~/.local/share/smashsoda-linux/portal-token"), "--remember"], stderr=log)
time.sleep(2)
fh = open(T + "/video.shm", "r+b"); m = mmap.mmap(fh.fileno(), 0)
struct.pack_into("III", m, 96, mode, source, 1)          # capture_mode, source_types, remember
struct.pack_into("I", m, 76, 1)                          # want_capture
t = time.time()
while struct.unpack_from("I", m, 80)[0] != 2 and time.time() - t < 25: time.sleep(0.2)
time.sleep(1.5)
struct.pack_into("I", m, 76, 0); time.sleep(1); sup.terminate()
print(f"app asked mode={mode} source={source}:", [l.strip().split("ssb-capture: ")[-1] for l in open(T + "/h.log") if "capture mode" in l or "streaming node" in l])
