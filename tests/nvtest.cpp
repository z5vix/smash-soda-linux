#include <windows.h>
#include <cstdio>
#include <cstdint>
typedef int (__stdcall *GetMaxVer)(uint32_t*);
int main(int argc, char** argv)
{
	if (argc > 1) freopen(argv[1], "w", stdout);
	HMODULE h = LoadLibraryA("nvEncodeAPI64.dll");
	if (!h) { printf("LoadLibrary nvEncodeAPI64.dll FAILED err=%lu\n", GetLastError()); return 1; }
	char path[512]; GetModuleFileNameA(h, path, sizeof path);
	printf("loaded: %s\n", path);
	auto f = (GetMaxVer)GetProcAddress(h, "NvEncodeAPIGetMaxSupportedVersion");
	if (!f) { printf("no NvEncodeAPIGetMaxSupportedVersion export\n"); return 2; }
	uint32_t v = 0; int r = f(&v);
	printf("NvEncodeAPIGetMaxSupportedVersion -> status %d version %u.%u\n", r, v >> 4, v & 0xF);
	HMODULE c = LoadLibraryA("nvcuda.dll"); printf("nvcuda.dll: %s\n", c ? "ok" : "missing");
	return 0;
}
