// nvEncodeAPI64.dll proxy for running Parsec's host under Proton.
//
// Parsec opens its NVENC session on a Direct3D 11 device and feeds it D3D11 textures. Proton's
// NVENC bridge only understands CUDA contexts and CUDA device pointers, so Parsec reports
// "host does not support hardware video encoding". This proxy sits in front of the real
// (Proton) library and translates:
//
//   NvEncOpenEncodeSession[Ex](DIRECTX, ID3D11Device*)  -> a CUDA session on the same GPU
//   NvEncRegisterResource(DIRECTX, ID3D11Texture2D*)    -> a CUDA pitched buffer is registered instead
//   NvEncMapInputResource                               -> texture is copied into that buffer first
//
// Everything else is forwarded untouched (and logged, so we can see what Parsec does).
// Chain:  parsec.dll -> this proxy -> nvEncodeAPI64_inner.dll (legacy-preset shim) -> Proton NVENC.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <map>
#include <vector>
#include <mutex>
#include <string>
#include <utility>
#include "ssb_proto.h"

// ------------------------------------------------------------------ logging
static std::string bridgeLogPath()
{
	const char* dir = getenv("SSB_BRIDGE_DIR");
	if (!dir || !*dir) return std::string();
	std::string d(dir);
	if (d[0] == '/') d = "Z:" + d;
	for (auto& c : d) if (c == '/') c = '\\';
	return d + "\\smashsoda-wine.log";
}

static void LOGF(const char* fmt, ...)
{
	static std::mutex m;
	static std::string path = bridgeLogPath();
	if (path.empty()) return;
	char buf[600];
	SYSTEMTIME t; GetLocalTime(&t);
	int n = snprintf(buf, sizeof buf, "%02d:%02d:%02d nvenc: ", t.wHour, t.wMinute, t.wSecond);
	va_list ap; va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
	va_end(ap);
	buf[n++] = '\n';
	std::lock_guard<std::mutex> lock(m);
	HANDLE f = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return;
	DWORD w; WriteFile(f, buf, (DWORD)n, &w, nullptr); CloseHandle(f);
}

// ------------------------------------------------------------------ CUDA (loaded dynamically)
typedef int CUresult;
typedef void* CUcontext;
typedef int CUdevice;
typedef unsigned long long CUdeviceptr;

struct CUipcMemHandle { char reserved[64]; };

struct CUDA_MEMCPY2D {
	size_t srcXInBytes, srcY;
	int srcMemoryType; const void* srcHost; CUdeviceptr srcDevice; void* srcArray; size_t srcPitch;
	size_t dstXInBytes, dstY;
	int dstMemoryType; void* dstHost; CUdeviceptr dstDevice; void* dstArray; size_t dstPitch;
	size_t WidthInBytes, Height;
};

static struct {
	HMODULE h = nullptr;
	CUresult (WINAPI *cuInit)(unsigned) = nullptr;
	CUresult (WINAPI *cuDeviceGetCount)(int*) = nullptr;
	CUresult (WINAPI *cuDeviceGet)(CUdevice*, int) = nullptr;
	CUresult (WINAPI *cuDeviceGetName)(char*, int, CUdevice) = nullptr;
	CUresult (WINAPI *cuCtxCreate)(CUcontext*, unsigned, CUdevice) = nullptr;
	CUresult (WINAPI *cuCtxDestroy)(CUcontext) = nullptr;
	CUresult (WINAPI *cuCtxPushCurrent)(CUcontext) = nullptr;
	CUresult (WINAPI *cuCtxPopCurrent)(CUcontext*) = nullptr;
	CUresult (WINAPI *cuMemAllocPitch)(CUdeviceptr*, size_t*, size_t, size_t, unsigned) = nullptr;
	CUresult (WINAPI *cuMemFree)(CUdeviceptr) = nullptr;
	CUresult (WINAPI *cuMemcpy2D)(const CUDA_MEMCPY2D*) = nullptr;
	CUresult (WINAPI *cuIpcOpenMemHandle)(CUdeviceptr*, CUipcMemHandle, unsigned) = nullptr;
	CUresult (WINAPI *cuIpcCloseMemHandle)(CUdeviceptr) = nullptr;
	bool ok = false;
} cu;

static bool cudaLoad()
{
	static bool tried = false;
	if (tried) return cu.ok;
	tried = true;
	cu.h = LoadLibraryA("nvcuda.dll");
	if (!cu.h) { LOGF("CUDA: nvcuda.dll not available"); return false; }
#define L(name, sym) cu.name = (decltype(cu.name))GetProcAddress(cu.h, sym); if (!cu.name) { LOGF("CUDA: missing %s", sym); return false; }
	L(cuInit, "cuInit") L(cuDeviceGetCount, "cuDeviceGetCount") L(cuDeviceGet, "cuDeviceGet")
	L(cuDeviceGetName, "cuDeviceGetName") L(cuCtxCreate, "cuCtxCreate_v2") L(cuCtxDestroy, "cuCtxDestroy_v2")
	L(cuCtxPushCurrent, "cuCtxPushCurrent_v2") L(cuCtxPopCurrent, "cuCtxPopCurrent_v2")
	cu.cuIpcOpenMemHandle = (decltype(cu.cuIpcOpenMemHandle))GetProcAddress(cu.h, "cuIpcOpenMemHandle_v2");
	cu.cuIpcCloseMemHandle = (decltype(cu.cuIpcCloseMemHandle))GetProcAddress(cu.h, "cuIpcCloseMemHandle");
	L(cuMemAllocPitch, "cuMemAllocPitch_v2") L(cuMemFree, "cuMemFree_v2") L(cuMemcpy2D, "cuMemcpy2D_v2")
#undef L
	CUresult r = cu.cuInit(0);
	if (r) { LOGF("CUDA: cuInit failed %d", r); return false; }
	cu.ok = true;
	return true;
}

// ------------------------------------------------------------------ NVENC plumbing
// Only the stable leading fields of the structs we touch (identical in every SDK version).
struct OPENEX_PARAMS { uint32_t version, deviceType; void* device; void* reserved; uint32_t apiVersion; };
struct REGISTER_RESOURCE { uint32_t version, resourceType, width, height, pitch, subResourceIndex; void* resourceToRegister; void* registeredResource; uint32_t bufferFormat; };
struct MAP_INPUT_RESOURCE { uint32_t version, subResourceIndex; void* inputResource; void* registeredResource; void* mappedResource; uint32_t mappedBufferFmt; };

enum { NV_DEVICE_DIRECTX = 0, NV_DEVICE_CUDA = 1 };
enum { NV_RES_DIRECTX = 0, NV_RES_CUDADEVICEPTR = 1 };

typedef int (WINAPI *PFN_Create)(void*);
typedef int (WINAPI *PFN_GetMax)(uint32_t*);
typedef int (WINAPI *Fn4)(void*, void*, void*, void*);

static const int SLOTS = 48;
static void* g_orig[SLOTS];
static HMODULE g_inner;
static PFN_Create g_innerCreate;
static std::mutex g_mutex;

