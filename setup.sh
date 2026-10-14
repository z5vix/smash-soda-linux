#!/usr/bin/env bash
# Smash Soda for Linux - builds everything from source and installs it. One command:
#
#   ./setup.sh
#
# What it does:
#   1. checks (and, with your OK, installs) the build tools
#   2. downloads the pinned Smash Soda source from GitHub - this is also where Parsec's SDK (parsec.dll) comes from:
#      its licence forbids distributing binaries, so this project never ships them - you build it yourself
#   3. applies the Linux patch, cross-compiles SmashSoda.exe + the helpers with MinGW-w64
#   4. installs to ~/.local/share/smashsoda-linux and adds `smashsoda` to ~/.local/bin and the application menu
#
# Options: --yes (answer yes to questions)   --no-deps (do not touch system packages)   --clean (start from scratch)
#          --uninstall   -h
set -euo pipefail

UPSTREAM_URL="https://github.com/trybuchet/smash-soda.git"
UPSTREAM_COMMIT="c85043fcceece192a7955edd0d46a1793a687d35"

ROOT="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
cd "$ROOT"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/smashsoda-linux"
APP="$DATA/app"
BUILD="$ROOT/build"
CACHE="$ROOT/.cache/upstream"

YES=0; DEPS=1; CLEAN=0
for a in "$@"; do
	case "$a" in
		--yes|-y) YES=1 ;;
		--no-deps) DEPS=0 ;;
		--clean) CLEAN=1 ;;
		--uninstall)
			rm -rf "$APP" "$HOME/.local/bin/smashsoda" "$HOME/.local/share/applications/smashsoda.desktop"
			echo "removed $APP and the launcher entries."
			echo "Your Wine prefix and settings stay in $DATA (delete that folder to remove them too)."
			exit 0 ;;
		-h|--help) sed -n 2,16p "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option: $a (try -h)" >&2; exit 2 ;;
	esac
done

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
ask()  { [[ $YES == 1 ]] && return 0; local r; read -r -p "$1 [Y/n] " r || return 1; [[ -z "$r" || "$r" =~ ^[Yy] ]]; }

[[ $CLEAN == 1 ]] && { step "Cleaning previous build"; rm -rf "$BUILD" "$ROOT/.cache"; }

# ------------------------------------------------------------------------------------------------ 1. build tools
step "Checking build tools"
. /etc/os-release 2>/dev/null || true
FAMILY="${ID:-unknown} ${ID_LIKE:-}"
case "$FAMILY" in
	*arch*|*cachyos*|*manjaro*|*endeavouros*) PM=pacman
		BUILD_PKGS="mingw-w64-gcc cmake ninja gcc git patch python"
		RUN_PKGS="umu-launcher python-gobject gst-plugin-pipewire gst-plugins-base gst-plugins-bad pipewire-pulse xdg-desktop-portal"
		INSTALL="sudo pacman -S --needed" ;;
	*debian*|*ubuntu*|*mint*|*pop*) PM=apt
		BUILD_PKGS="g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 cmake ninja-build build-essential git patch python3"
		RUN_PKGS="python3-gi gir1.2-gst-plugins-base-1.0 gstreamer1.0-pipewire gstreamer1.0-plugins-bad pipewire-pulse pulseaudio-utils xdg-desktop-portal (umu-launcher: https://github.com/Open-Wine-Components/umu-launcher)"
		INSTALL="sudo apt install" ;;
	*fedora*|*rhel*|*centos*) PM=dnf
		BUILD_PKGS="mingw64-gcc-c++ mingw64-gcc mingw64-binutils mingw64-winpthreads-static cmake ninja-build gcc git patch python3"
		RUN_PKGS="umu-launcher python3-gobject gstreamer1-plugin-pipewire gstreamer1-plugins-bad-free pipewire-pulseaudio xdg-desktop-portal"
		INSTALL="sudo dnf install" ;;
	*) PM=""; BUILD_PKGS="MinGW-w64 (x86_64, posix threads) cmake ninja gcc git patch python3"; RUN_PKGS=""; INSTALL="" ;;
esac

