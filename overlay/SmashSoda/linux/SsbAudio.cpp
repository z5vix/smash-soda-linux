#include "SsbAudio.h"
#include "SsbBridge.h"

namespace SsbAudio {

	static ssb_audio* audio()
	{
		auto* a = (ssb_audio*)SsbBridge::map(SSB_AUDIO_FILE, SSB_AUDIO_FILE_BYTES);
		return (a && a->magic == SSB_AUDIO_MAGIC && a->version == SSB_AUDIO_VERSION) ? a : nullptr;
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

		ssb_audio* a = audio();
		if (!a) return lastResult = false;
		const uint32_t hb = a->heartbeat;
		if (hb != lastHb || lastHbTick == 0) { lastHb = hb; lastHbTick = now; }
		const bool res = (now - lastHbTick) < 2000;
		static bool logged = false;
		if (res && !logged) { logged = true; SsbBridge::log("audio: using Linux desktop audio"); }
		return lastResult = res;
	}

	void read(std::vector<int16_t>& out)
	{
		static uint64_t readPos = 0;
		static bool attached = false;

		ssb_audio* a = audio();
		if (!a) return;

		const uint64_t write = a->write_pos;
		const uint64_t maxLag = SSB_AUDIO_RATE / 5; // 200 ms
		if (!attached || write < readPos || write - readPos > SSB_AUDIO_RING_FRAMES - 1024) {
			readPos = write > 480 ? write - 480 : 0; // start ~10 ms behind
			attached = true;
		} else if (write - readPos > maxLag) {
			readPos = write - maxLag / 2;
		}

		const int16_t* data = reinterpret_cast<const int16_t*>(reinterpret_cast<const uint8_t*>(a) + SSB_AUDIO_HDR_BYTES);
		while (readPos < write) {
			const size_t idx = (size_t)(readPos % SSB_AUDIO_RING_FRAMES) * 2;
			out.push_back(data[idx]);
			out.push_back(data[idx + 1]);
			readPos++;
		}
	}
}
