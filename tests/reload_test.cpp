#include <windows.h>
#include <cstdio>
#include <cstdint>
typedef int (__stdcall *PFN_Create)(void*);
typedef int (__stdcall *PFN_GetMax)(uint32_t*);
int main(int argc, char** argv)
{
	if (argc > 1) freopen(argv[1], "w", stdout);
	setvbuf(stdout, NULL, _IONBF, 0);
	for (int i = 1; i <= 3; i++) {
		HMODULE h = LoadLibraryA("nvEncodeAPI64.dll");
		printf("cycle %d: LoadLibrary -> %p (err %lu)\n", i, (void*)h, h ? 0 : GetLastError());
		if (!h) continue;
		auto create = (PFN_Create)GetProcAddress(h, "NvEncodeAPICreateInstance");
		auto getmax = (PFN_GetMax)GetProcAddress(h, "NvEncodeAPIGetMaxSupportedVersion");
		uint32_t v = 0; int r1 = getmax ? getmax(&v) : -1;
		struct { uint32_t version, reserved; void* fn[400]; } list = {}; list.version = 12 | (2u << 16) | (0x7u << 28);
		int r2 = create ? create(&list) : -1;
		printf("  GetMaxSupportedVersion -> %d (%u.%u), CreateInstance -> %d, OpenEncodeSessionEx slot %s\n", r1, v >> 4, v & 15, r2, list.fn[29] ? "present" : "MISSING");
		BOOL f = FreeLibrary(h);
		printf("  FreeLibrary -> %d, still loaded: %s\n", f, GetModuleHandleA("nvEncodeAPI64.dll") ? "yes" : "no");
		fflush(stdout);
	}
	return 0;
}