// Latest frame already uploaded to CUDA by the app thread (see SsbOfferFrame): the encoder thread then only
// needs a GPU-side copy instead of a GPU->CPU->GPU round trip.
struct FrameStore {
	CUdeviceptr buf[2] = { 0, 0 };
	size_t pitch = 0;
	uint32_t w = 0, h = 0;
	int latest = -1;
	void* tex = nullptr;       // the D3D11 texture this frame was submitted as
	double doneMs = 0;
};
static uint64_t g_nextSessionId = 1;
struct Session {
	uint64_t id = g_nextSessionId++;
	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	CUcontext cuctx = nullptr;
	FrameStore fs;
};
// CUDA buffer + staging texture for one D3D11 texture. Parsec registers and unregisters its texture
// around every frame, so these are cached and reused instead of being rebuilt 60 times a second.
struct Buf {
	Session* session;
	ID3D11Texture2D* tex;          // AddRef'd while cached
	D3D11_TEXTURE2D_DESC desc;
	uint32_t width, height;
	CUdeviceptr ptr;
	size_t pitch;
	ID3D11Texture2D* staging = nullptr;
	uint64_t uses = 0;
	uint64_t lastUse = 0;
	int registrations = 0;         // Parsec registrations currently pointing at ptr
	void* enc = nullptr;           // NVENC encoder that owns nvHandle
	void* nvHandle = nullptr;      // real NVENC registration, kept alive across Parsec's per-frame register/unregister
	uint32_t nvFormat = 0;
};
struct Registered {
	Buf* buf;
	uint32_t subResource;
	uint64_t maps = 0;
};
static std::map<ID3D11Texture2D*, Buf*> g_bufs;
static uint64_t g_useClock = 0;
static std::map<void*, Session*> g_sessions;      // NVENC encoder handle -> session
static std::map<void*, Registered*> g_registered; // NVENC registered handle -> info

static const char* const SLOT_NAMES[SLOTS] = {
	"OpenEncodeSession", "GetEncodeGUIDCount", "GetEncodeProfileGUIDCount", "GetEncodeProfileGUIDs", "GetEncodeGUIDs",
	"GetInputFormatCount", "GetInputFormats", "GetEncodeCaps", "GetEncodePresetCount", "GetEncodePresetGUIDs",
	"GetEncodePresetConfig", "InitializeEncoder", "CreateInputBuffer", "DestroyInputBuffer", "CreateBitstreamBuffer",
	"DestroyBitstreamBuffer", "EncodePicture", "LockBitstream", "UnlockBitstream", "LockInputBuffer",
	"UnlockInputBuffer", "GetEncodeStats", "GetSequenceParams", "RegisterAsyncEvent", "UnregisterAsyncEvent",
	"MapInputResource", "UnmapInputResource", "DestroyEncoder", "InvalidateRefFrames", "OpenEncodeSessionEx",
	"RegisterResource", "UnregisterResource", "ReconfigureEncoder", "Reserved1", "CreateMVBuffer", "DestroyMVBuffer",
	"RunMotionEstimationOnly", "GetLastErrorString", "SetIOCudaStreams", "GetEncodePresetConfigEx", "GetSequenceParamEx",
	"Slot41", "Slot42", "Slot43", "Slot44", "Slot45", "Slot46", "Slot47"
};

static int g_calls[SLOTS];

// Generic pass-through that logs the first few calls of each function and every failure.
template <int I>
static int WINAPI Passthrough(void* a, void* b, void* c, void* d)
{
	int r = ((Fn4)g_orig[I])(a, b, c, d);
	int n = ++g_calls[I];
	if (n <= 3 || r != 0) {
		if (n <= 20 || r != 0) LOGF("%s -> %d (call %d)", SLOT_NAMES[I], r, n);
	}
	return r;
}

template <int... Is> struct Seq {};
template <int N, int... Is> struct Gen : Gen<N - 1, N - 1, Is...> {};
template <int... Is> struct Gen<0, Is...> { using type = Seq<Is...>; };
template <int... Is> static void fillStubs(void** t, Seq<Is...>) { void* tmp[] = { (void*)&Passthrough<Is>... }; for (int i = 0; i < SLOTS; i++) t[i] = tmp[i]; }

// ---- helpers
static size_t bytesPerPixel(DXGI_FORMAT f, bool& planar)
{
	planar = false;
	switch (f) {
	case DXGI_FORMAT_NV12: planar = true; return 1;
	case DXGI_FORMAT_P010: case DXGI_FORMAT_P016: planar = true; return 2;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
	case DXGI_FORMAT_R8_UNORM: return 1;
	default: return 4; // 8:8:8:8 and 10:10:10:2 variants
	}
}

static bool pickCudaDevice(ID3D11Device* dev, CUdevice* out)
{
	int count = 0;
	if (cu.cuDeviceGetCount(&count) || count <= 0) return false;
	std::string want;
	IDXGIDevice* dx = nullptr;
	if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx)) && dx) {
		IDXGIAdapter* ad = nullptr;
		if (SUCCEEDED(dx->GetAdapter(&ad)) && ad) {
			DXGI_ADAPTER_DESC d; ad->GetDesc(&d);
			char nm[256]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, nm, sizeof nm, nullptr, nullptr);
			want = nm; ad->Release();
		}
		dx->Release();
	}
	for (int i = 0; i < count; i++) {
		CUdevice cd; char nm[256] = "";
		if (cu.cuDeviceGet(&cd, i)) continue;
		cu.cuDeviceGetName(nm, sizeof nm, cd);
		LOGF("CUDA device %d: %s", i, nm);
		if (!want.empty() && want.find(nm) != std::string::npos) { *out = cd; LOGF("matched D3D11 adapter '%s' to CUDA device %d", want.c_str(), i); return true; }
	}
	LOGF("no CUDA device matched adapter '%s'; using device 0", want.c_str());
	return cu.cuDeviceGet(out, 0) == 0;
}

static Session* makeSession(ID3D11Device* dev, CUcontext* ctxOut)
{
	if (!cudaLoad()) return nullptr;
	CUdevice cd;
	if (!pickCudaDevice(dev, &cd)) return nullptr;
	CUcontext c = nullptr;
	CUresult r = cu.cuCtxCreate(&c, 0, cd);
	if (r) { LOGF("cuCtxCreate failed %d", r); return nullptr; }
	CUcontext popped; cu.cuCtxPopCurrent(&popped);
	auto* s = new Session();
	s->dev = dev; dev->AddRef(); dev->GetImmediateContext(&s->ctx);
	s->cuctx = c; *ctxOut = c;
	return s;
}


// frame-pipeline timing (per 5 s window)
static LARGE_INTEGER g_qf, g_lastMap;
static double g_periodMax = 0, g_encMs = 0, g_encMax = 0, g_lockMs = 0, g_lockMax = 0;
static int g_stalls = 0, g_stallLogs = 0;
static uint32_t g_periodHist[8] = {};
static double g_lastSubmit = 0, g_pickupMs = 0, g_pickupMax = 0; static uint64_t g_submits = 0, g_submitsSeen = 0;
static double g_regMs = 0, g_mapOrigMs = 0, g_unmapMs = 0, g_unregMs = 0, g_idleMs = 0, g_busyMs = 0;
static double g_lastUnregEnd = 0, g_regStart = 0; static uint64_t g_regCount = 0;
static double qpcMs() { LARGE_INTEGER t; QueryPerformanceCounter(&t); if (!g_qf.QuadPart) QueryPerformanceFrequency(&g_qf); return t.QuadPart * 1000.0 / g_qf.QuadPart; }

static uint64_t g_gpuModeFrames = 0;
static uint64_t g_fastCopies = 0, g_slowCopies = 0; static double g_lastFastMs = 0;
static bool g_fastVerified = false, g_fastDisabled = false; static int g_verifyTries = 0;
static double g_offerMs = 0; static uint64_t g_offers = 0;