missing=()
need() { command -v "$1" >/dev/null 2>&1 || missing+=("$1"); }
need x86_64-w64-mingw32-g++; need x86_64-w64-mingw32-windres; need x86_64-w64-mingw32-strip
need cmake; need gcc; need git; need patch; need python3
command -v ninja >/dev/null 2>&1 || command -v make >/dev/null 2>&1 || missing+=("ninja or make")

if (( ${#missing[@]} )); then
	echo "Missing: ${missing[*]}"
	if [[ $DEPS == 1 && -n "$PM" ]]; then
		echo "Packages needed: $BUILD_PKGS"
		if ask "Install them with: $INSTALL ...?"; then $INSTALL $BUILD_PKGS || die "package installation failed"; else die "install the build tools and run again"; fi
	else
		die "install the build tools ($BUILD_PKGS) and run again"
	fi
fi
# The code uses std::thread/std::mutex: it needs the POSIX thread model (Debian ships it as a separate '-posix' compiler).
CXX=x86_64-w64-mingw32-g++
if command -v x86_64-w64-mingw32-g++-posix >/dev/null 2>&1; then CXX=x86_64-w64-mingw32-g++-posix; CC=x86_64-w64-mingw32-gcc-posix; else CC=x86_64-w64-mingw32-gcc; fi
[[ "$($CXX -v 2>&1)" == *"Thread model: posix"* ]] || die "$CXX does not use the POSIX thread model (install the '-posix' MinGW-w64 variant)"
echo "ok: $($CXX --version | head -1)"

# ------------------------------------------------------------------------------------------------ 2. source
step "Fetching Smash Soda source (pinned to ${UPSTREAM_COMMIT:0:7})"
mkdir -p "$CACHE"
[[ -d "$CACHE/.git" ]] || { git init -q "$CACHE"; git -C "$CACHE" remote add origin "$UPSTREAM_URL"; }
if ! git -C "$CACHE" cat-file -e "${UPSTREAM_COMMIT}^{commit}" 2>/dev/null; then
	git -C "$CACHE" fetch -q --depth 1 origin "$UPSTREAM_COMMIT" || git -C "$CACHE" fetch -q origin || die "could not download $UPSTREAM_URL"
fi
git -C "$CACHE" -c core.autocrlf=false -c advice.detachedHead=false checkout -q -f "$UPSTREAM_COMMIT"
[[ -f "$CACHE/Dependencies/parsecsdk/windows/parsec.dll" ]] || die "the downloaded source has no Parsec SDK (parsec.dll) - the upstream layout changed"

step "Applying the Linux patch"
STAMP="$(cat patches/linux-port.patch bridge/ssb_proto.h $(find overlay -type f | sort) | sha256sum | cut -c1-16)-${UPSTREAM_COMMIT:0:7}"
if [[ "$(cat "$BUILD/.stamp" 2>/dev/null)" != "$STAMP" ]]; then
	rm -rf "$BUILD/src"; mkdir -p "$BUILD/src"
	cp -a "$CACHE/." "$BUILD/src/"; rm -rf "$BUILD/src/.git"
	patch -p1 -s --binary --no-backup-if-mismatch -d "$BUILD/src" < patches/linux-port.patch || die "the Linux patch does not apply to this source"
	cp -a overlay/. "$BUILD/src/"
	cp bridge/ssb_proto.h "$BUILD/src/SmashSoda/linux/ssb_proto.h"
	echo "$STAMP" > "$BUILD/.stamp"
else
	echo "already staged"
fi

# ------------------------------------------------------------------------------------------------ 3. build
step "Building SmashSoda.exe (this takes a few minutes the first time)"
GEN=Ninja; command -v ninja >/dev/null 2>&1 || GEN="Unix Makefiles"
cmake -S "$ROOT" -B "$BUILD/cmake" -G "$GEN" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64-x86_64.cmake" \
	-DCMAKE_BUILD_TYPE=Release -DSSB_SRC="$BUILD/src" >/dev/null
python3 gen-include-shim.py "$BUILD/src" | tail -1
cmake --build "$BUILD/cmake" -j"$(nproc)"

step "Building the helpers"
rm -f bridge/ssb-padd   # a running copy keeps its old file
gcc -O2 -Wall -Wextra -o bridge/ssb-padd bridge/ssb-padd.c
$CXX -std=gnu++20 -O2 -shared -static -w nvenc-proxy/nvenc_proxy.cpp -o "$BUILD/nvEncodeAPI64.dll" -ld3d11 -ldxgi -luuid
$CXX -std=gnu++20 -O2 -static -w bridge/ipc_probe.cpp -o bridge/ipc_probe.exe
echo "ok"

# ------------------------------------------------------------------------------------------------ 4. install
step "Installing to $APP"
SRC="$BUILD/src"
mkdir -p "$APP" "$HOME/.local/bin" "$HOME/.local/share/applications" "$APP/overlay/plugins" "$APP/overlay/themes"
for d in fonts icons sfx; do cp -a "$SRC/SmashSoda/$d/." "$APP/$d/" 2>/dev/null || { mkdir -p "$APP/$d"; cp -a "$SRC/SmashSoda/$d/." "$APP/$d/"; }; done
[[ -f "$APP/imgui.ini" ]] || cp "$SRC/imgui.ini" "$APP/imgui.ini" 2>/dev/null || true
[[ -f "$APP/icons/icon.ico" ]] || cp "$SRC/SmashSoda/icon.ico" "$APP/icons/icon.ico" 2>/dev/null || true
# Write-then-rename keeps an already running instance intact (it keeps the old file open).
put() { cp "$1" "$2.new" && mv -f "$2.new" "$2"; }
x86_64-w64-mingw32-strip -o "$APP/SmashSoda.exe.new" "$BUILD/cmake/SmashSoda.exe" && mv -f "$APP/SmashSoda.exe.new" "$APP/SmashSoda.exe"
put "$SRC/Dependencies/parsecsdk/windows/parsec.dll" "$APP/parsec.dll"
put "$SRC/Dependencies/sdl/lib/x64/SDL2.dll" "$APP/SDL2.dll"
# parsec.dll -> NVENC proxy (nvEncodeAPI64.dll) -> legacy-preset shim (nvEncodeAPI64_inner.dll) -> Proton's NVENC
put "$SRC/Dependencies/nvenc_shim/nvEncodeAPI64.dll" "$APP/nvEncodeAPI64_inner.dll"
put "$BUILD/nvEncodeAPI64.dll" "$APP/nvEncodeAPI64.dll"

ln -sf "$ROOT/smashsoda" "$HOME/.local/bin/smashsoda"
chmod +x "$ROOT/smashsoda"
cat > "$HOME/.local/share/applications/smashsoda.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Smash Soda
Comment=Host Parsec rooms (Linux build via Proton)
Exec=$ROOT/smashsoda
Icon=$APP/icons/icon.ico
Terminal=false
Categories=Game;
DESK

# ------------------------------------------------------------------------------------------------ runtime checks
step "Checking what is needed to run it"
todo=0
chk() { if ! eval "$2" >/dev/null 2>&1; then warn "$1"; todo=1; fi; }
chk "umu-run (umu-launcher) is missing - it runs the app under Proton"      "command -v umu-run"
chk "pactl/parec are missing (PipeWire's PulseAudio tools) - needed for audio" "command -v pactl && command -v parec"
chk "python GObject / GStreamer bindings are missing - needed for screen capture" \
	"python3 -c \"import gi; gi.require_version('Gst','1.0'); gi.require_version('GstApp','1.0'); from gi.repository import Gst\""
chk "GStreamer's pipewiresrc plugin is missing - needed for screen capture" "gst-inspect-1.0 pipewiresrc"
chk "/dev/uinput is not writable - virtual gamepads and keyboard/mouse from guests will not work (see README)" "test -w /dev/uinput"
chk "no NVIDIA driver found (libcuda) - hardware encoding needs an NVIDIA GPU" "(ldconfig -p || /sbin/ldconfig -p || /usr/sbin/ldconfig -p) 2>/dev/null | grep libcuda.so.1"
if (( todo )) && [[ -n "$RUN_PKGS" ]]; then echo "  packages that provide these: $RUN_PKGS"; fi

printf '\n\033[1mDone.\033[0m  Start it with:  smashsoda   (or from your application menu)\n'
echo "First run: umu downloads GE-Proton if none is installed, then sign in with your Soda Arcade/Parsec account."
