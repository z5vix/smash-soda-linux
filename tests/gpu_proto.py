#!/usr/bin/env python3
"""Prototype: screen -> DMA-BUF -> GL -> CUDA memory -> device pointer -> D2D copy into an IPC-exportable ring."""
import ctypes, importlib.util, os, sys, time
import gi
gi.require_version("Gst", "1.0"); gi.require_version("GstCuda", "1.0")
from gi.repository import Gst, GLib, GstCuda
spec = importlib.util.spec_from_file_location("cap", os.path.join(os.path.dirname(__file__), "..", "bridge", "ssb-capture.py"))
cap = importlib.util.module_from_spec(spec); spec.loader.exec_module(cap)
Gst.init(None)

cu = ctypes.CDLL("libcuda.so.1")
class MEMCPY2D(ctypes.Structure):
    _fields_ = [("srcXInBytes", ctypes.c_size_t), ("srcY", ctypes.c_size_t), ("srcMemoryType", ctypes.c_int), ("srcHost", ctypes.c_void_p),
                ("srcDevice", ctypes.c_ulonglong), ("srcArray", ctypes.c_void_p), ("srcPitch", ctypes.c_size_t),
                ("dstXInBytes", ctypes.c_size_t), ("dstY", ctypes.c_size_t), ("dstMemoryType", ctypes.c_int), ("dstHost", ctypes.c_void_p),
                ("dstDevice", ctypes.c_ulonglong), ("dstArray", ctypes.c_void_p), ("dstPitch", ctypes.c_size_t),
                ("WidthInBytes", ctypes.c_size_t), ("Height", ctypes.c_size_t)]
class PyBuf(ctypes.Structure):
    _fields_ = [("buf", ctypes.c_void_p), ("obj", ctypes.py_object), ("len", ctypes.c_ssize_t), ("itemsize", ctypes.c_ssize_t),
                ("readonly", ctypes.c_int), ("ndim", ctypes.c_int), ("format", ctypes.c_char_p), ("shape", ctypes.c_void_p),
                ("strides", ctypes.c_void_p), ("suboffsets", ctypes.c_void_p), ("internal", ctypes.c_void_p)]
def addr_of(mv):
    v = PyBuf(); ctypes.pythonapi.PyObject_GetBuffer(ctypes.py_object(mv), ctypes.byref(v), 0)
    a = v.buf; ctypes.pythonapi.PyBuffer_Release(ctypes.byref(v)); return a

tok = os.path.expanduser("~/.local/share/smashsoda-linux/portal-token")
loop = GLib.MainLoop()
portal = cap.Portal(loop, tok, True)
node, size, pos = portal.start(); fd = portal.open_pipewire_remote()
W, H = size
pipe = (f"pipewiresrc fd={fd} path={node} ! video/x-raw(memory:DMABuf) ! videorate drop-only=true max-rate=120 ! "
        f"glupload ! glcolorconvert ! cudaupload ! video/x-raw(memory:CUDAMemory),format=BGRx ! appsink name=s emit-signals=true sync=false max-buffers=2 drop=true")
pl = Gst.parse_launch(pipe)
state = {"n": 0, "info": None, "ring": None, "t_copy": 0.0, "err": None, "ptr_seen": []}
gstlib = ctypes.CDLL("libgstreamer-1.0.so.0")
gstlib.gst_buffer_peek_memory.restype = ctypes.c_void_p; gstlib.gst_buffer_peek_memory.argtypes = [ctypes.c_void_p, ctypes.c_uint]
gstlib.gst_memory_map.restype = ctypes.c_int
class MapInfo(ctypes.Structure):
    _fields_ = [("memory", ctypes.c_void_p), ("flags", ctypes.c_int), ("data", ctypes.c_void_p), ("size", ctypes.c_size_t), ("maxsize", ctypes.c_size_t),
                ("user_data", ctypes.c_void_p * 4), ("reserved", ctypes.c_void_p * 4)]
gstlib.gst_memory_map.argtypes = [ctypes.c_void_p, ctypes.POINTER(MapInfo), ctypes.c_int]
gstlib.gst_memory_unmap.argtypes = [ctypes.c_void_p, ctypes.POINTER(MapInfo)]
ctypes.pythonapi.PyCapsule_GetPointer.restype = ctypes.c_void_p
ctypes.pythonapi.PyCapsule_GetPointer.argtypes = [ctypes.py_object, ctypes.c_char_p]

