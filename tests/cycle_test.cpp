// Windows side: encoder session create -> use the GPU ring -> destroy, repeated (what Parsec does when a guest joins).
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
typedef int (__stdcall *PFN_Create)(void*);
typedef int (__stdcall *PFN_OpenEx)(void*, void**);
typedef int (__stdcall *PFN_Destroy)(void*);
typedef int (__stdcall *PFN_Probe)(void*);
struct FUNCLIST { uint32_t version, reserved; void* fn[400]; };
struct OPENEX { uint32_t version, deviceType; void* device; void* reserved; uint32_t apiVersion; uint32_t r1[253]; void* r2[64]; };
static uint32_t sv(uint32_t api, uint32_t v) { return api | (v << 16) | (0x7u << 28); }
int main(int argc, char** argv)
{
	if (argc > 1) freopen(argv[1], "w", stdout);
	setvbuf(stdout, NULL, _IONBF, 0);
	HMODULE h = LoadLibraryA("nvEncodeAPI64.dll");
	auto create = (PFN_Create)GetProcAddress(h, "NvEncodeAPICreateInstance");
	auto probe = (PFN_Probe)GetProcAddress(h, "SsbDebugGpuProbe");
	printf("proxy loaded: %s, probe export: %s\n", h ? "yes" : "NO", probe ? "yes" : "NO");
	ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; D3D_FEATURE_LEVEL fl;
	D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
	for (int i = 1; i <= 3; i++) {
		FUNCLIST list = {}; list.version = sv(6, 2);
		create(&list);
		OPENEX p = {}; p.version = sv(6, 1); p.deviceType = 0; p.device = dev; p.apiVersion = 6;
		void* enc = nullptr;
		int r = ((PFN_OpenEx)list.fn[29])(&p, &enc);
		int pr = probe(enc);
		printf("cycle %d: OpenEncodeSessionEx=%d, GPU ring probe=%d (1 = frames readable)\n", i, r, pr);
		// let the helper deliver a few more frames, probe again
		Sleep(300);
		printf("          again after 300 ms: %d\n", probe(enc));
		int d = ((PFN_Destroy)list.fn[27])(enc);
		printf("          DestroyEncoder=%d\n", d);
	}
	printf("survived 3 create/destroy cycles\n");
	return 0;
}
