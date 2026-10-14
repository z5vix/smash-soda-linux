#pragma once
// Windows-side access to the Linux bridge (shared-memory files written/read by
// the native helpers ssb-padd and ssb-capture). Only meaningful under Wine/Proton.

#include <windows.h>
#include "ssb_proto.h"

namespace SsbBridge {

	// Maps (creating the view lazily) one of the bridge files. Returns nullptr when the
	// bridge directory is not available (e.g. running on real Windows).
	void* map(const char* fileName, size_t size);

	// Fresh heartbeat check: true if the counter at `hb` advanced within `ms` milliseconds.
	bool alive(volatile const uint32_t* hb, DWORD ms = 400);

	ssb_pads*  pads();
	ssb_input* input();
	ssb_video* video();

	// Appends a line to <bridge dir>/smashsoda-wine.log (no-op without a bridge dir).
	void log(const char* fmt, ...);

	// Appends a line to another log in the bridge dir (IPv4 addresses are masked).
	void logFile(const char* fileName, const char* text);

	// Live tuning: reads "key=value" lines from <bridge dir>/tuning.txt (re-read every second), so settings
	// can be changed while a room is running.
	int tuningInt(const char* key, int defaultValue);

	// True when running under the Linux bridge (the launcher exports SSB_BRIDGE_DIR). Settings that only make
	// sense on Windows (monitor/adapter/capture method, audio devices...) are hidden then.
	bool linuxMode();

	// Refresh rate (Hz) of the display being shared, as reported by the Linux helper; 0 until known.
	int displayRefreshHz();

	// Asks the Linux helper to scale the stream to w x h the next time it starts (0,0 = native resolution).
	void setWantedResolution(int w, int h);

	// Passes the Video settings' capture choices to the Linux helper (applied the next time you press Host).
	void setCaptureOptions(unsigned captureMode, unsigned sourceTypes, bool remember);

	// Per-app audio mixer (published by ssb-audioapps.py). apps() is null when the helper is not running.
	ssb_apps* apps();
	bool appsAlive();
	// Sends one command (SSB_APPCMD_*). Returns false while the previous command is still being applied - retry later.
	bool appsCommand(uint32_t type, uint32_t id, int32_t value);

	// Asks the Linux helper to open the share picker again and switch to another screen/window without stopping the room.
	void requestChangeScreen();

	// Upload budget for the whole room in kbps (0 = unlimited) and the number of connected guests; the NVENC layer
	// limits the stream bitrate to budget / guests (Parsec sends a separate copy of the stream to every guest).
	void setBitrateBudget(unsigned kbps);
	void setGuestCount(unsigned guests);

	// Logs the exception code and where it happened (module + offset, with a short stack) if the app crashes.
	void installCrashLogger();

	// Tells the Linux helper whether a room is being hosted (it opens the screen picker on
	// the first request and stops capturing when cleared).
	void setCaptureWanted(bool wanted);

	// Queue a keyboard / mouse event for injection on the Linux desktop.
	bool pushInput(uint32_t type, int32_t a, int32_t b);
}