// Called by Smash Soda for every new frame, right before it submits the texture to Parsec. `px` is tightly
// packed BGRA (w*4 bytes per row) in shared memory; `tex` is the texture about to be submitted.
extern "C" __declspec(dllexport) void WINAPI SsbOfferFrame(const void* px, uint32_t w, uint32_t h, void* tex)
{
	// The lock is held for the whole upload: Parsec destroys sessions on its own thread when guests join or leave,
	// and a session deleted mid-copy would be a use-after-free.
	std::lock_guard<std::mutex> l(g_mutex);
	if (g_sessions.empty()) return;
	Session* s = g_sessions.begin()->second;
	FrameStore& fs = s->fs;
	CUcontext popped;
	if (fs.w != w || fs.h != h) {
		cu.cuCtxPushCurrent(s->cuctx);
		for (int i = 0; i < 2; i++) { if (fs.buf[i]) cu.cuMemFree(fs.buf[i]); fs.buf[i] = 0; }
		size_t pitch = 0, pitch1 = 0;
		CUresult r0 = cu.cuMemAllocPitch(&fs.buf[0], &pitch, (size_t)w * 4, h, 16);
		CUresult r1 = cu.cuMemAllocPitch(&fs.buf[1], &pitch1, (size_t)w * 4, h, 16);
		cu.cuCtxPopCurrent(&popped);
		if (r0 || r1) { LOGF("frame store alloc failed (%d,%d)", r0, r1); fs.w = fs.h = 0; fs.latest = -1; return; }
		fs.pitch = pitch; fs.w = w; fs.h = h; fs.latest = -1;
		LOGF("frame store: %ux%u, pitch %zu", w, h, pitch);
	}
	const int idx = (fs.latest + 1) & 1;   // write the buffer the encoder is NOT reading
	const double t0 = qpcMs();
	CUDA_MEMCPY2D c = {};
	c.srcMemoryType = 1; c.srcHost = px; c.srcPitch = (size_t)w * 4;
	c.dstMemoryType = 2; c.dstDevice = fs.buf[idx]; c.dstPitch = fs.pitch;
	c.WidthInBytes = (size_t)w * 4; c.Height = h;
	cu.cuCtxPushCurrent(s->cuctx);
	CUresult cr = cu.cuMemcpy2D(&c);
	cu.cuCtxPopCurrent(&popped);
	if (cr) { LOGF("frame upload failed %d", cr); return; }
	fs.latest = idx; fs.tex = tex; fs.doneMs = qpcMs();
	g_offerMs += fs.doneMs - t0; g_offers++;
}

// 1 while encoding from the pre-uploaded frames is verified and in use (the D3D texture is not read then).
extern "C" __declspec(dllexport) int WINAPI SsbFastPathActive()
{
	if (g_gpuModeFrames && g_lastFastMs > 0 && qpcMs() - g_lastFastMs < 150.0) return 1;   // GPU ring in use
	return (g_fastVerified && !g_fastDisabled && g_lastFastMs > 0 && qpcMs() - g_lastFastMs < 150.0) ? 1 : 0;
}

extern "C" __declspec(dllexport) void WINAPI SsbNoteSubmit() { g_lastSubmit = qpcMs(); g_submits++; }

// ---- diagnostic wrappers (log what Parsec asks for and what NVENC returns)
static std::string guidStr(const void* g)
{
	if (!g) return "null";
	const uint8_t* b = (const uint8_t*)g;
	char t[64];
	snprintf(t, sizeof t, "%08x-%04x-%04x", *(uint32_t*)b, *(uint16_t*)(b + 4), *(uint16_t*)(b + 6));
	return t;
}

static int WINAPI W_GetEncodeCaps(void* enc, void* guid, void* capsParam, void* capsVal)
{
	int r = ((Fn4)g_orig[7])(enc, guid, capsParam, capsVal);
	LOGF("GetEncodeCaps(guid %s, cap %u) -> status %d value %d", guidStr(guid).c_str(),
		capsParam ? ((uint32_t*)capsParam)[1] : 0, r, capsVal ? *(int*)capsVal : -1);
	return r;
}

static void clampRateControl(uint8_t* initParams, const char* what);

static int WINAPI W_Init(void* enc, void* params, void*, void*)
{
	const uint8_t* p = (const uint8_t*)params;
	LOGF("InitializeEncoder(version %08x, codec %s, preset %s, %ux%u, dar %ux%u, fps %u/%u, async %u, ptd %u, maxEnc %ux%u)",
		*(uint32_t*)p, guidStr(p + 4).c_str(), guidStr(p + 20).c_str(), *(uint32_t*)(p + 36), *(uint32_t*)(p + 40),
		*(uint32_t*)(p + 44), *(uint32_t*)(p + 48), *(uint32_t*)(p + 52), *(uint32_t*)(p + 56),
		*(uint32_t*)(p + 60), *(uint32_t*)(p + 64), *(uint32_t*)(p + 96), *(uint32_t*)(p + 100));
	{
		const uint8_t* cfg = *(const uint8_t* const*)(p + 88);
		if (cfg && !IsBadReadPtr(cfg, 128))
			LOGF("  encode config as requested by Parsec: gop %u, B-frame interval %u, rate control mode 0x%x, avg %u bps, max %u bps, vbv %u",
				*(uint32_t*)(cfg + 20), *(uint32_t*)(cfg + 24), *(uint32_t*)(cfg + 44), *(uint32_t*)(cfg + 60), *(uint32_t*)(cfg + 64), *(uint32_t*)(cfg + 68));
	}
	clampRateControl((uint8_t*)params, "initialize");
	int r = ((Fn4)g_orig[11])(enc, params, nullptr, nullptr);
	LOGF("InitializeEncoder -> %d", r);
	return r;
}

static int WINAPI W_Encode(void* enc, void* params, void*, void*)
{
	const uint8_t* p = (const uint8_t*)params;
	int n = ++g_calls[16];
	if (n <= 6)
		LOGF("EncodePicture(in %ux%u pitch %u, flags %08x, frameIdx %u, completionEvent %p, bufFmt %08x, picType %u)",
			*(uint32_t*)(p + 4), *(uint32_t*)(p + 8), *(uint32_t*)(p + 12), *(uint32_t*)(p + 16), *(uint32_t*)(p + 20),
			*(void**)(p + 56), *(uint32_t*)(p + 64), *(uint32_t*)(p + 72));
	const double t0 = qpcMs();
	int r = ((Fn4)g_orig[16])(enc, params, nullptr, nullptr);
	const double dt = qpcMs() - t0; g_encMs += dt; if (dt > g_encMax) g_encMax = dt;
	if (dt > 25) LOGF("STALL: EncodePicture took %.0f ms", dt);
	if (n <= 6 || r != 0) LOGF("EncodePicture -> %d", r);
	return r;
}

