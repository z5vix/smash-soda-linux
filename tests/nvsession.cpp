// Opens an NVENC session on a D3D11 device (what Parsec's host does) and reports each step.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

typedef int (__stdcall *PFN_GetMax)(uint32_t*);
typedef struct { uint32_t version, reserved; void* fn[96]; } FUNCLIST;
typedef struct { uint32_t version, deviceType; void* device; void* reserved; uint32_t apiVersion; uint32_t reserved1[253]; void* reserved2[64]; } OPENEX;
typedef int (__stdcall *PFN_Create)(FUNCLIST*);
typedef int (__stdcall *PFN_OpenEx)(OPENEX*, void**);
typedef int (__stdcall *PFN_GuidCount)(void*, uint32_t*);
typedef int (__stdcall *PFN_Destroy)(void*);

static uint32_t sv(uint32_t api, uint32_t v) { return api | (v << 16) | (0x7u << 28); }

int main(int argc, char** argv)
{
	if (argc > 1) freopen(argv[1], "w", stdout);
	HMODULE h = LoadLibraryA("nvEncodeAPI64.dll");
	char path[512] = ""; if (h) GetModuleFileNameA(h, path, sizeof path);
	printf("lib: %s\n", h ? path : "NOT FOUND");
	if (!h) return 1;
	uint32_t maxv = 0; ((PFN_GetMax)GetProcAddress(h, "NvEncodeAPIGetMaxSupportedVersion"))(&maxv);
	printf("max supported API %u.%u\n", maxv >> 4, maxv & 15);

	IDXGIFactory1* fac; CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac);
	for (UINT ai = 0; ai < 4; ai++) {
		IDXGIAdapter1* ad; if (FAILED(fac->EnumAdapters1(ai, &ad))) break;
		DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d);
		printf("\n== adapter %u: %ls (vendor %04x)\n", ai, d.Description, d.VendorId);
		ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; D3D_FEATURE_LEVEL fl;
		HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
		printf("D3D11CreateDevice hr=%08lx\n", hr);
		if (FAILED(hr)) continue;

		int apis[][2] = { {6,0},{7,0},{8,0},{9,0},{11,0},{12,0},{12,1},{13,0} };
		for (auto& a : apis) {
			uint32_t apiver = a[0] | (a[1] << 24);
			FUNCLIST fl2 = {}; fl2.version = sv(apiver, 2);
			int r = ((PFN_Create)GetProcAddress(h, "NvEncodeAPICreateInstance"))(&fl2);
			if (r) { printf("API %d.%d: CreateInstance failed %d\n", a[0], a[1], r); continue; }
			OPENEX p = {}; p.version = sv(apiver, 1); p.deviceType = 0 /*DIRECTX*/; p.device = dev; p.apiVersion = apiver;
			void* enc = nullptr;
			r = ((PFN_OpenEx)fl2.fn[29])(&p, &enc);
			uint32_t cnt = 0; int r2 = -1;
			if (r == 0 && enc) { r2 = ((PFN_GuidCount)fl2.fn[1])(enc, &cnt); ((PFN_Destroy)fl2.fn[27])(enc); }
			printf("API %d.%d: OpenEncodeSessionEx(DIRECTX) status=%d%s; GUID count status=%d count=%u\n", a[0], a[1], r, r ? "" : " (OK)", r2, cnt);
		}
	}
	return 0;
}
