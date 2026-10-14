#include "SsbBridge.h"

#include <string>
#include <mutex>
#include <map>
#include <cstdlib>
#include <cstdarg>
#include <cctype>
#include <cstring>
#include <cstdio>

namespace SsbBridge {

	static std::string bridgeDir()
	{
		// Launcher exports the unix path of $XDG_RUNTIME_DIR/smashsoda
		const char* dir = getenv("SSB_BRIDGE_DIR");
		if (!dir || !*dir) return std::string();
		std::string d(dir);
		// Unix path -> Wine's Z: drive
		if (d.size() > 0 && d[0] == '/') d = "Z:" + d;
		for (auto& c : d) if (c == '/') c = '\\';
		return d;
	}

	void* map(const char* fileName, size_t size)
	{
		static std::mutex mtx;
		static std::map<std::string, void*> views;
		std::lock_guard<std::mutex> lock(mtx);

		auto it = views.find(fileName);
		if (it != views.end()) return it->second;

		std::string dir = bridgeDir();
		if (dir.empty()) return nullptr;
		std::string path = dir + "\\" + fileName;

		HANDLE file = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return nullptr;

		LARGE_INTEGER sz{};
		if (!GetFileSizeEx(file, &sz) || (size_t)sz.QuadPart < size) { CloseHandle(file); return nullptr; }

		HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
		CloseHandle(file);
		if (!mapping) return nullptr;
		void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
		CloseHandle(mapping);
		if (view) views[fileName] = view;
		return view;
	}

