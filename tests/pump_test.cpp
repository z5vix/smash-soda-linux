// Runs the Windows-side audio pump exactly like Hosting does, but records what would be submitted.
#include <windows.h>
#include <cstdio>
#include <vector>
#include "SsbBridge.h"
#include "SsbAudio.h"

int main(int argc, char** argv)
{
	timeBeginPeriod(1);
	FILE* out = fopen(argc > 1 ? argv[1] : "pump.bin", "wb");
	LARGE_INTEGER f, t; QueryPerformanceFrequency(&f);
	std::vector<int16_t> buf; size_t head = 0; bool primed = false;
	const size_t chunk = 960;
	DWORD start = GetTickCount();
	while (GetTickCount() - start < 12000) {
		if (SsbAudio::active()) {
			SsbAudio::read(buf);
			if (!primed && buf.size() - head >= chunk * 3) primed = true;
			while (primed && buf.size() - head >= chunk * 2) {
				QueryPerformanceCounter(&t);
				double ms = t.QuadPart * 1000.0 / f.QuadPart;
				fwrite(&ms, sizeof ms, 1, out);
				fwrite(buf.data() + head, 2, chunk, out);
				head += chunk;
			}
			if (primed && buf.size() - head < chunk) primed = false;
			if (head > 48000) { buf.erase(buf.begin(), buf.begin() + head); head = 0; }
		}
		Sleep(2);
	}
	fclose(out);
	return 0;
}
