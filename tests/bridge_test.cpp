// Runs under Proton: exercises the Windows side of the Linux bridge end to end.
#include <windows.h>
#include <cstdio>
#include "ViGEm/Client.h"
#include "SsbBridge.h"
#include "SsbInput.h"
#include "SsbAudio.h"

#define XUSB_A 0x1000
#define XUSB_DPAD_UP 0x0001

static void say(const char* s) { printf("%s\n", s); fflush(stdout); }

int main(int argc, char** argv)
{
	if (argc > 1) freopen(argv[1], "w", stdout);
	printf("bridge dir env: %s\n", getenv("SSB_BRIDGE_DIR") ? getenv("SSB_BRIDGE_DIR") : "(unset)");

	// ---- gamepad ----
	PVIGEM_CLIENT c = vigem_alloc();
	VIGEM_ERROR e = vigem_connect(c);
	printf("vigem_connect: %s\n", VIGEM_SUCCESS(e) ? "OK" : "FAILED");
	PVIGEM_TARGET t = vigem_target_x360_alloc();
	e = vigem_target_add(c, t);
	printf("target_add: %s (index %lu)\n", VIGEM_SUCCESS(e) ? "OK" : "FAILED", vigem_target_get_index(t));
	Sleep(800);
	XUSB_REPORT r{};
	r.wButtons = XUSB_A | XUSB_DPAD_UP; r.sThumbLX = 32767; r.sThumbLY = 16000; r.bRightTrigger = 255;
	vigem_target_x360_update(c, t, r);
	say("pad: sent A + dpad up + stick right + RT");
	Sleep(800);
	r = XUSB_REPORT{};
	vigem_target_x360_update(c, t, r);
	Sleep(300);

	// ---- keyboard / mouse ----
	printf("input active: %d\n", SsbInput::active());
	SsbInput::key('A', false, true); SsbInput::key('A', false, false);
	SsbInput::mouseRelative(10, 5);
	SsbInput::mouseAbsolute(1280, 540, 2560, 1080);
	say("input: sent key A, rel move, abs move to centre");
	Sleep(500);

	// ---- video / audio ----
	ssb_video* v = SsbBridge::video();
	if (v) {
		uint32_t a = v->latest; Sleep(1000); uint32_t b = v->latest;
		printf("video: frames/s ~ %u, size %ux%u, monitor %d,%d %dx%d\n", b - a,
			v->info[b % SSB_VIDEO_SLOTS].width, v->info[b % SSB_VIDEO_SLOTS].height, v->mon_x, v->mon_y, v->mon_w, v->mon_h);
		const uint8_t* px = (const uint8_t*)v + SSB_VIDEO_HDR_BYTES + (uint64_t)(b % SSB_VIDEO_SLOTS) * SSB_VIDEO_SLOT_BYTES;
		unsigned nz = 0; for (int i = 0; i < 4096; i++) nz += px[i * 4099 % (1920 * 1000)] != 0;
		printf("video: non-zero sample bytes %u/4096\n", nz);
	} else say("video: NOT AVAILABLE");
	printf("audio active: %d\n", SsbAudio::active());
	std::vector<int16_t> samples; SsbAudio::read(samples); Sleep(500); samples.clear(); SsbAudio::read(samples);
	printf("audio: %zu samples in 0.5 s\n", samples.size());

	vigem_target_remove(c, t);
	vigem_target_free(t); vigem_disconnect(c); vigem_free(c);
	say("done");
	return 0;
}
