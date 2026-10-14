#include "AudioProcessLoopback.h"
#include "helpers/WStringConvert.h"

#include <audioclientactivationparams.h>
#include <Audiopolicy.h>
#include <objidl.h>
#include <TlHelp32.h>
#include <atomic>
#include <set>
#include <unordered_map>
#include <thread>

namespace {

	std::wstring Utf8ToWide(const std::string& str)
	{
		if (str.empty()) return std::wstring();
		int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
		std::wstring wstrTo(sizeNeeded, 0);
		MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstrTo[0], sizeNeeded);
		return wstrTo;
	}

	std::string GetProcessExeName(DWORD pid)
	{
		std::string name;
		HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (hProcess != NULL) {
			wchar_t buffer[MAX_PATH];
			DWORD size = MAX_PATH;
			if (QueryFullProcessImageNameW(hProcess, 0, buffer, &size)) {
				std::wstring fullPath(buffer, size);
				size_t slash = fullPath.find_last_of(L"\\/");
				std::wstring fileName = (slash == std::wstring::npos) ? fullPath : fullPath.substr(slash + 1);
				name = WStringToString(fileName);
			}
			CloseHandle(hProcess);
		}
		return name;
	}

	// Blocking wrapper around the (inherently async) ActivateAudioInterfaceAsync call.
	// Waiting synchronously on the completion event is the pattern used by Microsoft's own
	// "ApplicationLoopback" sample; the completion callback fires on a system thread pool
	// thread regardless of the calling thread's message loop, so this is safe off the UI thread.
	//
	// Must also implement IAgileObject (a no-method marker interface): per Microsoft's docs for
	// ActivateAudioInterfaceAsync, the completion handler "needs to implement IAgileObject...
	// Otherwise, an E_ILLEGAL_METHOD_CALL will occur" (0x8000000E) - without it, COM can't safely
	// hand this object across the apartment boundary to the thread that invokes ActivateCompleted,
	// and activation fails synchronously before it even goes async.
	class LoopbackActivateHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
	{
	public:
		LoopbackActivateHandler()
		{
			m_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		}

		~LoopbackActivateHandler()
		{
			if (m_audioClient) m_audioClient->Release();
			if (m_event) CloseHandle(m_event);
		}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
		{
			if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
				*ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
				AddRef();
				return S_OK;
			}
			if (riid == __uuidof(IAgileObject)) {
				*ppv = static_cast<IAgileObject*>(this);
				AddRef();
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}

		ULONG STDMETHODCALLTYPE AddRef() override
		{
			return static_cast<ULONG>(++m_refCount);
		}

		ULONG STDMETHODCALLTYPE Release() override
		{
			LONG result = --m_refCount;
			if (result == 0) delete this;
			return static_cast<ULONG>(result);
		}

		HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override
		{
			HRESULT hrActivate = E_FAIL;
			IUnknown* pUnknown = nullptr;

			operation->GetActivateResult(&hrActivate, &pUnknown);

			m_activateResult = hrActivate;
			if (SUCCEEDED(hrActivate) && pUnknown != nullptr) {
				pUnknown->QueryInterface(__uuidof(IAudioClient), (void**)&m_audioClient);
			}
			if (pUnknown != nullptr) pUnknown->Release();

			SetEvent(m_event);
			return S_OK;
		}

		HANDLE m_event = nullptr;
		std::atomic<LONG> m_refCount{ 1 };
		HRESULT m_activateResult = E_FAIL;
		IAudioClient* m_audioClient = nullptr;
	};

}

std::vector<AudioSessionApp> AudioProcessLoopback::ListAudioSessionApps()
{
	std::vector<AudioSessionApp> result;

	IMMDeviceEnumerator* pEnumerator = nullptr;
	IMMDevice* pDevice = nullptr;
	IAudioSessionManager2* pSessionManager = nullptr;
	IAudioSessionEnumerator* pSessionEnum = nullptr;

	CoInitializeEx(NULL, COINIT_MULTITHREADED);

	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
	if (SUCCEEDED(hr)) hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
	if (SUCCEEDED(hr)) hr = pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&pSessionManager);
	if (SUCCEEDED(hr)) hr = pSessionManager->GetSessionEnumerator(&pSessionEnum);

	if (SUCCEEDED(hr) && pSessionEnum != nullptr) {
		int count = 0;
		pSessionEnum->GetCount(&count);

		const DWORD selfPid = GetCurrentProcessId();
		std::set<DWORD> seenPids;

		for (int i = 0; i < count; i++) {
			IAudioSessionControl* pControl = nullptr;
			if (FAILED(pSessionEnum->GetSession(i, &pControl)) || pControl == nullptr) continue;

			IAudioSessionControl2* pControl2 = nullptr;
			if (SUCCEEDED(pControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pControl2)) && pControl2 != nullptr) {
				DWORD pid = 0;
				if (SUCCEEDED(pControl2->GetProcessId(&pid)) && pid != 0 && pid != selfPid &&
					pControl2->IsSystemSoundsSession() != S_OK &&
					seenPids.insert(pid).second) {

					std::string exeName = GetProcessExeName(pid);
					if (!exeName.empty()) {
						AudioSessionApp app;
						app.pid = pid;
						app.exeName = exeName;
						result.push_back(app);
					}
				}
				pControl2->Release();
			}
			pControl->Release();
		}
	}

	if (pSessionEnum) pSessionEnum->Release();
	if (pSessionManager) pSessionManager->Release();
	if (pDevice) pDevice->Release();
	if (pEnumerator) pEnumerator->Release();

	return result;
}

