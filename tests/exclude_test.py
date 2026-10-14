import array, mmap, os, struct, subprocess, sys, time
T = "/tmp/claude-1000/ex"; os.makedirs(T, exist_ok=True)
for f in os.listdir(T): os.remove(os.path.join(T, f))
here = os.path.dirname(os.path.abspath(__file__))
def tone(name, freq, vol):
    return subprocess.Popen(["ffmpeg", "-loglevel", "quiet", "-re", "-f", "lavfi", "-i", f"sine=frequency={freq}:duration=50", "-af", f"volume={vol}", "-f", "pulse", "-stream_name", name, "x"], stderr=subprocess.DEVNULL)
loud = tone("tone-loud", 440, 3.0); quiet = tone("tone-quiet", 880, 0.6)
mix = subprocess.Popen([sys.executable, os.path.join(here, "..", "bridge", "ssb-audioapps.py"), T], stderr=subprocess.DEVNULL)
time.sleep(3)
fh = open(T + "/audioapps.shm", "r+b"); m = mmap.mmap(fh.fileno(), 0)
def find(tag):
    for i in range(struct.unpack_from("I", m, 12)[0]):
        off = 64 + i * 64
        if tag in m[off + 16:off + 64].split(b"\0")[0].decode(): return struct.unpack_from("I", m, off)[0]
def send(t, id_, v):
    seq = struct.unpack_from("I", m, 16)[0] + 1; struct.pack_into("IIi", m, 24, t, id_, v); struct.pack_into("I", m, 16, seq)
    for _ in range(40):
        if struct.unpack_from("I", m, 20)[0] == seq: return
        time.sleep(0.05)
def guest_peak(sec=1.5):
    dev = open(T + "/audio-device").read().strip() or "@DEFAULT_MONITOR@"      # what the capture helper would record
    p = subprocess.Popen(["parec", f"--device={dev}", "--format=s16le", "--rate=48000", "--channels=2", "--raw"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    time.sleep(0.6); data = b""; t = time.time()
    while time.time() - t < sec: data += os.read(p.stdout.fileno(), 65536)
    p.terminate(); a = array.array("h"); a.frombytes(data[: len(data) // 2 * 2])
    left = a[0::2]
    import math
    def tone_level(f):   # Goertzel: amplitude of one frequency
        w = 2 * math.pi * f / 48000; c = 2 * math.cos(w); s1 = s2 = 0.0
        for x in left: s1, s2 = x + c * s1 - s2, s1
        return math.sqrt(max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) * 2 / len(left)
    return f"440Hz(loud app)={tone_level(440):7.0f}  880Hz(quiet app)={tone_level(880):6.0f}", dev
li, qi = find("tone-loud"), find("tone-quiet")
print("both apps listed:", li is not None and qi is not None)
send(1, li, 100); send(1, qi, 100); send(2, li, 0); send(2, qi, 0); time.sleep(1.0)
def level(tag):
    pk, dev = guest_peak(2.5); print(f"{tag:46s} {pk}   [{dev}]")
level("1) nothing unshared (default output monitor)")
send(3, qi, 0); time.sleep(2.0)          # exclude the QUIET one: routing mode on, loud tone is shared
level("2) routing on, loud tone SHARED")
send(3, li, 0); time.sleep(2.0)          # now exclude the loud one as well
level("3) routing on, loud tone NOT shared  (440Hz must drop)")
send(3, li, 1); time.sleep(2.0)
level("4) loud tone shared again")
send(3, qi, 1); time.sleep(2.0)
level("5) everything shared again (back to default monitor)")
mix.terminate(); mix.wait(8); loud.terminate(); quiet.terminate()
time.sleep(0.5); print("leftover share sink modules:", subprocess.run("pactl list short modules | grep -c smashsoda_share", shell=True, capture_output=True, text=True).stdout.strip())
