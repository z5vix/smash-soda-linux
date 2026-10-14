#pragma once
// Desktop audio published by ssb-capture (monitor of the Linux default output).
#include <cstdint>
#include <vector>

namespace SsbAudio {
	// True when the Linux audio helper is running.
	bool active();
	// Appends all interleaved 48 kHz stereo samples published since the last call.
	// Drops old audio when the reader fell far behind to keep latency low.
	void read(std::vector<int16_t>& out);
}
