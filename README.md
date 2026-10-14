# smash-soda-linux
Parsec Smash Soda for Linux systems. because ~~nobody else'd~~ port this, and i waited simply far too long. AI was used. what you got an issue with that? don't use the app. simple... 

Tested on CachyOS. Built with Claude. 
Wait you're telling me there's BEEN a smash soda fork for Linux systems already?!?! well... this exists now i guess.. this isn't just a simple build, it adds entirely new things to make it more native than the other existing fork that exists.
Discord is 5vix if you want to tell me something. 
if you want to give anyone anything, let it be MickeyUK. go buy him a coffee.

---


Host [Parsec](https://parsec.app) rooms from your **Linux desktop** with [Smash Soda](https://github.com/soda-arcade/SmashSoda).
The Windows build runs under Proton (GE-Proton via `umu-launcher`); the Windows-only pieces are replaced by native Linux helpers,
so guests see and control your real desktop:

| Windows piece                | Replaced by                                                              |
|------------------------------|--------------------------------------------------------------------------|
| ViGEmBus virtual gamepads    | `bridge/ssb-padd` - real `/dev/uinput` controllers (+ rumble back)       |
| Desktop duplication / WGC    | `bridge/ssb-capture.py` - XDG ScreenCast portal -> PipeWire -> GPU frames |
| WASAPI loopback audio        | `bridge/ssb-capture.py` (`parec`) + `bridge/ssb-audioapps.py` per-app mixer |
| `SendInput` keyboard/mouse   | `ssb-padd` - virtual keyboard + pointer on the real desktop              |
| NVENC through D3D11          | `nvenc-proxy` - translates Parsec's D3D11 encoder session to CUDA (Proton's NVENC) |

## Install - one command

```bash
git clone <this repository> smash-soda-linux && cd smash-soda-linux
./setup.sh
```

`setup.sh` installs the build tools (asks first; Arch, Debian/Ubuntu and Fedora families), downloads the pinned Smash Soda
source from GitHub, applies the Linux patch, cross-compiles everything with MinGW-w64, and installs to
`~/.local/share/smashsoda-linux`. Afterwards run `smashsoda` (or use the application menu entry).
Re-running it updates the install; `./setup.sh --clean` starts from scratch, `--uninstall` removes it, `--yes` skips questions.

