// Replacement for ViGEmClient (which needs the Windows ViGEmBus kernel driver).
// Implements the subset of the ViGEm client API Smash Soda uses on top of the
// Linux uinput bridge (ssb-padd), via shared memory.

#include <windows.h>
#include <atomic>
#include <thread>
#include <cstring>

#include "ViGEm/Client.h"
#include "SsbBridge.h"

struct _VIGEM_CLIENT_T {
	ssb_pads* pads = nullptr;
};

struct _VIGEM_TARGET_T {
	VIGEM_TARGET_TYPE type = Xbox360Wired;
	USHORT vid = 0, pid = 0;
	int slot = -1;
	PVIGEM_CLIENT client = nullptr;

	PFN_VIGEM_X360_NOTIFICATION x360Cb = nullptr;
	PFN_VIGEM_DS4_NOTIFICATION ds4Cb = nullptr;
	LPVOID userData = nullptr;
	std::atomic<bool> stop{ false };
	std::thread* rumbleThread = nullptr;
};

static void startRumbleThread(PVIGEM_TARGET t);
static void stopRumbleThread(PVIGEM_TARGET t);

PVIGEM_CLIENT vigem_alloc(void) { return new _VIGEM_CLIENT_T(); }
void vigem_free(PVIGEM_CLIENT c) { delete c; }

VIGEM_ERROR vigem_connect(PVIGEM_CLIENT c)
{
	if (!c) return VIGEM_ERROR_BUS_INVALID_HANDLE;
	ssb_pads* p = SsbBridge::pads();
	if (!p) { SsbBridge::log("gamepads: bridge file not found (is ssb-padd running?)"); return VIGEM_ERROR_BUS_NOT_FOUND; }
	if (!SsbBridge::alive(&p->heartbeat)) { SsbBridge::log("gamepads: ssb-padd heartbeat is not moving"); return VIGEM_ERROR_BUS_NOT_FOUND; }
	SsbBridge::log("gamepads: connected to ssb-padd");
	c->pads = p;
	return VIGEM_ERROR_NONE;
}

void vigem_disconnect(PVIGEM_CLIENT c) { if (c) c->pads = nullptr; }

BOOLEAN vigem_target_is_waitable_add_supported(PVIGEM_TARGET) { return FALSE; }

PVIGEM_TARGET vigem_target_x360_alloc(void)
{
	auto* t = new _VIGEM_TARGET_T();
	t->type = Xbox360Wired; t->vid = 0x045E; t->pid = 0x028E;
	return t;
}

PVIGEM_TARGET vigem_target_ds4_alloc(void)
{
	auto* t = new _VIGEM_TARGET_T();
	t->type = DualShock4Wired; t->vid = 0x054C; t->pid = 0x05C4;
	return t;
}

void vigem_target_free(PVIGEM_TARGET t)
{
	if (!t) return;
	stopRumbleThread(t);
	delete t;
}

VIGEM_ERROR vigem_target_add(PVIGEM_CLIENT c, PVIGEM_TARGET t)
{
	if (!c || !c->pads) return VIGEM_ERROR_BUS_INVALID_HANDLE;
	if (!t) return VIGEM_ERROR_INVALID_TARGET;
	if (t->slot >= 0) return VIGEM_ERROR_ALREADY_CONNECTED;

	for (int i = 0; i < SSB_MAX_PADS; i++) {
		ssb_pad& s = c->pads->pads[i];
		if (s.active) continue;
		s.type = (t->type == DualShock4Wired) ? SSB_PAD_DS4 : SSB_PAD_X360;
		s.vidpid = ((uint32_t)t->vid << 16) | t->pid;
		s.buttons = 0; s.lt = s.rt = 0; s.lx = s.ly = s.rx = s.ry = 0;
		s.rumble_large = s.rumble_small = 0;
		s.seq = s.seq + 1;
		MemoryBarrier();
		s.active = 1;
		t->slot = i;
		t->client = c;
		SsbBridge::log("gamepads: target added in slot %d (vid %04x pid %04x)", i, t->vid, t->pid);
		if (t->x360Cb || t->ds4Cb) startRumbleThread(t);
		return VIGEM_ERROR_NONE;
	}
	return VIGEM_ERROR_NO_FREE_SLOT;
}

VIGEM_ERROR vigem_target_add_async(PVIGEM_CLIENT c, PVIGEM_TARGET t, PFN_VIGEM_TARGET_ADD_RESULT result)
{
	VIGEM_ERROR e = vigem_target_add(c, t);
	if (result) result(c, t, e);
	return e;
}

