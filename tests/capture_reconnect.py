#!/usr/bin/env python3
"""Usage: capture_reconnect.py <supervisor pid>
Simulates Smash Soda pressing Host, then a dropped stream, against a running ssb-capture supervisor."""
import mmap, os, signal, struct, subprocess, sys, time

path = os.environ["XDG_RUNTIME_DIR"] + "/smashsoda/video.shm"
f = open(path, "r+b"); m = mmap.mmap(f.fileno(), 0)
W = 16 + 24 + 4 + 32; S = W + 4
state = lambda: struct.unpack_from("I", m, S)[0]
def wait_state(target, timeout):
    t = time.time()
    while state() != target and time.time() - t < timeout:
        time.sleep(0.25)
    return time.time() - t

struct.pack_into("I", m, W, 1)
print("pressed Host; waiting for the picker...", flush=True)
print("streaming after %.1fs (state %d)" % (wait_state(2, 90), state()), flush=True)
sess = subprocess.run(["pgrep", "-P", sys.argv[1]], capture_output=True, text=True).stdout.split()  # children of the supervisor
print("session pids:", sess, flush=True)
if sess:
    os.kill(int(sess[0]), signal.SIGKILL)
    time.sleep(1.0)
    time.sleep(0.5); print("state right after drop:", state(), "(1 = waiting)", flush=True)
    print("recovered to streaming after %.1fs (state %d)" % (wait_state(2, 40), state()), flush=True)
struct.pack_into("I", m, W, 0)
time.sleep(1.5)
print("after stop, state:", state(), flush=True)
