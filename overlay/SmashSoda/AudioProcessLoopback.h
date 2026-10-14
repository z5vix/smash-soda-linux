#pragma once

#include <Windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <string>
#include <vector>

typedef struct AudioSessionApp
{
	DWORD pid = 0;
	std::string exeName;	// e.g. "Discord.exe"
} AudioSessionApp;

/**
* Wraps the Windows "process loopback" capture API (Windows 10 2004+ / Windows 11),
* which can loopback-capture everything playing on the default output device while
* excluding a single chosen process (and its child processes).
*
* Used to keep a voice chat app's own audio (e.g. Discord) out of what gets streamed
* to guests, so guests using that same voice chat don't hear an echo of themselves
* coming back a moment later through the stream.
*/
class AudioProcessLoopback
{
public:
	// Apps currently holding an audio session on the default output device (i.e. apps
	// that are, or recently were, making sound). Used to populate a picker.
	static std::vector<AudioSessionApp> ListAudioSessionApps();

	// Best-effort lookup by executable name (e.g. "Discord.exe"). Returns 0 if not running.
	static DWORD FindRunningProcessIdByName(const std::string& exeName);

	// Activates an IAudioClient that loopback-captures the default render device while
	// excluding the given process tree. The client is activated only; the caller still
	// needs to Initialize()/GetService()/Start() it, same as a normal IAudioClient.
	static HRESULT ActivateExcludeProcessClient(DWORD excludeProcessId, IAudioClient** ppAudioClient);
};