static int WINAPI W_Lock(void* enc, void* params, void*, void*)
{
	const double t0 = qpcMs();
	int r = ((Fn4)g_orig[17])(enc, params, nullptr, nullptr);
	const double dt = qpcMs() - t0; g_lockMs += dt; if (dt > g_lockMax) g_lockMax = dt;
	if (dt > 25) LOGF("STALL: LockBitstream waited %.0f ms", dt);
	int n = ++g_calls[17];
	if (n <= 6 || r != 0) {
		const uint8_t* p = (const uint8_t*)params;
		uint32_t size = *(uint32_t*)(p + 36);
		const uint8_t* data = *(const uint8_t**)(p + 56);
		char hex[40] = "";
		if (r == 0 && data && size >= 8) snprintf(hex, sizeof hex, "%02x %02x %02x %02x %02x %02x %02x %02x", data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
		LOGF("LockBitstream -> %d: size %u, picType %u, hwStatus %u, data %s", r, size, *(uint32_t*)(p + 64), *(uint32_t*)(p + 28), hex);
	}
	return r;
}

static int WINAPI W_GuidList(void* enc, void* guids, void* arraySizeV, void* countV)
{
	int r = ((Fn4)g_orig[4])(enc, guids, arraySizeV, countV);
	uint32_t cnt = countV ? *(uint32_t*)countV : 0;
	std::string s;
	for (uint32_t i = 0; i < cnt && i < 8 && guids; i++) s += guidStr((uint8_t*)guids + 16 * i) + " ";
	LOGF("GetEncodeGUIDs -> %d count %u: %s", r, cnt, s.c_str());
	return r;
}

static void freeBuf(Buf* b)
{
	if (b->nvHandle && b->enc) { ((Fn4)g_orig[31])(b->enc, b->nvHandle, nullptr, nullptr); b->nvHandle = nullptr; }
	CUcontext popped;
	if (b->ptr) { cu.cuCtxPushCurrent(b->session->cuctx); cu.cuMemFree(b->ptr); cu.cuCtxPopCurrent(&popped); }
	if (b->staging) b->staging->Release();
	if (b->tex) b->tex->Release();
	delete b;
}

// Returns the cached buffer for `tex`, creating it on first use. Caller holds g_mutex.
static Buf* getBuf(Session* s, ID3D11Texture2D* tex, uint32_t w, uint32_t h, const D3D11_TEXTURE2D_DESC& d, bool planar, size_t bpp)
{
	auto it = g_bufs.find(tex);
	if (it != g_bufs.end()) {
		Buf* b = it->second;
		if (b->session == s && b->width == w && b->height == h && b->desc.Format == d.Format) { b->lastUse = ++g_useClock; return b; }
		if (b->registrations == 0) { g_bufs.erase(it); freeBuf(b); }
		else return nullptr;
	}
	// bound the cache: drop the least recently used unregistered entry
	while (g_bufs.size() >= 6) {
		Buf* victim = nullptr; ID3D11Texture2D* key = nullptr;
		for (auto& kv : g_bufs) if (kv.second->registrations == 0 && (!victim || kv.second->lastUse < victim->lastUse)) { victim = kv.second; key = kv.first; }
		if (!victim) break;
		g_bufs.erase(key); freeBuf(victim);
	}
	CUcontext popped;
	CUdeviceptr ptr = 0; size_t pitch = 0;
	size_t rows = planar ? (size_t)h * 3 / 2 : h;
	cu.cuCtxPushCurrent(s->cuctx);
	CUresult cr = cu.cuMemAllocPitch(&ptr, &pitch, (size_t)w * bpp, rows, 16);
	cu.cuCtxPopCurrent(&popped);
	if (cr) { LOGF("cuMemAllocPitch failed %d", cr); return nullptr; }
	Buf* b = new Buf{ s, tex, d, w, h, ptr, pitch };
	tex->AddRef();
	b->lastUse = ++g_useClock;
	g_bufs[tex] = b;
	LOGF("new cached CUDA buffer for texture %p (%ux%u, pitch %zu)", (void*)tex, w, h, pitch);
	return b;
}


// ------------------------------------------------------------------ GPU mode (CUDA IPC ring from the Linux helper)
static ssb_video* g_vid = nullptr;
static struct {
	bool open = false;
	uint64_t owner = 0;            // Session::id whose CUDA context the opened IPC pointers belong to
	uint32_t generation = 0;
	CUdeviceptr ptr[SSB_GPU_SLOTS] = {};
	uint32_t pitch = 0, w = 0, h = 0;
	uint32_t lastSeq = 0; double lastSeqMs = 0;
	bool failed = false;
} g_gpu;

static ssb_video* videoShm()
{
	if (g_vid) return g_vid;
	const char* dir = getenv("SSB_BRIDGE_DIR");
	if (!dir || !*dir) return nullptr;
	std::string d(dir); if (d[0] == '/') d = "Z:" + d;
	for (auto& c : d) if (c == '/') c = '\\';
	HANDLE f = CreateFileA((d + "\\" + SSB_VIDEO_FILE).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return nullptr;
	HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READWRITE, 0, 0, nullptr);
	CloseHandle(f);
	if (!m) return nullptr;
	void* v = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, SSB_VIDEO_HDR_BYTES);   // header only: GPU mode has no pixel slots to read
	CloseHandle(m);
	if (!v) return nullptr;
	auto* h = (ssb_video*)v;
	if (h->magic != SSB_VIDEO_MAGIC) return nullptr;
	g_vid = h;
	return g_vid;
}

static ssb_gpu* gpuHdr() { ssb_video* v = videoShm(); return v ? (ssb_gpu*)((uint8_t*)v + SSB_GPU_OFFSET) : nullptr; }

// Closes the opened IPC buffers. `s` must be the session that opened them and its CUDA context must still exist
// (call this BEFORE destroying the session); with s == nullptr the pointers are only forgotten.
static void gpuClose(Session* s)
{
	if (!g_gpu.open) return;
	if (s && s->id == g_gpu.owner && cu.cuIpcCloseMemHandle) {
		CUcontext popped; cu.cuCtxPushCurrent(s->cuctx);
		for (int i = 0; i < SSB_GPU_SLOTS; i++) if (g_gpu.ptr[i]) { cu.cuIpcCloseMemHandle(g_gpu.ptr[i]); }
		cu.cuCtxPopCurrent(&popped);
	}
	for (int i = 0; i < SSB_GPU_SLOTS; i++) g_gpu.ptr[i] = 0;
	g_gpu.open = false;
	g_gpu.owner = 0;
	g_gpu.lastSeq = 0; g_gpu.lastSeqMs = 0;
}

// Returns the device pointer + pitch of the newest frame in the shared ring, or false if GPU mode is not active.
static bool gpuLatest(Session* s, CUdeviceptr* ptr, size_t* pitch, uint32_t* w, uint32_t* h)
{
	ssb_gpu* g = gpuHdr();
	if (!g || !g->enabled || g_gpu.failed) { if (g_gpu.open && g && !g->enabled) gpuClose(s); return false; }
	if (g_gpu.open && g_gpu.owner != s->id) gpuClose(nullptr);          // belonged to a destroyed session: forget, never reuse
	if (g_gpu.open && g_gpu.generation != g->generation) gpuClose(s);   // helper reallocated the ring
	if (!g_gpu.open) {
		if (!cu.cuIpcOpenMemHandle) { g->consumer_state = SSB_GPU_CONSUMER_FAILED; g_gpu.failed = true; LOGF("GPU mode: CUDA IPC not available in this CUDA layer"); return false; }
		CUcontext popped; cu.cuCtxPushCurrent(s->cuctx);
		bool ok = true;
		for (int i = 0; i < SSB_GPU_SLOTS && ok; i++) {
			CUipcMemHandle hd; memcpy(hd.reserved, (const void*)g->handle[i], 64);
			CUresult r = cu.cuIpcOpenMemHandle(&g_gpu.ptr[i], hd, 1 /*LAZY_ENABLE_PEER_ACCESS*/);
			if (r) { LOGF("GPU mode: cuIpcOpenMemHandle slot %d failed (%d)", i, r); ok = false; }
		}
		cu.cuCtxPopCurrent(&popped);
		if (!ok) { g->consumer_state = SSB_GPU_CONSUMER_FAILED; g_gpu.failed = true; return false; }
		g_gpu.open = true; g_gpu.owner = s->id; g_gpu.generation = g->generation; g_gpu.pitch = g->pitch; g_gpu.w = g->width; g_gpu.h = g->height;
		g->consumer_state = SSB_GPU_CONSUMER_OK;
		LOGF("GPU mode: opened %d shared CUDA buffers (%ux%u, pitch %u) - frames reach the encoder without touching system memory", SSB_GPU_SLOTS, g_gpu.w, g_gpu.h, g_gpu.pitch);
	}
	const uint32_t seq = g->latest_seq;
	if (seq != g_gpu.lastSeq) { g_gpu.lastSeq = seq; g_gpu.lastSeqMs = qpcMs(); }
	if (g_gpu.lastSeqMs == 0 || qpcMs() - g_gpu.lastSeqMs > 400.0) return false;     // helper stopped delivering
	const uint32_t slot = g->latest_slot % SSB_GPU_SLOTS;
	*ptr = g_gpu.ptr[slot]; *pitch = g_gpu.pitch; *w = g_gpu.w; *h = g_gpu.h;
	return true;
}

