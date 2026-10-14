#!/usr/bin/env python3
"""Native side: allocate CUDA memory, fill a pattern, export an IPC handle, wait."""
import ctypes, sys, time
c = ctypes.CDLL("libcuda.so.1")
def ck(r, what):
    if r: print(f"{what} failed: {r}"); sys.exit(1)
ck(c.cuInit(0), "cuInit")
dev = ctypes.c_int(); ck(c.cuDeviceGet(ctypes.byref(dev), 0), "cuDeviceGet")
ctx = ctypes.c_void_p(); ck(c.cuCtxCreate_v2(ctypes.byref(ctx), 0, dev), "cuCtxCreate")
ptr = ctypes.c_ulonglong(); ck(c.cuMemAlloc_v2(ctypes.byref(ptr), 1 << 20), "cuMemAlloc")
ck(c.cuMemsetD8_v2(ptr, 0xAB, 1 << 20), "memset")
h = (ctypes.c_char * 64)(); ck(c.cuIpcGetMemHandle(h, ptr), "cuIpcGetMemHandle")
open(sys.argv[1], "wb").write(bytes(h))
print("native: exported IPC handle, holding allocation for 40 s", flush=True)
time.sleep(float(sys.argv[2]) if len(sys.argv) > 2 else 40)
