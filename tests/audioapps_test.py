#!/usr/bin/env python3
import json, mmap, os, struct, subprocess, sys, time
T = "/tmp/claude-1000/aa"; os.makedirs(T, exist_ok=True)
here = os.path.dirname(os.path.abspath(__file__))
tone = subprocess.Popen(["ffmpeg", "-loglevel", "quiet", "-re", "-f", "lavfi", "-i", "sine=frequency=330:duration=60", "-af", "volume=0.03",
                         "-f", "pulse", "-stream_name", "ssb-test-tone", "x"], stderr=subprocess.DEVNULL)
mix = subprocess.Popen([sys.executable, os.path.join(here, "..", "bridge", "ssb-audioapps.py"), T], stderr=subprocess.DEVNULL)
time.sleep(3)
fh = open(T + "/audioapps.shm", "r+b"); m = mmap.mmap(fh.fileno(), 0)
def apps():
    n = struct.unpack_from("I", m, 12)[0]; out = []
    for i in range(n):
        off = 64 + i * 64
        id_, vol, mute, share, play = struct.unpack_from("I4B", m, off); name = m[off + 16:off + 64].split(b"\0")[0].decode()
        out.append(dict(id=id_, vol=vol, mute=mute, share=share, play=play, name=name))
    return out
def send(t, id_, v):
    seq = struct.unpack_from("I", m, 16)[0] + 1
    struct.pack_into("IIi", m, 24, t, id_, v); struct.pack_into("I", m, 16, seq)
    for _ in range(40):
        if struct.unpack_from("I", m, 20)[0] == seq: return True
        time.sleep(0.05)
    return False
def si(id_):
    for s in json.loads(subprocess.run(["pactl", "--format=json", "list", "sink-inputs"], capture_output=True, text=True).stdout):
        if s["index"] == id_: return s
def sinks(): return {s["index"]: s["name"] for s in json.loads(subprocess.run(["pactl", "--format=json", "list", "sinks"], capture_output=True, text=True).stdout)}
print("listed apps:", [(a["name"], a["vol"], a["share"]) for a in apps()])
mine = next((a for a in apps() if "ssb-test-tone" in a["name"] or "ffmpeg" in a["name"].lower()), None)
if not mine: print("test tone stream not found"); sys.exit(1)
i = mine["id"]
print("volume 30 applied:", send(1, i, 30), "-> pactl says", list(si(i)["volume"].values())[0]["value_percent"])
print("mute applied:", send(2, i, 1), "-> pactl mute =", si(i)["mute"]); send(2, i, 0)
print("share off applied:", send(3, i, 0)); time.sleep(1.2)
names = sinks(); print("stream now on sink:", names.get(si(i)["sink"]), "| private sink exists:", "smashsoda_private" in names.values())
print("listed as shared?", [a["share"] for a in apps() if a["id"] == i])
print("share on applied:", send(3, i, 1)); time.sleep(1.2); print("stream back on sink:", sinks().get(si(i)["sink"]))
send(3, i, 0); time.sleep(1)
mix.terminate(); mix.wait(8); tone.terminate()
time.sleep(0.5)
print("after shutdown - private sink gone:", "smashsoda_private" not in sinks().values())
