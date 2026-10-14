#!/usr/bin/env python3
"""Prints input events arriving on the Smash Soda virtual devices."""
import os, re, select, struct, sys, time, glob

NAMES = ("Smash Soda Virtual Pad", "Smash Soda Virtual Keyboard/Mouse", "Smash Soda Virtual Pointer")
opened = {}
deadline = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 20)
EV = struct.Struct("llHHi")
codes = {(1,0x130):"BTN_A",(3,0):"ABS_X",(3,1):"ABS_Y",(3,5):"ABS_RZ",(3,16):"HAT0X",(3,17):"HAT0Y",
         (1,30):"KEY_A",(2,0):"REL_X",(2,1):"REL_Y",(1,0x110):"BTN_LEFT"}
while time.time() < deadline:
    for ev in glob.glob("/sys/class/input/event*"):
        try:
            name = open(ev + "/device/name").read().strip()
        except OSError:
            continue
        if name.startswith(NAMES) and ev not in opened:
            try:
                fd = os.open("/dev/input/" + os.path.basename(ev), os.O_RDONLY | os.O_NONBLOCK)
            except PermissionError:
                continue  # udev has not applied the seat ACL yet; retry
            opened[ev] = (name, fd)
            print(f"[+] {name} -> {os.path.basename(ev)}", flush=True)
    for ev, (name, fd) in list(opened.items()):
        if not os.path.exists(ev):
            os.close(fd); del opened[ev]; print(f"[-] {name} removed", flush=True); continue
        try:
            data = os.read(fd, EV.size * 64)
        except BlockingIOError:
            continue
        except OSError:
            continue
        for i in range(0, len(data) - EV.size + 1, EV.size):
            _, _, t, c, v = EV.unpack_from(data, i)
            if t == 0: continue
            print(f"    {name.split()[-1]}: {codes.get((t,c), f'type{t}/code{c}')} = {v}", flush=True)
    time.sleep(0.01)