DWORD AudioProcessLoopback::FindRunningProcessIdByName(const std::string& exeName)
{
	if (exeName.empty()) return 0;
	std::wstring wideExeName = Utf8ToWide(exeName);

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return 0;

	// Multi-process apps (Discord, browsers, etc.) run several instances of the same
	// exe as a parent + children (renderer/GPU/utility processes). "Exclude process tree"
	// only excludes the target PID and its descendants, so excluding a non-root child
	// would miss sibling processes that could be the ones actually rendering audio.
	// Collect every matching process plus a full pid->parent map so we can walk up to
	// the top-most matching ancestor instead of just taking whichever one the OS listed first.
	std::vector<PROCESSENTRY32W> matches;
	std::unordered_map<DWORD, DWORD> parentOf;

	PROCESSENTRY32W entry;
	entry.dwSize = sizeof(entry);
	if (Process32FirstW(snapshot, &entry)) {
		do {
			parentOf[entry.th32ProcessID] = entry.th32ParentProcessID;
			if (_wcsicmp(entry.szExeFile, wideExeName.c_str()) == 0) {
				matches.push_back(entry);
			}
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);

	if (matches.empty()) return 0;

	std::set<DWORD> matchPids;
	for (const PROCESSENTRY32W& m : matches) matchPids.insert(m.th32ProcessID);

	for (const PROCESSENTRY32W& m : matches) {
		auto it = parentOf.find(m.th32ProcessID);
		DWORD parentPid = (it != parentOf.end()) ? it->second : 0;
		// Root: its parent isn't itself one of the matching processes.
		if (matchPids.find(parentPid) == matchPids.end()) {
			return m.th32ProcessID;
		}
	}

	// Every match had a matching parent (shouldn't happen in practice) - fall back to the first.
	return matches.front().th32ProcessID;
}

namespace {

	// The actual activation logic, run on a thread we know is freshly spun up (see below
	// for why that matters). Returns the activated client via outClient.
	HRESULT DoActivateExcludeProcessClient(DWORD excludeProcessId, IAudioClient** outClient)
	{
		AUDIOCLIENT_ACTIVATION_PARAMS activationParams = {};
		activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
		activationParams.ProcessLoopbackParams.TargetProcessId = excludeProcessId;
		activationParams.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

		PROPVARIANT propvariant;
		PropVariantInit(&propvariant);
		propvariant.vt = VT_BLOB;
		propvariant.blob.cbSize = sizeof(activationParams);
		propvariant.blob.pBlobData = reinterpret_cast<BYTE*>(&activationParams);

		LoopbackActivateHandler* handler = new LoopbackActivateHandler();
		IActivateAudioInterfaceAsyncOperation* asyncOp = nullptr;

		HRESULT hr = ActivateAudioInterfaceAsync(
			VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
			__uuidof(IAudioClient),
			&propvariant,
			handler,
			&asyncOp);

		if (FAILED(hr)) {
			handler->Release();
			return hr;
		}

		WaitForSingleObject(handler->m_event, INFINITE);
		hr = handler->m_activateResult;

		if (SUCCEEDED(hr) && handler->m_audioClient != nullptr) {
			*outClient = handler->m_audioClient;
			handler->m_audioClient = nullptr; // ownership transferred to caller
		}
		else if (SUCCEEDED(hr)) {
			hr = E_FAIL; // activation reported success but gave us no client
		}

		if (asyncOp) asyncOp->Release();
		handler->Release();

		return hr;
	}

}

HRESULT AudioProcessLoopback::ActivateExcludeProcessClient(DWORD excludeProcessId, IAudioClient** ppAudioClient)
{
	*ppAudioClient = nullptr;
	if (excludeProcessId == 0) return E_INVALIDARG;

	// Runs on a dedicated, freshly-spawned thread rather than whatever thread called us (the UI
	// thread, or one of Hosting's worker threads). Those threads may already have been initialized
	// single-threaded (STA) by something else (SDL/DirectX/etc.) before we ever get here, in which
	// case CoInitializeEx here can't actually switch apartments - a brand-new thread has no such
	// history, so this keeps activation's behavior independent of caller context.
	HRESULT activateHr = E_FAIL;
	IAudioClient* resultClient = nullptr;

	std::thread worker([excludeProcessId, &activateHr, &resultClient]() {
		HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

		activateHr = DoActivateExcludeProcessClient(excludeProcessId, &resultClient);

		if (coHr == S_OK || coHr == S_FALSE) {
			CoUninitialize();
		}
	});
	worker.join();

	*ppAudioClient = resultClient;
	return activateHr;
}
