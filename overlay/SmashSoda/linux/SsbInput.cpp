#include "SsbInput.h"
#include "SsbBridge.h"

#include <algorithm>
#include <cmath>

namespace SsbInput {

	// evdev codes (linux/input-event-codes.h) - duplicated because this is a Windows build.
	namespace ev {
		enum : int {
			KEY_ESC = 1, KEY_1 = 2, KEY_MINUS = 12, KEY_EQUAL = 13, KEY_BACKSPACE = 14, KEY_TAB = 15,
			KEY_Q = 16, KEY_LEFTBRACE = 26, KEY_RIGHTBRACE = 27, KEY_ENTER = 28, KEY_LEFTCTRL = 29,
			KEY_A = 30, KEY_SEMICOLON = 39, KEY_APOSTROPHE = 40, KEY_GRAVE = 41, KEY_LEFTSHIFT = 42,
			KEY_BACKSLASH = 43, KEY_Z = 44, KEY_COMMA = 51, KEY_DOT = 52, KEY_SLASH = 53,
			KEY_RIGHTSHIFT = 54, KEY_KPASTERISK = 55, KEY_LEFTALT = 56, KEY_SPACE = 57, KEY_CAPSLOCK = 58,
			KEY_F1 = 59, KEY_NUMLOCK = 69, KEY_SCROLLLOCK = 70, KEY_KP7 = 71, KEY_KPMINUS = 74, KEY_KPPLUS = 78,
			KEY_KP1 = 79, KEY_KP0 = 82, KEY_KPDOT = 83, KEY_F11 = 87, KEY_F12 = 88, KEY_KPENTER = 96,
			KEY_RIGHTCTRL = 97, KEY_KPSLASH = 98, KEY_SYSRQ = 99, KEY_RIGHTALT = 100, KEY_HOME = 102,
			KEY_UP = 103, KEY_PAGEUP = 104, KEY_LEFT = 105, KEY_RIGHT = 106, KEY_END = 107, KEY_DOWN = 108,
			KEY_PAGEDOWN = 109, KEY_INSERT = 110, KEY_DELETE = 111, KEY_MUTE = 113, KEY_VOLUMEDOWN = 114,
			KEY_VOLUMEUP = 115, KEY_PAUSE = 119, KEY_LEFTMETA = 125, KEY_RIGHTMETA = 126, KEY_COMPOSE = 127,
			KEY_NEXTSONG = 163, KEY_PLAYPAUSE = 164, KEY_PREVIOUSSONG = 165, KEY_STOPCD = 166,
			KEY_F13 = 183,
			BTN_LEFT = 0x110, BTN_RIGHT = 0x111, BTN_MIDDLE = 0x112, BTN_SIDE = 0x113, BTN_EXTRA = 0x114,
			REL_X = 0, REL_Y = 1, REL_HWHEEL = 6, REL_WHEEL = 8,
		};
	}