// Test hook: runs the GPU-ring lookup for the session behind `encoder` and reads one row of the newest frame
// back to host memory. Returns 1 on success, 0 when GPU mode is inactive, -1 if the buffers are unusable.
extern "C" __declspec(dllexport) int WINAPI SsbDebugGpuProbe(void* encoder)
{
	Session* s = nullptr;
	{ std::lock_guard<std::mutex> l(g_mutex); auto it = g_sessions.find(encoder); if (it != g_sessions.end()) s = it->second; }
	if (!s) return -2;
	CUdeviceptr src = 0; size_t pitch = 0; uint32_t w = 0, h = 0;
	std::lock_guard<std::mutex> l(g_mutex);
	if (!gpuLatest(s, &src, &pitch, &w, &h)) return 0;
	std::vector<uint8_t> row((size_t)w * 4);
	CUDA_MEMCPY2D c = {};
	c.srcMemoryType = 2; c.srcDevice = src + pitch * (h / 2); c.srcPitch = pitch;
	c.dstMemoryType = 1; c.dstHost = row.data(); c.dstPitch = (size_t)w * 4;
	c.WidthInBytes = (size_t)w * 4; c.Height = 1;
	CUcontext popped; cu.cuCtxPushCurrent(s->cuctx);
	CUresult r = cu.cuMemcpy2D(&c);
	cu.cuCtxPopCurrent(&popped);
	if (r) return -1;
	unsigned nz = 0; for (size_t i = 0; i < row.size(); i += 7) nz += row[i] != 0;
	return nz > 4 ? 1 : -1;
}


// ---- bitrate budget: Parsec's host sends a separate copy of the stream to every guest, so the host's upload is
// (stream bitrate x guests). The app's Bandwidth setting is a budget for the WHOLE room; clamp the encoder's
// bitrate to budget / guests so one guest cannot starve the rest and more guests do not multiply the upload.
static uint32_t g_lastClampBps = 0;
static void clampRateControl(uint8_t* initParams, const char* what)
{
	ssb_video* v = videoShm();
	if (!v || !initParams) return;
	const uint32_t totalKbps = v->bitrate_total_kbps;
	uint8_t* cfg = *(uint8_t**)(initParams + 88);          // NV_ENC_INITIALIZE_PARAMS::encodeConfig
	if (!cfg || IsBadWritePtr(cfg, 128)) return;
	uint32_t* avg = (uint32_t*)(cfg + 60);                  // NV_ENC_CONFIG::rcParams.averageBitRate
	uint32_t* mx  = (uint32_t*)(cfg + 64);                  //                          .maxBitRate
	uint32_t guests = v->guest_count ? v->guest_count : 1;
	const uint32_t askedAvg = *avg, askedMax = *mx;
	uint32_t limit = 0;
	if (totalKbps) {
		limit = (uint32_t)((uint64_t)totalKbps * 1000 / guests);
		if (limit < 1000000) limit = 1000000;                // never starve a guest below 1 Mbps
		if (*avg > limit) *avg = limit;
		if (*mx > limit) *mx = limit;
	}
	static int logs = 0;
	if (logs++ < 40 || (limit && limit != g_lastClampBps)) {
		LOGF("bitrate (%s): Parsec asked avg %.2f / max %.2f Mbps; budget %u Mbps over %u guest(s) -> %s",
			what, askedAvg / 1e6, askedMax / 1e6, totalKbps / 1000, guests,
			!limit ? "no limit set" : (askedAvg > limit || askedMax > limit) ? ("limited to " + std::to_string(limit / 1e6) + " Mbps").c_str() : "within budget");
		g_lastClampBps = limit;
	}
}

static int WINAPI W_Reconfigure(void* enc, void* params, void*, void*)
{
	clampRateControl((uint8_t*)params + 8, "reconfigure");   // NV_ENC_RECONFIGURE_PARAMS embeds NV_ENC_INITIALIZE_PARAMS after version+padding
	int r = ((Fn4)g_orig[32])(enc, params, nullptr, nullptr);
	static int n = 0;
	if (n++ < 20 || r != 0) LOGF("ReconfigureEncoder -> %d", r);
	return r;
}

// ---- wrapped entry points
static int WINAPI W_OpenEx(void* paramsV, void* encPtrV, void*, void*)
{
	auto* p = (OPENEX_PARAMS*)paramsV;
	void** encOut = (void**)encPtrV;
	if (p->deviceType != NV_DEVICE_DIRECTX) {
		LOGF("OpenEncodeSessionEx deviceType=%u passed through", p->deviceType);
		return ((Fn4)g_orig[29])(paramsV, encPtrV, nullptr, nullptr);
	}
	LOGF("OpenEncodeSessionEx(DIRECTX, api=%08x) -> translating to CUDA", p->apiVersion);
	CUcontext cuctx = nullptr;
	Session* s = makeSession((ID3D11Device*)p->device, &cuctx);
	if (!s) return 2; // NV_ENC_ERR_UNSUPPORTED_DEVICE
	void* origDev = p->device; uint32_t origType = p->deviceType;
	p->deviceType = NV_DEVICE_CUDA; p->device = cuctx;
	int r = ((Fn4)g_orig[29])(paramsV, encPtrV, nullptr, nullptr);
	p->deviceType = origType; p->device = origDev;
	LOGF("OpenEncodeSessionEx(CUDA) -> %d", r);
	if (r == 0 && encOut && *encOut) { std::lock_guard<std::mutex> l(g_mutex); g_sessions[*encOut] = s; }
	else { s->ctx->Release(); s->dev->Release(); cu.cuCtxDestroy(cuctx); delete s; }
	return r;
}

