// Calls InitializeEncoder and ReconfigureEncoder with a 10 Mbps rate-control config and prints what the proxy leaves in it.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
typedef int (__stdcall *PFN_Create)(void*);
typedef int (__stdcall *PFN_OpenEx)(void*, void**);
typedef int (__stdcall *PFN_Fn2)(void*, void*);
typedef struct { uint32_t version, reserved; void* fn[96]; } FUNCLIST;
static uint32_t sv(uint32_t api, uint32_t v) { return api | (v << 16) | (0x7u << 28); }
int main(int argc, char** argv)
{
	if (argc > 1) freopen(argv[1], "w", stdout);
	HMODULE h = LoadLibraryA("nvEncodeAPI64.dll");
	if (!h) return 1;
	IDXGIFactory1* fac; CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac);
	for (UINT ai = 0; ai < 4; ai++) {
		IDXGIAdapter1* ad; if (FAILED(fac->EnumAdapters1(ai, &ad))) break;
		DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d);
		if (d.VendorId != 0x10de) continue;
		ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; D3D_FEATURE_LEVEL fl;
		if (FAILED(D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx))) continue;
		uint32_t apiver = 12 | (0 << 24);
		static FUNCLIST fl2; fl2.version = sv(apiver, 2);
		((PFN_Create)GetProcAddress(h, "NvEncodeAPICreateInstance"))(&fl2);
		struct { uint32_t version, deviceType; void* device; void* reserved; uint32_t apiVersion; uint32_t r1[253]; void* r2[64]; } p = {};
		p.version = sv(apiver, 1); p.deviceType = 0; p.device = dev; p.apiVersion = apiver;
		void* enc = nullptr;
		int r = ((PFN_OpenEx)fl2.fn[29])(&p, &enc);
		printf("adapter %u %ls open=%d\n", ai, d.Description, r);
		if (r || !enc) continue;
		static uint8_t cfg[2048]; static uint8_t init[512]; static uint8_t rec[1024];
		for (int pass = 0; pass < 2; pass++) {
			memset(cfg, 0, sizeof cfg); memset(init, 0, sizeof init); memset(rec, 0, sizeof rec);
			*(uint32_t*)(cfg + 60) = 10000000; *(uint32_t*)(cfg + 64) = 10000000;
			uint8_t* ip = pass == 0 ? init : rec + 8;
			*(uint32_t*)(ip + 40) = 1920; *(uint32_t*)(ip + 44) = 1080; *(uint32_t*)(ip + 56) = 60; *(uint32_t*)(ip + 60) = 1;
			*(void**)(ip + 88) = cfg;
			int rr = ((PFN_Fn2)fl2.fn[pass == 0 ? 11 : 32])(enc, pass == 0 ? (void*)init : (void*)rec);
			printf("%s status=%d  avg=%u max=%u\n", pass == 0 ? "initialize" : "reconfigure", rr, *(uint32_t*)(cfg + 60), *(uint32_t*)(cfg + 64));
		}
		break;
	}
	return 0;
}
