// Windows-side half of the CUDA-IPC probe: opens the buffer exported by ipc_probe_native.py and checks its contents.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
typedef int CUresult; typedef unsigned long long CUdeviceptr;
struct CUipcMemHandle { char reserved[64]; };
int main(int argc, char** argv)
{
	if (argc > 1) freopen(argv[1], "w", stdout);
	setvbuf(stdout, NULL, _IONBF, 0);
	HMODULE h = LoadLibraryA("nvcuda.dll");
	if (!h) { printf("no nvcuda.dll\n"); return 1; }
	auto cuInit = (CUresult(WINAPI*)(unsigned))GetProcAddress(h, "cuInit");
	auto cuDeviceGet = (CUresult(WINAPI*)(int*, int))GetProcAddress(h, "cuDeviceGet");
	auto cuCtxCreate = (CUresult(WINAPI*)(void**, unsigned, int))GetProcAddress(h, "cuCtxCreate_v2");
	auto cuOpen = (CUresult(WINAPI*)(CUdeviceptr*, CUipcMemHandle, unsigned))GetProcAddress(h, "cuIpcOpenMemHandle_v2");
	auto cuDtoH = (CUresult(WINAPI*)(void*, CUdeviceptr, size_t))GetProcAddress(h, "cuMemcpyDtoH_v2");
	if (!cuInit || !cuDeviceGet || !cuCtxCreate || !cuOpen || !cuDtoH) { printf("missing CUDA exports\n"); return 2; }
	int dev = 0; void* ctx = nullptr;
	if (cuInit(0) || cuDeviceGet(&dev, 0) || cuCtxCreate(&ctx, 0, dev)) { printf("cuda init failed\n"); return 3; }
	const char* dir = getenv("SSB_BRIDGE_DIR"); char path[600];
	snprintf(path, sizeof path, "Z:%s/ipc.bin", dir ? dir : "/tmp");
	for (char* c = path; *c; c++) if (*c == '/') *c = '\\';
	FILE* f = fopen(path, "rb"); if (!f) { printf("no handle file %s\n", path); return 4; }
	CUipcMemHandle hd = {}; fread(hd.reserved, 1, 64, f); fclose(f);
	CUdeviceptr p = 0;
	if (cuOpen(&p, hd, 1)) { printf("cuIpcOpenMemHandle failed\n"); return 5; }
	unsigned char b[8] = {};
	if (cuDtoH(b, p, 8) || b[0] != 0xAB || b[1] != 0xAB) { printf("contents wrong\n"); return 6; }
	printf("IPC_OK\n");
	return 0;
}