static int WINAPI W_Open(void* device, void* deviceTypeV, void* encPtrV, void*)
{
	uint32_t type = (uint32_t)(uintptr_t)deviceTypeV;
	void** encOut = (void**)encPtrV;
	if (type != NV_DEVICE_DIRECTX) return ((Fn4)g_orig[0])(device, deviceTypeV, encPtrV, nullptr);
	LOGF("OpenEncodeSession(DIRECTX) -> translating to CUDA");
	CUcontext cuctx = nullptr;
	Session* s = makeSession((ID3D11Device*)device, &cuctx);
	if (!s) return 2;
	int r = ((Fn4)g_orig[0])(cuctx, (void*)(uintptr_t)NV_DEVICE_CUDA, encPtrV, nullptr);
	LOGF("OpenEncodeSession(CUDA) -> %d", r);
	if (r == 0 && encOut && *encOut) { std::lock_guard<std::mutex> l(g_mutex); g_sessions[*encOut] = s; }
	else { s->ctx->Release(); s->dev->Release(); cu.cuCtxDestroy(cuctx); delete s; }
	return r;
}

static int WINAPI W_Register(void* enc, void* paramsV, void*, void*)
{
	auto* p = (REGISTER_RESOURCE*)paramsV;
	Session* s = nullptr;
	{ std::lock_guard<std::mutex> l(g_mutex); auto it = g_sessions.find(enc); if (it != g_sessions.end()) s = it->second; }
	if (!s || p->resourceType != NV_RES_DIRECTX) {
		int r = ((Fn4)g_orig[30])(enc, paramsV, nullptr, nullptr);
		LOGF("RegisterResource(type=%u) passed through -> %d", p->resourceType, r);
		return r;
	}

	auto* tex = (ID3D11Texture2D*)p->resourceToRegister;
	D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
	bool planar; size_t bpp = bytesPerPixel(d.Format, planar);
	uint32_t w = p->width ? p->width : d.Width, h = p->height ? p->height : d.Height;
	static int regInfoLogs = 0;
	if (regInfoLogs++ < 4) LOGF("RegisterResource(DIRECTX tex %p %ux%u fmt %d sub %u, nvenc fmt %08x)", (void*)tex, d.Width, d.Height, (int)d.Format, p->subResourceIndex, p->bufferFormat);

	Buf* buf;
	{ std::lock_guard<std::mutex> l(g_mutex); buf = getBuf(s, tex, w, h, d, planar, bpp); }
	if (!buf) return 8;

	g_regStart = qpcMs();
	if (g_lastUnregEnd > 0) { g_idleMs += g_regStart - g_lastUnregEnd; }

	int r = 0;
	bool reused = false;
	{
		std::lock_guard<std::mutex> l(g_mutex);
		if (buf->nvHandle && buf->enc == enc && buf->nvFormat == p->bufferFormat) {
			p->registeredResource = buf->nvHandle;   // already registered with NVENC: skip the expensive call
			reused = true;
		}
	}
	if (!reused) {
		void* origRes = p->resourceToRegister; uint32_t origType = p->resourceType, origPitch = p->pitch;
		p->resourceType = NV_RES_CUDADEVICEPTR; p->resourceToRegister = (void*)(uintptr_t)buf->ptr; p->pitch = (uint32_t)buf->pitch;
		r = ((Fn4)g_orig[30])(enc, paramsV, nullptr, nullptr);
		p->resourceType = origType; p->resourceToRegister = origRes; p->pitch = origPitch;
		static int regLogs = 0;
		if (regLogs++ < 4 || r != 0) LOGF("RegisterResource(CUDA ptr, pitch %zu) -> %d", buf->pitch, r);
		if (r != 0) return r;
		std::lock_guard<std::mutex> l(g_mutex);
		if (buf->nvHandle && buf->enc) ((Fn4)g_orig[31])(buf->enc, buf->nvHandle, nullptr, nullptr); // stale one
		buf->nvHandle = p->registeredResource; buf->enc = enc; buf->nvFormat = p->bufferFormat;
	}
	g_regMs += qpcMs() - g_regStart; g_regCount++;

	std::lock_guard<std::mutex> l(g_mutex);
	auto existing = g_registered.find(p->registeredResource);
	if (existing == g_registered.end()) g_registered[p->registeredResource] = new Registered{ buf, p->subResourceIndex };
	buf->registrations++;
	return 0;
}

static double g_mapMs = 0, g_cudaMs = 0;
static double g_copyMsTotal = 0; static uint64_t g_copyCount = 0; static DWORD g_copyTick = 0;
static void reportStatsIfDue();

static void reportStatsIfDue()
{
	DWORD now = GetTickCount();
	if (!g_copyTick) g_copyTick = now;
	if (now - g_copyTick < 5000) return;

		const uint64_t nFrames = g_copyCount ? g_copyCount : 1;
		LOGF("stats: %.1f frames/s encoded, copy %.2f ms (GPU copy+map %.2f, CUDA upload %.2f), EncodePicture %.2f/%.2f ms avg/max, LockBitstream %.2f/%.2f ms, longest frame gap %.1f ms, stalls(>40ms) %d",
			g_copyCount * 1000.0 / (now - g_copyTick), g_copyMsTotal / nFrames, g_mapMs / nFrames, g_cudaMs / nFrames,
			g_encMs / nFrames, g_encMax, g_lockMs / nFrames, g_lockMax, g_periodMax, g_stalls);
		LOGF("stats: per frame - register %.2f ms, NVENC map %.2f, unmap %.2f, unregister %.2f, busy (register->unregister) %.2f, Parsec-side gap between frames %.2f",
			g_regMs / nFrames, g_mapOrigMs / nFrames, g_unmapMs / nFrames, g_unregMs / nFrames, g_busyMs / nFrames, g_idleMs / nFrames);
		LOGF("stats: encode spacing ms [<8 | 8-14 | 14-16 | 16-17.5 | 17.5-20 | 20-30 | 30-50 | >50] = [%u | %u | %u | %u | %u | %u | %u | %u]",
			g_periodHist[0], g_periodHist[1], g_periodHist[2], g_periodHist[3], g_periodHist[4], g_periodHist[5], g_periodHist[6], g_periodHist[7]);
		memset(g_periodHist, 0, sizeof g_periodHist);
		LOGF("stats: copy path - %llu from CUDA frames (%llu of them straight from the Linux helper's GPU ring), %llu via D3D11 readback; app-side upload %.2f ms avg",
			(unsigned long long)g_fastCopies, (unsigned long long)g_gpuModeFrames, (unsigned long long)g_slowCopies, g_offers ? g_offerMs / g_offers : 0.0);
		g_gpuModeFrames = 0;
		g_fastCopies = g_slowCopies = 0; g_offerMs = 0; g_offers = 0;
		LOGF("stats: submit->encode pickup delay %.2f ms avg / %.2f max (latest submit before each encoded frame), %.2f submits per encoded frame",
			g_pickupMs / nFrames, g_pickupMax, g_copyCount ? (double)g_submitsSeen / g_copyCount : 0.0);
		g_pickupMs = g_pickupMax = 0; g_submitsSeen = 0;
		g_copyMsTotal = 0; g_copyCount = 0; g_copyTick = now; g_mapMs = g_cudaMs = 0;
		g_regMs = g_mapOrigMs = g_unmapMs = g_unregMs = g_idleMs = g_busyMs = 0; g_regCount = 0;
		g_periodMax = g_encMs = g_encMax = g_lockMs = g_lockMax = 0; g_stalls = 0; g_stallLogs = 0;
	}

