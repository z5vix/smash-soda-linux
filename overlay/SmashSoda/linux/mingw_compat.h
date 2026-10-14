// Force-included for the MinGW build: small gaps between MSVC's and MinGW's headers.
#pragma once
#ifdef __MINGW32__
#ifdef __cplusplus
// Standard headers first, so std::byte is declared before Windows' `byte` is renamed below.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <span>
#include <bit>
#include <array>
#include <map>
#include <set>
#include <deque>
#include <list>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <optional>
#include <variant>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <random>
#include <regex>
#include <sstream>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <filesystem>
#include <locale>
#include <codecvt>
#include <numeric>
#include <cmath>
#include <cassert>
#include <ctime>
#include <cstdlib>
#endif

// Windows declares `typedef unsigned char byte;`, which collides with std::byte wherever the
// code says `using namespace std`. MSVC avoids this with _HAS_STD_BYTE=0; rename Windows' one.
#define byte win_byte_t
#define _WINSOCKAPI_
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <wininet.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <xinput.h>
#include <dwmapi.h>
#undef byte
#undef _WINSOCKAPI_

#ifndef _Function_class_
#define _Function_class_(x)
#endif
#ifndef _In_
#define _In_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#endif