VIGEM_ERROR vigem_target_remove(PVIGEM_CLIENT c, PVIGEM_TARGET t)
{
	if (!c || !c->pads) return VIGEM_ERROR_BUS_INVALID_HANDLE;
	if (!t || t->slot < 0) return VIGEM_ERROR_TARGET_NOT_PLUGGED_IN;
	stopRumbleThread(t);
	ssb_pad& s = c->pads->pads[t->slot];
	s.active = 0;
	MemoryBarrier();
	t->slot = -1;
	return VIGEM_ERROR_NONE;
}

static void rumbleLoop(PVIGEM_TARGET t)
{
	uint32_t last = 0;
	bool first = true;
	while (!t->stop.load()) {
		if (t->slot >= 0 && t->client && t->client->pads) {
			ssb_pad& s = t->client->pads->pads[t->slot];
			uint32_t seq = s.rumble_seq;
			if (first) { last = seq; first = false; }
			if (seq != last) {
				last = seq;
				UCHAR large = s.rumble_large, small = s.rumble_small;
				if (t->x360Cb) t->x360Cb(t->client, t, large, small, 0, t->userData);
				else if (t->ds4Cb) { DS4_LIGHTBAR_COLOR col{}; t->ds4Cb(t->client, t, large, small, col, t->userData); }
			}
		}
		Sleep(8);
	}
}

static void startRumbleThread(PVIGEM_TARGET t)
{
	if (t->rumbleThread) return;
	t->stop = false;
	t->rumbleThread = new std::thread(rumbleLoop, t);
}

static void stopRumbleThread(PVIGEM_TARGET t)
{
	if (!t->rumbleThread) return;
	t->stop = true;
	if (t->rumbleThread->joinable()) t->rumbleThread->join();
	delete t->rumbleThread;
	t->rumbleThread = nullptr;
}

VIGEM_ERROR vigem_target_x360_register_notification(PVIGEM_CLIENT, PVIGEM_TARGET t, PFN_VIGEM_X360_NOTIFICATION cb, LPVOID user)
{
	if (!t || !cb) return VIGEM_ERROR_INVALID_PARAMETER;
	t->x360Cb = cb; t->userData = user;
	if (t->slot >= 0) startRumbleThread(t);
	return VIGEM_ERROR_NONE;
}

VIGEM_ERROR vigem_target_ds4_register_notification(PVIGEM_CLIENT, PVIGEM_TARGET t, PFN_VIGEM_DS4_NOTIFICATION cb, LPVOID user)
{
	if (!t || !cb) return VIGEM_ERROR_INVALID_PARAMETER;
	t->ds4Cb = cb; t->userData = user;
	if (t->slot >= 0) startRumbleThread(t);
	return VIGEM_ERROR_NONE;
}

void vigem_target_x360_unregister_notification(PVIGEM_TARGET t) { if (t) { stopRumbleThread(t); t->x360Cb = nullptr; } }
void vigem_target_ds4_unregister_notification(PVIGEM_TARGET t)  { if (t) { stopRumbleThread(t); t->ds4Cb = nullptr; } }

void vigem_target_set_vid(PVIGEM_TARGET t, USHORT vid) { if (t) t->vid = vid; }
void vigem_target_set_pid(PVIGEM_TARGET t, USHORT pid) { if (t) t->pid = pid; }
USHORT vigem_target_get_vid(PVIGEM_TARGET t) { return t ? t->vid : 0; }
USHORT vigem_target_get_pid(PVIGEM_TARGET t) { return t ? t->pid : 0; }

static void writePad(PVIGEM_CLIENT c, PVIGEM_TARGET t, USHORT buttons, BYTE lt, BYTE rt, SHORT lx, SHORT ly, SHORT rx, SHORT ry)
{
	ssb_pad& s = c->pads->pads[t->slot];
	s.buttons = buttons; s.lt = lt; s.rt = rt;
	s.lx = lx; s.ly = ly; s.rx = rx; s.ry = ry;
	MemoryBarrier();
	s.seq = s.seq + 1;
}

VIGEM_ERROR vigem_target_x360_update(PVIGEM_CLIENT c, PVIGEM_TARGET t, XUSB_REPORT r)
{
	if (!c || !c->pads) return VIGEM_ERROR_BUS_INVALID_HANDLE;
	if (!t || t->slot < 0) return VIGEM_ERROR_TARGET_NOT_PLUGGED_IN;
	writePad(c, t, r.wButtons, r.bLeftTrigger, r.bRightTrigger, r.sThumbLX, r.sThumbLY, r.sThumbRX, r.sThumbRY);
	return VIGEM_ERROR_NONE;
}

static SHORT ds4Axis(BYTE v)            { return (SHORT)(((int)v - 128) * 257); }
static SHORT ds4AxisInv(BYTE v)         { int r = -((int)v - 128) * 257; return (SHORT)(r > 32767 ? 32767 : r); }

