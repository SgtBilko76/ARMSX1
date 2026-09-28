# ARMSX1
ARMSX1 is a fork of `psxe`, rebuilt around a native FSUI donor shell and SDL2-hosted frontends for desktop, Android, iOS, UWP, web, and PSVita.

## What It Supports
ARMSX1 currently has the following emulator pieces wired up:
- CPU, DMA, GPU, SPU, MDEC, GTE, and timers
- CD-ROM loading
- memory cards
- BIOS selection and PS-X EXE boot
- screenshot capture
- logging controls
- VSync control
- fast forward
- protocol/file launch through `armsx:///...` on supported hosts
- the FSUI settings shell

Disc image support is:
- `BIN/CUE`
- single-track `BIN`
- `ISO`
- `CHD`, including mixed-mode metadata, pregaps/postgaps, audio byte order, and Q subchannel data
- `ZIP` archives containing a supported game image and its companion files

## Accuracy and Acceleration

ARMSX1 has two portable CPU engines:

- `cached` is the default. It caches decoded instruction handlers, but still performs a real bus fetch and opcode check for every emulated instruction. Writes invalidate matching entries, including cached/uncached address aliases.
- `interpreter` is the reference path and remains selectable for diagnostics.

Neither engine emits native code, uses executable memory, or relies on host threading behavior. Select the engine in Settings, with `--cpu-engine=cached|interpreter`, or with `ARMSX_CPU_ENGINE`.

The GPU always uses the accurate software PlayStation rasterizer and keeps its 16-bit VRAM authoritative. Presentation has two SDL2 modes:

- `SDL software`, the default
- `SDL accelerated`, opt-in through Settings or `ARMSX_GPU_BACKEND=sdl-accelerated`

The accelerated mode uploads only changed VRAM scanlines to an SDL texture. It does not replace PS1 rasterization with host triangles, so switching presentation modes does not change emulated GPU results. If an accelerated SDL renderer is unavailable, ARMSX falls back to SDL software rendering.

## Supported Targets
- Desktop: macOS, Linux, Windows
- Mobile and alternate hosts: Android, Meta Quest (standalone VR), iOS, UWP, WebAssembly, PSVita
- CI also builds Linux `x64`, `x32`, `arm64`, `arm32` and Windows `x64`/`x32` packages

Desktop and Android are the main day-to-day targets. The other ports are present and buildable, but they are more platform-specific and may lag behind the desktop path.

## CI Runners
The Gitea workflow matches whatever label your act runner advertises. If that label is a docker-mode label like `ubuntu-latest:docker://...`, Gitea will keep creating per-job bridge networks and can eventually hit Docker address pool exhaustion.

To run the jobs directly on the runner container or host, change the runner labels to host mode, for example `ubuntu-latest:host` or `linux_amd64:host`, then restart act_runner. If the runner is started from a Docker image, remove the `/var/run/docker.sock` mount when you want host mode.

## Running
On desktop, launch the emulator directly:

```sh
./bin/armsx --bios=/path/to/bios.bin --cdrom=/path/to/game.cue
```

You can also launch a file with the protocol handler on supported hosts:

```text
armsx:///absolute/path/to/game.cue
```

The app opens an FSUI shell where you can choose BIOS files, start discs, change settings, take screenshots, and toggle fast forward or VSync.

Settings are stored under SDL's pref path, usually `SDL_GetPrefPath("nanodata", "armsx")`, in `settings.toml`.

On WebAssembly, **Start File**, **Start Disc**, and **Open Game Folder** use the browser's permission-gated file and directory pickers. Selected files are copied into the Emscripten virtual filesystem for the current session. Directory access falls back to a multi-file directory input where the File System Access API is unavailable. USB mass-storage drives are intentionally handled through these file pickers; WebUSB does not expose protected mass-storage-class devices as a general filesystem.

## Building
### Linux Desktop
Install SDL2 development packages, then run:

```sh
./build.sh
```

### macOS
Install SDL2 and dylibbundler:

```sh
brew install sdl2 dylibbundler
```

Then build a bundle:

```sh
./build.sh macosapp
```

### Windows
Install a MinGW toolchain and SDL2, then run:

```powershell
./build-deps.ps1
./build-win64.ps1
./build-win32.ps1
```

### Android
Set `ANDROID_NDK_ROOT` to an NDK 27+ install and build:

```sh
./build.sh android
```

### Meta Quest
The `quest` Gradle flavor is a standalone immersive APK for Quest 2, Quest Pro, Quest 3 and Quest 3S. It runs the whole frontend (library, settings, pause menu and the game) on a curved virtual screen, and maps the Touch controllers to the pad. Build the native library as for Android, then:

```sh
cd android
gradle :app:assembleQuestDebug   # Gradle 8.13+; app/build/outputs/apk/quest/debug/app-quest-debug.apk
adb install -r app/build/outputs/apk/quest/debug/app-quest-debug.apk
```

The Quest build installs as `com.nanodata.armsx.quest` next to the phone build (`phone` flavor, `com.nanodata.armsx`). Both use the same `libarmsx.so`; the quest activity switches it into VR mode, and only the quest APK carries the Khronos OpenXR loader.

The frontend still draws with the SDL GLES renderer, but into an offscreen texture. An XR thread copies each frame into an OpenXR swapchain and submits it as a cylinder layer (a flat quad when curved is off). The compositor renders that layer per eye, so the screen sits in the room with real stereoscopic depth. The PlayStation image itself is flat, because the console has no depth buffer. The emulator keeps its own 60/50 Hz pacing; the headset runs at 120 Hz when offered, so each NTSC frame is shown exactly twice.

| Touch controller | In game | In menus |
| --- | --- | --- |
| A / B / X / Y | Cross / Circle / Square / Triangle | Accept / Back / FSUI actions |
| Grips / Triggers | L1 R1 / L2 R2 | Grips switch pages; trigger clicks where you point |
| Left stick | Analog; a hard push is the D-pad | Navigate |
| Right stick | Analog | Scroll |
| Left / right stick click | Start / Select | |
| Menu | Open the pause menu | Close the pause menu |
| Menu + left / right stick click | L3 / R3 | |
| Menu + B | Left stick analog-only (toggle) | |
| Menu + right trigger | Recenter the screen | Recenter the screen |

Screen width, distance and curvature are under **Settings → VR Screen** and are stored in `settings.toml` under `[vr]`. Opening the system menu or taking the headset off pauses the game. OpenXR messages are logged with the `ARMSX-XR` tag (`adb logcat -s ARMSX-XR`). For frame timing and audio queue statistics every two seconds, run `adb shell setprop debug.armsx.perf 1` before starting the app and read them with `adb logcat -s ARMSX-PERF`.

### iOS
Use the bundled SDL2 xcframework and run:

```sh
./build.sh ios
```

### UWP
Build the native runtime or solution from Windows:

```sh
./build-uwp-native.sh x64 Debug
./build-uwp-native.sh x86 Release
./build-uwp-all.ps1
```

### WebAssembly
Requires Emscripten:

```sh
./build.sh wasm
```

Serve the generated `bin/wasm` directory over HTTP(S); browser file APIs are not available from a raw `file://` URL.

### PSVita
Requires VITASDK:

```sh
./build.sh psvita
```

## Configuration
`settings.toml` is generated on first run. CLI flags always override file settings for the current session. Some internal fields still use historical `psxe_*` names for compatibility.

The generated defaults are:

```toml
[cpu]
execution_mode = "cached"

[gpu]
backend = "software"
```

Legacy renderer names such as `hardware` are accepted when reading old settings and normalize to `sdl-accelerated` when saved.

## Validation

Run the deterministic source and subsystem matrix:

```sh
python3 tests/run_validation.py
```

The cases cover source invariants, cached/reference CPU differential execution and self-modifying code, GPU VRAM parity, CHD layout/subchannel logic, ZIP extraction and traversal rejection, and browser file-selection rules. `make disc-probe IMAGE=/path/to/game.cue` performs a read-only disc-open and sector-read smoke test.

`make test-sdl-runtime` is an optional headed host check that creates an accelerated SDL2 renderer, uploads a BGR555 texture, and presents it. It is intentionally not part of the deterministic matrix because graphical CI runners may not have a display or accelerated SDL driver.

## License

ARMSX-specific material owned by Moon is proprietary and all rights are reserved. No use, copying, modification, redistribution, publication, commercial exploitation, or creation of derivative works is permitted without Moon's prior express written permission.

This proprietary license applies only to material Moon owns or is authorized to license. Upstream psxe code, contributor-owned portions, bundled frameworks, and third-party dependencies retain their existing licenses. See [LICENSE](LICENSE) and [LICENSES/UPSTREAM-MIT.txt](LICENSES/UPSTREAM-MIT.txt).

## Acknowledgements
ARMSX uses:
- `argparse.c`
- `log.c`
- `tomlc99`
- SDL2
- FSUI donor / ImGui
- libchdr
- OpenXR headers and loader (Khronos)