	static int vkToEvdev(WORD vk, bool extended)
	{
		if (vk >= 'A' && vk <= 'Z') {
			static const int row[26] = { // A..Z -> evdev
				30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44 };
			return row[vk - 'A'];
		}
		if (vk >= '1' && vk <= '9') return ev::KEY_1 + (vk - '1');
		if (vk == '0') return 11;
		if (vk >= VK_F1 && vk <= VK_F12) return (vk <= VK_F10) ? ev::KEY_F1 + (vk - VK_F1) : ev::KEY_F11 + (vk - VK_F11);
		if (vk >= VK_F13 && vk <= VK_F24) return ev::KEY_F13 + (vk - VK_F13);
		if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
			static const int kp[10] = { 82, 79, 80, 81, 75, 76, 77, 71, 72, 73 };
			return kp[vk - VK_NUMPAD0];
		}
		switch (vk) {
		case VK_ESCAPE: return ev::KEY_ESC;
		case VK_OEM_MINUS: return ev::KEY_MINUS;
		case VK_OEM_PLUS: return ev::KEY_EQUAL;
		case VK_BACK: return ev::KEY_BACKSPACE;
		case VK_TAB: return ev::KEY_TAB;
		case VK_OEM_4: return ev::KEY_LEFTBRACE;
		case VK_OEM_6: return ev::KEY_RIGHTBRACE;
		case VK_RETURN: return extended ? ev::KEY_KPENTER : ev::KEY_ENTER;
		case VK_LCONTROL: case VK_CONTROL: return ev::KEY_LEFTCTRL;
		case VK_RCONTROL: return ev::KEY_RIGHTCTRL;
		case VK_OEM_1: return ev::KEY_SEMICOLON;
		case VK_OEM_7: return ev::KEY_APOSTROPHE;
		case VK_OEM_3: return ev::KEY_GRAVE;
		case VK_LSHIFT: case VK_SHIFT: return ev::KEY_LEFTSHIFT;
		case VK_RSHIFT: return ev::KEY_RIGHTSHIFT;
		case VK_OEM_5: return ev::KEY_BACKSLASH;
		case VK_OEM_COMMA: return ev::KEY_COMMA;
		case VK_OEM_PERIOD: return ev::KEY_DOT;
		case VK_OEM_2: return ev::KEY_SLASH;
		case VK_MULTIPLY: return ev::KEY_KPASTERISK;
		case VK_LMENU: case VK_MENU: return ev::KEY_LEFTALT;
		case VK_RMENU: return ev::KEY_RIGHTALT;
		case VK_SPACE: return ev::KEY_SPACE;
		case VK_CAPITAL: return ev::KEY_CAPSLOCK;
		case VK_NUMLOCK: return ev::KEY_NUMLOCK;
		case VK_SCROLL: return ev::KEY_SCROLLLOCK;
		case VK_SUBTRACT: return ev::KEY_KPMINUS;
		case VK_ADD: return ev::KEY_KPPLUS;
		case VK_DECIMAL: return ev::KEY_KPDOT;
		case VK_DIVIDE: return ev::KEY_KPSLASH;
		case VK_SNAPSHOT: return ev::KEY_SYSRQ;
		case VK_HOME: return ev::KEY_HOME;
		case VK_UP: return ev::KEY_UP;
		case VK_PRIOR: return ev::KEY_PAGEUP;
		case VK_LEFT: return ev::KEY_LEFT;
		case VK_RIGHT: return ev::KEY_RIGHT;
		case VK_END: return ev::KEY_END;
		case VK_DOWN: return ev::KEY_DOWN;
		case VK_NEXT: return ev::KEY_PAGEDOWN;
		case VK_INSERT: return ev::KEY_INSERT;
		case VK_DELETE: return ev::KEY_DELETE;
		case VK_VOLUME_MUTE: return ev::KEY_MUTE;
		case VK_VOLUME_DOWN: return ev::KEY_VOLUMEDOWN;
		case VK_VOLUME_UP: return ev::KEY_VOLUMEUP;
		case VK_PAUSE: return ev::KEY_PAUSE;
		case VK_LWIN: return ev::KEY_LEFTMETA;
		case VK_RWIN: return ev::KEY_RIGHTMETA;
		case VK_APPS: return ev::KEY_COMPOSE;
		case VK_MEDIA_NEXT_TRACK: return ev::KEY_NEXTSONG;
		case VK_MEDIA_PLAY_PAUSE: return ev::KEY_PLAYPAUSE;
		case VK_MEDIA_PREV_TRACK: return ev::KEY_PREVIOUSSONG;
		case VK_MEDIA_STOP: return ev::KEY_STOPCD;
		default: return 0;
		}
	}

	bool active()
	{
		static DWORD lastCheck = 0;
		static bool lastResult = false;
		static uint32_t lastHb = 0;
		static DWORD lastHbTick = 0;

		const DWORD now = GetTickCount();
		if (lastCheck != 0 && now - lastCheck < 250) return lastResult;
		lastCheck = now;

		ssb_input* in = SsbBridge::input();
		if (!in) return lastResult = false;
		const uint32_t hb = in->heartbeat;
		if (hb != lastHb || lastHbTick == 0) { lastHb = hb; lastHbTick = now; }
		const bool res = (now - lastHbTick) < 2000;
		static bool logged = false;
		if (res && !logged) { logged = true; SsbBridge::log("input: forwarding keyboard/mouse to the Linux desktop"); }
		return lastResult = res;
	}

	bool key(WORD vk, bool extended, bool down)
	{
		const int code = vkToEvdev(vk, extended);
		if (!code) return false;
		return SsbBridge::pushInput(SSB_EV_KEY, code, down ? 1 : 0);
	}

	bool mouseButton(DWORD flag, DWORD data)
	{
		switch (flag) {
		case MOUSEEVENTF_LEFTDOWN:   return SsbBridge::pushInput(SSB_EV_KEY, ev::BTN_LEFT, 1);
		case MOUSEEVENTF_LEFTUP:     return SsbBridge::pushInput(SSB_EV_KEY, ev::BTN_LEFT, 0);
		case MOUSEEVENTF_RIGHTDOWN:  return SsbBridge::pushInput(SSB_EV_KEY, ev::BTN_RIGHT, 1);
		case MOUSEEVENTF_RIGHTUP:    return SsbBridge::pushInput(SSB_EV_KEY, ev::BTN_RIGHT, 0);
		case MOUSEEVENTF_MIDDLEDOWN: return SsbBridge::pushInput(SSB_EV_KEY, ev::BTN_MIDDLE, 1);
		case MOUSEEVENTF_MIDDLEUP:   return SsbBridge::pushInput(SSB_EV_KEY, ev::BTN_MIDDLE, 0);
		case MOUSEEVENTF_XDOWN:
		case MOUSEEVENTF_XUP: {
			const int btn = (data == XBUTTON1) ? ev::BTN_SIDE : ev::BTN_EXTRA;
			return SsbBridge::pushInput(SSB_EV_KEY, btn, flag == MOUSEEVENTF_XDOWN ? 1 : 0);
		}
		case MOUSEEVENTF_WHEEL:
			return SsbBridge::pushInput(SSB_EV_REL, ev::REL_WHEEL, (int)data / WHEEL_DELTA);
		case MOUSEEVENTF_HWHEEL:
			return SsbBridge::pushInput(SSB_EV_REL, ev::REL_HWHEEL, (int)data / WHEEL_DELTA);
		default: return false;
		}
	}

	bool mouseRelative(LONG dx, LONG dy)
	{
		bool ok = true;
		if (dx) ok = SsbBridge::pushInput(SSB_EV_REL, ev::REL_X, dx) && ok;
		if (dy) ok = SsbBridge::pushInput(SSB_EV_REL, ev::REL_Y, dy) && ok;
		return ok;
	}

	void captureSize(int& w, int& h)
	{
		w = h = 0;
		ssb_video* v = SsbBridge::video();
		if (!v || v->latest == 0) return;
		const uint32_t slot = v->latest % SSB_VIDEO_SLOTS;
		w = (int)v->info[slot].width;
		h = (int)v->info[slot].height;
	}

	bool mouseAbsolute(LONG x, LONG y, int streamW, int streamH)
	{
		ssb_video* v = SsbBridge::video();
		if (!v) return false;
		if (streamW <= 1 || streamH <= 1) captureSize(streamW, streamH);
		if (streamW <= 1 || streamH <= 1) return false;

		const double mx = (double)v->mon_x, my = (double)v->mon_y;
		const double mw = v->mon_w > 0 ? v->mon_w : streamW;
		const double mh = v->mon_h > 0 ? v->mon_h : streamH;
		const double dx = (double)v->desk_x, dy = (double)v->desk_y;
		const double dw = v->desk_w > 0 ? v->desk_w : mw;
		const double dh = v->desk_h > 0 ? v->desk_h : mh;

		const double fx = std::clamp((double)x / (double)(streamW - 1), 0.0, 1.0);
		const double fy = std::clamp((double)y / (double)(streamH - 1), 0.0, 1.0);
		const double lx = mx + fx * (mw - 1), ly = my + fy * (mh - 1);
		const int nx = (int)std::lround(std::clamp((lx - dx) / std::max(1.0, dw - 1), 0.0, 1.0) * 65535.0);
		const int ny = (int)std::lround(std::clamp((ly - dy) / std::max(1.0, dh - 1), 0.0, 1.0) * 65535.0);
		bool ok = SsbBridge::pushInput(SSB_EV_ABS, 0, nx);
		ok = SsbBridge::pushInput(SSB_EV_ABS, 1, ny) && ok;
		return ok;
	}
}