static bool copyTextureToCuda(Registered* rg)
{
	Buf* r = rg->buf;
	Session* s = r->session;
	LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);

	// GPU mode: the newest frame already sits in CUDA memory shared by the Linux helper.
	{
		CUdeviceptr src = 0; size_t srcPitch = 0; uint32_t gw = 0, gh = 0;
		if (r->desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM && gpuLatest(s, &src, &srcPitch, &gw, &gh) && gw == r->width && gh == r->height) {
			CUDA_MEMCPY2D c = {};
			c.srcMemoryType = 2; c.srcDevice = src; c.srcPitch = srcPitch;
			c.dstMemoryType = 2; c.dstDevice = r->ptr; c.dstPitch = r->pitch;
			c.WidthInBytes = (size_t)r->width * 4; c.Height = r->height;
			CUcontext popped;
			cu.cuCtxPushCurrent(s->cuctx);
			CUresult cr = cu.cuMemcpy2D(&c);
			cu.cuCtxPopCurrent(&popped);
			if (!cr) {
				g_fastCopies++; g_lastFastMs = qpcMs(); g_gpuModeFrames++;
				LARGE_INTEGER t2; QueryPerformanceCounter(&t2);
				const double ms = (t2.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
				g_copyMsTotal += ms; g_copyCount++; g_cudaMs += ms;
				reportStatsIfDue();
				return true;
			}
		}
	}

	// Fast path: the app already put this very frame into CUDA memory; copy GPU-to-GPU.
	{
		CUdeviceptr src = 0; size_t srcPitch = 0; bool usable = false;
		{
			std::lock_guard<std::mutex> l(g_mutex);
			const FrameStore& fs = s->fs;
			if (g_fastVerified && !g_fastDisabled && fs.latest >= 0 && fs.w == r->width && fs.h == r->height &&
				r->desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM && qpcMs() - fs.doneMs < 120.0) {
				src = fs.buf[fs.latest]; srcPitch = fs.pitch; usable = true;
			}
		}
		if (usable) {
			CUDA_MEMCPY2D c = {};
			c.srcMemoryType = 2; c.srcDevice = src; c.srcPitch = srcPitch;
			c.dstMemoryType = 2; c.dstDevice = r->ptr; c.dstPitch = r->pitch;
			c.WidthInBytes = (size_t)r->width * 4; c.Height = r->height;
			CUcontext popped;
			cu.cuCtxPushCurrent(s->cuctx);
			CUresult cr = cu.cuMemcpy2D(&c);
			cu.cuCtxPopCurrent(&popped);
			if (!cr) {
				g_fastCopies++; g_lastFastMs = qpcMs();
				QueryPerformanceCounter(&t1);
				const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
				g_copyMsTotal += ms; g_copyCount++; g_cudaMs += ms;
				reportStatsIfDue();
				return true;
			}
		}
		g_slowCopies++;
	}
	if (!r->staging) {
		D3D11_TEXTURE2D_DESC sd = r->desc;
		sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
		sd.MipLevels = 1; sd.ArraySize = 1;
		HRESULT hr = s->dev->CreateTexture2D(&sd, nullptr, &r->staging);
		if (FAILED(hr)) { LOGF("staging texture create failed hr=%08lx (fmt %d)", hr, (int)r->desc.Format); return false; }
	}
	s->ctx->CopySubresourceRegion(r->staging, 0, 0, 0, 0, r->tex, rg->subResource, nullptr);
	D3D11_MAPPED_SUBRESOURCE m;
	HRESULT hr = s->ctx->Map(r->staging, 0, D3D11_MAP_READ, 0, &m);
	if (FAILED(hr)) { LOGF("staging Map failed hr=%08lx", hr); return false; }
	LARGE_INTEGER tm; QueryPerformanceCounter(&tm);
	g_mapMs += (tm.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;

	bool planar; size_t bpp = bytesPerPixel(r->desc.Format, planar);
	const uint8_t* src = (const uint8_t*)m.pData;

	// One-off check that the frames the app pre-uploaded are really what Parsec hands to the encoder: Parsec
	// copies the submitted texture into its own, so compare sample pixels of the readback with both stored frames.
	if (!g_fastVerified && !g_fastDisabled && r->desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM && !planar) {
		FrameStore snap; bool have = false;
		{ std::lock_guard<std::mutex> l(g_mutex); snap = s->fs; have = snap.latest >= 0 && snap.w == r->width && snap.h == r->height; }
		if (have) {
			std::vector<uint8_t> host[2];
			bool ok = true;
			cu.cuCtxPushCurrent(s->cuctx);
			for (int i = 0; i < 2 && ok; i++) {
				host[i].resize((size_t)snap.w * 4 * snap.h);
				CUDA_MEMCPY2D d = {};
				d.srcMemoryType = 2; d.srcDevice = snap.buf[i]; d.srcPitch = snap.pitch;
				d.dstMemoryType = 1; d.dstHost = host[i].data(); d.dstPitch = (size_t)snap.w * 4;
				d.WidthInBytes = (size_t)snap.w * 4; d.Height = snap.h;
				ok = cu.cuMemcpy2D(&d) == 0;
			}
			CUcontext popped2; cu.cuCtxPopCurrent(&popped2);
			if (ok) {
				int samples = 0, match[2] = { 0, 0 };
				for (uint32_t y = 7; y < snap.h; y += snap.h / 24) for (uint32_t x = 5; x < snap.w; x += snap.w / 40) {
					samples++;
					const uint8_t* a = src + (size_t)y * m.RowPitch + (size_t)x * 4;
					for (int i = 0; i < 2; i++) {
						const uint8_t* b = host[i].data() + (size_t)y * snap.w * 4 + (size_t)x * 4;
						if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) match[i]++;
					}
				}
				const int best = match[0] > match[1] ? match[0] : match[1];
				const bool good = best * 100 >= samples * 97;
				LOGF("verify pre-uploaded frames vs Parsec's texture: %d/%d and %d/%d sample pixels match (%s); offered tex %p, Parsec tex %p",
					match[0], samples, match[1], samples, good ? "OK" : "MISMATCH", snap.tex, (void*)r->tex);
				if (good) { g_fastVerified = true; }
				else if (++g_verifyTries >= 6) { g_fastDisabled = true; LOGF("fast path disabled (frames do not match)"); }
			}
		}
	}

	CUDA_MEMCPY2D c = {};
	c.srcMemoryType = 1; c.srcHost = src; c.srcPitch = m.RowPitch;
	c.dstMemoryType = 2; c.dstDevice = r->ptr; c.dstPitch = r->pitch;
	c.WidthInBytes = (size_t)r->width * bpp; c.Height = r->height;
	cu.cuCtxPushCurrent(s->cuctx);
	CUresult cr = cu.cuMemcpy2D(&c);
	if (!cr && planar) { // interleaved chroma plane follows the luma plane
		CUDA_MEMCPY2D u = c;
		u.srcHost = src + (size_t)m.RowPitch * r->desc.Height;
		u.dstDevice = r->ptr + r->pitch * r->height;
		u.Height = r->height / 2;
		cr = cu.cuMemcpy2D(&u);
	}
	CUcontext popped; cu.cuCtxPopCurrent(&popped);
	s->ctx->Unmap(r->staging, 0);
	if (cr) { LOGF("cuMemcpy2D failed %d", cr); return false; }

	QueryPerformanceCounter(&t1);
	g_cudaMs += (t1.QuadPart - tm.QuadPart) * 1000.0 / f.QuadPart;
	g_copyMsTotal += (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart; g_copyCount++;
	reportStatsIfDue();
	return true;
}

static int WINAPI W_Map(void* enc, void* paramsV, void*, void*)
{
	auto* p = (MAP_INPUT_RESOURCE*)paramsV;
	Registered* reg = nullptr;
	{ std::lock_guard<std::mutex> l(g_mutex); auto it = g_registered.find(p->registeredResource); if (it != g_registered.end()) reg = it->second; }
	if (reg) {
		const double nowMs = qpcMs();
		if (g_lastSubmit > 0) {
			const double d = nowMs - g_lastSubmit; g_pickupMs += d; if (d > g_pickupMax) g_pickupMax = d;
			g_submitsSeen += g_submits; g_submits = 0;
		}
		if (g_lastMap.QuadPart) {
			const double period = nowMs - g_lastMap.QuadPart * 1000.0 / g_qf.QuadPart;
			if (period > g_periodMax) g_periodMax = period;
			{ static const double edges[7] = { 8, 14, 16, 17.5, 20, 30, 50 }; int b = 7; for (int i = 0; i < 7; i++) if (period < edges[i]) { b = i; break; } g_periodHist[b]++; }
			if (period > 40) { g_stalls++; if (g_stallLogs++ < 8) LOGF("STALL: %.0f ms between encoded frames", period); }
		}
		g_lastMap.QuadPart = (LONGLONG)(nowMs * g_qf.QuadPart / 1000.0);
		bool ok = copyTextureToCuda(reg);
		static int mapLogs = 0;
		if (!ok || mapLogs++ < 2) LOGF("MapInputResource: texture copy %s", ok ? "ok" : "FAILED");
	}
	const double tm0 = qpcMs();
	int rr = ((Fn4)g_orig[25])(enc, paramsV, nullptr, nullptr);
	g_mapOrigMs += qpcMs() - tm0;
	return rr;
}

static int WINAPI W_Unmap(void* enc, void* mapped, void*, void*)
{
	const double t0 = qpcMs();
	int r = ((Fn4)g_orig[26])(enc, mapped, nullptr, nullptr);
	g_unmapMs += qpcMs() - t0;
	return r;
}

static int WINAPI W_Unregister(void* enc, void* handle, void*, void*)
{
	const double t0 = qpcMs();
	bool ours = false;
	{
		std::lock_guard<std::mutex> l(g_mutex);
		auto it = g_registered.find(handle);
		if (it != g_registered.end()) {
			ours = true;
			Buf* b = it->second->buf;
			if (b->registrations > 0) b->registrations--;
			// The NVENC registration stays alive on the cached buffer for the next frame (see W_Register).
		}
	}
	int r = ours ? 0 : ((Fn4)g_orig[31])(enc, handle, nullptr, nullptr);
	const double now = qpcMs();
	g_unregMs += now - t0;
	g_lastUnregEnd = now;
	if (g_regStart > 0) g_busyMs += now - g_regStart;
	return r;
}

static int WINAPI W_Destroy(void* enc, void*, void*, void*)
{
	Session* s = nullptr;
	{
		std::lock_guard<std::mutex> l(g_mutex);
		auto it = g_sessions.find(enc);
		if (it != g_sessions.end()) s = it->second;
		if (s) {
			gpuClose(s);
			for (auto rit = g_registered.begin(); rit != g_registered.end();) {
				if (rit->second->buf->session == s) { delete rit->second; rit = g_registered.erase(rit); } else ++rit;
			}
			for (auto bit = g_bufs.begin(); bit != g_bufs.end();) {
				if (bit->second->session == s) { freeBuf(bit->second); bit = g_bufs.erase(bit); } else ++bit;
			}
			g_sessions.erase(enc);
		}
	}
	int r = ((Fn4)g_orig[27])(enc, nullptr, nullptr, nullptr);
	if (s) { if (s->ctx) s->ctx->Release(); if (s->dev) s->dev->Release(); if (s->cuctx) cu.cuCtxDestroy(s->cuctx); delete s; }
	LOGF("DestroyEncoder -> %d", r);
	return r;
}

// ------------------------------------------------------------------ exports
// Parsec FreeLibrary()s this DLL when the last guest leaves. Proton's own nvEncodeAPI64.dll (loaded
// underneath us) shares our file name and stays resident, so the next LoadLibrary("nvEncodeAPI64.dll")
// would return *that* - without the D3D11 translation - and encoder init would fail until the host is
// restarted. Pinning makes this DLL permanent so every later load finds the proxy again.
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		HMODULE pinned = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)&DllMain, &pinned);
	}
	return TRUE;
}