**Why you build it yourself.** Smash Soda needs Parsec's old SDK (`parsec.dll`). Its terms forbid distributing binaries that use the
personal API, so this repository contains no binaries and no Parsec SDK files: `setup.sh` fetches the upstream source (which includes the SDK
as part of its own tree) and builds on your machine, for your own use. Please keep it that way if you fork this. Smash Soda's own
[API usage licence](https://github.com/trybuchet/smash-soda/blob/master/API%20Usage%20License.md) also applies to its Soda Arcade API.

### What you need on the system
Build (installed for you): `mingw-w64` (x86_64, **posix threads**), `cmake`, `ninja` (or make), `gcc`, `git`, `patch`, `python3`.
Run (checked at the end of setup, not installed): 

* `umu-launcher` (`umu-run`) - downloads GE-Proton on the first run if you have none
* PipeWire with its PulseAudio layer (`pactl`, `parec`), `xdg-desktop-portal` + your desktop's backend (KDE / GNOME)
* GStreamer with the PipeWire plugin and Python bindings (`python-gobject`, `gst-plugin-pipewire`, `gst-plugins-bad`)
* an **NVIDIA GPU + driver** - hardware encoding goes through NVENC (Proton's `PROTON_NVIDIA_NVENC`)
* write access to `/dev/uinput` for virtual gamepads and guest input, e.g.
  `echo 'KERNEL=="uinput", MODE="0660", GROUP="input", OPTIONS+="static_node=uinput"' | sudo tee /etc/udev/rules.d/99-uinput.rules`,
  then add yourself to the `input` group and log in again (and `sudo modprobe uinput`).

## Run it

Open a terminal and type:

```bash
smashsoda
```

That's it - it starts the helpers and the app (also available as **Smash Soda** in your application menu). If the command isn't found, make sure
`~/.local/bin` is in your `PATH`, or run `./smashsoda` from this folder. Stop it by closing the window or pressing Ctrl+C in the terminal.

## Using it
Press **Host** in Smash Soda: a screen-share picker (listed as "Smash Soda") opens - choose a screen or window; stopping the room ends the share.

* **Hosting tab -> Change Screen**: pick another screen/window without stopping the room - nobody is kicked.
* **Audio tab -> Apps playing audio**: every app making sound with its own volume, *Mute* and *Share*. Share off keeps an app out of what
  guests hear while you still hear it. (While any app is unshared, the others are routed through a "share" sink; you hear it via a ~20 ms loopback.)
* **Video tab**: *Screen Capture* method (Automatic / GPU zero-copy / GPU download / Compatibility), *Share* (screen, window or either),
  *Remember my screen choice*, *Resolution*, *Bandwidth* and *FPS* (capped at 60 - Parsec's host never streams more). Capture settings apply the next time you press Host.
* **Bandwidth is the upload budget for the whole room.** Parsec sends a separate copy of the stream to every guest (upload = bitrate x guests) and asks
  for ~10 Mbps each, so the NVENC layer limits the encoder to budget / guests (never below 1 Mbps). Lowering it softens the picture for everyone but
  keeps latency down and the connection steady. Parsec has no per-guest bitrate control.

Environment overrides: `SSB_CAPTURE` (auto|gl|shm), `SSB_SOURCE` (monitor|window|any), `SSB_FPS`, `SSB_MAX_HEIGHT`, `SSB_REMEMBER_SCREEN=1`,
`SSB_GPU` (pin to a GPU; not recommended), `PROTONPATH`, `SMASHSODA_APP`, `SMASHSODA_PREFIX`. `smashsoda --reset` forgets a remembered screen choice.

## Troubleshooting
Logs are in `$XDG_RUNTIME_DIR/smashsoda/`: `smashsoda-wine.log` (the app, including `CRASH:` lines with the exception and module+offset if it crashes),
`helpers.log` (capture/audio/gamepad helpers), `parsec.log` (Parsec's own log, IPs masked).

* Guest sees "peer-to-peer connection failed [-6023 / -11004]" (host log: `-6024`): Parsec could not hole-punch between you and them. Check that
  `smashsoda-wine.log` shows `parsec: hosting port ...`. This is typical when the guest is on a mobile network or 5G home internet (carrier-grade NAT).
  Fix it from your side by pinning Parsec's UDP port and forwarding it on your router (the guest then only needs to reach you, no hole punching):
  `echo 8000 > ~/.local/share/smashsoda-linux/host-port` (or `SSB_HOST_PORT=8000 smashsoda`), then forward UDP 8000 to this PC in the router settings.
  UPnP is enabled by default, so on many routers this happens automatically; the port Parsec really uses is logged at start.
* "Host does not support hardware video encoding" (-15000) -> NVENC is not reachable: NVIDIA driver, `PROTON_NVIDIA_NVENC`, and the `nvenc:` lines in `smashsoda-wine.log`.
* "ViGEm not found" in the app -> `ssb-padd` is not running or `/dev/uinput` is not writable.
* No picture -> a picker should appear when you press Host; `helpers.log` tells why not.
* Guest pointer lands on the wrong spot with several monitors -> the captured monitor's position comes from the compositor; use the primary monitor first.
* Only one Smash Soda per Wine prefix can run.

## How it fits together
Windows and Linux sides talk through shared-memory files in `$XDG_RUNTIME_DIR/smashsoda/` (protocol: `bridge/ssb_proto.h`).
The source layout of this repository:

```
setup.sh                  one-command build + install
smashsoda                 launcher (starts the helpers, runs the app under umu/Proton)
patches/linux-port.patch  changes to upstream Smash Soda files (patches/FILES.txt lists them)
overlay/                  new source files added to the upstream tree (SmashSoda/linux/*: the Windows-side bridge)
bridge/                   native helpers: ssb-padd.c (uinput), ssb-capture.py (screen/audio), ssb-audioapps.py (per-app mixer)
nvenc-proxy/              D3D11 -> CUDA NVENC translation layer used by Parsec's encoder
CMakeLists.txt, cmake/    MinGW-w64 cross build of SmashSoda.exe;  gen-include-shim.py fixes #include letter case
tests/                    diagnostic programs used during development (build with MinGW, run with umu-run)
```
The upstream source is pinned in `setup.sh` (`UPSTREAM_COMMIT`); `build/` and `.cache/` are created by it and can be deleted.

Notes for hacking: encoder and pacing details live in `nvenc-proxy/nvenc_proxy.cpp` and `overlay/SmashSoda/linux/`; the capture session logic
(modes gpu -> gl -> shm, Change Screen, exit codes) in `bridge/ssb-capture.py`. `make-dist.py <upstream checkout> <out dir>` (development tree only)
regenerates the patch and overlay from a modified source tree.

## License
This project's own files are licensed under the GNU GPL v3 (see `LICENSE`). Smash Soda itself is MIT licensed, (c) Flavio Freitas and Soda Arcade, and is
downloaded from upstream at build time; Parsec and its SDK belong to Parsec Cloud / Unity. Not affiliated with either.