# our CUDA context = the primary context of device 0, which is the one gst-cuda uses too
cu.cuInit(0)
dev = ctypes.c_int(0); cu.cuDeviceGet(ctypes.byref(dev), 0)
pctx = ctypes.c_void_p(); print("primary ctx retain rc", cu.cuDevicePrimaryCtxRetain(ctypes.byref(pctx), dev))

def on(s):
    smp = s.emit("pull-sample")
    buf = smp.get_buffer()
    caps = smp.get_caps(); st = caps.get_structure(0)
    if state["info"] is None:
        print("caps:", caps.to_string()[:120]); state["info"] = (st.get_value("width"), st.get_value("height"))
    bptr = ctypes.c_void_p.from_address(id(buf) + 16).value   # PyGBoxed: PyObject_HEAD, then the boxed pointer
    mem = gstlib.gst_buffer_peek_memory(bptr, 0)
    info = MapInfo()
    ok = gstlib.gst_memory_map(mem, ctypes.byref(info), 1 | (1 << 17))     # GST_MAP_READ | GST_MAP_CUDA
    if not ok: state["err"] = "gst_memory_map failed"; return Gst.FlowReturn.OK
    try:
        ptr = info.data; w, h = state["info"]
        gi.require_version("GstVideo", "1.0")
        from gi.repository import GstVideo
        vm = GstVideo.buffer_get_video_meta(buf); stride = vm.stride[0] if vm else w * 4
        state["ptr_seen"].append(ptr)
        if state["ring"] is None:
            cu.cuCtxPushCurrent_v2(pctx)
            pitch = ctypes.c_size_t(); dptr = ctypes.c_ulonglong()
            r = cu.cuMemAllocPitch_v2(ctypes.byref(dptr), ctypes.byref(pitch), w * 4, h, 16)
            hnd = (ctypes.c_char * 64)(); r2 = cu.cuIpcGetMemHandle(hnd, dptr)
            cu.cuCtxPopCurrent_v2(None)
            print(f"ring alloc rc={r}, ipc handle rc={r2}, pitch {pitch.value}, src stride {stride}, mapped size {info.size}")
            state["ring"] = (dptr.value, pitch.value, pctx.value)
        dptr, pitch, hctx = state["ring"]
        c = MEMCPY2D(); c.srcMemoryType = 2; c.srcDevice = ptr; c.srcPitch = stride
        c.dstMemoryType = 2; c.dstDevice = dptr; c.dstPitch = pitch; c.WidthInBytes = w * 4; c.Height = h
        t0 = time.perf_counter()
        cu.cuCtxPushCurrent_v2(ctypes.c_void_p(hctx)); rc = cu.cuMemcpy2D_v2(ctypes.byref(c)); cu.cuCtxPopCurrent_v2(None)
        state["t_copy"] += time.perf_counter() - t0
        if rc: state["err"] = f"cuMemcpy2D rc={rc}"
    finally:
        gstlib.gst_memory_unmap(mem, ctypes.byref(info))
    state["n"] += 1
    return Gst.FlowReturn.OK
pl.get_by_name("s").connect("new-sample", on)
bus = pl.get_bus(); bus.add_signal_watch()
bus.connect("message::error", lambda b, m: (state.__setitem__("err", m.parse_error()[0].message + " | " + (m.parse_error()[1] or "")), loop.quit()))
pl.set_state(Gst.State.PLAYING)
GLib.timeout_add(8000, loop.quit); loop.run()
pl.set_state(Gst.State.NULL)
print("frames:", state["n"], "error:", state["err"])
if state["n"]:
    print("avg D2D copy %.3f ms" % (state["t_copy"] / state["n"] * 1000))
    # verify content: read back the ring and check it is not black
    dptr, pitch, hctx = state["ring"]; w, h = state["info"]
    host = (ctypes.c_ubyte * (w * 4 * h))()
    c = MEMCPY2D(); c.srcMemoryType = 2; c.srcDevice = dptr; c.srcPitch = pitch; c.dstMemoryType = 1; c.dstHost = ctypes.addressof(host); c.dstPitch = w * 4
    c.WidthInBytes = w * 4; c.Height = h
    cu.cuCtxPushCurrent_v2(ctypes.c_void_p(hctx)); rc = cu.cuMemcpy2D_v2(ctypes.byref(c)); cu.cuCtxPopCurrent_v2(None)
    nz = sum(1 for i in range(0, len(host), 4099) if host[i]); print("readback rc", rc, "- non-zero samples:", nz, "of", len(range(0, len(host), 4099)))
    print("distinct device pointers from gst pool:", len(set(state["ptr_seen"])))
