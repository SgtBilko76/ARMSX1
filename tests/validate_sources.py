#!/usr/bin/env python3

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> int:
    failures: list[str] = []

    def require(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    makefile = read("Makefile")
    cpu = read("psx/cpu.c")
    cpu_header = read("psx/cpu.h")
    config = read("frontend/config.c")
    frontend = read("frontend/main.cpp")
    archive = read("frontend/archive.cpp")
    gpu_hw = read("frontend/gpu_hw.c")
    chd = read("psx/dev/cdrom/chd.c")
    web = read("web/file_access.js")
    android = read("android/app/src/main/java/com/nanodata/armsx/EmulatorActivity.java")
    ios = read("ios/HostApp/Sources/AppDelegate.mm")
    platform_file = read("frontend/platform_file.c")

    require("PSX_CPU_CACHED_INTERPRETER" in cpu_header, "cached interpreter API is missing")
    require("cfg->cpu_engine = 1;" in config, "cached interpreter is not the default")
    require('"    execution_mode  = \\"cached\\"' in config, "generated settings do not default to cached")
    require('"cpu-engine"' in config, "--cpu-engine CLI selection is missing")
    require('"ARMSX_CPU_ENGINE"' in frontend, "ARMSX_CPU_ENGINE environment override is missing")
    # psx_bus_read32_fast (psx/bus_init.h) is the same bus fetch with RAM/BIOS decoded inline.
    require("psx_bus_read32(cpu->bus, cpu->pc)" in cpu or "psx_bus_read32_fast(cpu->bus, cpu->pc)" in cpu,
            "cached mode must preserve a real instruction bus fetch")
    require("entry->opcode == cpu->opcode" in cpu, "cached mode lacks opcode verification")
    require("psx_cpu_invalidate_range" in cpu, "cached mode lacks targeted invalidation")
    require("pthread_" not in cpu and "SDL_CreateThread" not in cpu, "CPU engine must not depend on host threading")
    require("mprotect(" not in cpu and "VirtualProtect(" not in cpu, "JIT-style executable memory is forbidden")
    require("USE_CHD ?= 1" in makefile, "CHD must be enabled by default")
    require('export USE_CHD="${USE_CHD:-1}"' in read("build.sh"), "build.sh disables default CHD support")
    require("gpu_render_triangle(gpu, v0, v1, v2, data, edge);" in gpu_hw,
            "SDL acceleration must preserve the software PS1 rasterizer")
    require("SDL_RenderGeometry" not in gpu_hw, "experimental incomplete triangle renderer is still active")
    require("GpuBackend gpu_backend = GpuBackend::Software;" in frontend,
            "SDL acceleration must be opt-in and default off")
    require("SDL_RENDERER_ACCELERATED" in frontend and "SDL_RENDERER_SOFTWARE" in frontend,
            "both SDL presentation modes must remain selectable")
    require("desired.callback = nullptr;" in frontend and "SDL_QueueAudio" in frontend,
            "audio must use SDL queued mode instead of a starvation-prone callback")
    require("desired.samples = 1024;" in frontend and "kAudioPrimeBytes" in frontend,
            "audio must use a power-of-two device buffer and startup prebuffer")
    require("presentation_upscale = 1;" in frontend and "SDL_TEXTUREACCESS_TARGET" in frontend,
            "1x-default SDL presentation upscaling is missing")
    require("SDL_FINGERDOWN" in frontend and "ImGuiMobileControls" in frontend,
            "ImGui multitouch PlayStation controls are missing")
    require("kMobileControlsAutoHideMs = 3000" in frontend and "takeControllerActivity" in frontend,
            "mobile controls must auto-hide and react to physical-controller use")
    require("DefaultMobileControlsEnabled" in frontend and "mobile_controls = DefaultMobileControlsEnabled()" in frontend,
            "mobile control defaults are not platform-aware")
    require("ACTION_OPEN_DOCUMENT_TREE" in android and "takePersistableUriPermission" in android,
            "Android persistent Storage Access Framework directory access is missing")
    require("DocumentsContract.buildChildDocumentsUriUsingTree" in android and "openVirtualFileDescriptor" in android,
            "Android document-tree enumeration or no-copy file access is missing")
    require("WindowInsets.Type.systemBars()" in android and "BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE" in android,
            "Android immersive fullscreen is missing")
    require("UIDocumentPickerViewController" in ios and "UTTypeFolder" in ios,
            "iOS system directory picker is missing")
    require("bookmarkDataWithOptions:0" in ios and "startAccessingSecurityScopedResource" in ios,
            "iOS persistent security-scoped directory access is missing")
    require("psxe_platform_fopen" in platform_file and "psxe_platform_fopen" in archive,
            "platform file-descriptor bridge is missing")
    require("USE_CONNECTIVITY" not in makefile and "FTP" not in frontend and "ftp" not in frontend,
            "FTP connectivity code is still present")
    require("showOpenFilePicker" in web and "showDirectoryPicker" in web,
            "web permission-based file and directory access is missing")
    require("webkitdirectory" in web, "web directory picker fallback is missing")
    require("kMaxArchiveBytes" in archive and "SafeArchiveRelativePath" in archive,
            "bounded path-safe ZIP extraction is missing")
    require("CHD_TRACK1_PREGAP_FRAMES" in chd and "postgap_frames" in chd,
            "CHD pregap/postgap handling is missing")
    require("chd_read_subchannel_q" in chd, "CHD subchannel Q support is missing")

    if failures:
        for failure in failures:
            print(f"ARMSX_SOURCE_CHECK failed: {failure}", file=sys.stderr)
        return 1

    print("ARMSX_SOURCE_CHECK passed")
    print("  cached interpreter: default, selectable, real-fetch verified")
    print("  cache invalidation: explicit range hook plus opcode guard")
    print("  portability: no JIT memory or host-thread dependency")
    print("  renderer: software-authoritative SDL presentation, acceleration opt-in")
    print("  presentation: SDL target upscaling 1x-8x, default 1x")
    print("  audio: queued SDL transport with prebuffer and GPU-derived frame timing")
    print("  mobile: ImGui PlayStation controls with idle/controller auto-hide")
    print("  storage: Android SAF and iOS security-scoped persistent game directories")
    print("  media: CHD default plus bounded ZIP ingestion and browser permission bridge")
    print("  connectivity: FTP removed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
