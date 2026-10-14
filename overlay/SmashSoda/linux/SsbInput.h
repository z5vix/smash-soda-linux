#pragma once
// Forwards guest keyboard / mouse input to the Linux desktop through the bridge
// (ssb-padd injects it via uinput). Falls back to nothing on real Windows.

#include <windows.h>

namespace SsbInput {

	// True when running under the Linux bridge and the daemon is alive.
	bool active();

	bool key(WORD virtualKey, bool extended, bool down);
	bool mouseButton(DWORD mouseEventFlag, DWORD mouseData);   // MOUSEEVENTF_* button / wheel flags
	bool mouseRelative(LONG dx, LONG dy);
	// x/y are in stream pixels (streamW x streamH); mapped onto the captured monitor.
	bool mouseAbsolute(LONG x, LONG y, int streamW, int streamH);
	// Size of the frame currently being captured (0 if unknown).
	void captureSize(int& w, int& h);
}
