# Cross-compiles the Windows build of Smash Soda with MinGW-w64 so it can run
# under Proton/Wine. The Windows-only pieces (ViGEm driver, WinRT capture) are
# replaced by the Linux bridge in src/SmashSoda/linux.
cmake_minimum_required(VERSION 3.18)
project(SmashSodaLinux LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# SSB_SRC: the patched Smash Soda tree (setup.sh stages it in build/src).
set(SSB_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src CACHE PATH "patched Smash Soda source tree")
set(SRC ${SSB_SRC})
set(APP ${SRC}/SmashSoda)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS ${APP}/*.cpp ${APP}/linux/*.c)
list(FILTER SOURCES EXCLUDE REGEX "/imgui/(imgui_demo|implot_demo)\\.cpp$")

add_executable(SmashSoda WIN32 ${SOURCES} ${APP}/Resource.rc)

target_include_directories(SmashSoda PRIVATE
	${APP} ${APP}/chatbot ${APP}/services ${APP}/core ${APP}/fonts ${APP}/globals ${APP}/helpers
	${APP}/icons ${APP}/imgui ${APP}/lists ${APP}/models ${APP}/sfx ${APP}/structs ${APP}/widgets
	${APP}/linux
	${SRC}/include-shim
	${SRC}/external
	${SRC}/Dependencies/parsecsdk
	${SRC}/Dependencies/matoya
	${SRC}/Dependencies/vigem/include
	${SRC}/Dependencies/sdl/include
)

target_compile_definitions(SmashSoda PRIVATE
	SSB_LINUX ASIO_STANDALONE _WEBSOCKETPP_CPP11_STL_ _WEBSOCKETPP_CPP11_THREAD_ _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN=0
	_WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000007
)

target_precompile_headers(SmashSoda PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${APP}/pch.h>)
target_compile_options(SmashSoda PRIVATE -Wno-all -Wno-extra -Wreturn-type -fpermissive -O2 -Wno-attributes $<$<COMPILE_LANGUAGE:CXX>:-include ${APP}/linux/mingw_compat.h>)
target_link_options(SmashSoda PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--allow-multiple-definition)

# SDL2 is linked straight against the shipped DLL (GNU ld can link DLLs directly).
set(SDL2_DLL ${SRC}/Dependencies/sdl/lib/x64/SDL2.dll)

target_link_libraries(SmashSoda PRIVATE
	${SDL2_DLL}
	${SRC}/Dependencies/matoya/64/matoya.lib
	winhttp wininet crypt32 secur32 gdi32 winmm imm32 shell32 shlwapi advapi32 ole32 oleaut32
	user32 uuid version setupapi hid dxgi ws2_32 bcrypt windowscodecs xinput d3d9 d3d11 d3dcompiler
	dwmapi psapi opengl32 mmdevapi comdlg32 mswsock ksuser
)