static bool loadInner()
{
	if (g_inner) return true;
	HMODULE self = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&loadInner, &self);
	char dir[MAX_PATH] = "";
	GetModuleFileNameA(self, dir, MAX_PATH);
	char* slash = strrchr(dir, '\\'); if (slash) *slash = 0;
	std::string inner = std::string(dir) + "\\nvEncodeAPI64_inner.dll";
	if (!getenv("SSB_NO_INNER")) g_inner = LoadLibraryA(inner.c_str());   // SSB_NO_INNER=1: skip the legacy-preset shim
	if (!g_inner) {
		char sys[MAX_PATH]; GetSystemDirectoryA(sys, MAX_PATH);
		inner = std::string(sys) + "\\nvEncodeAPI64.dll";
		g_inner = LoadLibraryA(inner.c_str());
	}
	if (!g_inner) { LOGF("cannot load the real NVENC library"); return false; }
	g_innerCreate = (PFN_Create)GetProcAddress(g_inner, "NvEncodeAPICreateInstance");
	LOGF("proxy active; inner library: %s", inner.c_str());
	return g_innerCreate != nullptr;
}

extern "C" __declspec(dllexport) int WINAPI NvEncodeAPIGetMaxSupportedVersion(uint32_t* v)
{
	if (!loadInner()) return 20;
	auto f = (PFN_GetMax)GetProcAddress(g_inner, "NvEncodeAPIGetMaxSupportedVersion");
	return f ? f(v) : 20;
}

extern "C" __declspec(dllexport) int WINAPI NvEncodeAPICreateInstance(void* list)
{
	if (!loadInner()) return 20;
	int r = g_innerCreate(list);
	if (r != 0) { LOGF("inner NvEncodeAPICreateInstance failed %d", r); return r; }

	void** fn = (void**)((char*)list + 8); // {uint32 version, uint32 reserved, void* fn[...]}
	static void* stubs[SLOTS];
	static bool built = false;
	if (!built) { fillStubs(stubs, typename Gen<SLOTS>::type()); built = true; }

	for (int i = 0; i < SLOTS; i++) g_orig[i] = fn[i];
	for (int i = 0; i < SLOTS; i++) if (g_orig[i]) fn[i] = stubs[i];
	if (g_orig[0])  fn[0]  = (void*)&W_Open;
	if (g_orig[29]) fn[29] = (void*)&W_OpenEx;
	if (g_orig[30]) fn[30] = (void*)&W_Register;
	if (g_orig[31]) fn[31] = (void*)&W_Unregister;
	if (g_orig[25]) fn[25] = (void*)&W_Map;
	if (g_orig[26]) fn[26] = (void*)&W_Unmap;
	if (g_orig[27]) fn[27] = (void*)&W_Destroy;
	if (g_orig[4])  fn[4]  = (void*)&W_GuidList;
	if (g_orig[7])  fn[7]  = (void*)&W_GetEncodeCaps;
	if (g_orig[11]) fn[11] = (void*)&W_Init;
	if (g_orig[16]) fn[16] = (void*)&W_Encode;
	if (g_orig[17]) fn[17] = (void*)&W_Lock;
	if (g_orig[32]) fn[32] = (void*)&W_Reconfigure;
	LOGF("NvEncodeAPICreateInstance(version %08x) wrapped", *(uint32_t*)list);
	return 0;
}