VIGEM_ERROR vigem_target_ds4_update(PVIGEM_CLIENT c, PVIGEM_TARGET t, DS4_REPORT r)
{
	if (!c || !c->pads) return VIGEM_ERROR_BUS_INVALID_HANDLE;
	if (!t || t->slot < 0) return VIGEM_ERROR_TARGET_NOT_PLUGGED_IN;

	USHORT b = 0;
	const USHORT w = r.wButtons;
	switch (w & 0xF) {
	case DS4_BUTTON_DPAD_NORTH:     b |= XUSB_GAMEPAD_DPAD_UP; break;
	case DS4_BUTTON_DPAD_NORTHEAST: b |= XUSB_GAMEPAD_DPAD_UP | XUSB_GAMEPAD_DPAD_RIGHT; break;
	case DS4_BUTTON_DPAD_EAST:      b |= XUSB_GAMEPAD_DPAD_RIGHT; break;
	case DS4_BUTTON_DPAD_SOUTHEAST: b |= XUSB_GAMEPAD_DPAD_DOWN | XUSB_GAMEPAD_DPAD_RIGHT; break;
	case DS4_BUTTON_DPAD_SOUTH:     b |= XUSB_GAMEPAD_DPAD_DOWN; break;
	case DS4_BUTTON_DPAD_SOUTHWEST: b |= XUSB_GAMEPAD_DPAD_DOWN | XUSB_GAMEPAD_DPAD_LEFT; break;
	case DS4_BUTTON_DPAD_WEST:      b |= XUSB_GAMEPAD_DPAD_LEFT; break;
	case DS4_BUTTON_DPAD_NORTHWEST: b |= XUSB_GAMEPAD_DPAD_UP | XUSB_GAMEPAD_DPAD_LEFT; break;
	default: break;
	}
	if (w & DS4_BUTTON_CROSS)          b |= XUSB_GAMEPAD_A;
	if (w & DS4_BUTTON_CIRCLE)         b |= XUSB_GAMEPAD_B;
	if (w & DS4_BUTTON_SQUARE)         b |= XUSB_GAMEPAD_X;
	if (w & DS4_BUTTON_TRIANGLE)       b |= XUSB_GAMEPAD_Y;
	if (w & DS4_BUTTON_SHOULDER_LEFT)  b |= XUSB_GAMEPAD_LEFT_SHOULDER;
	if (w & DS4_BUTTON_SHOULDER_RIGHT) b |= XUSB_GAMEPAD_RIGHT_SHOULDER;
	if (w & DS4_BUTTON_SHARE)          b |= XUSB_GAMEPAD_BACK;
	if (w & DS4_BUTTON_OPTIONS)        b |= XUSB_GAMEPAD_START;
	if (w & DS4_BUTTON_THUMB_LEFT)     b |= XUSB_GAMEPAD_LEFT_THUMB;
	if (w & DS4_BUTTON_THUMB_RIGHT)    b |= XUSB_GAMEPAD_RIGHT_THUMB;
	if (r.bSpecial & DS4_SPECIAL_BUTTON_PS) b |= XUSB_GAMEPAD_GUIDE;

	writePad(c, t, b, r.bTriggerL, r.bTriggerR, ds4Axis(r.bThumbLX), ds4AxisInv(r.bThumbLY), ds4Axis(r.bThumbRX), ds4AxisInv(r.bThumbRY));
	return VIGEM_ERROR_NONE;
}

VIGEM_ERROR vigem_target_ds4_update_ex(PVIGEM_CLIENT c, PVIGEM_TARGET t, DS4_REPORT_EX r)
{
	DS4_REPORT base{};
	base.bThumbLX = r.Report.bThumbLX; base.bThumbLY = r.Report.bThumbLY;
	base.bThumbRX = r.Report.bThumbRX; base.bThumbRY = r.Report.bThumbRY;
	base.wButtons = r.Report.wButtons; base.bSpecial = r.Report.bSpecial;
	base.bTriggerL = r.Report.bTriggerL; base.bTriggerR = r.Report.bTriggerR;
	return vigem_target_ds4_update(c, t, base);
}

ULONG vigem_target_get_index(PVIGEM_TARGET t) { return t && t->slot >= 0 ? (ULONG)t->slot : 0; }
VIGEM_TARGET_TYPE vigem_target_get_type(PVIGEM_TARGET t) { return t ? t->type : Xbox360Wired; }
BOOL vigem_target_is_attached(PVIGEM_TARGET t) { return t && t->slot >= 0; }

VIGEM_ERROR vigem_target_x360_get_user_index(PVIGEM_CLIENT, PVIGEM_TARGET t, PULONG index)
{
	if (!t || !index) return VIGEM_ERROR_INVALID_PARAMETER;
	if (t->slot < 0) return VIGEM_ERROR_TARGET_NOT_PLUGGED_IN;
	*index = (ULONG)t->slot;
	return VIGEM_ERROR_NONE;
}