	int tuningInt(const char* key, int defaultValue)
	{
		static std::mutex mtx;
		static std::map<std::string, int> values;
		static DWORD lastRead = 0;
		std::lock_guard<std::mutex> lock(mtx);
		const DWORD now = GetTickCount();
		if (lastRead == 0 || now - lastRead >= 1000) {
			lastRead = now;
			values.clear();
			std::string dir = bridgeDir();
			if (!dir.empty()) {
				HANDLE f = CreateFileA((dir + "\\tuning.txt").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (f != INVALID_HANDLE_VALUE) {
					char buf[2048]; DWORD n = 0;
					if (ReadFile(f, buf, sizeof buf - 1, &n, nullptr)) {
						buf[n] = 0;
						for (char* line = strtok(buf, "\r\n"); line; line = strtok(nullptr, "\r\n")) {
							char* eq = strchr(line, '=');
							if (eq && line[0] != '#') { *eq = 0; values[line] = atoi(eq + 1); }
						}
					}
					CloseHandle(f);
				}
			}
		}
		auto it = values.find(key);
		return it == values.end() ? defaultValue : it->second;
	}

	void logFile(const char* fileName, const char* text)
	{
		static std::mutex mtx;
		std::string dir = bridgeDir();
		if (dir.empty() || !text) return;

		// mask a.b.c.d so guest addresses never end up in a file that gets pasted around
		// (Parsec's own STUN/relay server lines are not guest addresses and are kept: they are what you need to debug connections.
		// tuning.txt: parsec_log_raw=1 turns the masking off entirely.)
		std::string out;
		const size_t n = strlen(text);
		const bool keep = strncmp(text, "stun", 4) == 0 || tuningInt("parsec_log_raw", 0) != 0;
		for (size_t i = 0; i < n;) {
			if (keep) { out.assign(text, n); break; }
			size_t j = i, dots = 0, digits = 0;
			while (j < n && (isdigit((unsigned char)text[j]) || (text[j] == '.' && digits > 0))) { if (text[j] == '.') { dots++; digits = 0; } else digits++; j++; }
			if (dots == 3 && digits > 0) { out += "x.x.x.x"; i = j; }
			else { out += text[i]; i++; }
		}

		SYSTEMTIME t; GetLocalTime(&t);
		char stamp[32]; snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		std::string line = std::string(stamp) + out + "\n";

		std::lock_guard<std::mutex> lock(mtx);
		HANDLE f = CreateFileA((dir + "\\" + fileName).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f == INVALID_HANDLE_VALUE) return;
		DWORD written;
		WriteFile(f, line.data(), (DWORD)line.size(), &written, nullptr);
		CloseHandle(f);
	}

	void log(const char* fmt, ...)
	{
		static std::mutex mtx;
		std::string dir = bridgeDir();
		if (dir.empty()) return;

		char line[512];
		SYSTEMTIME t;
		GetLocalTime(&t);
		int n = snprintf(line, sizeof line, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
		va_list ap;
		va_start(ap, fmt);
		n += vsnprintf(line + n, sizeof line - n - 2, fmt, ap);
		va_end(ap);
		line[n++] = '\n';

		std::lock_guard<std::mutex> lock(mtx);
		HANDLE f = CreateFileA((dir + "\\smashsoda-wine.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f == INVALID_HANDLE_VALUE) return;
		DWORD written;
		WriteFile(f, line, (DWORD)n, &written, nullptr);
		CloseHandle(f);
	}

	bool alive(volatile const uint32_t* hb, DWORD ms)
	{
		uint32_t a = *hb;
		DWORD start = GetTickCount();
		while (GetTickCount() - start < ms) {
			Sleep(10);
			if (*hb != a) return true;
		}
		return *hb != a;
	}

	ssb_pads* pads()
	{
		auto* p = (ssb_pads*)map(SSB_PADS_FILE, sizeof(ssb_pads));
		return (p && p->magic == SSB_PADS_MAGIC && p->version == SSB_PADS_VERSION) ? p : nullptr;
	}

	ssb_input* input()
	{
		auto* p = (ssb_input*)map(SSB_INPUT_FILE, sizeof(ssb_input));
		return (p && p->magic == SSB_INPUT_MAGIC && p->version == SSB_INPUT_VERSION) ? p : nullptr;
	}

	ssb_video* video()
	{
		auto* p = (ssb_video*)map(SSB_VIDEO_FILE, (size_t)SSB_VIDEO_FILE_BYTES);
		return (p && p->magic == SSB_VIDEO_MAGIC && p->version == SSB_VIDEO_VERSION) ? p : nullptr;
	}

	bool linuxMode()
	{
		static const bool mode = [] { const char* d = getenv("SSB_BRIDGE_DIR"); return d && *d; }();
		return mode;
	}

	int displayRefreshHz()
	{
		ssb_video* v = video();
		return v ? (int)((v->refresh_mhz + 500) / 1000) : 0;
	}

	void setWantedResolution(int w, int h)
	{
		ssb_video* v = video();
		if (!v) return;
		v->want_w = w > 0 ? (uint32_t)w : 0;
		v->want_h = h > 0 ? (uint32_t)h : 0;
	}

	ssb_apps* apps()
	{
		auto* a = (ssb_apps*)map(SSB_APPS_FILE, sizeof(ssb_apps));
		return (a && a->magic == SSB_APPS_MAGIC && a->version == SSB_APPS_VERSION) ? a : nullptr;
	}

	bool appsAlive()
	{
		static uint32_t lastHb = 0; static DWORD lastTick = 0, lastCheck = 0; static bool res = false;
		const DWORD now = GetTickCount();
		if (lastCheck != 0 && now - lastCheck < 250) return res;
		lastCheck = now;
		ssb_apps* a = apps();
		if (!a) return res = false;
		const uint32_t hb = a->heartbeat;
		if (hb != lastHb || lastTick == 0) { lastHb = hb; lastTick = now; }
		return res = (now - lastTick) < 3000;
	}

	bool appsCommand(uint32_t type, uint32_t id, int32_t value)
	{
		ssb_apps* a = apps();
		if (!a || a->cmd_seq != a->cmd_ack) return false;
		a->cmd_type = type; a->cmd_id = id; a->cmd_value = value;
		MemoryBarrier();
		a->cmd_seq = a->cmd_seq + 1;   // last: the helper acts when this changes
		return true;
	}

	void requestChangeScreen()
	{
		ssb_video* v = video();
		if (!v) return;
		v->change_request = v->change_request + 1;
		log("video: asked the Linux helper to switch to another screen/window (a picker should appear)");
	}

	void setBitrateBudget(unsigned kbps)
	{
		ssb_video* v = video();
		if (v) v->bitrate_total_kbps = kbps;
	}

	void setGuestCount(unsigned guests)
	{
		ssb_video* v = video();
		if (v) v->guest_count = guests;
	}

	static std::string whereIs(void* addr)
	{
		HMODULE m = nullptr;
		char name[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m) {
			GetModuleFileNameA(m, name, MAX_PATH);
			const char* base = strrchr(name, '\\');
			char out[400];
			snprintf(out, sizeof out, "%s+0x%llx", base ? base + 1 : name, (unsigned long long)((uintptr_t)addr - (uintptr_t)m));
			return out;
		}
		char out[64]; snprintf(out, sizeof out, "%p", addr);
		return out;
	}

	static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep)
	{
		const EXCEPTION_RECORD* r = ep->ExceptionRecord;
		log("CRASH: exception 0x%08lx at %s, thread %lu%s", r->ExceptionCode, whereIs(r->ExceptionAddress).c_str(), GetCurrentThreadId(),
			(r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
				? (std::string(r->ExceptionInformation[0] ? " (write to " : " (read of ") + std::to_string(r->ExceptionInformation[1]) + ")").c_str() : "");
		void* frames[24];
		const USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
		for (USHORT i = 0; i < n; i++) log("CRASH:   frame %u: %s", (unsigned)i, whereIs(frames[i]).c_str());
		return EXCEPTION_CONTINUE_SEARCH;
	}

	void installCrashLogger()
	{
		SetUnhandledExceptionFilter(crashFilter);
	}

	void setCaptureOptions(unsigned captureMode, unsigned sourceTypes, bool remember)
	{
		ssb_video* v = video();
		if (!v) return;
		v->capture_mode = captureMode;
		v->source_types = sourceTypes;
		v->remember = remember ? 1 : 0;
	}

	void setCaptureWanted(bool wanted)
	{
		ssb_video* v = video();
		if (!v) return;
		if (wanted && !v->want_capture) log("video: asking the Linux helper to share the screen (a picker should appear)");
		v->want_capture = wanted ? 1 : 0;
	}

	bool pushInput(uint32_t type, int32_t a, int32_t b)
	{
		static std::mutex mtx;
		ssb_input* in = input();
		if (!in) return false;
		std::lock_guard<std::mutex> lock(mtx);
		uint32_t head = in->head;
		if (head - in->tail >= SSB_INPUT_RING) return false; // ring full, drop
		in->ring[head % SSB_INPUT_RING] = { type, a, b, 0 };
		MemoryBarrier();
		in->head = head + 1;
		return true;
	}
}
