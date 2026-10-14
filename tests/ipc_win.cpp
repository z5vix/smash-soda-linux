// Windows side (under Proton): open a CUDA IPC handle exported by a native Linux process and read it.
#include <windows.h>
#include <cstdio>
#include <cstdint>
typedef int CUresult; typedef unsigned long long CUdeviceptr;
struct CUipcMemHandle { char reserved[64]; };
int main(int argc, char** argv)
{
	if (argc > 1) freopen(argv[1], "w", stdout);
	setvbuf(stdout, NULL, _IONBF, 0);
	HMODULE h = LoadLibraryA("nvcuda.dll");
	if (!h) { printf("no nvcuda.dll\n"); return 1; }
#define F(t, n) auto n = (t)GetProcAddress(h, #n); printf("%-26s %s\n", #n, n ? "found" : "MISSING");
	F(CUresult(WINAPI*)(unsigned), cuInit)
	F(CUresult(WINAPI*)(int*, int), cuDeviceGet)
	F(CUresult(WINAPI*)(void**, unsigned, int), cuCtxCreate_v2)
	F(CUresult(WINAPI*)(CUdeviceptr*, CUipcMemHandle, unsigned), cuIpcOpenMemHandle_v2)
	F(CUresult(WINAPI*)(CUdeviceptr*, CUipcMemHandle, unsigned), cuIpcOpenMemHandle)
	F(CUresult(WINAPI*)(void*, CUdeviceptr, size_t), cuMemcpyDtoH_v2)
	F(CUresult(WINAPI*)(CUdeviceptr), cuIpcCloseMemHandle)
	if (!cuInit) return 2;
	printf("cuInit -> %d\n", cuInit(0));
	int dev = 0; printf("cuDeviceGet -> %d\n", cuDeviceGet(&dev, 0));
	void* ctx = nullptr; printf("cuCtxCreate -> %d\n", cuCtxCreate_v2(&ctx, 0, dev));
	CUipcMemHandle hd = {};
	FILE* f = fopen("Z:\\tmp\\claude-1000\\ipc.bin", "rb");
	if (!f) { printf("no handle file\n"); return 3; }
	fread(hd.reserved, 1, 64, f); fclose(f);
	CUdeviceptr p = 0;
	auto open = cuIpcOpenMemHandle_v2 ? cuIpcOpenMemHandle_v2 : cuIpcOpenMemHandle;
	if (!open) { printf("no cuIpcOpenMemHandle export\n"); return 4; }
	int r = open(&p, hd, 1 /*CU_IPC_MEM_LAZY_ENABLE_PEER_ACCESS*/);
	printf("cuIpcOpenMemHandle -> %d (ptr %llx)\n", r, p);
	if (r == 0) {
		unsigned char buf[16] = {};
		int r2 = cuMemcpyDtoH_v2(buf, p, 16);
		printf("DtoH -> %d, first bytes: %02x %02x %02x %02x (expect ab ab ab ab)\n", r2, buf[0], buf[1], buf[2], buf[3]);
	}
	return 0;
}
