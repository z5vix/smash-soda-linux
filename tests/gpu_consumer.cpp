// Windows side (under Proton): consume the Linux helper's CUDA ring exactly like the NVENC layer does.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "ssb_proto.h"
typedef int CUresult; typedef unsigned long long CUdeviceptr;
struct CUipcMemHandle { char reserved[64]; };
int main(int argc, char** argv)
{
	if (argc > 1) freopen(argv[1], "w", stdout);
	setvbuf(stdout, NULL, _IONBF, 0);
	HMODULE h = LoadLibraryA("nvcuda.dll");
	auto cuInit = (CUresult(WINAPI*)(unsigned))GetProcAddress(h, "cuInit");
	auto cuDeviceGet = (CUresult(WINAPI*)(int*, int))GetProcAddress(h, "cuDeviceGet");
	auto cuCtxCreate = (CUresult(WINAPI*)(void**, unsigned, int))GetProcAddress(h, "cuCtxCreate_v2");
	auto cuOpen = (CUresult(WINAPI*)(CUdeviceptr*, CUipcMemHandle, unsigned))GetProcAddress(h, "cuIpcOpenMemHandle_v2");
	auto cuDtoH = (CUresult(WINAPI*)(void*, CUdeviceptr, size_t))GetProcAddress(h, "cuMemcpyDtoH_v2");
	int dev = 0; void* ctx = nullptr; cuInit(0); cuDeviceGet(&dev, 0); cuCtxCreate(&ctx, 0, dev);
	const char* dir = getenv("SSB_BRIDGE_DIR"); char path[600]; snprintf(path, sizeof path, "Z:%s/video.shm", dir);
	for (char* c = path; *c; c++) if (*c == '/') *c = '\\';
	HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
	HANDLE m = CreateFileMappingA(f, 0, PAGE_READWRITE, 0, 0, 0);
	ssb_video* v = (ssb_video*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, SSB_VIDEO_HDR_BYTES);
	ssb_gpu* g = (ssb_gpu*)((uint8_t*)v + SSB_GPU_OFFSET);
	printf("waiting for GPU mode...\n");
	for (int i = 0; i < 600 && !g->enabled; i++) Sleep(100);
	if (!g->enabled) { printf("GPU mode never became enabled\n"); return 1; }
	CUdeviceptr ptr[SSB_GPU_SLOTS] = {};
	for (int i = 0; i < SSB_GPU_SLOTS; i++) {
		CUipcMemHandle hd; memcpy(hd.reserved, (const void*)g->handle[i], 64);
		int r = cuOpen(&ptr[i], hd, 1); printf("open slot %d -> %d\n", i, r); if (r) { g->consumer_state = SSB_GPU_CONSUMER_FAILED; return 2; }
	}
	g->consumer_state = SSB_GPU_CONSUMER_OK;
	printf("ring %ux%u pitch %u, generation %u\n", g->width, g->height, g->pitch, g->generation);
	uint32_t last = g->latest_seq; unsigned frames = 0, nonblack = 0, bad = 0; DWORD t0 = GetTickCount();
	static unsigned char row[10240 * 2];
	while (GetTickCount() - t0 < 6000) {
		uint32_t seq = g->latest_seq;
		if (seq == last) { Sleep(1); continue; }
		last = seq; frames++;
		uint32_t slot = g->latest_slot % SSB_GPU_SLOTS;
		if (cuDtoH(row, ptr[slot] + (uint64_t)g->pitch * (g->height / 2), g->width * 4 < sizeof row ? g->width * 4 : sizeof row)) { bad++; continue; }
		unsigned nz = 0; for (unsigned x = 0; x < g->width * 4; x += 7) nz += row[x] != 0;
		if (nz > 4) nonblack++;
	}
	printf("consumed %u frames in 6 s (%.1f fps), %u middle rows non-black, %u read errors\n", frames, frames / 6.0, nonblack, bad);
	return 0;
}
