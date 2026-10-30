# Platforms

LYNX SHARE runs on Windows, macOS and Linux from one code base. The core (`src/core`, `src/flp`) is plain C++20 without
JUCE and builds anywhere; the app uses JUCE 8.0.4. Everything that differs per OS sits behind a few small helpers in
`src/platform/`, so the rest of the code never spells out `%APPDATA%` or an `HWND`.

| | Windows | macOS | Linux |
|---|---|---|---|
| Builds | Zig cross build from Linux/WSL (`scripts/build-windows-zig.sh`, used for releases) or CMake + MSVC | CMake + Apple clang, universal arm64 + x86_64 | CMake + GCC or Clang |
| Package | per-user installer (NSIS) + portable .zip | .dmg (ad-hoc signed, **not notarized**) | AppImage + .tar.gz (`install.sh`) |
| Minimum | Windows 10 x64 | macOS 11 Big Sur | x86_64, glibc 2.35 (Ubuntu 22.04, Debian 12, Fedora 36), X11 or XWayland |
| Audio drivers | Windows Audio (WASAPI shared / low-latency / exclusive), DirectSound, ASIO, built-in LYNX ASIO | Core Audio | PipeWire (LYNX SHARE's own client, loaded at run time), JACK, ALSA ([LINUX.md](LINUX.md)) |
| Plugin formats | VST3, CLAP, VST2 | VST3, CLAP, Audio Units | VST3, CLAP, LV2, VST2 |
| Plugin sandbox (separate process) | yes (opt-in) | no | yes (opt-in) |
| Settings | `%APPDATA%\LYNX` | `~/Library/Application Support/LYNX` | `$XDG_CONFIG_HOME/LYNX` (`~/.config/LYNX`) |
| Waveform cache | `%APPDATA%\LYNX\wavecache` | `~/Library/Caches/LYNX/wavecache` | `$XDG_CACHE_HOME/LYNX/wavecache` (`~/.cache/LYNX`) |
| Crash report + emergency save | `%APPDATA%\LYNX\autosave` | `.../Application Support/LYNX/autosave` | `~/.config/LYNX/autosave` |
| FL Studio installation (factory samples, plugin database) | registry, else `Program Files\Image-Line\FL Studio*` | `/Applications/FL Studio*.app/Contents/Resources/FL` (layout not confirmed) | a Windows FL Studio under Wine: `$WINEPREFIX` or `~/.wine`, `drive_c/Program Files/Image-Line/FL Studio*` |

`LYNX_FL_INSTALL=<folder>` points LYNX SHARE at an FL Studio installation anywhere (the folder that contains `Data`).
Importing `.flp` projects works everywhere; the installation only adds FL's factory samples and plugin database.

## Platform helpers (`src/platform/`)

- `Paths.h` — `settingsFolder()`, `settingsFile()`, `cacheFolder()`, `standardPluginFolders()` (VST3/VST2/CLAP/AU/LV2
  folders per OS, scanned by `PluginHost`).
- `NativeEmbed.h` — the native child view a CLAP or VST2 editor draws into: a child `HWND` (Windows), an `NSView`
  (macOS), an X11 window through `juce::XEmbedComponent` (Linux), plus the matching CLAP window API.
- `FileTypes.h` — `.lynx` file association (Windows registry; the Linux packages ship a `.desktop` file and a MIME type,
  the macOS bundle declares the document types in its Info.plist). The AppImage installs its own menu entry and MIME
  type into `~/.local/share` when the user turns it on (setup wizard, *Options › General*) and keeps the entry's path
  current when the AppImage file moves.
- `MingwFixes.cpp` — only for the MinGW cross build.

Other per-OS code, kept small and guarded in place:

- `core/WorkerPool.cpp` — the audio worker wake-up semaphore: Win32 semaphore, libdispatch semaphore (macOS), POSIX
  `sem_t` on a futex (Linux). All wake a sleeping worker at once (std::counting_semaphore is avoided: MinGW's libc++
  polls with Sleep()).
- `engine/AudioEngine.cpp` — priority of the audio workers: MMCSS "Pro Audio" (Windows), the user-interactive QoS
  class (macOS), SCHED_FIFO 70 when the user's limits allow it (Linux; otherwise unchanged).
- `engine/PipeWireDevice.cpp` — Linux: the native PipeWire driver (`libpipewire-0.3.so.0` through dlopen, vendored MIT
  headers in `third_party/pipewire`). `platform/LinuxRealtime.cpp` — Linux: real-time priority for the audio threads
  through RLIMIT_RTPRIO, RealtimeKit or the desktop portal (`libdbus-1.so.3` through dlopen); no-ops elsewhere.
- `engine/DllAsioDevice.cpp`, `asio/` — ASIO and the LYNX ASIO driver: Windows only (ASIO is a Windows driver model).
- `engine/PluginSandbox.cpp` — out-of-process plugins: Win32 shared memory and events (Windows), POSIX shared memory
  (`shm_open`) and process-shared semaphores (Linux); on Linux a `pidfd` notices a crashed child at once. Not macOS yet.
- `engine/Vst2Format.cpp` — VST2 hosting loads Windows DLLs and Linux shared objects (`.so`, yabridge bridges included); not macOS (VST2 is deprecated and rare there).
- `ui/WindowOwner.cpp` — dark native title bar and owned tool windows (Windows); JUCE defaults elsewhere.
- `ui/Theme.cpp` — Segoe UI on Windows; the system sans-serif elsewhere. On Linux (JUCE's software renderer) knobs and
  rounded plates are drawn from cached pre-rendered sprites, which are much cheaper there than path filling.
- `ui/Toolbar.cpp` — process memory (Windows API, `/proc/self/statm` on Linux).

## Floating point

Every build compiles with `-ffp-contract=off` (no fused multiply-add), so the exact FL engines keep the operation
order of the Windows build on CPUs with FMA (Apple Silicon, newer x86). Results can still differ in the last bits
between operating systems because each system's maths library (`sin`, `exp`, `pow`...) rounds differently; the
round 117 notes (`docs/analise/REA_rodada117.md`) list the measured differences.

## Building

- Windows (release): `scripts/build-windows-zig.sh` from Linux/WSL.
- Linux: see README ("Build from source"). Release builds run in an Ubuntu 22.04 container with GCC 13
  (toolchain PPA) and a static libstdc++/libgcc, so they need only glibc 2.35; `scripts/check-linux-compat.sh <binary>`
  checks that (highest GLIBC_ symbol version, no system libstdc++) and `scripts/check-appimage-start.sh <AppImage>`
  starts the AppImage offline on Xvfb like the AppImage catalog test does; `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --target LYNX`,
  then `scripts/package-linux.sh build/LYNX_artefacts/Release/LYNX <label> dist`.
- macOS: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" && cmake --build build --target LYNX`,
  then `scripts/package-macos.sh build/LYNX_artefacts/Release/LYNX.app <label> dist`.
- CI: `.github/workflows/tests.yml` runs the core tests on Linux (GCC, Clang) and macOS and builds + self-tests the
  app on Linux and macOS; `release.yml` builds all three platforms on a tag and attaches the packages and
  `SHA256SUMS.txt` to the draft release.

## Windows plugins on Linux (yabridge)

[yabridge](https://github.com/robbert-vdh/yabridge) turns Windows VST2/VST3 plugins into Linux ones running in Wine.
LYNX SHARE treats them like any other Linux plugin: add the folder yabridge syncs to (`~/.vst3/yabridge` for VST3, the folder
of the `.so` files for VST2) in *Options › Plugins* or let the standard folders be scanned. Checked in round 161 with
yabridge 5.1.1 and Wine 10.0: scanning, the plugin window, playback, export, and the plugin sandbox (the Wine host then
runs under the sandbox child, so a crash there leaves LYNX SHARE running). Wine has to be installed by the user; LYNX SHARE
itself needs nothing extra.
