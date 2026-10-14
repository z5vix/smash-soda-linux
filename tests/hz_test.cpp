#include <windows.h>
#include <cstdio>
static BOOL CALLBACK cb(HMONITOR m, HDC, LPRECT r, LPARAM) {
	MONITORINFOEXA mi = { sizeof(mi) }; GetMonitorInfoA(m, &mi);
	DEVMODEA dm = { }; dm.dmSize = sizeof(dm);
	BOOL ok = EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
	printf("monitor %s at %ld,%ld %ldx%ld: current mode %s %lux%lu @ %lu Hz\n", mi.szDevice, r->left, r->top, r->right - r->left, r->bottom - r->top, ok ? "ok" : "FAILED", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
	int best = 0; DEVMODEA d2 = { }; d2.dmSize = sizeof(d2);
	for (int i = 0; EnumDisplaySettingsA(mi.szDevice, i, &d2); i++) if ((int)d2.dmDisplayFrequency > best) best = d2.dmDisplayFrequency;
	printf("   highest listed mode: %d Hz\n", best);
	return TRUE;
}
int main(int argc, char** argv) { if (argc > 1) freopen(argv[1], "w", stdout); setvbuf(stdout, NULL, _IONBF, 0); EnumDisplayMonitors(NULL, NULL, cb, 0); return 0; }
