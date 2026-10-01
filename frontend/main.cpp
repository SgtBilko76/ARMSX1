#include <SDL.h>
#include <SDL_gamecontroller.h>
#include <SDL_render.h>
#include <SDL_system.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include <unistd.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if !defined(UWP_TARGET)
#include <dbghelp.h>
#endif
#elif !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(PSVITA_TARGET)
#if defined(__has_include)
#if __has_include(<execinfo.h>)
#include <dlfcn.h>
#include <execinfo.h>
#define PSXE_HAS_EXECINFO 1
#endif
#endif
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten/html5.h>
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#include <jni.h>
#include <sys/system_properties.h>
#endif

extern "C" {
#include "../psx/psx.h"
#include "../psx/dev/cdrom/cdrom.h"
#include "../psx/dev/gpu.h"
#include "../psx/dev/input.h"
#include "../psx/dev/pad.h"
#include "../psx/dev/timer.h"
#include "../psx/input/sda.h"
#include "../psx/savestate.h"
#include "common.h"
#include "config.h"
#include "toml.h"
}

#include "archive.h"
#include "IconsFontAwesome5.h"
#include "platform_file.h"
#ifdef USE_HARDWARE
#include "gpu_hw.h"
#endif
#include "fsui/backend_sdl.hpp"
#include "fsui/fsui.hpp"
#include "fsui/imgui_fullscreen.hpp"
#include "imgui.h"
#include "imgui_impl_sdlrenderer2.h"
#include "vr/parallel_for.h"
#include "vr/stereo_reproject.h"
#include "vr/thread_affinity.h"
#include "vr/vr_host.h"

#undef main

namespace {

constexpr Uint32 kPauseChordGraceMs = 120;
constexpr std::string_view kCustomFsuiAppIconPath = "icons/FsuiAppIcon.png";

constexpr bool SupportsManagedWindowSizing() {
#if defined(__ANDROID__) || defined(IOS_TARGET) || defined(__EMSCRIPTEN__) || defined(UWP_TARGET) || defined(PSVITA_TARGET)
    return false;
#else
    return true;
#endif
}

constexpr bool DefaultVsyncEnabled() {
#if defined(UWP_TARGET)
    return false;
#else
    return true;
#endif
}

constexpr bool DefaultMobileControlsEnabled() {
#if defined(__ANDROID__) || defined(IOS_TARGET)
    return true;
#else
    return false;
#endif
}

#ifdef USE_HARDWARE
enum class GpuBackend {
    Software = 0,
    SDLAccelerated = 1,
};
#endif

class ArmsxApp;

ArmsxApp* g_active_app = nullptr;
std::atomic_bool g_crash_reporting{false};
std::mutex g_pending_launch_lock;
std::vector<std::string> g_pending_launch_arguments;
std::vector<std::string> g_pending_web_errors;

struct PlatformLibraryRoot {
    std::string path;
    std::string label;
    bool recursive = true;
};

struct PlatformLibraryGame {
    std::string path;
    std::string label;
    std::uint64_t size = 0;
    std::time_t modified = 0;
};

using PlatformDirectoryPickerCallback = void (*)(int recursive, void* userdata);
using PlatformDirectoryRemovedCallback = void (*)(const char* path, void* userdata);

std::mutex g_platform_library_lock;
std::vector<PlatformLibraryRoot> g_platform_library_roots;
std::vector<PlatformLibraryGame> g_platform_library_games;
std::uint64_t g_platform_library_version = 0;
PlatformDirectoryPickerCallback g_platform_directory_picker_callback = nullptr;
void* g_platform_directory_picker_userdata = nullptr;
PlatformDirectoryRemovedCallback g_platform_directory_removed_callback = nullptr;
void* g_platform_directory_removed_userdata = nullptr;

constexpr double kUiFrameRate = 60.0;
constexpr Uint32 kMobileControlsAutoHideMs = 3000;

const char* LogLevelTitle(int level) {
    switch (level) {
        case LOG_TRACE: return "trace";
        case LOG_DEBUG: return "debug";
        case LOG_INFO: return "info";
        case LOG_WARN: return "warn";
        case LOG_ERROR: return "error";
        case LOG_FATAL: return "fatal";
        default: return "unknown";
    }
}

void StructuredLogCallback(log_Event* ev) {
    if (!ev) {
        return;
    }

    char message[4096] = {};
    vsnprintf(message, sizeof(message), ev->fmt, ev->ap);
    psxe_diag_logf("psx", "%s %s:%d %s", LogLevelTitle(ev->level), ev->file, ev->line, message);
}

void SdlLogOutput(void*, int category, SDL_LogPriority priority, const char* message) {
    psxe_diag_logf("sdl", "category=%d priority=%d %s", category, static_cast<int>(priority), message ? message : "");
}

[[noreturn]] void ReportNativeCrash(const char* reason);

#if defined(_WIN32) && !defined(UWP_TARGET)
LONG WINAPI WindowsUnhandledExceptionFilter(EXCEPTION_POINTERS* exception_info) {
    const DWORD code = exception_info && exception_info->ExceptionRecord
        ? exception_info->ExceptionRecord->ExceptionCode
        : 0u;
    char reason[64] = {};
    SDL_snprintf(reason, sizeof(reason), "SEH 0x%08lx", static_cast<unsigned long>(code));
    ReportNativeCrash(reason);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

double CounterTicksToMilliseconds(uint64_t ticks) {
    const uint64_t frequency = SDL_GetPerformanceFrequency();
    if (frequency == 0) {
        return 0.0;
    }

    return (static_cast<double>(ticks) * 1000.0) / static_cast<double>(frequency);
}

std::string RendererFlagsTitle(Uint32 flags) {
    std::string title;

    auto append = [&](const char* value) {
        if (!title.empty()) {
            title.append("|");
        }
        title.append(value);
    };

    if (flags & SDL_RENDERER_SOFTWARE) {
        append("software");
    }
    if (flags & SDL_RENDERER_ACCELERATED) {
        append("accelerated");
    }
    if (flags & SDL_RENDERER_PRESENTVSYNC) {
        append("present-vsync");
    }
    if (flags & SDL_RENDERER_TARGETTEXTURE) {
        append("target-texture");
    }

    if (title.empty()) {
        title = "none";
    }

    return title;
}

[[noreturn]] void ReportNativeCrash(const char* reason);
void WriteNativeStackTraceImpl();

enum class LaunchKind {
    None,
    Bios,
    Disc,
    Exe,
};

const char* CpuEngineTitle(psx_cpu_execution_mode_t mode) {
    return mode == PSX_CPU_INTERPRETER ? "Interpreter" : "Cached interpreter";
}

const char* CpuEngineSettingToken(psx_cpu_execution_mode_t mode) {
    return mode == PSX_CPU_INTERPRETER ? "interpreter" : "cached";
}

std::optional<psx_cpu_execution_mode_t> ParseCpuEngine(const char* value) {
    if (!value || !value[0]) {
        return std::nullopt;
    }

    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lowered == "interpreter" || lowered == "reference") {
        return PSX_CPU_INTERPRETER;
    }
    if (lowered == "cached" || lowered == "cached-interpreter") {
        return PSX_CPU_CACHED_INTERPRETER;
    }
    return std::nullopt;
}

const char* LaunchKindTitle(LaunchKind kind) {
    switch (kind) {
        case LaunchKind::Bios: return "bios";
        case LaunchKind::Disc: return "disc";
        case LaunchKind::Exe: return "exe";
        case LaunchKind::None:
        default:
            return "none";
    }
}

struct CliFlags {
    bool bios = false;
    bool bios_folder = false;
    bool model = false;
    bool region = false;
    bool scale = false;
    bool settings_file = false;
    bool quiet = false;
    bool log_level = false;
    bool exp_rom = false;
    bool exe = false;
    bool cdrom = false;
    bool cpu_engine = false;
    bool has_boot_request = false;
};

struct LaunchRequest {
    LaunchKind kind = LaunchKind::None;
    std::filesystem::path path;
    std::string label;
};

void EnqueuePendingLaunchArgument(std::string argument) {
    if (argument.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_pending_launch_lock);
    g_pending_launch_arguments.push_back(std::move(argument));
}

std::vector<std::string> DrainPendingLaunchArguments() {
    std::lock_guard<std::mutex> lock(g_pending_launch_lock);
    std::vector<std::string> pending;
    pending.swap(g_pending_launch_arguments);
    return pending;
}

void EnqueueWebError(std::string message) {
    if (message.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_pending_launch_lock);
    g_pending_web_errors.push_back(std::move(message));
}

std::vector<std::string> DrainWebErrors() {
    std::lock_guard<std::mutex> lock(g_pending_launch_lock);
    std::vector<std::string> pending;
    pending.swap(g_pending_web_errors);
    return pending;
}

bool IsAndroidVirtualPath(std::string_view path) {
    return path.rfind("armsx-android:///", 0) == 0;
}

bool PlatformPathBelongsToRoot(std::string_view path, std::string_view root, bool recursive) {
    if (path.size() <= root.size() || path.substr(0, root.size()) != root || path[root.size()] != '/') {
        return false;
    }

    const std::string_view relative = path.substr(root.size() + 1);
    return recursive || relative.find('/') == std::string_view::npos;
}

void SetPlatformLibrarySnapshot(
    std::vector<PlatformLibraryRoot> roots,
    std::vector<PlatformLibraryGame> games
) {
    std::lock_guard<std::mutex> lock(g_platform_library_lock);
    g_platform_library_roots = std::move(roots);
    g_platform_library_games = std::move(games);
    g_platform_library_version++;
}

void RegisterPlatformLibraryRoot(std::string path, std::string label, bool recursive) {
    if (path.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_platform_library_lock);
    const auto found = std::find_if(g_platform_library_roots.begin(), g_platform_library_roots.end(), [&](const auto& root) {
        return root.path == path;
    });
    if (found == g_platform_library_roots.end()) {
        g_platform_library_roots.push_back(PlatformLibraryRoot{
            .path = std::move(path),
            .label = std::move(label),
            .recursive = recursive,
        });
    } else {
        found->label = std::move(label);
        found->recursive = recursive;
    }
    g_platform_library_version++;
}

std::uint64_t CopyPlatformLibrarySnapshot(
    std::vector<PlatformLibraryRoot>& roots,
    std::vector<PlatformLibraryGame>& games
) {
    std::lock_guard<std::mutex> lock(g_platform_library_lock);
    roots = g_platform_library_roots;
    games = g_platform_library_games;
    return g_platform_library_version;
}

#if defined(__ANDROID__)
bool CallAndroidActivityVoidMethod(const char* name, const char* signature, jvalue* args = nullptr) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) {
        return false;
    }

    jclass cls = env->GetObjectClass(activity);
    if (!cls) {
        env->DeleteLocalRef(activity);
        return false;
    }

    jmethodID method = env->GetMethodID(cls, name, signature);
    if (!method) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(activity);
        return false;
    }

    env->CallVoidMethodA(activity, method, args);
    const bool succeeded = !env->ExceptionCheck();
    if (!succeeded) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return succeeded;
}

bool RequestAndroidLibraryDirectory(bool recursive) {
    jvalue args[1]{};
    args[0].z = recursive ? JNI_TRUE : JNI_FALSE;
    return CallAndroidActivityVoidMethod("openLibraryDirectoryPicker", "(Z)V", args);
}

void RemoveAndroidLibraryDirectory(std::string_view path) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) {
        return;
    }

    jclass cls = env->GetObjectClass(activity);
    jmethodID method = cls ? env->GetMethodID(cls, "removeLibraryDirectory", "(Ljava/lang/String;)V") : nullptr;
    if (method) {
        jstring value = env->NewStringUTF(std::string(path).c_str());
        env->CallVoidMethod(activity, method, value);
        env->DeleteLocalRef(value);
    } else {
        env->ExceptionClear();
    }
    if (cls) {
        env->DeleteLocalRef(cls);
    }
    env->DeleteLocalRef(activity);
}

FILE* AndroidPlatformFopen(const char* path, const char* mode, void*) {
    if (!path || !mode || !IsAndroidVirtualPath(path)) {
        return fopen(path, mode);
    }
    if (mode[0] != 'r') {
        return nullptr;
    }

    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) {
        return nullptr;
    }

    int fd = -1;
    jclass cls = env->GetObjectClass(activity);
    jmethodID method = cls ? env->GetMethodID(cls, "openVirtualFileDescriptor", "(Ljava/lang/String;)I") : nullptr;
    if (method) {
        jstring value = env->NewStringUTF(path);
        fd = env->CallIntMethod(activity, method, value);
        env->DeleteLocalRef(value);
    } else {
        env->ExceptionClear();
    }
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        fd = -1;
    }
    if (cls) {
        env->DeleteLocalRef(cls);
    }
    env->DeleteLocalRef(activity);

    if (fd < 0) {
        return nullptr;
    }

    FILE* file = fdopen(fd, mode);
    if (!file) {
        close(fd);
    }
    return file;
}
#endif

bool RequestPlatformLibraryDirectory(bool recursive) {
#if defined(__ANDROID__)
    if (RequestAndroidLibraryDirectory(recursive)) {
        return true;
    }
#endif
    if (g_platform_directory_picker_callback) {
        g_platform_directory_picker_callback(recursive ? 1 : 0, g_platform_directory_picker_userdata);
        return true;
    }
    return false;
}

void NotifyPlatformLibraryDirectoryRemoved(std::string_view path) {
#if defined(__ANDROID__)
    if (IsAndroidVirtualPath(path)) {
        RemoveAndroidLibraryDirectory(path);
    }
#endif
    if (g_platform_directory_removed_callback) {
        g_platform_directory_removed_callback(std::string(path).c_str(), g_platform_directory_removed_userdata);
    }
}

struct FrontendSettings {
    std::string settings_path;
    std::string bios_override;
    std::string bios_search = "bios";
    std::string model = "scph1001";
    std::string region = "auto";
    std::string exp_path;
    std::string default_exe_path;
    int scale = 3;
    int log_level = LOG_INFO;
    bool quiet = true;
    bool logging_enabled = false;
    bool vsync_enabled = DefaultVsyncEnabled();
    psx_cpu_execution_mode_t cpu_engine = PSX_CPU_CACHED_INTERPRETER;
#ifdef USE_HARDWARE
    GpuBackend gpu_backend = GpuBackend::Software;
#endif
    bool texture_scale_mode = false;
    bool debug_panel = false;
    bool stretch_mode = false;
    int display_aspect = 0;
    int upscale_height = 480;
    int presentation_upscale = 1;
    bool mobile_controls = DefaultMobileControlsEnabled();
    armsx::vr::ScreenConfig vr_screen{};
    int vr_depth_percent = 100; // stereoscopic 3D strength; 0 is flat
    int vr_popout = 2;          // stereo convergence: what sits on the screen plane
    fsui::UiState ui_state{
        .theme = "Dark",
        .prompt_icon_pack = fsui::PromptIconPack::Auto,
        .background_image_path = {},
        .default_game_view = 0,
        .game_sort = 2,
        .game_sort_reverse = false,
        .game_list_paths = {},
        .game_list_recursive_paths = {},
        .covers_path = {},
        .show_inputs_overlay = false,
        .show_settings_overlay = false,
        .show_performance_overlay = false,
    };
};

struct PendingChordButton {
    bool physical_down = false;
    bool pending = false;
    bool forwarded = false;
    Uint32 pending_since = 0;
};

enum class FsuiWindowState {
    None,
    Landing,
    StartGame,
    Exit,
    GameList,
    Settings,
    PauseMenu,
};

std::string ToLower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::string Trim(std::string_view value) {
    size_t begin = 0;
    size_t end = value.size();

    while ((begin < end) && std::isspace(static_cast<unsigned char>(value[begin]))) {
        begin++;
    }

    while ((end > begin) && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        end--;
    }

    return std::string(value.substr(begin, end - begin));
}

std::string NormalizeModel(std::string_view value) {
    std::string out;

    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(std::tolower(ch)));
        }
    }

    return out;
}

std::string DefaultSettingsPath() {
    const char* pref = psxe_cfg_get_pref_path();

    if (pref && pref[0]) {
        return std::string(pref) + "settings.toml";
    }

    return "settings.toml";
}

std::filesystem::path DefaultBrowseDirectory() {
    const char* pref = psxe_cfg_get_pref_path();

    if (pref && pref[0]) {
        return std::filesystem::path(pref);
    }

    return std::filesystem::current_path();
}

std::filesystem::path DefaultDiagnosticsLogPath() {
    std::filesystem::path path = DefaultBrowseDirectory() / "logs";
#if defined(UWP_TARGET)
    path /= "armsx-uwp.log";
#else
    path /= "armsx.log";
#endif
    return path;
}

std::string AppendBrowseRootHint(std::string summary) {
#if defined(_WIN32)
    if (!summary.empty() && summary.back() != ' ') {
        summary.push_back(' ');
    }
    summary += "Use Filesystem Roots or Parent Directory to switch drives.";
#endif
    return summary;
}

bool PathsMatch(const std::filesystem::path& left, const std::filesystem::path& right) {
#if defined(_WIN32)
    return ToLower(left.lexically_normal().generic_string()) == ToLower(right.lexically_normal().generic_string());
#else
    return left.lexically_normal() == right.lexically_normal();
#endif
}

bool PathListContains(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& candidate) {
    return std::any_of(paths.begin(), paths.end(), [&](const std::filesystem::path& path) {
        return PathsMatch(path, candidate);
    });
}

bool FileContainsCaseInsensitive(const std::filesystem::path& path, const std::string& needle_lower) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return ToLower(content).find(needle_lower) != std::string::npos;
}

bool CueDirectoryReferencesImage(const std::filesystem::path& image_path) {
    if (!image_path.has_parent_path()) {
        return false;
    }

    const std::string needle = ToLower(image_path.filename().string());
    std::error_code ec;

    for (const auto& item : std::filesystem::directory_iterator(image_path.parent_path(), ec)) {
        if (ec) {
            break;
        }
        if (!item.is_regular_file()) {
            continue;
        }
        if (ToLower(item.path().extension().string()) != ".cue") {
            continue;
        }
        if (FileContainsCaseInsensitive(item.path(), needle)) {
            return true;
        }
    }

    return false;
}

bool IsLikelyBiosImagePath(const std::filesystem::path& path, const FrontendSettings& settings) {
    const std::string ext = ToLower(path.extension().string());
    if (ext != ".bin" && ext != ".rom") {
        return false;
    }

    if (!settings.bios_override.empty() && PathsMatch(path, std::filesystem::path(settings.bios_override))) {
        return true;
    }

    const std::string stem = NormalizeModel(path.stem().string());
    if (stem == "bios" || stem == "biosbin") {
        return true;
    }
    if (stem.starts_with("scph")) {
        return true;
    }

    return false;
}

std::string NormalizePathString(std::string value) {
#if defined(_WIN32)
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.size() == 2 && std::isalpha(static_cast<unsigned char>(value[0])) && value[1] == ':') {
        value.push_back('/');
    }
#endif
    return value;
}

bool StartsWithCaseInsensitive(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }

    for (size_t index = 0; index < prefix.size(); index++) {
        const unsigned char lhs = static_cast<unsigned char>(value[index]);
        const unsigned char rhs = static_cast<unsigned char>(prefix[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }

    return true;
}

int HexValue(unsigned char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

std::string PercentDecode(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());

    for (size_t index = 0; index < value.size(); index++) {
        const unsigned char ch = static_cast<unsigned char>(value[index]);
        if ((ch == '%') && ((index + 2) < value.size())) {
            const int hi = HexValue(static_cast<unsigned char>(value[index + 1]));
            const int lo = HexValue(static_cast<unsigned char>(value[index + 2]));
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                index += 2;
                continue;
            }
        }

        decoded.push_back(ch == '+' ? ' ' : static_cast<char>(ch));
    }

    return decoded;
}

std::optional<std::string> NormalizedPickerSelection(const std::string& path) {
    const std::string trimmed = Trim(path);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    return NormalizePathString(trimmed);
}

std::string WithHiddenId(std::string_view label, std::string_view id) {
    std::string out(label);
    out += "##";
    out += id;
    return out;
}

bool ReadSdlFileBytes(std::string_view path, std::vector<unsigned char>& bytes) {
    SDL_RWops* handle = SDL_RWFromFile(std::string(path).c_str(), "rb");
    if (!handle) {
        return false;
    }

    bytes.clear();
    std::array<unsigned char, 16 * 1024> buffer{};
    for (;;) {
        const size_t read = SDL_RWread(handle, buffer.data(), 1, buffer.size());
        if (read == 0) {
            break;
        }
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read));
    }

    SDL_RWclose(handle);
    return !bytes.empty();
}

std::string MaterializeBundledFsuiAppIcon() {
    const std::filesystem::path destination = DefaultBrowseDirectory() / kCustomFsuiAppIconPath;
    std::error_code ec;

    if (std::filesystem::exists(destination, ec) && std::filesystem::is_regular_file(destination, ec)) {
        return destination.string();
    }

    std::vector<unsigned char> bytes;
    if (!ReadSdlFileBytes(kCustomFsuiAppIconPath, bytes)) {
        return {};
    }

    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
        return {};
    }

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        return {};
    }

    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output.good()) {
        return {};
    }

    return destination.string();
}

std::string ResolveFsuiAppIconPath() {
    const std::filesystem::path relative(kCustomFsuiAppIconPath);
    std::error_code ec;

    auto try_path = [&](const std::filesystem::path& candidate) -> std::string {
        if (candidate.empty()) {
            return {};
        }
        ec.clear();
        if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec)) {
            return candidate.lexically_normal().string();
        }
        return {};
    };

#ifdef PSVITA_TARGET
    const char* vita_base = "app0:/";
    struct VitaBaseDeleter { void operator()(const char*) const {} };
    std::unique_ptr<const char, VitaBaseDeleter> base_path(vita_base);
#else
    std::unique_ptr<char, decltype(&SDL_free)> base_path(SDL_GetBasePath(), &SDL_free);
#endif
    if (base_path) {
        const std::filesystem::path base(base_path.get());

        if (std::string resolved = try_path(base / relative); !resolved.empty()) {
            return resolved;
        }
        if (std::string resolved = try_path((base / ".." / "Resources" / relative).lexically_normal()); !resolved.empty()) {
            return resolved;
        }
    }

    if (std::string resolved = try_path(relative); !resolved.empty()) {
        return resolved;
    }
    if (std::string resolved = try_path(std::filesystem::current_path(ec) / relative); !resolved.empty()) {
        return resolved;
    }
    if (std::string resolved = MaterializeBundledFsuiAppIcon(); !resolved.empty()) {
        return resolved;
    }

    return std::string(kCustomFsuiAppIconPath);
}

bool IsDiscPath(const std::filesystem::path& path) {
    const std::string ext = ToLower(path.extension().string());
    return ext == ".cue" || ext == ".bin" || ext == ".iso" || ext == ".img"
#ifdef USE_CHD
        || ext == ".chd"
#endif
        ;
}

bool IsExePath(const std::filesystem::path& path) {
    const std::string ext = ToLower(path.extension().string());
    return ext == ".exe" || ext == ".ps-exe" || ext == ".psexe";
}

bool IsZipPath(const std::filesystem::path& path) {
    return armsx::IsZipPath(path);
}

std::string StemToTitle(const std::filesystem::path& path) {
    std::string value = path.stem().string();

    if (value.empty()) {
        value = path.filename().string();
    }

    std::replace(value.begin(), value.end(), '_', ' ');

    return Trim(value);
}

std::string EscapeTomlString(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);

    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }

    return out;
}

const char* AspectToString(int aspect) {
    switch (aspect) {
        case 1: return "square";
        case 2: return "wide16x9";
        default: return "classic";
    }
}

const char* AspectTitle(int aspect) {
    switch (aspect) {
        case 1: return "Square";
        case 2: return "Wide 16:9";
        default: return "Classic";
    }
}

const char* UpscaleToString(int height) {
    switch (height) {
        case 720: return "720p";
        case 1080: return "1080p";
        case 1440: return "1440p";
        case 2160: return "2160p";
        default: return "480p";
    }
}

std::string BoolTitle(bool value) {
    return value ? "On" : "Off";
}

std::string RendererValueTitle(const FrontendSettings& settings) {
    std::string value = settings.texture_scale_mode ? "Linear" : "Nearest";

    if (settings.stretch_mode) {
        value += ", Stretch";
    } else {
        value += ", Fit";
    }

    return value;
}

#ifdef USE_HARDWARE
constexpr bool SupportsHardwareGpuBackend() {
    return true;
}

const char* GpuBackendTitle(GpuBackend backend) {
    switch (backend) {
        case GpuBackend::SDLAccelerated:
            return "SDL accelerated";
        case GpuBackend::Software:
        default:
            return "SDL software";
    }
}

const char* GpuBackendSettingToken(GpuBackend backend) {
    switch (backend) {
        case GpuBackend::SDLAccelerated:
            return "sdl-accelerated";
        case GpuBackend::Software:
        default:
            return "software";
    }
}

std::optional<GpuBackend> ParseGpuBackendOverride(const char* value) {
    if (!value || !value[0]) {
        return std::nullopt;
    }

    const std::string lowered = ToLower(value);
    if (lowered == "sdl-accelerated" || lowered == "hardware" || lowered == "hardware (experimental)" || lowered == "hw" || lowered == "hw-renderer") {
        return GpuBackend::SDLAccelerated;
    }

    if (lowered == "software" || lowered == "sw" || lowered == "sw-renderer") {
        return GpuBackend::Software;
    }

    return std::nullopt;
}
#endif

int RegionFromSettings(const FrontendSettings& settings) {
    const std::string region = ToLower(settings.region);

    if (region == "pal") {
        return CDR_REGION_EUROPE;
    }

    if (region == "ntsc") {
        const std::string model = NormalizeModel(settings.model);
        static const std::array<const char*, 8> japan_models = {
            "scph1000",
            "scph3000",
            "scph3500",
            "scph5000",
            "scph5500",
            "scph7000",
            "scph7003",
            "scph100",
        };

        if (std::find(japan_models.begin(), japan_models.end(), model) != japan_models.end()) {
            return CDR_REGION_JAPAN;
        }

        return CDR_REGION_AMERICA;
    }

    const std::string model = NormalizeModel(settings.model);
    static const std::array<const char*, 8> europe_models = {
        "scph1002",
        "scph5502",
        "scph5552",
        "scph7002",
        "scph7502",
        "scph9002",
        "scph102a",
        "scph102b",
    };
    static const std::array<const char*, 8> japan_models = {
        "scph1000",
        "scph3000",
        "scph3500",
        "scph5000",
        "scph5500",
        "scph7000",
        "scph7003",
        "scph100",
    };

    if (std::find(europe_models.begin(), europe_models.end(), model) != europe_models.end()) {
        return CDR_REGION_EUROPE;
    }

    if (std::find(japan_models.begin(), japan_models.end(), model) != japan_models.end()) {
        return CDR_REGION_JAPAN;
    }

    return CDR_REGION_AMERICA;
}

LaunchRequest LaunchForPath(const std::filesystem::path& path) {
    LaunchRequest request;
    if (path.empty()) {
        return request;
    }

    request.path = std::filesystem::path(NormalizePathString(path.string()));
    request.label = StemToTitle(request.path);

    if (IsExePath(request.path)) {
        request.kind = LaunchKind::Exe;
    } else if (IsDiscPath(request.path)) {
        request.kind = LaunchKind::Disc;
    }

    return request;
}

std::optional<LaunchKind> ParseUriLaunchKind(std::string_view value) {
    const std::string lower = ToLower(Trim(value));

    if (lower == "bios") {
        return LaunchKind::Bios;
    }
    if (lower == "disc" || lower == "cdrom") {
        return LaunchKind::Disc;
    }
    if (lower == "exe") {
        return LaunchKind::Exe;
    }

    return std::nullopt;
}

std::optional<std::string> QueryValue(std::string_view query, std::string_view key) {
    size_t begin = 0;

    while (begin <= query.size()) {
        const size_t end = query.find('&', begin);
        const std::string_view pair = query.substr(begin, end == std::string_view::npos ? query.size() - begin : end - begin);
        const size_t sep = pair.find('=');
        const std::string_view pair_key = pair.substr(0, sep);

        if (ToLower(PercentDecode(pair_key)) == ToLower(key)) {
            if (sep == std::string_view::npos) {
                return std::string();
            }
            return PercentDecode(pair.substr(sep + 1));
        }

        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }

    return std::nullopt;
}

std::string DecodeLaunchUriPathPayload(std::string_view payload) {
    std::string decoded = PercentDecode(payload);

    if (decoded.rfind("///", 0) == 0) {
        decoded.erase(0, 2);
    } else if (decoded.rfind("//", 0) == 0) {
        decoded.erase(0, 2);
    }

#if defined(_WIN32)
    if (decoded.size() >= 3 &&
        decoded[0] == '/' &&
        std::isalpha(static_cast<unsigned char>(decoded[1])) &&
        decoded[2] == ':') {
        decoded.erase(decoded.begin());
    }
#endif

    return NormalizePathString(decoded);
}

LaunchRequest LaunchForUri(std::string_view uri) {
    LaunchRequest request;

    std::string_view remainder = uri;
    if (StartsWithCaseInsensitive(uri, "armsx:")) {
        remainder = uri.substr(6);
    } else if (StartsWithCaseInsensitive(uri, "web+armsx:")) {
        remainder = uri.substr(10);
    } else {
        return request;
    }
    const size_t fragment_pos = remainder.find('#');
    if (fragment_pos != std::string_view::npos) {
        remainder = remainder.substr(0, fragment_pos);
    }

    std::string_view query;
    const size_t query_pos = remainder.find('?');
    if (query_pos != std::string_view::npos) {
        query = remainder.substr(query_pos + 1);
        remainder = remainder.substr(0, query_pos);
    }

    std::optional<LaunchKind> kind = std::nullopt;
    if (const std::optional<std::string> kind_value = QueryValue(query, "kind")) {
        kind = ParseUriLaunchKind(*kind_value);
    }

    if (kind == LaunchKind::Bios) {
        request.kind = LaunchKind::Bios;
        request.label = "PlayStation BIOS";
        return request;
    }

    std::string launch_path;
    if (const std::optional<std::string> query_path = QueryValue(query, "path")) {
        launch_path = NormalizePathString(*query_path);
    } else {
        launch_path = DecodeLaunchUriPathPayload(remainder);
    }

    const std::string launch_path_lower = ToLower(Trim(launch_path));
    if (launch_path_lower == "bios" || launch_path_lower == "/bios") {
        request.kind = LaunchKind::Bios;
        request.label = "PlayStation BIOS";
        return request;
    }

    request = LaunchForPath(std::filesystem::path(launch_path));
    if (request.kind == LaunchKind::None) {
        return request;
    }

    if (kind == LaunchKind::Disc || kind == LaunchKind::Exe) {
        request.kind = *kind;
    }

    return request;
}

LaunchRequest LaunchForArgument(std::string_view argument, std::optional<LaunchKind> forced_kind = std::nullopt) {
    LaunchRequest request;
    const std::string trimmed = Trim(argument);
    if (trimmed.empty()) {
        return request;
    }

    if (StartsWithCaseInsensitive(trimmed, "armsx:")) {
        request = LaunchForUri(trimmed);
    } else {
        const std::filesystem::path path(trimmed);
#ifdef USE_CHD
        if (IsZipPath(path)) {
            std::string archive_error;
            const auto extracted = armsx::ExtractZipLaunchCandidate(path, archive_error);
            if (!extracted.has_value()) {
                psxe_diag_logf(
                    "launch",
                    "ZIP launch failed path=%s error=%s",
                    path.string().c_str(),
                    archive_error.c_str()
                );
                return request;
            }
            request = LaunchForPath(*extracted);
        } else
#endif
        {
            request = LaunchForPath(path);
        }
    }

    if (request.kind == LaunchKind::None) {
        return request;
    }

    if (forced_kind == LaunchKind::Bios) {
        return LaunchRequest{.kind = LaunchKind::Bios, .path = {}, .label = "PlayStation BIOS"};
    }

    if ((forced_kind == LaunchKind::Disc || forced_kind == LaunchKind::Exe) && !request.path.empty()) {
        request.kind = *forced_kind;
    }

    return request;
}

std::vector<fsui::SettingsChoiceOption> BuildChoices(const std::vector<std::string>& titles, int selected_index) {
    std::vector<fsui::SettingsChoiceOption> options;
    options.reserve(titles.size());

    for (size_t index = 0; index < titles.size(); index++) {
        fsui::SettingsChoiceOption option;
        option.title = titles[index];
        option.selected = (static_cast<int>(index) == selected_index);
        options.push_back(std::move(option));
    }

    return options;
}

std::vector<fsui::SettingsChoiceOption> BuildCpuEngineChoices(psx_cpu_execution_mode_t mode) {
    return BuildChoices(
        {"Cached interpreter", "Reference interpreter"},
        mode == PSX_CPU_INTERPRETER ? 1 : 0
    );
}

#ifdef USE_HARDWARE
std::vector<fsui::SettingsChoiceOption> BuildGpuBackendChoices(GpuBackend current_backend) {
    return BuildChoices(
        {"SDL software", "SDL accelerated"},
        static_cast<int>(current_backend == GpuBackend::SDLAccelerated ? 1 : 0)
    );
}
#endif

std::vector<fsui::SettingsChoiceOption> BuildAspectChoices(int current) {
    return BuildChoices({"Classic", "Square", "Wide 16:9"}, current);
}

std::vector<fsui::SettingsChoiceOption> BuildUpscaleChoices(int current_height) {
    const std::vector<int> heights = {480, 720, 1080, 1440, 2160};
    const std::vector<std::string> labels = {"480p", "720p", "1080p", "1440p", "2160p"};
    int selected = 0;

    for (size_t index = 0; index < heights.size(); index++) {
        if (heights[index] == current_height) {
            selected = static_cast<int>(index);
            break;
        }
    }

    return BuildChoices(labels, selected);
}

std::vector<fsui::SettingsChoiceOption> BuildScaleChoices(int current_scale) {
    const std::vector<int> scales = {1, 2, 3, 4, 5, 6};
    std::vector<std::string> labels;
    labels.reserve(scales.size());

    int selected = 0;
    for (size_t index = 0; index < scales.size(); index++) {
        labels.push_back(std::to_string(scales[index]) + "x");
        if (scales[index] == current_scale) {
            selected = static_cast<int>(index);
        }
    }

    return BuildChoices(labels, selected);
}

std::vector<fsui::SettingsChoiceOption> BuildPresentationUpscaleChoices(int current_scale) {
    std::vector<std::string> labels;
    labels.reserve(8);
    for (int scale = 1; scale <= 8; scale++) {
        labels.push_back(std::to_string(scale) + "x");
    }
    return BuildChoices(labels, std::clamp(current_scale, 1, 8) - 1);
}

std::vector<fsui::SettingsChoiceOption> BuildRegionChoices(const std::string& current_region) {
    const std::vector<std::string> values = {"auto", "ntsc", "pal"};
    const std::vector<std::string> labels = {"Auto", "NTSC", "PAL"};
    const std::string current = ToLower(current_region);
    int selected = 0;

    for (size_t index = 0; index < values.size(); index++) {
        if (values[index] == current) {
            selected = static_cast<int>(index);
            break;
        }
    }

    return BuildChoices(labels, selected);
}

std::vector<std::string> ModelChoices() {
    return {
        "scph1000", "scph1001", "scph1002", "scph3000", "scph3500", "scph5000", "scph5500",
        "scph5501", "scph5502", "scph5552", "scph7000", "scph7001", "scph7002", "scph7003",
        "scph7501", "scph7502", "scph9002", "scph100", "scph101", "scph102a", "scph102b", "scph102c",
    };
}

std::vector<fsui::SettingsChoiceOption> BuildModelChoices(const std::string& current_model) {
    const std::vector<std::string> models = ModelChoices();
    const std::string current = NormalizeModel(current_model);
    int selected = 0;

    for (size_t index = 0; index < models.size(); index++) {
        if (NormalizeModel(models[index]) == current) {
            selected = static_cast<int>(index);
            break;
        }
    }

    return BuildChoices(models, selected);
}

std::vector<fsui::SettingsChoiceOption> BuildLogLevelChoices(int current_level) {
    const std::vector<std::string> labels = {"Trace", "Debug", "Info", "Warn", "Error", "Fatal"};
    const int clamped = std::clamp(current_level, static_cast<int>(LOG_TRACE), static_cast<int>(LOG_FATAL));
    return BuildChoices(labels, clamped);
}

std::time_t FileTimeToTimeT(const std::filesystem::file_time_type& value) {
    using namespace std::chrono;
    const auto adjusted = time_point_cast<system_clock::duration>(
        value - std::filesystem::file_time_type::clock::now() + system_clock::now()
    );
    return system_clock::to_time_t(adjusted);
}

void CopyTomlString(const toml_datum_t& datum, std::string& out) {
    if (!datum.ok || !datum.u.s) {
        return;
    }

    out = datum.u.s;
    free(datum.u.s);
}

void CopyTomlPathString(const toml_datum_t& datum, std::string& out) {
    if (!datum.ok || !datum.u.s) {
        return;
    }

    out = NormalizePathString(datum.u.s);
    free(datum.u.s);
}

void CopyTomlArrayStrings(const toml_array_t* array, std::vector<std::filesystem::path>& out) {
    out.clear();

    if (!array) {
        return;
    }

    const int count = toml_array_nelem(array);
    out.reserve(static_cast<size_t>(count));

    for (int index = 0; index < count; index++) {
        toml_datum_t value = toml_string_at(array, index);

        if (!value.ok || !value.u.s) {
            continue;
        }

        out.emplace_back(NormalizePathString(value.u.s));
        free(value.u.s);
    }
}

CliFlags ScanCliFlags(int argc, const char* argv[]) {
    CliFlags flags;

    for (int index = 1; index < argc; index++) {
        const std::string_view arg(argv[index] ? argv[index] : "");

        auto mark_value_opt = [&](const char* short_name, const char* long_name, bool* target) {
            const std::string_view short_opt(short_name);
            const std::string_view long_opt(long_name);

            if (arg == short_opt || arg == long_opt) {
                *target = true;
                index++;
                return true;
            }

            if (arg.starts_with(long_opt) && arg.size() > (long_opt.size() + 1) && arg[long_opt.size()] == '=') {
                *target = true;
                return true;
            }

            return false;
        };

        if (mark_value_opt("-b", "--bios", &flags.bios) ||
            mark_value_opt("-B", "--bios-folder", &flags.bios_folder) ||
            mark_value_opt("-M", "--model", &flags.model) ||
            mark_value_opt("-r", "--region", &flags.region) ||
            mark_value_opt("-s", "--scale", &flags.scale) ||
            mark_value_opt("-S", "--settings-file", &flags.settings_file) ||
            mark_value_opt("-L", "--log-level", &flags.log_level) ||
            mark_value_opt("-e", "--exp-rom", &flags.exp_rom) ||
            mark_value_opt("-x", "--exe", &flags.exe) ||
            mark_value_opt("", "--cpu-engine", &flags.cpu_engine) ||
            mark_value_opt("", "--cdrom", &flags.cdrom)) {
            continue;
        }

        if (arg == "-q" || arg == "--quiet") {
            flags.quiet = true;
            continue;
        }

        if (!arg.empty() && arg[0] != '-') {
            flags.cdrom = true;
        }
    }

    flags.has_boot_request = flags.cdrom || flags.exe;

    return flags;
}

void LoadExtraSettings(FrontendSettings& settings, const CliFlags& cli) {
    if (settings.settings_path.empty()) {
        return;
    }

    FILE* file = fopen(settings.settings_path.c_str(), "rb");

    if (!file) {
        return;
    }

    char error[256] = {};
    toml_table_t* root = toml_parse_file(file, error, sizeof(error));
    fclose(file);

    if (!root) {
        return;
    }

    if (toml_table_t* bios = toml_table_in(root, "bios")) {
        if (!cli.bios_folder) {
            CopyTomlPathString(toml_string_in(bios, "search_path"), settings.bios_search);
        }

        if (!cli.model) {
            CopyTomlString(toml_string_in(bios, "preferred_model"), settings.model);
        }

        if (!cli.bios) {
            CopyTomlPathString(toml_string_in(bios, "override_file"), settings.bios_override);
        }
    }

    if (toml_table_t* console = toml_table_in(root, "console")) {
        if (!cli.region) {
            CopyTomlString(toml_string_in(console, "region"), settings.region);
        }
    }

    if (toml_table_t* runtime = toml_table_in(root, "runtime")) {
        if (!cli.scale) {
            toml_datum_t value = toml_int_in(runtime, "display_scale");
            if (value.ok) {
                settings.scale = static_cast<int>(value.u.i);
            }
        }

        if (!cli.log_level) {
            toml_datum_t value = toml_int_in(runtime, "log_level");
            if (value.ok) {
                settings.log_level = static_cast<int>(value.u.i);
            }
        }

        if (!cli.quiet) {
            bool resolved_logging_enabled = false;
            toml_datum_t logging_enabled = toml_bool_in(runtime, "logging_enabled");
            if (logging_enabled.ok) {
                settings.logging_enabled = logging_enabled.u.b != 0;
                settings.quiet = !settings.logging_enabled;
                resolved_logging_enabled = true;
            }

            toml_datum_t value = toml_bool_in(runtime, "quiet");
            if (!resolved_logging_enabled && value.ok) {
                settings.quiet = value.u.b != 0;
                settings.logging_enabled = !settings.quiet;
            }
        }
    }

    if (toml_table_t* paths = toml_table_in(root, "paths")) {
        if (!cli.exp_rom) {
            CopyTomlPathString(toml_string_in(paths, "expansion_rom"), settings.exp_path);
        }

        if (!cli.exe) {
            CopyTomlPathString(toml_string_in(paths, "default_psx_exe"), settings.default_exe_path);
        }
    }

    if (toml_table_t* video = toml_table_in(root, "video")) {
        toml_datum_t vsync = toml_bool_in(video, "vsync");
        if (vsync.ok) {
            settings.vsync_enabled = vsync.u.b != 0;
        }

#ifdef USE_HARDWARE
        toml_datum_t gpu_backend = toml_string_in(video, "gpu_backend");
        if (gpu_backend.ok && gpu_backend.u.s) {
            if (std::optional<GpuBackend> backend = ParseGpuBackendOverride(gpu_backend.u.s)) {
                settings.gpu_backend = *backend;
            } else {
                settings.gpu_backend = GpuBackend::Software;
            }
            free(gpu_backend.u.s);
        }
#endif

        toml_datum_t texture_scale_mode = toml_bool_in(video, "texture_scale_mode");
        if (texture_scale_mode.ok) {
            settings.texture_scale_mode = texture_scale_mode.u.b != 0;
        }

        toml_datum_t presentation_upscale = toml_int_in(video, "presentation_upscale");
        if (presentation_upscale.ok) {
            settings.presentation_upscale = std::clamp(static_cast<int>(presentation_upscale.u.i), 1, 8);
        }

        toml_datum_t debug_panel = toml_bool_in(video, "debug_panel");
        if (debug_panel.ok) {
            settings.debug_panel = debug_panel.u.b != 0;
        }

        toml_datum_t stretch_mode = toml_bool_in(video, "stretch_mode");
        if (stretch_mode.ok) {
            settings.stretch_mode = stretch_mode.u.b != 0;
        }

        toml_datum_t display_aspect = toml_string_in(video, "display_aspect");
        if (display_aspect.ok && display_aspect.u.s) {
            const std::string value = ToLower(display_aspect.u.s);
            if (value == "square") {
                settings.display_aspect = 1;
            } else if (value == "wide16x9") {
                settings.display_aspect = 2;
            } else {
                settings.display_aspect = 0;
            }
            free(display_aspect.u.s);
        }

        toml_datum_t wide_upscale = toml_string_in(video, "wide_upscale");
        if (wide_upscale.ok && wide_upscale.u.s) {
            const std::string value = ToLower(wide_upscale.u.s);
            if (value == "720p") settings.upscale_height = 720;
            else if (value == "1080p") settings.upscale_height = 1080;
            else if (value == "1440p") settings.upscale_height = 1440;
            else if (value == "2160p") settings.upscale_height = 2160;
            else settings.upscale_height = 480;
            free(wide_upscale.u.s);
        }
    }

    if (toml_table_t* input = toml_table_in(root, "input")) {
        toml_datum_t mobile_controls = toml_bool_in(input, "mobile_controls");
        if (mobile_controls.ok) {
            settings.mobile_controls = mobile_controls.u.b != 0;
        }
    }

    if (toml_table_t* library = toml_table_in(root, "library")) {
        CopyTomlArrayStrings(toml_array_in(library, "folders"), settings.ui_state.game_list_paths);
        CopyTomlArrayStrings(toml_array_in(library, "recursive_folders"), settings.ui_state.game_list_recursive_paths);
    }

    if (toml_table_t* vr = toml_table_in(root, "vr")) {
        // Centimetres, like the FSUI choices that set them.
        toml_datum_t width_cm = toml_int_in(vr, "screen_width_cm");
        toml_datum_t distance_cm = toml_int_in(vr, "screen_distance_cm");
        toml_datum_t curved = toml_bool_in(vr, "curved");
        if (width_cm.ok) {
            settings.vr_screen.width_m = std::clamp(static_cast<float>(width_cm.u.i) / 100.0f, 0.5f, 8.0f);
        }
        if (distance_cm.ok) {
            settings.vr_screen.distance_m = std::clamp(static_cast<float>(distance_cm.u.i) / 100.0f, 0.5f, 6.0f);
        }
        if (curved.ok) {
            settings.vr_screen.curved = curved.u.b != 0;
        }
        toml_datum_t depth = toml_int_in(vr, "depth_3d_percent");
        toml_datum_t popout = toml_int_in(vr, "popout");
        if (depth.ok) {
            settings.vr_depth_percent = std::clamp(static_cast<int>(depth.u.i), 0, 200);
        }
        if (popout.ok) {
            settings.vr_popout = std::clamp(static_cast<int>(popout.u.i), 1, 16);
        }
    }

    if (toml_table_t* fsui_state = toml_table_in(root, "fsui")) {
        toml_datum_t default_view = toml_int_in(fsui_state, "default_game_view");
        toml_datum_t sort = toml_int_in(fsui_state, "game_sort");
        toml_datum_t reverse = toml_bool_in(fsui_state, "game_sort_reverse");

        if (default_view.ok) {
            settings.ui_state.default_game_view = static_cast<int>(default_view.u.i);
        }
        if (sort.ok) {
            settings.ui_state.game_sort = static_cast<int>(sort.u.i);
        }
        if (reverse.ok) {
            settings.ui_state.game_sort_reverse = reverse.u.b != 0;
        }
    }

    toml_free(root);
}

FrontendSettings BuildSettings(const psxe_config_t* cfg, const CliFlags& cli) {
    FrontendSettings settings;
#ifdef USE_HARDWARE
    std::optional<GpuBackend> env_gpu_backend;
#endif

    settings.settings_path = (cfg && cfg->settings_path && cfg->settings_path[0]) ? cfg->settings_path : DefaultSettingsPath();
    settings.bios_override = (cfg && cfg->bios && cfg->bios[0]) ? cfg->bios : "";
    settings.bios_search = (cfg && cfg->bios_search && cfg->bios_search[0]) ? cfg->bios_search : "bios";
    settings.model = (cfg && cfg->model && cfg->model[0]) ? cfg->model : "scph1001";
    settings.region = (cfg && cfg->region && cfg->region[0]) ? cfg->region : "auto";
    settings.exp_path = (cfg && cfg->exp_path && cfg->exp_path[0]) ? cfg->exp_path : "";
    settings.default_exe_path = (cfg && cfg->exe && cfg->exe[0]) ? cfg->exe : "";
    settings.scale = cfg ? cfg->scale : 3;
    settings.log_level = cfg ? cfg->log_level : LOG_INFO;
    settings.quiet = cfg ? (cfg->quiet != 0) : true;
    settings.logging_enabled = !settings.quiet;
    settings.vsync_enabled = cfg ? (cfg->vsync_enabled != 0) : DefaultVsyncEnabled();
    settings.cpu_engine = cfg && !cfg->cpu_engine ? PSX_CPU_INTERPRETER : PSX_CPU_CACHED_INTERPRETER;
    if (const char* env_cpu_engine = std::getenv("ARMSX_CPU_ENGINE")) {
        if (std::optional<psx_cpu_execution_mode_t> parsed = ParseCpuEngine(env_cpu_engine)) {
            settings.cpu_engine = *parsed;
        }
    }
#if defined(USE_HARDWARE)
    settings.gpu_backend = cfg && cfg->gpu_backend ? GpuBackend::SDLAccelerated : GpuBackend::Software;
    if (const char* env_backend = std::getenv("ARMSX_GPU_BACKEND")) {
        env_gpu_backend = ParseGpuBackendOverride(env_backend);
    }
#endif
    settings.texture_scale_mode = cfg ? (cfg->texture_scale_mode != 0) : false;
    settings.debug_panel = cfg ? (cfg->debug_panel != 0) : false;
    settings.stretch_mode = cfg ? (cfg->stretch_mode != 0) : false;
    settings.display_aspect = cfg ? cfg->display_aspect : 0;
    settings.upscale_height = cfg ? cfg->upscale_height : 480;
    settings.presentation_upscale = 1;
    settings.mobile_controls = DefaultMobileControlsEnabled();
    settings.ui_state.show_settings_overlay = settings.debug_panel;
    settings.ui_state.show_performance_overlay = settings.debug_panel;

    if (!cli.bios && !settings.bios_override.empty()) {
        const std::filesystem::path bios_path(settings.bios_override);
        if (!std::filesystem::exists(bios_path) && (settings.bios_override == "bios.bin")) {
            settings.bios_override.clear();
        }
    }

    LoadExtraSettings(settings, cli);

#if defined(USE_HARDWARE)
    if (env_gpu_backend) {
        settings.gpu_backend = *env_gpu_backend;
    }
#endif
    if (const char* env_upscale = std::getenv("ARMSX_PRESENTATION_UPSCALE")) {
        char* end = nullptr;
        const long scale = std::strtol(env_upscale, &end, 10);
        if (end && *end == '\0') {
            settings.presentation_upscale = std::clamp(static_cast<int>(scale), 1, 8);
        }
    }
    if (const char* env_mobile_controls = std::getenv("ARMSX_MOBILE_CONTROLS")) {
        const std::string value = ToLower(env_mobile_controls);
        settings.mobile_controls = value == "1" || value == "true" || value == "on" || value == "yes";
    }

    settings.scale = std::max(1, settings.scale);
    settings.presentation_upscale = std::clamp(settings.presentation_upscale, 1, 8);
    settings.log_level = std::clamp(settings.log_level, static_cast<int>(LOG_TRACE), static_cast<int>(LOG_FATAL));
    settings.logging_enabled = !settings.quiet;
    settings.ui_state.show_settings_overlay = settings.debug_panel;
    settings.ui_state.show_performance_overlay = settings.debug_panel;

    if (settings.ui_state.covers_path.empty()) {
        settings.ui_state.covers_path = DefaultBrowseDirectory() / "covers";
    }

    return settings;
}

bool SaveSettings(const FrontendSettings& settings) {
    const std::filesystem::path target = settings.settings_path.empty() ? std::filesystem::path(DefaultSettingsPath()) : std::filesystem::path(settings.settings_path);
    std::filesystem::path temp = target;
    temp += ".tmp";

    try {
        if (target.has_parent_path()) {
            std::filesystem::create_directories(target.parent_path());
        }
    } catch (...) {
        return false;
    }

    std::ofstream out(temp, std::ios::binary | std::ios::trunc);

    if (!out.is_open()) {
        return false;
    }

    auto write_path_array = [&](const std::vector<std::filesystem::path>& paths) {
        out << "[";
        for (size_t index = 0; index < paths.size(); index++) {
            if (index) {
                out << ", ";
            }
            out << "\"" << EscapeTomlString(paths[index].string()) << "\"";
        }
        out << "]";
    };

    out
        << "# Settings file generated by ARMSX\n\n"
        << "psxe_version = \"" << STR(REP_VERSION) << "\"\n\n"
        << "[bios]\n"
        << "    search_path     = \"" << EscapeTomlString(settings.bios_search) << "\"\n"
        << "    preferred_model = \"" << EscapeTomlString(settings.model) << "\"\n"
        << "    override_file   = \"" << EscapeTomlString(settings.bios_override) << "\"\n\n"
        << "[console]\n"
        << "    region          = \"" << EscapeTomlString(settings.region) << "\"\n\n"
        << "[cpu]\n"
        << "    execution_mode  = \"" << CpuEngineSettingToken(settings.cpu_engine) << "\"\n\n"
        << "[runtime]\n"
        << "    display_scale = " << settings.scale << "\n"
        << "    logging_enabled = " << (settings.logging_enabled ? "true" : "false") << "\n"
        << "    log_level = " << settings.log_level << "\n"
        << "    quiet = " << (settings.logging_enabled ? "false" : "true") << "\n\n"
        << "[paths]\n"
        << "    expansion_rom = \"" << EscapeTomlString(settings.exp_path) << "\"\n"
        << "    default_psx_exe = \"" << EscapeTomlString(settings.default_exe_path) << "\"\n\n"
        << "[video]\n"
        << "    vsync = " << (settings.vsync_enabled ? "true" : "false") << "\n"
#ifdef USE_HARDWARE
        << "    gpu_backend = \"" << GpuBackendSettingToken(settings.gpu_backend) << "\"\n"
#endif
        << "    texture_scale_mode = " << (settings.texture_scale_mode ? "true" : "false") << "\n"
        << "    presentation_upscale = " << settings.presentation_upscale << "\n"
        << "    debug_panel = " << (settings.debug_panel ? "true" : "false") << "\n"
        << "    stretch_mode = " << (settings.stretch_mode ? "true" : "false") << "\n"
        << "    display_aspect = \"" << AspectToString(settings.display_aspect) << "\"\n"
        << "    wide_upscale = \"" << UpscaleToString(settings.upscale_height) << "\"\n\n"
        << "[input]\n"
        << "    mobile_controls = " << (settings.mobile_controls ? "true" : "false") << "\n\n"
        << "[vr]\n"
        << "    screen_width_cm = " << std::lround(settings.vr_screen.width_m * 100.0f) << "\n"
        << "    screen_distance_cm = " << std::lround(settings.vr_screen.distance_m * 100.0f) << "\n"
        << "    curved = " << (settings.vr_screen.curved ? "true" : "false") << "\n"
        << "    depth_3d_percent = " << settings.vr_depth_percent << "\n"
        << "    popout = " << settings.vr_popout << "\n\n"
        << "[library]\n"
        << "    folders = ";
    write_path_array(settings.ui_state.game_list_paths);
    out << "\n    recursive_folders = ";
    write_path_array(settings.ui_state.game_list_recursive_paths);
    out
        << "\n\n[fsui]\n"
        << "    default_game_view = " << settings.ui_state.default_game_view << "\n"
        << "    game_sort = " << settings.ui_state.game_sort << "\n"
        << "    game_sort_reverse = " << (settings.ui_state.game_sort_reverse ? "true" : "false") << "\n";

    out.flush();
    if (!out.good()) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return false;
    }

    out.close();
    if (!out.good()) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return false;
    }

#if defined(_WIN32)
    if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
#endif

    return true;
}

// CD audio plus the SPU samples the core produced while the CPU ran this frame (psx_read_audio),
// summed with saturation. `size` bytes of 16-bit stereo; the core's samples fill as much as they
// cover.
void MixPsxAudio(psx_t* psx, uint8_t* buffer, int size) {
    if (!psx || !buffer || size <= 0) {
        return;
    }

    psx_cdrom_t* cdrom = psx->cdrom;
    psx_spu_t* spu = psx->spu;

    std::memset(buffer, 0, static_cast<size_t>(size));

    psx_cdrom_get_audio_samples(cdrom, buffer, size);
    psx_spu_update_cdda_buffer(spu, cdrom->cdda_buf);

    const int frames = size >> 2;
    std::vector<int16_t> spu_samples(static_cast<size_t>(frames) * 2);
    const int produced = psx_read_audio(psx, spu_samples.data(), frames);

    auto* out = reinterpret_cast<int16_t*>(buffer);
    for (int i = 0; i < produced * 2; i++) {
        out[i] = static_cast<int16_t>(std::clamp(static_cast<int>(out[i]) + spu_samples[static_cast<size_t>(i)], -32768, 32767));
    }
}

class ArmsxSession {
  public:
    ~ArmsxSession() {
        destroy();
    }

    bool create(SDL_Renderer* renderer, const FrontendSettings& settings, const LaunchRequest& request, std::string& error) {
        destroy();

        if (!renderer) {
            error = "Display is not ready.";
            return false;
        }

        renderer_ = renderer;
        vblank_counter_ = 0;

        psxe_config_t cfg{};
        cfg.bios = settings.bios_override.empty() ? nullptr : settings.bios_override.c_str();
        cfg.bios_search = settings.bios_search.c_str();
        cfg.model = settings.model.c_str();

        char* bios_path = psxe_cfg_get_bios_path(&cfg);
        std::unique_ptr<char, decltype(&free)> bios_guard(bios_path, &free);

        if (!bios_path || !bios_path[0]) {
            error = "No BIOS could be resolved. Set a BIOS override file or a BIOS folder that contains the selected model.";
            return false;
        }

        psx_ = psx_create();
        if (!psx_) {
            error = "Failed to allocate a PSX session.";
            return false;
        }

        const char* expansion = settings.exp_path.empty() ? nullptr : settings.exp_path.c_str();
        const int init_result = psx_init(psx_, bios_path, expansion);

        if (init_result != 0) {
            error = "Failed to initialize the emulator core. Check the BIOS and expansion ROM paths.";
            destroy();
            return false;
        }

        psx_cpu_set_execution_mode(psx_get_cpu(psx_), settings.cpu_engine);
        psxe_diag_logf("cpu", "execution engine=%s", CpuEngineSettingToken(settings.cpu_engine));
        psx_set_stereo_depth(psx_, stereo_enabled_ ? 1 : 0);

        psx_gpu_t* gpu = psx_get_gpu(psx_);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_DMODE, nullptr);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_VBLANK, SessionVblankEvent);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_HBLANK, psxe_gpu_hblank_event_cb);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_VBLANK_END, psxe_gpu_vblank_end_event_cb);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_HBLANK_END, psxe_gpu_hblank_end_event_cb);
        psx_gpu_set_udata(gpu, 0, this);
        psx_gpu_set_udata(gpu, 1, psx_->timer);
        psx_gpu_set_udata(gpu, 2, nullptr);

#ifdef USE_HARDWARE
        hardware_backend_active_ = settings.gpu_backend == GpuBackend::SDLAccelerated;
        if (hardware_backend_active_) {
            SDL_RendererInfo renderer_info{};
            if (SDL_GetRendererInfo(renderer_, &renderer_info) != 0 ||
                !(renderer_info.flags & SDL_RENDERER_ACCELERATED)) {
                psxe_diag_logf("renderer", "SDL acceleration requested but unavailable; using SDL software presentation.");
                hardware_backend_active_ = false;
            } else {
                psxe_diag_logf(
                    "renderer",
                    "SDL accelerated presentation enabled driver=%s; PlayStation VRAM remains software-authoritative.",
                    renderer_info.name ? renderer_info.name : "(unknown)"
                );
            }
        }
#endif

        psx_cdrom_set_region(psx_get_cdrom(psx_), RegionFromSettings(settings));

        input_ = psx_input_create();
        if (input_) {
            psx_input_init(input_);
        }
        pad_device_ = psxi_sda_create();

        if (!input_ || !pad_device_) {
            error = "Failed to create the controller input bridge.";
            destroy();
            return false;
        }

        psxi_sda_init(pad_device_, SDA_MODEL_DIGITAL);
        psxi_sda_init_input(pad_device_, input_);
        psx_pad_attach_joy(psx_->pad, 0, input_);
        pad_device_ = nullptr;

        const std::string slot1 = std::string(psxe_cfg_get_pref_path() ? psxe_cfg_get_pref_path() : "") + "slot1.mcd";
        const std::string slot2 = std::string(psxe_cfg_get_pref_path() ? psxe_cfg_get_pref_path() : "") + "slot2.mcd";
        psx_pad_attach_mcd(psx_->pad, 0, slot1.c_str());
        psx_pad_attach_mcd(psx_->pad, 1, slot2.c_str());

        switch (request.kind) {
            case LaunchKind::Disc:
                if (psx_cdrom_open(psx_get_cdrom(psx_), request.path.string().c_str()) == 0) {
                    error = "Failed to open the selected disc image.";
                    destroy();
                    return false;
                }
                disc_path_ = request.path;
                exe_path_.clear();
                title_ = request.label.empty() ? StemToTitle(request.path) : request.label;
                break;

            case LaunchKind::Exe:
                while (psx_->cpu->pc != 0x80030000) {
                    psx_update(psx_);
                }
                psx_load_exe(psx_, request.path.string().c_str());
                exe_path_ = request.path;
                disc_path_.clear();
                title_ = request.label.empty() ? StemToTitle(request.path) : request.label;
                break;

            case LaunchKind::Bios:
                disc_path_.clear();
                exe_path_.clear();
                title_ = "PlayStation BIOS";
                break;

            case LaunchKind::None:
            default:
                error = "Invalid launch request.";
                destroy();
                return false;
        }

        launch_kind_ = request.kind;
#if defined(HW_DEBUG)
#ifdef USE_HARDWARE
        if (hardware_backend_active_ && psx_ && psx_->gpu) {
            int output_width = 0;
            int output_height = 0;
            SDL_GetRendererOutputSize(renderer_, &output_width, &output_height);
            psxe_diag_logf(
                "hw",
                "session-create kind=%s title=%s renderer=%p output=%dx%d texture=%dx%d timing=%s target_fps=%.3f display_mode=0x%08x gpustat=0x%08x",
                LaunchKindTitle(request.kind),
                title_.c_str(),
                (void*)renderer_,
                output_width,
                output_height,
                texture_width_,
                texture_height_,
                timingModeTitle(),
                targetFrameRate(),
                psx_->gpu->display_mode,
                psx_->gpu->gpustat
            );
        }
#endif
#endif
        psxe_diag_breadcrumbf(
            "Session created kind=%s title=%s path=%s bios=%s model=%s region=%s",
            LaunchKindTitle(request.kind),
            title_.c_str(),
            request.path.empty() ? "(none)" : request.path.string().c_str(),
            bios_path,
            settings.model.c_str(),
            settings.region.c_str()
        );

        SDL_AudioSpec desired{};
        SDL_AudioSpec obtained{};
        desired.freq = 44100;
        desired.format = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = 1024;
#if defined(__ANDROID__)
        // SDL's OpenSL ES output keeps just two buffers of this size. At 1024 frames that is 46 ms
        // in all, and a short stall of its thread on a busy device (a VR headset especially) empties
        // them: every such gap is an audible click.
        desired.samples = 2048;
#endif
        desired.callback = nullptr;
        desired.userdata = nullptr;

        {
            const armsx::vr::ScopedUnpinned unpinned;
            audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        }
        paused_ = false;
        fast_forward_enabled_ = false;
        debug_view_ = false;
        texture_width_ = 0;
        texture_height_ = 0;
        texture_format_ = SDL_PIXELFORMAT_UNKNOWN;
        if (audio_dev_) {
            audio_sample_accumulator_ = 0.0;
            audio_playback_started_ = false;
            psxe_diag_logf(
                "audio",
                "SDL queued audio opened frequency=%d channels=%u samples=%u prime_bytes=%zu max_bytes=%zu",
                obtained.freq,
                static_cast<unsigned>(obtained.channels),
                static_cast<unsigned>(obtained.samples),
                kAudioPrimeBytes,
                kMaxQueuedAudioBytes
            );
            updateAudioPlaybackState();
        }
        updateTexture(settings);

        return true;
    }

    void destroy() {
        if (psx_) {
            psxe_diag_breadcrumbf(
                "Session destroyed kind=%s title=%s path=%s",
                LaunchKindTitle(launch_kind_),
                title_.empty() ? "(none)" : title_.c_str(),
                disc_path_.empty() ? (exe_path_.empty() ? "(none)" : exe_path_.string().c_str()) : disc_path_.string().c_str()
            );
        }

        if (audio_dev_) {
            SDL_PauseAudioDevice(audio_dev_, 1);
            SDL_ClearQueuedAudio(audio_dev_);
            SDL_CloseAudioDevice(audio_dev_);
            audio_dev_ = 0;
        }

        if (texture_) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }
        destroyPresentationTexture();
        texture_snapshot_.clear();
        destroyStereoTexture();

#ifdef USE_HARDWARE
        if (hw_renderer_) {
            armsx_hw_renderer_destroy(hw_renderer_);
            hw_renderer_ = nullptr;
        }
#endif

        const bool input_attached = psx_ && psx_->pad && (psx_->pad->joy_slot[0] == input_);

        if (psx_) {
            psx_destroy(psx_);
            psx_ = nullptr;
        }

        if (input_ && !input_attached) {
            psx_input_destroy(input_);
        }

        if (pad_device_) {
            psxi_sda_destroy(pad_device_);
            pad_device_ = nullptr;
        }

        input_ = nullptr;
        renderer_ = nullptr;
        disc_path_.clear();
        exe_path_.clear();
        title_.clear();
        launch_kind_ = LaunchKind::None;
        paused_ = false;
        fast_forward_enabled_ = false;
        debug_view_ = false;
        vblank_counter_ = 0;
#ifdef USE_HARDWARE
        hardware_backend_active_ = false;
#endif
    }

    bool valid() const {
        return psx_ != nullptr;
    }

    psx_t* psx() const {
        return psx_;
    }

    psx_pad_t* pad() const {
        return psx_ ? psx_->pad : nullptr;
    }

    void rebindRenderer(SDL_Renderer* renderer, const FrontendSettings& settings) {
        renderer_ = renderer;
#ifdef USE_HARDWARE
        if (hw_renderer_) {
            armsx_hw_renderer_set_renderer(hw_renderer_, renderer_);
            if (texture_width_ > 0 && texture_height_ > 0) {
                armsx_hw_renderer_set_output_size(hw_renderer_, texture_width_, texture_height_);
            }
        }
#endif
        if (texture_) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }
        destroyPresentationTexture();
        texture_snapshot_.clear();
        texture_width_ = 0;
        texture_height_ = 0;
        texture_format_ = SDL_PIXELFORMAT_UNKNOWN;
        updateTexture(settings);
    }

    void setPaused(bool paused) {
        paused_ = paused;
        if (audio_dev_) {
            if (paused || fast_forward_enabled_) {
                clearQueuedAudio();
            }
            updateAudioPlaybackState();
        }
    }

    bool paused() const {
        return paused_;
    }

    void setFastForwardEnabled(bool enabled) {
        if (fast_forward_enabled_ == enabled) {
            return;
        }

        fast_forward_enabled_ = enabled;
        clearQueuedAudio();

        if (audio_dev_) {
            updateAudioPlaybackState();
        }
    }

    // For the VR performance probe: how much audio is waiting, and how often it ran dry.
    double queuedAudioMs() const {
        if (!audio_dev_ || !audio_playback_started_) {
            return -1.0;
        }
        return static_cast<double>(SDL_GetQueuedAudioSize(audio_dev_)) * 1000.0 /
            static_cast<double>(kAudioMixRate * kAudioBytesPerSampleFrame);
    }

    std::uint64_t audioUnderruns() const {
        return audio_underruns_;
    }

    bool fastForwardEnabled() const {
        return fast_forward_enabled_;
    }

    std::uint32_t runFrame() {
        if (!psx_ || paused_) {
            return 0;
        }

#if defined(HW_DEBUG)
        traceHardwareFrameState("frame-start", 0);
#endif

#ifdef USE_HARDWARE
        if (hardware_backend_active_ && hw_renderer_) {
            armsx_hw_renderer_begin_frame(hw_renderer_);
        }
#endif

        const std::uint64_t start_vblank = vblank_counter_;
        std::uint32_t steps = 0;

        while (vblank_counter_ == start_vblank) {
            psx_update(psx_);
            steps++;

            if (steps >= kMaxFrameSteps) {
                psxe_diag_logf("timing", "Session frame advance exceeded vblank step budget title=%s", title_.empty() ? "(none)" : title_.c_str());
#if defined(HW_DEBUG)
                traceHardwareFrameState("frame-budget-exceeded", steps);
#endif
                return steps;
            }
        }

        queueAudioForFrame();

#if defined(HW_DEBUG)
        traceHardwareFrameState("frame-end", steps);
#endif
        return steps;
    }

    void setDebugView(bool enabled) {
        debug_view_ = enabled;
    }

    bool debugView() const {
        return debug_view_;
    }

    bool reset() {
        if (!psx_) {
            return false;
        }

        psxe_diag_breadcrumbf("Soft reset requested title=%s", title_.empty() ? "(none)" : title_.c_str());
        setFastForwardEnabled(false);
        psx_soft_reset(psx_);
        return true;
    }

    bool swapDisc(const std::filesystem::path& path) {
        if (!psx_) {
            return false;
        }

        if (psx_swap_disc(psx_, path.string().c_str()) == 0) {
            return false;
        }

        setFastForwardEnabled(false);
        disc_path_ = path;
        launch_kind_ = LaunchKind::Disc;
        title_ = StemToTitle(path);
        psxe_diag_breadcrumbf("Disc swapped path=%s", path.string().c_str());
        psx_soft_reset(psx_);
        return true;
    }

    fsui::CurrentGameInfo currentGameInfo() const {
        fsui::CurrentGameInfo info;

        if (!psx_) {
            return info;
        }

        info.has_game = true;
        info.title = title_.empty() ? "ARMSX" : title_;
        info.subtitle = launch_kind_ == LaunchKind::Bios ? "No disc inserted" : (disc_path_.empty() ? exe_path_.filename().string() : disc_path_.filename().string());
        info.title_id = NormalizeModel(info.title);
        info.path = launch_kind_ == LaunchKind::Bios ? std::filesystem::path("bios://boot") : (disc_path_.empty() ? exe_path_ : disc_path_);
        return info;
    }

    std::vector<fsui::OverlayTextLine> settingsOverlayLines(const FrontendSettings& settings) const {
        std::vector<fsui::OverlayTextLine> lines;
        lines.push_back(fsui::OverlayTextLine{.text = std::string("Aspect: ") + AspectTitle(settings.display_aspect)});
        lines.push_back(fsui::OverlayTextLine{.text = std::string("Video: ") + RendererValueTitle(settings)});
        lines.push_back(fsui::OverlayTextLine{.text = std::string("VSync: ") + BoolTitle(settings.vsync_enabled)});
        lines.push_back(fsui::OverlayTextLine{.text = std::string("Debug View: ") + BoolTitle(debug_view_)});
        lines.push_back(fsui::OverlayTextLine{.text = std::string("Fast Forward: ") + (fast_forward_enabled_ ? "2x" : "Off")});
        return lines;
    }

    std::vector<fsui::OverlayTextLine> performanceOverlayLines(const FrontendSettings& settings) const {
        (void)settings;
        std::vector<fsui::OverlayTextLine> lines;
        char fps[64] = {};
        char video[64] = {};
        std::snprintf(fps, sizeof(fps), "FPS: %.1f", ImGui::GetIO().Framerate);
        std::snprintf(video, sizeof(video), "Video: %dx%d", texture_width_, texture_height_);
        lines.push_back(fsui::OverlayTextLine{.text = fps});
        lines.push_back(fsui::OverlayTextLine{.text = video});
        lines.push_back(fsui::OverlayTextLine{.text = std::string("State: ") + (paused_ ? "Paused" : "Running")});
        if (fast_forward_enabled_) {
            lines.push_back(fsui::OverlayTextLine{.text = "Speed: 2x"});
        }
        return lines;
    }

    void updateTexture(const FrontendSettings& settings) {
        if (!psx_ || !renderer_) {
            return;
        }

        int next_width = debug_view_ ? PSX_GPU_FB_WIDTH : static_cast<int>(psx_get_display_width(psx_));
        int next_height = debug_view_ ? PSX_GPU_FB_HEIGHT : static_cast<int>(psx_get_display_height(psx_));
        if (next_width <= 0) {
            next_width = texture_width_ > 0 ? texture_width_ : 320;
        }
        if (next_height <= 0) {
            next_height = texture_height_ > 0 ? texture_height_ : 240;
        }
        const Uint32 next_format = debug_view_ || !psx_get_display_format(psx_) ? SDL_PIXELFORMAT_BGR555 : SDL_PIXELFORMAT_RGB24;
        const bool use_vram_source = debug_view_ || !psx_ || !psx_->gpu ? false : ((psx_->gpu->disp_y + next_height) > PSX_GPU_FB_HEIGHT);

#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (hardware_backend_active_) {
            int output_width = 0;
            int output_height = 0;
            SDL_GetRendererOutputSize(renderer_, &output_width, &output_height);
            psxe_diag_logf(
                "hw",
                "texture-select frame=%llu display_mode=0x%08x gpustat=0x%08x display_enable=%d dmode=%ux%u display=%ux%u aspect=%.3f disp_window=(%u,%u)-(%u,%u) disp_y=%u offset=(%d,%d) output=%dx%d next=%dx%d format=%s source=%s stretch=%s debug=%s timing=%s target_fps=%.3f",
                static_cast<unsigned long long>(vblank_counter_),
                psx_ && psx_->gpu ? psx_->gpu->display_mode : 0u,
                psx_ && psx_->gpu ? psx_->gpu->gpustat : 0u,
                psx_ && psx_->gpu ? psx_->gpu->display_enable : 0,
                psx_ ? psx_get_dmode_width(psx_) : 0u,
                psx_ ? psx_get_dmode_height(psx_) : 0u,
                psx_ ? psx_get_display_width(psx_) : 0u,
                psx_ ? psx_get_display_height(psx_) : 0u,
                psx_ ? psx_get_display_aspect(psx_) : 0.0,
                psx_ && psx_->gpu ? psx_->gpu->disp_x1 : 0u,
                psx_ && psx_->gpu ? psx_->gpu->disp_y1 : 0u,
                psx_ && psx_->gpu ? psx_->gpu->disp_x2 : 0u,
                psx_ && psx_->gpu ? psx_->gpu->disp_y2 : 0u,
                psx_ && psx_->gpu ? psx_->gpu->disp_y : 0u,
                psx_ && psx_->gpu ? psx_->gpu->off_x : 0,
                psx_ && psx_->gpu ? psx_->gpu->off_y : 0,
                output_width,
                output_height,
                next_width,
                next_height,
                SDL_GetPixelFormatName(next_format),
                use_vram_source ? "vram" : "display",
                settings.stretch_mode ? "true" : "false",
                debug_view_ ? "true" : "false",
                timingModeTitle(),
                targetFrameRate()
            );
        }
#endif

        if ((next_width != texture_width_) || (next_height != texture_height_) || (next_format != texture_format_) || !texture_) {
            if (texture_) {
                SDL_DestroyTexture(texture_);
                texture_ = nullptr;
            }

            texture_ = SDL_CreateTexture(renderer_, next_format, SDL_TEXTUREACCESS_STREAMING, next_width, next_height);
            texture_width_ = next_width;
            texture_height_ = next_height;
            texture_format_ = next_format;
            texture_snapshot_.clear();

#ifdef USE_HARDWARE
            if (hw_renderer_) {
                armsx_hw_renderer_set_output_size(hw_renderer_, texture_width_, texture_height_);
            }
#endif
        }

        if (!texture_) {
            return;
        }

        if (settings.texture_scale_mode) {
#if SDL_VERSION_ATLEAST(2, 0, 12)
            SDL_SetTextureScaleMode(texture_, SDL_ScaleModeLinear);
#endif
        } else {
#if SDL_VERSION_ATLEAST(2, 0, 12)
            SDL_SetTextureScaleMode(texture_, SDL_ScaleModeNearest);
#endif
        }

        void* display_buffer = use_vram_source ? psx_get_vram(psx_) : psx_get_display_buffer(psx_);
        const int bytes_per_pixel = SDL_BYTESPERPIXEL(texture_format_);
        const size_t row_bytes = static_cast<size_t>(texture_width_) * static_cast<size_t>(bytes_per_pixel);
        const size_t snapshot_size = static_cast<size_t>(PSX_GPU_FB_STRIDE) * static_cast<size_t>(texture_height_);
        const auto* source = static_cast<const uint8_t*>(display_buffer);

        int first_dirty_row = 0;
        int last_dirty_row = texture_height_ - 1;
        if (texture_snapshot_.size() == snapshot_size) {
            first_dirty_row = texture_height_;
            last_dirty_row = -1;
            for (int row = 0; row < texture_height_; ++row) {
                const size_t offset = static_cast<size_t>(row) * static_cast<size_t>(PSX_GPU_FB_STRIDE);
                if (std::memcmp(source + offset, texture_snapshot_.data() + offset, row_bytes) != 0) {
                    first_dirty_row = std::min(first_dirty_row, row);
                    last_dirty_row = row;
                }
            }
        } else {
            texture_snapshot_.assign(snapshot_size, 0);
        }

        if (last_dirty_row >= first_dirty_row) {
            SDL_Rect dirty_rect{
                0,
                first_dirty_row,
                texture_width_,
                last_dirty_row - first_dirty_row + 1,
            };
            const size_t first_offset = static_cast<size_t>(first_dirty_row) * static_cast<size_t>(PSX_GPU_FB_STRIDE);
            SDL_UpdateTexture(texture_, &dirty_rect, source + first_offset, PSX_GPU_FB_STRIDE);
            for (int row = first_dirty_row; row <= last_dirty_row; ++row) {
                const size_t offset = static_cast<size_t>(row) * static_cast<size_t>(PSX_GPU_FB_STRIDE);
                std::memcpy(texture_snapshot_.data() + offset, source + offset, row_bytes);
            }
        }

        updatePresentationTexture(settings);
        updateStereoFrame(settings, use_vram_source ? nullptr : static_cast<const uint16_t*>(display_buffer));
    }

    // VR stereoscopic 3D (see frontend/vr/stereo_reproject.h). Depth capture in the core follows
    // `enabled`; the rest only shapes the parallax. Takes effect on the next emulated frame.
    void setStereo(bool enabled, float strength, float convergence, float screen_width_m) {
        stereo_enabled_ = enabled;
        stereo_strength_ = strength;
        stereo_convergence_ = convergence;
        stereo_screen_width_m_ = screen_width_m;
        if (psx_) {
            psx_set_stereo_depth(psx_, enabled ? 1 : 0);
        }
        if (!enabled) {
            stereo_ready_ = false;
        }
    }

    // Save states (psx/savestate.c). The file name is derived from the running game.
    std::string stateKey() const {
        const std::filesystem::path source = disc_path_.empty() ? exe_path_ : disc_path_;
        return source.empty() ? std::string("bios") : source.stem().string();
    }

    bool saveState(const std::filesystem::path& path) {
        if (!psx_) {
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        return psx_state_save(psx_, path.string().c_str()) == 0;
    }

    bool loadState(const std::filesystem::path& path) {
        if (!psx_ || psx_state_load(psx_, path.string().c_str()) != 0) {
            return false;
        }
        // Nothing queued from before belongs to the restored moment.
        clearQueuedAudio();
        texture_snapshot_.clear();
        finishStereoJob(true);
        stereo_ready_ = false;
        return true;
    }

    // True when the latest frame has two eye images (a 3D game frame, not FMV or a 2D screen).
    bool stereoReady() const {
        return stereo_ready_ && stereo_texture_[0] && stereo_texture_[1];
    }

    // draw() places the left eye's texture; rendering the right eye swaps in this one.
    SDL_Texture* stereoTexture(int eye) const {
        return stereo_texture_[eye == 0 ? 0 : 1];
    }

    void draw(const FrontendSettings& settings) {
        if (!texture_) {
            return;
        }

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const ImVec2 framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale;
        const float display_width = std::max(display.x, 1.0f);
        const float display_height = std::max(display.y, 1.0f);

        float aspect = 4.0f / 3.0f;
        if (debug_view_) {
            aspect = static_cast<float>(texture_width_) / static_cast<float>(std::max(texture_height_, 1));
        } else if (settings.display_aspect == 2) {
            aspect = 16.0f / 9.0f;
        } else if (settings.display_aspect == 1) {
            aspect = 1.0f;
        } else {
            aspect = static_cast<float>(psx_get_display_aspect(psx_));
        }

        float target_width = display_width;
        float target_height = display_height;
        float offset_x = 0.0f;
        float offset_y = 0.0f;

        if (!settings.stretch_mode) {
            target_width = display_width;
            target_height = target_width / aspect;

            if (target_height > display_height) {
                target_height = display_height;
                target_width = target_height * aspect;
            }

            offset_x = (display_width - target_width) * 0.5f;
            offset_y = (display_height - target_height) * 0.5f;
        }

        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        psxe_diag_logf(
            "ui",
            "session-draw frame=%llu display=(%.1f,%.1f) fb_scale=(%.2f,%.2f) texture=%dx%d aspect=%.3f stretch=%s debug=%s target=(%.1f,%.1f) offset=(%.1f,%.1f) hardware=%s timing=%s",
            static_cast<unsigned long long>(vblank_counter_),
            display.x,
            display.y,
            framebuffer_scale.x,
            framebuffer_scale.y,
            texture_width_,
            texture_height_,
            aspect,
            settings.stretch_mode ? "true" : "false",
            debug_view_ ? "true" : "false",
            target_width,
            target_height,
            offset_x,
            offset_y,
#ifdef USE_HARDWARE
            hardware_backend_active_ ? "true" : "false",
#else
            "false",
#endif
            timingModeTitle()
        );
#endif
        draw_list->AddRectFilled(ImVec2(0.0f, 0.0f), display, IM_COL32(0, 0, 0, 255));
        stereo_image_fraction_ = target_width / display_width;
        SDL_Texture* presentation = stereoReady() ? stereo_texture_[0] : (presentation_texture_ ? presentation_texture_ : texture_);
        draw_list->AddImage(
            reinterpret_cast<ImTextureID>(presentation),
            ImVec2(offset_x, offset_y),
            ImVec2(offset_x + target_width, offset_y + target_height)
        );

#ifdef USE_HARDWARE
        if (!debug_view_ && hw_renderer_) {
            if (SDL_Texture* overlay = armsx_hw_renderer_overlay_texture(hw_renderer_)) {
                draw_list->AddImage(
                    reinterpret_cast<ImTextureID>(overlay),
                    ImVec2(offset_x, offset_y),
                    ImVec2(offset_x + target_width, offset_y + target_height)
                );
            }
        }
#endif
    }

    void finishHardwareFrame() {
#ifdef USE_HARDWARE
        if (hardware_backend_active_ && hw_renderer_) {
            armsx_hw_renderer_end_frame(hw_renderer_);
        }
#endif
    }

#if defined(HW_DEBUG)
    void traceHardwareFrameState(const char* stage, std::uint32_t steps) const {
#ifdef USE_HARDWARE
        if (!hardware_backend_active_ || !psx_ || !psx_->gpu) {
            return;
        }

        const psx_gpu_t* gpu = psx_->gpu;
        psxe_diag_logf(
            "hw",
            "%s frame=%llu steps=%u timing=%s target_fps=%.3f display_mode=0x%08x gpustat=0x%08x display_enable=%d dmode=%ux%u display=%ux%u draw=(%u,%u)-(%u,%u) disp=(%u,%u)-(%u,%u) offset=(%d,%d) texture=%dx%d renderer=%p",
            stage ? stage : "frame-state",
            static_cast<unsigned long long>(vblank_counter_),
            steps,
            timingModeTitle(),
            targetFrameRate(),
            gpu->display_mode,
            gpu->gpustat,
            (gpu->gpustat & 0x00800000) != 0,
            psx_get_dmode_width(psx_),
            psx_get_dmode_height(psx_),
            psx_get_display_width(psx_),
            psx_get_display_height(psx_),
            gpu->draw_x1,
            gpu->draw_y1,
            gpu->draw_x2,
            gpu->draw_y2,
            gpu->disp_x1,
            gpu->disp_y1,
            gpu->disp_x2,
            gpu->disp_y2,
            gpu->off_x,
            gpu->off_y,
            texture_width_,
            texture_height_,
            (void*)renderer_
        );
#else
        (void)stage;
        (void)steps;
#endif
    }
#endif

    bool saveScreenshot(const std::filesystem::path& path) {
        if (!psx_) {
            return false;
        }

        const int width = texture_width_;
        const int height = texture_height_;
        const Uint32 format = texture_format_;
        void* source = debug_view_ ? psx_get_vram(psx_) : psx_get_display_buffer(psx_);

        if (!debug_view_ && ((psx_->gpu->disp_y + texture_height_) > PSX_GPU_FB_HEIGHT)) {
            source = psx_get_vram(psx_);
        }

        SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);

        if (!surface) {
            return false;
        }

        const size_t row_size = static_cast<size_t>(surface->pitch);
        const uint8_t* src = static_cast<const uint8_t*>(source);
        uint8_t* dst = static_cast<uint8_t*>(surface->pixels);

        for (int row = 0; row < height; row++) {
            std::memcpy(dst + (static_cast<size_t>(row) * static_cast<size_t>(surface->pitch)), src + (static_cast<size_t>(row) * static_cast<size_t>(PSX_GPU_FB_STRIDE)), row_size);
        }

        const int result = SDL_SaveBMP(surface, path.string().c_str());
        SDL_FreeSurface(surface);
        return result == 0;
    }

    double frameRate() const {
        if (!psx_ || !psx_->gpu) {
            return 59.29;
        }

        return static_cast<double>(psx_gpu_frame_rate(psx_->gpu));
    }

    double targetFrameRate() const {
        return frameRate() * (fast_forward_enabled_ ? 2.0 : 1.0);
    }

    std::uint64_t vblankCounter() const {
        return vblank_counter_;
    }

#ifdef USE_HARDWARE
    bool hardwareBackendActive() const {
        return hardware_backend_active_;
    }

    int textureWidth() const {
        return texture_width_;
    }

    int textureHeight() const {
        return texture_height_;
    }

    Uint32 textureFormat() const {
        return texture_format_;
    }
#endif

    const char* timingModeTitle() const {
        if (!psx_ || !psx_->gpu) {
            return "NTSC-like";
        }

        return psx_gpu_is_pal_mode(psx_->gpu) ? "PAL-like" : "NTSC-like";
    }

  private:
    void destroyStereoTexture() {
        finishStereoJob(true);
        for (SDL_Texture*& texture : stereo_texture_) {
            if (texture) {
                SDL_DestroyTexture(texture);
                texture = nullptr;
            }
        }
        stereo_width_ = 0;
        stereo_height_ = 0;
        stereo_ready_ = false;
    }

    // Waits for the eye images started on the previous frame and, unless `discard`, uploads them.
    // The upload is GL work, so it stays on this (the renderer's) thread.
    void finishStereoJob(bool discard) {
        if (!stereo_job_pending_) {
            return;
        }
        stereo_job_pending_ = false;
        stereo_workers_->Wait();
        if (discard || !stereo_texture_[0] || stereo_job_width_ != stereo_width_ || stereo_job_height_ != stereo_height_) {
            return;
        }
        for (int eye = 0; eye < 2; eye++) {
            SDL_UpdateTexture(stereo_texture_[eye], nullptr, stereo_pixels_[eye].data(), stereo_width_ * static_cast<int>(sizeof(uint32_t)));
        }
        stereo_ready_ = true;
    }

    // The eye images run one frame behind the emulator: this frame's color and depth are copied
    // aside and reprojected on the worker cores while the emulator thread goes on to the next
    // frame; that next call shows them. Building them here and waiting cost the emulator thread
    // ~2 ms a frame, which heavy scenes do not have (16.7 ms at 60 Hz).
    void updateStereoFrame(const FrontendSettings& settings, const uint16_t* color) {
        stereo_ready_ = false;
        finishStereoJob(false);
        const bool shown = stereo_ready_;
        stereo_ready_ = false;

        const float* depth = psx_ ? psx_get_display_depth(psx_) : nullptr;
        // 24-bit frames are FMV, which has no depth; the debug view is VRAM itself.
        if (!stereo_enabled_ || debug_view_ || !color || !depth || texture_format_ != SDL_PIXELFORMAT_BGR555 ||
            stereo_image_fraction_ <= 0.0f) {
            return;
        }

        armsx::vr::StereoParams params;
        params.max_disparity_px = armsx::vr::MaxDisparityPx(
            stereo_strength_, stereo_screen_width_m_ * stereo_image_fraction_, texture_width_);
        params.convergence = stereo_convergence_;
        if (params.max_disparity_px <= 0.0f) {
            return;
        }

        const int source_width = texture_width_;
        const int width = source_width * params.scale;
        const int height = texture_height_;
        bool keep_shown = shown;
        if (!stereo_texture_[0] || stereo_width_ != width || stereo_height_ != height) {
            keep_shown = false; // new, empty textures
            destroyStereoTexture();
            // RGBA, the GLES renderer's native format: SDL uploads it as is. (BGR555 like the mono
            // texture would be converted pixel by pixel on the CPU at every upload.)
            for (SDL_Texture*& texture : stereo_texture_) {
                texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, width, height);
                if (!texture) {
                    destroyStereoTexture();
                    return;
                }
            }
            stereo_width_ = width;
            stereo_height_ = height;
        }
#if SDL_VERSION_ATLEAST(2, 0, 12)
        for (SDL_Texture* texture : stereo_texture_) {
            SDL_SetTextureScaleMode(texture, settings.texture_scale_mode ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        }
#else
        (void)settings;
#endif
        // The previous frame's images, if they matched this size, stay on show for this frame.
        stereo_ready_ = keep_shown;

        // Private copies: the emulator overwrites VRAM and depth while the workers read them.
        constexpr int kRowStride = PSX_GPU_FB_STRIDE / static_cast<int>(sizeof(uint16_t));
        const size_t source_pixels = static_cast<size_t>(source_width) * static_cast<size_t>(height);
        stereo_color_copy_.resize(source_pixels);
        stereo_depth_copy_.resize(source_pixels);
        for (int y = 0; y < height; y++) {
            std::memcpy(stereo_color_copy_.data() + static_cast<size_t>(y) * source_width, color + y * kRowStride,
                static_cast<size_t>(source_width) * sizeof(uint16_t));
            std::memcpy(stereo_depth_copy_.data() + static_cast<size_t>(y) * source_width, depth + y * kRowStride,
                static_cast<size_t>(source_width) * sizeof(float));
        }
        for (std::vector<uint32_t>& pixels : stereo_pixels_) {
            pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
        }
        if (!stereo_workers_) {
            const armsx::vr::ScopedUnpinned unpinned;
            stereo_workers_ = std::make_unique<armsx::vr::ParallelFor>(kStereoTasks);
        }

        // Both eyes, each in row bands, one band per worker.
        const int band = (height + kStereoBands - 1) / kStereoBands;
        stereo_task_ = [this, params, source_width, width, height, band](int task) {
            const std::array<uint32_t, 0x8000>& to_rgba = Bgr555ToRgbaTable();
            const int eye = task / kStereoBands;
            const int first_row = (task % kStereoBands) * band;
            const int last_row = std::min(height, first_row + band);
            std::vector<uint16_t>& row = stereo_rows_[task];
            row.resize(static_cast<size_t>(width));
            for (int y = first_row; y < last_row; y++) {
                const size_t source_row = static_cast<size_t>(y) * static_cast<size_t>(source_width);
                stereo_reprojectors_[task].ReprojectRow(
                    stereo_color_copy_.data() + source_row,
                    stereo_depth_copy_.data() + source_row,
                    source_width,
                    eye == 0 ? -1 : 1,
                    params,
                    row.data());
                uint32_t* out = stereo_pixels_[eye].data() + static_cast<size_t>(y) * static_cast<size_t>(width);
                for (int x = 0; x < width; x++) {
                    out[x] = to_rgba[row[static_cast<size_t>(x)] & 0x7fff];
                }
            }
        };
        stereo_job_width_ = width;
        stereo_job_height_ = height;
        stereo_job_pending_ = true;
        stereo_workers_->Start(kStereoTasks, stereo_task_);
    }

    // BGR555 -> RGBA bytes, expanding 5 bits to 8 the way SDL does (floor(v * 255 / 31)), so the
    // 3D image matches the mono one exactly.
    static const std::array<uint32_t, 0x8000>& Bgr555ToRgbaTable() {
        static const std::array<uint32_t, 0x8000> table = [] {
            std::array<uint32_t, 0x8000> t{};
            for (uint32_t v = 0; v < 0x8000; v++) {
                const uint32_t r = ((v >> 0) & 0x1f) * 255 / 31;
                const uint32_t g = ((v >> 5) & 0x1f) * 255 / 31;
                const uint32_t b = ((v >> 10) & 0x1f) * 255 / 31;
                t[v] = r | (g << 8) | (b << 16) | 0xff000000u;
            }
            return t;
        }();
        return table;
    }

    void destroyPresentationTexture() {
        if (presentation_texture_) {
            SDL_DestroyTexture(presentation_texture_);
            presentation_texture_ = nullptr;
        }
        presentation_width_ = 0;
        presentation_height_ = 0;
        presentation_scale_ = 1;
    }

    void updatePresentationTexture(const FrontendSettings& settings) {
        const int requested_scale = std::clamp(settings.presentation_upscale, 1, 8);
        if (!renderer_ || !texture_ || requested_scale == 1) {
            destroyPresentationTexture();
            return;
        }

        SDL_RendererInfo info{};
        if (SDL_GetRendererInfo(renderer_, &info) != 0 || (info.flags & SDL_RENDERER_TARGETTEXTURE) == 0) {
            destroyPresentationTexture();
            return;
        }

        int actual_scale = requested_scale;
        if (info.max_texture_width > 0) {
            actual_scale = std::min(actual_scale, info.max_texture_width / std::max(texture_width_, 1));
        }
        if (info.max_texture_height > 0) {
            actual_scale = std::min(actual_scale, info.max_texture_height / std::max(texture_height_, 1));
        }
        actual_scale = std::max(actual_scale, 1);
        if (actual_scale == 1) {
            destroyPresentationTexture();
            return;
        }

        const int target_width = texture_width_ * actual_scale;
        const int target_height = texture_height_ * actual_scale;
        if (!presentation_texture_ || presentation_width_ != target_width ||
            presentation_height_ != target_height || presentation_scale_ != actual_scale) {
            destroyPresentationTexture();
            presentation_texture_ = SDL_CreateTexture(
                renderer_,
                SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_TARGET,
                target_width,
                target_height
            );
            if (!presentation_texture_) {
                psxe_diag_logf(
                    "renderer",
                    "SDL presentation upscale unavailable requested=%dx target=%dx%d error=%s",
                    requested_scale,
                    target_width,
                    target_height,
                    SDL_GetError()
                );
                return;
            }
            presentation_width_ = target_width;
            presentation_height_ = target_height;
            presentation_scale_ = actual_scale;
            psxe_diag_logf(
                "renderer",
                "SDL presentation upscale created requested=%dx actual=%dx target=%dx%d",
                requested_scale,
                actual_scale,
                target_width,
                target_height
            );
            SDL_SetTextureBlendMode(presentation_texture_, SDL_BLENDMODE_NONE);
#if SDL_VERSION_ATLEAST(2, 0, 12)
            SDL_SetTextureScaleMode(presentation_texture_, SDL_ScaleModeBest);
#endif
        }

#if SDL_VERSION_ATLEAST(2, 0, 12)
        if (SDL_SetTextureScaleMode(texture_, SDL_ScaleModeBest) != 0) {
            SDL_SetTextureScaleMode(texture_, SDL_ScaleModeLinear);
        }
#endif

        SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);
        SDL_Rect previous_viewport{};
        SDL_Rect previous_clip{};
        float previous_scale_x = 1.0f;
        float previous_scale_y = 1.0f;
        Uint8 previous_r = 0;
        Uint8 previous_g = 0;
        Uint8 previous_b = 0;
        Uint8 previous_a = 0;
        SDL_BlendMode previous_blend = SDL_BLENDMODE_NONE;
        const SDL_bool clip_enabled = SDL_RenderIsClipEnabled(renderer_);
        SDL_RenderGetViewport(renderer_, &previous_viewport);
        SDL_RenderGetClipRect(renderer_, &previous_clip);
        SDL_RenderGetScale(renderer_, &previous_scale_x, &previous_scale_y);
        SDL_GetRenderDrawColor(renderer_, &previous_r, &previous_g, &previous_b, &previous_a);
        SDL_GetRenderDrawBlendMode(renderer_, &previous_blend);

        bool rendered = false;
        if (SDL_SetRenderTarget(renderer_, presentation_texture_) == 0) {
            SDL_RenderSetViewport(renderer_, nullptr);
            SDL_RenderSetClipRect(renderer_, nullptr);
            SDL_RenderSetScale(renderer_, 1.0f, 1.0f);
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
            rendered = SDL_RenderClear(renderer_) == 0 &&
                SDL_RenderCopy(renderer_, texture_, nullptr, nullptr) == 0;
        }

        SDL_SetRenderTarget(renderer_, previous_target);
        SDL_RenderSetViewport(renderer_, &previous_viewport);
        SDL_RenderSetScale(renderer_, previous_scale_x, previous_scale_y);
        SDL_RenderSetClipRect(renderer_, clip_enabled ? &previous_clip : nullptr);
        SDL_SetRenderDrawBlendMode(renderer_, previous_blend);
        SDL_SetRenderDrawColor(renderer_, previous_r, previous_g, previous_b, previous_a);

        if (!rendered) {
            psxe_diag_logf(
                "renderer",
                "SDL presentation upscale render failed requested=%dx actual=%dx error=%s",
                requested_scale,
                actual_scale,
                SDL_GetError()
            );
            destroyPresentationTexture();
        }
    }

    static void SessionVblankEvent(psx_gpu_t* gpu) {
        if (gpu) {
            if (auto* session = static_cast<ArmsxSession*>(gpu->udata[0])) {
                session->vblank_counter_++;
            }

            psxe_gpu_vblank_timer_event_cb(gpu);
        }
    }

    void updateAudioPlaybackState() {
        if (!audio_dev_) {
            return;
        }

        const bool should_pause = paused_ || fast_forward_enabled_ || !audio_playback_started_;
        SDL_PauseAudioDevice(audio_dev_, should_pause ? 1 : 0);
    }

    void queueAudioForFrame() {
        if (!psx_) {
            return;
        }

        // Exactly the samples the SPU produced during this frame's emulation.
        const int sample_count = psx_audio_available(psx_);
        if (!audio_dev_ || fast_forward_enabled_) {
            std::vector<int16_t> discard(static_cast<size_t>(std::max(sample_count, 0)) * 2);
            psx_read_audio(psx_, discard.data(), sample_count);
            return;
        }
        if (sample_count <= 0) {
            return;
        }

        const size_t byte_count = static_cast<size_t>(sample_count) * sizeof(int16_t) * 2;
        if (byte_count == 0) {
            return;
        }

        std::vector<uint8_t> frame_audio(byte_count);
        MixPsxAudio(psx_, frame_audio.data(), static_cast<int>(frame_audio.size()));

        size_t queue_before = static_cast<size_t>(SDL_GetQueuedAudioSize(audio_dev_));
        if (audio_playback_started_) {
            StretchFrameAudio(frame_audio, queue_before);
        }
        if (audio_playback_started_ && queue_before == 0) {
            audio_underruns_++;
        }
        if (queue_before > kMaxQueuedAudioBytes) {
            SDL_PauseAudioDevice(audio_dev_, 1);
            SDL_ClearQueuedAudio(audio_dev_);
            audio_playback_started_ = false;
            queue_before = 0;
            audio_sample_accumulator_ = 0.0;
        }

        if (SDL_QueueAudio(audio_dev_, frame_audio.data(), static_cast<Uint32>(frame_audio.size())) != 0) {
            psxe_diag_logf("audio", "SDL_QueueAudio failed: %s", SDL_GetError());
            return;
        }

        const size_t queue_after = static_cast<size_t>(SDL_GetQueuedAudioSize(audio_dev_));
        if (!audio_playback_started_ && queue_after >= kAudioPrimeBytes) {
            audio_playback_started_ = true;
            psxe_diag_logf(
                "audio",
                "SDL queued audio primed bytes=%zu target=%zu frame=%llu",
                queue_after,
                kAudioPrimeBytes,
                static_cast<unsigned long long>(vblank_counter_)
            );
            updateAudioPlaybackState();
        }

#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (hardware_backend_active_) {
            psxe_diag_logf(
                "audio",
                "frame-audio frame=%llu samples=%d bytes=%zu queue_before=%zu queue_after=%zu accumulator=%.3f frame_rate=%.3f fast_forward=%s paused=%s",
                static_cast<unsigned long long>(vblank_counter_),
                sample_count,
                byte_count,
                queue_before,
                queue_after,
                audio_sample_accumulator_,
                frameRate(),
                fast_forward_enabled_ ? "true" : "false",
                paused_ ? "true" : "false"
            );
        }
#endif
    }

    // Dynamic rate control. When the emulator runs a little slower than real time (heavy scenes,
    // 55-59 fps), every frame delivers fewer samples than the device plays, the queue drains and
    // each empty moment is a click. Below the target fill this frame's samples are resampled to
    // up to kMaxAudioStretch more (a pitch drop of under a semitone, which goes unnoticed next to
    // a crackle); above it, very slightly fewer, so the delay does not grow either.
    void StretchFrameAudio(std::vector<uint8_t>& frame_audio, size_t queued_bytes) {
        const int in_frames = static_cast<int>(frame_audio.size() / kAudioBytesPerSampleFrame);
        if (in_frames < 2) {
            return;
        }
        const double fill = static_cast<double>(queued_bytes) / static_cast<double>(kAudioTargetQueueBytes);
        double ratio = 1.0;
        if (fill < 1.0) {
            ratio = 1.0 + kMaxAudioStretch * std::min(1.0, 1.0 - fill);
        } else if (fill > 1.5) {
            ratio = 1.0 - 0.005;
        }
        if (ratio == 1.0) {
            return;
        }

        // Fractional output frames carry over to the next frame, so the long-run rate is exact.
        const double wanted = static_cast<double>(in_frames) * ratio + audio_sample_accumulator_;
        const int out_frames = static_cast<int>(wanted);
        audio_sample_accumulator_ = wanted - static_cast<double>(out_frames);
        if (out_frames < 2 || out_frames == in_frames) {
            return;
        }

        const auto* in = reinterpret_cast<const int16_t*>(frame_audio.data());
        std::vector<uint8_t> stretched(static_cast<size_t>(out_frames) * kAudioBytesPerSampleFrame);
        auto* out = reinterpret_cast<int16_t*>(stretched.data());
        const double step = static_cast<double>(in_frames - 1) / static_cast<double>(out_frames - 1);
        for (int i = 0; i < out_frames; i++) {
            const double pos = static_cast<double>(i) * step;
            const int a = std::min(static_cast<int>(pos), in_frames - 1);
            const int b = std::min(a + 1, in_frames - 1);
            const double t = pos - static_cast<double>(a);
            for (int c = 0; c < 2; c++) {
                const double v = in[a * 2 + c] * (1.0 - t) + in[b * 2 + c] * t;
                out[i * 2 + c] = static_cast<int16_t>(std::lround(v));
            }
        }
        frame_audio.swap(stretched);
    }

    void clearQueuedAudio() {
        audio_sample_accumulator_ = 0.0;
        audio_playback_started_ = false;
        if (!audio_dev_) {
            return;
        }

        SDL_PauseAudioDevice(audio_dev_, 1);
        SDL_ClearQueuedAudio(audio_dev_);
    }

    static constexpr int kAudioMixRate = 44100;
    static constexpr size_t kAudioBytesPerSampleFrame = sizeof(int16_t) * 2;
    static constexpr size_t kAudioPrimeBytes =
        static_cast<size_t>(kAudioMixRate * kAudioBytesPerSampleFrame * 50 / 1000);
    static constexpr size_t kMaxQueuedAudioBytes =
        static_cast<size_t>(kAudioMixRate * kAudioBytesPerSampleFrame * 200 / 1000);
    // Dynamic rate control (StretchFrameAudio): the fill it steers toward, and its largest stretch.
    static constexpr size_t kAudioTargetQueueBytes =
        static_cast<size_t>(kAudioMixRate * kAudioBytesPerSampleFrame * 80 / 1000);
    static constexpr double kMaxAudioStretch = 0.08;
    static constexpr std::uint32_t kMaxFrameSteps = PSX_CPU_CPS / 8u;

    SDL_Renderer* renderer_ = nullptr;
    psx_t* psx_ = nullptr;
    psx_input_t* input_ = nullptr;
    psxi_sda_t* pad_device_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    SDL_Texture* presentation_texture_ = nullptr;
    std::vector<uint8_t> texture_snapshot_;
    bool stereo_enabled_ = false;
    bool stereo_ready_ = false;
    float stereo_strength_ = 1.0f;
    float stereo_convergence_ = 2.0f;
    float stereo_screen_width_m_ = 2.4f;
    float stereo_image_fraction_ = 0.0f;
    SDL_Texture* stereo_texture_[2] = {nullptr, nullptr};
    int stereo_width_ = 0;
    int stereo_height_ = 0;
    std::vector<uint32_t> stereo_pixels_[2];
    static constexpr int kStereoBands = 2;
    static constexpr int kStereoTasks = 2 * kStereoBands;
    std::vector<uint16_t> stereo_rows_[kStereoTasks];
    armsx::vr::StereoReprojector stereo_reprojectors_[kStereoTasks];
    std::vector<uint16_t> stereo_color_copy_;
    std::vector<float> stereo_depth_copy_;
    std::function<void(int)> stereo_task_;
    bool stereo_job_pending_ = false;
    int stereo_job_width_ = 0;
    int stereo_job_height_ = 0;
    // Declared last, destroyed first: its threads are joined before what the task touches goes.
    std::unique_ptr<armsx::vr::ParallelFor> stereo_workers_;
#ifdef USE_HARDWARE
    armsx_hw_renderer_t* hw_renderer_ = nullptr;
#endif
    SDL_AudioDeviceID audio_dev_ = 0;
    double audio_sample_accumulator_ = 0.0;
    bool audio_playback_started_ = false;
    std::uint64_t audio_underruns_ = 0;
    std::filesystem::path disc_path_;
    std::filesystem::path exe_path_;
    std::string title_;
    LaunchKind launch_kind_ = LaunchKind::None;
    bool paused_ = false;
    bool fast_forward_enabled_ = false;
    bool debug_view_ = false;
#ifdef USE_HARDWARE
    bool hardware_backend_active_ = false;
#endif
    std::uint64_t vblank_counter_ = 0;
    int texture_width_ = 0;
    int texture_height_ = 0;
    int presentation_width_ = 0;
    int presentation_height_ = 0;
    int presentation_scale_ = 1;
    Uint32 texture_format_ = SDL_PIXELFORMAT_UNKNOWN;
};

class GameplayInputRouter {
  public:
    ~GameplayInputRouter() {
        if (controller_) {
            SDL_GameControllerClose(controller_);
            controller_ = nullptr;
        }
    }

    void attach(psx_pad_t* pad) {
        clearAll();
        pad_ = pad;
    }

    void detach() {
        clearAll();
        pad_ = nullptr;
    }

    void onFsuiOpened() {
        fsui_owns_input_ = true;
        clearAll();
    }

    void onFsuiClosed() {
        fsui_owns_input_ = false;
        clearAll();
    }

    bool takePauseRequest() {
        const bool requested = pause_requested_;
        pause_requested_ = false;
        return requested;
    }

    bool takeControllerActivity() {
        const bool active = controller_activity_;
        controller_activity_ = false;
        return active;
    }

    bool takeControllerDisconnected() {
        const bool disconnected = controller_disconnected_;
        controller_disconnected_ = false;
        return disconnected;
    }

    void setVirtualDigitalMask(uint32_t mask) {
        if (virtual_digital_mask_ == mask) {
            return;
        }
        virtual_digital_mask_ = mask;
        syncDigitalMask();
    }

    uint32_t activeDigitalMask() const {
        return active_digital_mask_;
    }

    // Quest Touch controllers, already mapped to the pad (armsx::vr::QuestPadMapper). Ignored while
    // FSUI owns input, like every other source.
    void setVrPad(uint32_t mask, const std::array<uint8_t, 4>& analog) {
        if (fsui_owns_input_) {
            mask = 0;
        }
        if (vr_digital_mask_ != mask) {
            vr_digital_mask_ = mask;
            syncDigitalMask();
        }
        if (!pad_ || fsui_owns_input_ || analog == vr_analog_) {
            return;
        }
        static constexpr std::array<uint32_t, 4> axes = {
            PSXI_AX_SDA_LEFT_HORZ, PSXI_AX_SDA_LEFT_VERT, PSXI_AX_SDA_RIGHT_HORZ, PSXI_AX_SDA_RIGHT_VERT,
        };
        for (size_t i = 0; i < axes.size(); i++) {
            if (analog[i] != vr_analog_[i]) {
                psx_pad_analog_change(pad_, 0, axes[i], analog[i]);
            }
        }
        vr_analog_ = analog;
    }

    void tick(bool fsui_active) {
        if (!fsui_active) {
            flushChordButtons(SDL_GetTicks());
        }
    }

    std::vector<fsui::InputOverlayDeviceState> buildInputOverlay() const {
        fsui::InputOverlayDeviceState device;
        device.title = "P1";
        device.bindings = {
            fsui::InputOverlayBinding{.label = "Up", .glyph = "^", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_PAD_UP) ? 1.0f : 0.0f},
            fsui::InputOverlayBinding{.label = "Down", .glyph = "v", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_PAD_DOWN) ? 1.0f : 0.0f},
            fsui::InputOverlayBinding{.label = "Left", .glyph = "<", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_PAD_LEFT) ? 1.0f : 0.0f},
            fsui::InputOverlayBinding{.label = "Right", .glyph = ">", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_PAD_RIGHT) ? 1.0f : 0.0f},
            fsui::InputOverlayBinding{.label = "Start", .glyph = "S", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_START) ? 1.0f : 0.0f},
            fsui::InputOverlayBinding{.label = "Select", .glyph = "=", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_SELECT) ? 1.0f : 0.0f},
        };
        return {device};
    }

    void processEvent(const SDL_Event& event, ArmsxSession* session, bool fsui_active) {
        flushChordButtons(SDL_GetTicks());
        handleControllerLifecycle(event);

        if (!session || !session->valid()) {
            return;
        }

        if (event.type == SDL_KEYDOWN) {
            if (event.key.repeat == 0 && event.key.keysym.sym == SDLK_ESCAPE && !fsui_active) {
                pause_requested_ = true;
                return;
            }

            if (fsui_active || fsui_owns_input_ || event.key.repeat != 0) {
                return;
            }

            const uint32_t mask = buttonForKey(event.key.keysym.sym);
            if (mask) {
                press(mask);
            }
            return;
        }

        if (event.type == SDL_KEYUP) {
            if (event.key.keysym.sym == SDLK_ESCAPE) {
                return;
            }

            if (fsui_active || fsui_owns_input_) {
                return;
            }

            const uint32_t mask = buttonForKey(event.key.keysym.sym);
            if (mask) {
                release(mask);
            }
            return;
        }

        if (!controller_) {
            return;
        }

        SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller_);
        if (!joystick) {
            return;
        }

        const SDL_JoystickID controller_id = SDL_JoystickInstanceID(joystick);

        if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP) {
            if (event.cbutton.which != controller_id) {
                return;
            }

            const bool pressed = event.type == SDL_CONTROLLERBUTTONDOWN;
            const SDL_GameControllerButton button = static_cast<SDL_GameControllerButton>(event.cbutton.button);
            if (pressed && !fsui_active && !fsui_owns_input_) {
                controller_activity_ = true;
            }

            if (button == SDL_CONTROLLER_BUTTON_START || button == SDL_CONTROLLER_BUTTON_BACK ||
                button == SDL_CONTROLLER_BUTTON_GUIDE || button == SDL_CONTROLLER_BUTTON_MISC1) {
                handlePauseChordButton(button, pressed, fsui_active);
                return;
            }

            if (fsui_active || fsui_owns_input_) {
                return;
            }

            const uint32_t mask = buttonForController(button);
            if (mask) {
                if (pressed) {
                    press(mask);
                } else {
                    release(mask);
                }
            }
            return;
        }

        if (event.type == SDL_CONTROLLERAXISMOTION) {
            if (event.caxis.which != controller_id || fsui_active || fsui_owns_input_ || !pad_) {
                return;
            }

            if (std::abs(static_cast<int>(event.caxis.value)) > 8000) {
                controller_activity_ = true;
            }
            const uint16_t mapped = static_cast<uint16_t>((static_cast<int>(event.caxis.value) + INT16_MAX + 1) / 0x100);
            switch (event.caxis.axis) {
                case SDL_CONTROLLER_AXIS_RIGHTX:
                    psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_RIGHT_HORZ, mapped);
                    break;
                case SDL_CONTROLLER_AXIS_RIGHTY:
                    psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_RIGHT_VERT, mapped);
                    break;
                case SDL_CONTROLLER_AXIS_LEFTX:
                    psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_LEFT_HORZ, mapped);
                    break;
                case SDL_CONTROLLER_AXIS_LEFTY:
                    psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_LEFT_VERT, mapped);
                    break;
#ifdef CONTROLLER_GENERIC
                case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                    handleTrigger(PSXI_SW_SDA_L2, trigger_left_down_, event.caxis.value);
                    break;
                case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                    handleTrigger(PSXI_SW_SDA_R2, trigger_right_down_, event.caxis.value);
                    break;
#endif
                default:
                    break;
            }
        }
    }

  private:
    void handleControllerLifecycle(const SDL_Event& event) {
        if (event.type == SDL_CONTROLLERDEVICEADDED) {
            if (!controller_ && SDL_IsGameController(event.cdevice.which)) {
                controller_ = SDL_GameControllerOpen(event.cdevice.which);
            }
            return;
        }

        if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller_) {
            SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller_);
            if (joystick && SDL_JoystickInstanceID(joystick) == event.cdevice.which) {
                SDL_GameControllerClose(controller_);
                controller_ = nullptr;
                controller_disconnected_ = true;
                start_button_ = {};
                select_button_ = {};
                trigger_left_down_ = false;
                trigger_right_down_ = false;
            }
        }
    }

    void handlePauseChordButton(SDL_GameControllerButton button, bool pressed, bool fsui_active) {
        PendingChordButton* state = nullptr;
        PendingChordButton* other = nullptr;

        if (button == SDL_CONTROLLER_BUTTON_START) {
            state = &start_button_;
            other = &select_button_;
        } else {
            state = &select_button_;
            other = &start_button_;
        }

        if (pressed) {
            state->physical_down = true;
            if (!fsui_active && !fsui_owns_input_ && !pause_latched_) {
                state->pending = true;
                state->pending_since = SDL_GetTicks();
                if (other->physical_down && other->pending) {
                    pause_latched_ = true;
                    pause_requested_ = true;
                    state->pending = false;
                    other->pending = false;
                    clearAll();
                }
            }
            return;
        }

        state->physical_down = false;

        if (pause_latched_) {
            if (!start_button_.physical_down && !select_button_.physical_down) {
                pause_latched_ = false;
                start_button_ = {};
                select_button_ = {};
            }
            return;
        }

        if (state->pending) {
            const uint32_t mask = (state == &start_button_) ? PSXI_SW_SDA_START : PSXI_SW_SDA_SELECT;
            press(mask);
            release(mask);
            state->pending = false;
            return;
        }

        if (state->forwarded) {
            const uint32_t mask = (state == &start_button_) ? PSXI_SW_SDA_START : PSXI_SW_SDA_SELECT;
            release(mask);
            state->forwarded = false;
        }
    }

    void flushChordButtons(Uint32 now) {
        if (pause_latched_ || fsui_owns_input_) {
            return;
        }

        flushOneChordButton(start_button_, select_button_, PSXI_SW_SDA_START, now);
        flushOneChordButton(select_button_, start_button_, PSXI_SW_SDA_SELECT, now);
    }

    void flushOneChordButton(PendingChordButton& state, PendingChordButton& other, uint32_t mask, Uint32 now) {
        if (!state.pending || other.physical_down || !pad_) {
            return;
        }

        if ((now - state.pending_since) < kPauseChordGraceMs) {
            return;
        }

        press(mask);
        state.pending = false;
        state.forwarded = true;
    }

    void press(uint32_t mask) {
        if (!mask) {
            return;
        }
        physical_digital_mask_ |= mask;
        syncDigitalMask();
    }

    void release(uint32_t mask) {
        if (!mask) {
            return;
        }
        physical_digital_mask_ &= ~mask;
        syncDigitalMask();
    }

    void syncDigitalMask() {
        const uint32_t desired_mask = physical_digital_mask_ | virtual_digital_mask_ | vr_digital_mask_;
        if (!pad_) {
            active_digital_mask_ = desired_mask;
            return;
        }

        const uint32_t pressed = desired_mask & ~active_digital_mask_;
        const uint32_t released = active_digital_mask_ & ~desired_mask;
        if (pressed) {
            psx_pad_button_press(pad_, 0, pressed);
        }
        if (released) {
            psx_pad_button_release(pad_, 0, released);
        }
        active_digital_mask_ = desired_mask;
    }

    void clearAll() {
        vr_digital_mask_ = 0;
        vr_analog_ = {0x80, 0x80, 0x80, 0x80};
        if (!pad_) {
            physical_digital_mask_ = 0;
            virtual_digital_mask_ = 0;
            active_digital_mask_ = 0;
            return;
        }

        static const std::array<uint32_t, 17> masks = {
            PSXI_SW_SDA_SELECT, PSXI_SW_SDA_L3, PSXI_SW_SDA_R3, PSXI_SW_SDA_START,
            PSXI_SW_SDA_PAD_UP, PSXI_SW_SDA_PAD_RIGHT, PSXI_SW_SDA_PAD_DOWN, PSXI_SW_SDA_PAD_LEFT,
            PSXI_SW_SDA_L2, PSXI_SW_SDA_R2, PSXI_SW_SDA_L1, PSXI_SW_SDA_R1,
            PSXI_SW_SDA_TRIANGLE, PSXI_SW_SDA_CIRCLE, PSXI_SW_SDA_CROSS, PSXI_SW_SDA_SQUARE,
            PSXI_SW_SDA_ANALOG,
        };

        for (uint32_t mask : masks) {
            if ((active_digital_mask_ & mask) != 0) {
                psx_pad_button_release(pad_, 0, mask);
            }
        }

        physical_digital_mask_ = 0;
        virtual_digital_mask_ = 0;
        active_digital_mask_ = 0;
        psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_RIGHT_HORZ, 0x80);
        psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_RIGHT_VERT, 0x80);
        psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_LEFT_HORZ, 0x80);
        psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_LEFT_VERT, 0x80);
    }

#ifdef CONTROLLER_GENERIC
    void handleTrigger(uint32_t mask, bool& down_flag, Sint16 value) {
        const bool pressed = value > 8000;
        if (pressed == down_flag) {
            return;
        }

        down_flag = pressed;
        if (pressed) {
            press(mask);
        } else {
            release(mask);
        }
    }
#endif

    static uint32_t buttonForKey(SDL_Keycode key) {
        switch (key) {
            case SDLK_x: return PSXI_SW_SDA_CROSS;
            case SDLK_a: return PSXI_SW_SDA_SQUARE;
            case SDLK_w: return PSXI_SW_SDA_TRIANGLE;
            case SDLK_d: return PSXI_SW_SDA_CIRCLE;
            case SDLK_RETURN: return PSXI_SW_SDA_START;
            case SDLK_s: return PSXI_SW_SDA_SELECT;
            case SDLK_UP: return PSXI_SW_SDA_PAD_UP;
            case SDLK_DOWN: return PSXI_SW_SDA_PAD_DOWN;
            case SDLK_LEFT: return PSXI_SW_SDA_PAD_LEFT;
            case SDLK_RIGHT: return PSXI_SW_SDA_PAD_RIGHT;
            case SDLK_q: return PSXI_SW_SDA_L1;
            case SDLK_e: return PSXI_SW_SDA_R1;
            case SDLK_1: return PSXI_SW_SDA_L2;
            case SDLK_3: return PSXI_SW_SDA_R2;
            case SDLK_z: return PSXI_SW_SDA_L3;
            case SDLK_c: return PSXI_SW_SDA_R3;
            case SDLK_2: return PSXI_SW_SDA_ANALOG;
            default: return 0;
        }
    }

    static uint32_t buttonForController(SDL_GameControllerButton button) {
        switch (button) {
            case SDL_CONTROLLER_BUTTON_A: return PSXI_SW_SDA_CROSS;
            case SDL_CONTROLLER_BUTTON_X: return PSXI_SW_SDA_SQUARE;
            case SDL_CONTROLLER_BUTTON_Y: return PSXI_SW_SDA_TRIANGLE;
            case SDL_CONTROLLER_BUTTON_B: return PSXI_SW_SDA_CIRCLE;
            case SDL_CONTROLLER_BUTTON_DPAD_UP: return PSXI_SW_SDA_PAD_UP;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return PSXI_SW_SDA_PAD_DOWN;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return PSXI_SW_SDA_PAD_LEFT;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return PSXI_SW_SDA_PAD_RIGHT;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return PSXI_SW_SDA_L1;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return PSXI_SW_SDA_R1;
            case SDL_CONTROLLER_BUTTON_LEFTSTICK: return PSXI_SW_SDA_L3;
            case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return PSXI_SW_SDA_R3;
            default: return 0;
        }
    }

    psx_pad_t* pad_ = nullptr;
    SDL_GameController* controller_ = nullptr;
    uint32_t physical_digital_mask_ = 0;
    uint32_t virtual_digital_mask_ = 0;
    uint32_t vr_digital_mask_ = 0;
    std::array<uint8_t, 4> vr_analog_ = {0x80, 0x80, 0x80, 0x80};
    uint32_t active_digital_mask_ = 0;
    PendingChordButton start_button_{};
    PendingChordButton select_button_{};
    bool pause_requested_ = false;
    bool pause_latched_ = false;
    bool fsui_owns_input_ = false;
    bool trigger_left_down_ = false;
    bool trigger_right_down_ = false;
    bool controller_activity_ = false;
    bool controller_disconnected_ = false;
};

class ImGuiMobileControls {
  public:
    void processEvent(const SDL_Event& event) {
        if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
            if (!visible_) {
                showNow();
                suppress_mouse_until_up_ = true;
            } else {
                last_activity_ms_ = SDL_GetTicks();
            }
            return;
        }
        if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
            suppress_mouse_until_up_ = false;
            return;
        }
        if (event.type != SDL_FINGERDOWN && event.type != SDL_FINGERMOTION && event.type != SDL_FINGERUP) {
            return;
        }

        const auto suppressed = std::find(suppressed_touches_.begin(), suppressed_touches_.end(), event.tfinger.fingerId);
        if (suppressed != suppressed_touches_.end()) {
            if (event.type == SDL_FINGERUP) {
                suppressed_touches_.erase(suppressed);
            }
            return;
        }

        if (event.type == SDL_FINGERDOWN && !visible_) {
            showNow();
            suppressed_touches_.push_back(event.tfinger.fingerId);
            return;
        }

        last_activity_ms_ = SDL_GetTicks();
        const auto found = std::find_if(touches_.begin(), touches_.end(), [&](const Touch& touch) {
            return touch.id == event.tfinger.fingerId;
        });

        if (event.type == SDL_FINGERUP) {
            if (found != touches_.end()) {
                touches_.erase(found);
            }
            return;
        }

        const Touch next{
            .id = event.tfinger.fingerId,
            .x = std::clamp(event.tfinger.x, 0.0f, 1.0f),
            .y = std::clamp(event.tfinger.y, 0.0f, 1.0f),
        };
        if (found == touches_.end()) {
            touches_.push_back(next);
        } else {
            *found = next;
        }
    }

    void reset(GameplayInputRouter& input) {
        touches_.clear();
        routed_mask_ = 0;
        input.setVirtualDigitalMask(0);
    }

    void showNow() {
        visible_ = true;
        controller_in_use_ = false;
        last_activity_ms_ = SDL_GetTicks();
    }

    void onControllerActivity(GameplayInputRouter& input) {
        controller_in_use_ = true;
        visible_ = false;
        reset(input);
    }

    void onControllerDisconnected() {
        showNow();
    }

    void drawAndRoute(GameplayInputRouter& input) {
        const Uint32 now = SDL_GetTicks();
        if (last_activity_ms_ == 0) {
            last_activity_ms_ = now;
        }
        if (visible_ && touches_.empty() && !suppress_mouse_until_up_ &&
            (now - last_activity_ms_) >= kMobileControlsAutoHideMs) {
            visible_ = false;
            reset(input);
        }
        if (!visible_) {
            input.setVirtualDigitalMask(0);
            return;
        }

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        if (display.x <= 0.0f || display.y <= 0.0f) {
            input.setVirtualDigitalMask(0);
            return;
        }

        const Layout layout = makeLayout(display);
        uint32_t mask = 0;
        for (const Touch& touch : touches_) {
            mask |= maskForPoint(layout, ImVec2(touch.x * display.x, touch.y * display.y));
        }

        ImGuiIO& io = ImGui::GetIO();
        if (touches_.empty() && io.MouseDown[0] && !io.WantCaptureMouse && !suppress_mouse_until_up_) {
            mask |= maskForPoint(layout, io.MousePos);
        }

        routed_mask_ = mask;
        input.setVirtualDigitalMask(mask);
        draw(layout, input.activeDigitalMask());
    }

  private:
    struct Touch {
        SDL_FingerID id = 0;
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Button {
        ImVec2 center{};
        float radius = 0.0f;
        uint32_t mask = 0;
    };

    struct RectButton {
        ImVec2 min{};
        ImVec2 max{};
        uint32_t mask = 0;
        const char* label = "";
    };

    struct Layout {
        ImVec2 display{};
        ImVec2 stick_center{};
        float stick_radius = 0.0f;
        float stick_deadzone = 0.0f;
        std::array<Button, 4> face{};
        std::array<RectButton, 6> utility{};
    };

    static Layout makeLayout(const ImVec2& display) {
        const float unit = std::max(std::min(display.x, display.y), 1.0f);
        const float bottom_margin = unit * 0.075f;
        const float stick_radius = unit * 0.135f;
        const float face_radius = unit * 0.066f;
        const float face_offset = unit * 0.125f;
        const ImVec2 stick_center(unit * 0.21f, display.y - bottom_margin - stick_radius);
        const ImVec2 face_center(display.x - unit * 0.21f, display.y - bottom_margin - stick_radius);

        Layout layout;
        layout.display = display;
        layout.stick_center = stick_center;
        layout.stick_radius = stick_radius;
        layout.stick_deadzone = stick_radius * 0.27f;
        layout.face = {
            Button{ImVec2(face_center.x, face_center.y - face_offset), face_radius, PSXI_SW_SDA_TRIANGLE},
            Button{ImVec2(face_center.x + face_offset, face_center.y), face_radius, PSXI_SW_SDA_CIRCLE},
            Button{ImVec2(face_center.x, face_center.y + face_offset), face_radius, PSXI_SW_SDA_CROSS},
            Button{ImVec2(face_center.x - face_offset, face_center.y), face_radius, PSXI_SW_SDA_SQUARE},
        };

        const float shoulder_width = unit * 0.23f;
        const float shoulder_height = unit * 0.075f;
        const float edge = unit * 0.035f;
        const float shoulder_gap = unit * 0.025f;
        const float pill_width = unit * 0.17f;
        const float pill_height = unit * 0.065f;
        const float pill_y = display.y - bottom_margin - pill_height;
        layout.utility = {
            RectButton{ImVec2(edge, edge), ImVec2(edge + shoulder_width, edge + shoulder_height), PSXI_SW_SDA_L2, "L2"},
            RectButton{ImVec2(edge, edge + shoulder_height + shoulder_gap), ImVec2(edge + shoulder_width, edge + shoulder_height * 2.0f + shoulder_gap), PSXI_SW_SDA_L1, "L1"},
            RectButton{ImVec2(display.x - edge - shoulder_width, edge), ImVec2(display.x - edge, edge + shoulder_height), PSXI_SW_SDA_R2, "R2"},
            RectButton{ImVec2(display.x - edge - shoulder_width, edge + shoulder_height + shoulder_gap), ImVec2(display.x - edge, edge + shoulder_height * 2.0f + shoulder_gap), PSXI_SW_SDA_R1, "R1"},
            RectButton{ImVec2(display.x * 0.5f - pill_width - shoulder_gap * 0.5f, pill_y), ImVec2(display.x * 0.5f - shoulder_gap * 0.5f, pill_y + pill_height), PSXI_SW_SDA_SELECT, "SELECT"},
            RectButton{ImVec2(display.x * 0.5f + shoulder_gap * 0.5f, pill_y), ImVec2(display.x * 0.5f + pill_width + shoulder_gap * 0.5f, pill_y + pill_height), PSXI_SW_SDA_START, "START"},
        };
        return layout;
    }

    static float distanceSquared(const ImVec2& lhs, const ImVec2& rhs) {
        const float dx = lhs.x - rhs.x;
        const float dy = lhs.y - rhs.y;
        return dx * dx + dy * dy;
    }

    static bool pointInRect(const ImVec2& point, const RectButton& button) {
        return point.x >= button.min.x && point.x <= button.max.x &&
            point.y >= button.min.y && point.y <= button.max.y;
    }

    static uint32_t maskForPoint(const Layout& layout, const ImVec2& point) {
        uint32_t mask = 0;
        const float stick_hit_radius = layout.stick_radius * 1.35f;
        if (distanceSquared(point, layout.stick_center) <= stick_hit_radius * stick_hit_radius) {
            const float dx = point.x - layout.stick_center.x;
            const float dy = point.y - layout.stick_center.y;
            if (dx <= -layout.stick_deadzone) mask |= PSXI_SW_SDA_PAD_LEFT;
            if (dx >= layout.stick_deadzone) mask |= PSXI_SW_SDA_PAD_RIGHT;
            if (dy <= -layout.stick_deadzone) mask |= PSXI_SW_SDA_PAD_UP;
            if (dy >= layout.stick_deadzone) mask |= PSXI_SW_SDA_PAD_DOWN;
        }

        for (const Button& button : layout.face) {
            const float hit_radius = button.radius * 1.35f;
            if (distanceSquared(point, button.center) <= hit_radius * hit_radius) {
                mask |= button.mask;
            }
        }
        for (const RectButton& button : layout.utility) {
            if (pointInRect(point, button)) {
                mask |= button.mask;
            }
        }
        return mask;
    }

    static void drawCenteredText(ImDrawList* draw_list, const ImVec2& center, ImU32 color, const char* text) {
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw_list->AddText(ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f), color, text);
    }

    static void drawFaceSymbol(ImDrawList* draw_list, const Button& button, uint32_t active_mask) {
        const bool active = (active_mask & button.mask) != 0;
        ImU32 accent = IM_COL32(225, 225, 235, 220);
        if (button.mask == PSXI_SW_SDA_TRIANGLE) accent = IM_COL32(70, 220, 150, 235);
        if (button.mask == PSXI_SW_SDA_CIRCLE) accent = IM_COL32(245, 90, 105, 235);
        if (button.mask == PSXI_SW_SDA_CROSS) accent = IM_COL32(95, 150, 255, 235);
        if (button.mask == PSXI_SW_SDA_SQUARE) accent = IM_COL32(225, 105, 220, 235);
        draw_list->AddCircleFilled(
            button.center,
            button.radius,
            active ? IM_COL32(255, 255, 255, 105) : IM_COL32(18, 20, 28, 112),
            32
        );
        draw_list->AddCircle(button.center, button.radius, accent, 32, active ? 4.0f : 2.5f);

        const float symbol = button.radius * 0.48f;
        if (button.mask == PSXI_SW_SDA_TRIANGLE) {
            draw_list->AddTriangle(
                ImVec2(button.center.x, button.center.y - symbol),
                ImVec2(button.center.x + symbol, button.center.y + symbol),
                ImVec2(button.center.x - symbol, button.center.y + symbol),
                accent,
                3.0f
            );
        } else if (button.mask == PSXI_SW_SDA_CIRCLE) {
            draw_list->AddCircle(button.center, symbol, accent, 24, 3.0f);
        } else if (button.mask == PSXI_SW_SDA_CROSS) {
            draw_list->AddLine(
                ImVec2(button.center.x - symbol, button.center.y - symbol),
                ImVec2(button.center.x + symbol, button.center.y + symbol),
                accent,
                3.0f
            );
            draw_list->AddLine(
                ImVec2(button.center.x + symbol, button.center.y - symbol),
                ImVec2(button.center.x - symbol, button.center.y + symbol),
                accent,
                3.0f
            );
        } else {
            draw_list->AddRect(
                ImVec2(button.center.x - symbol, button.center.y - symbol),
                ImVec2(button.center.x + symbol, button.center.y + symbol),
                accent,
                1.0f,
                0,
                3.0f
            );
        }
    }

    static void draw(const Layout& layout, uint32_t active_mask) {
        ImDrawList* draw_list = ImGui::GetForegroundDrawList();
        const ImU32 panel = IM_COL32(18, 20, 28, 112);
        const ImU32 outline = IM_COL32(235, 238, 246, 185);
        const ImU32 active = IM_COL32(255, 255, 255, 105);

        draw_list->AddCircleFilled(layout.stick_center, layout.stick_radius, panel, 40);
        draw_list->AddCircle(layout.stick_center, layout.stick_radius, outline, 40, 2.5f);
        ImVec2 knob = layout.stick_center;
        const float knob_offset = layout.stick_radius * 0.43f;
        if (active_mask & PSXI_SW_SDA_PAD_LEFT) knob.x -= knob_offset;
        if (active_mask & PSXI_SW_SDA_PAD_RIGHT) knob.x += knob_offset;
        if (active_mask & PSXI_SW_SDA_PAD_UP) knob.y -= knob_offset;
        if (active_mask & PSXI_SW_SDA_PAD_DOWN) knob.y += knob_offset;
        const bool stick_active = (active_mask &
            (PSXI_SW_SDA_PAD_LEFT | PSXI_SW_SDA_PAD_RIGHT | PSXI_SW_SDA_PAD_UP | PSXI_SW_SDA_PAD_DOWN)) != 0;
        draw_list->AddCircleFilled(
            knob,
            layout.stick_radius * 0.42f,
            stick_active ? IM_COL32(165, 185, 255, 180) : IM_COL32(105, 115, 140, 160),
            32
        );
        draw_list->AddCircle(knob, layout.stick_radius * 0.42f, outline, 32, 2.0f);

        for (const Button& button : layout.face) {
            drawFaceSymbol(draw_list, button, active_mask);
        }

        for (const RectButton& button : layout.utility) {
            const bool is_active = (active_mask & button.mask) != 0;
            draw_list->AddRectFilled(button.min, button.max, is_active ? active : panel, 10.0f);
            draw_list->AddRect(button.min, button.max, outline, 10.0f, 0, is_active ? 3.5f : 2.0f);
            drawCenteredText(
                draw_list,
                ImVec2((button.min.x + button.max.x) * 0.5f, (button.min.y + button.max.y) * 0.5f),
                IM_COL32(245, 247, 252, 225),
                button.label
            );
        }
    }

    std::vector<Touch> touches_;
    std::vector<SDL_FingerID> suppressed_touches_;
    uint32_t routed_mask_ = 0;
    Uint32 last_activity_ms_ = 0;
    bool visible_ = true;
    bool controller_in_use_ = false;
    bool suppress_mouse_until_up_ = false;
};

class ArmsxApp {
  public:
    ArmsxApp(int argc, const char* argv[], void* external_window, void* external_renderer)
        : argc_(argc), argv_(argv), external_window_(static_cast<SDL_Window*>(external_window)),
          external_renderer_(static_cast<SDL_Renderer*>(external_renderer)), cli_(ScanCliFlags(argc, argv)) {}

    int run() {
        psxe_diag_initialize(psxe_cfg_get_pref_path());
        psxe_diag_breadcrumbf("ARMSX startup argc=%d", argc_);
        psxe_diag_logf("diag", "Pref path: %s", psxe_cfg_get_pref_path() ? psxe_cfg_get_pref_path() : "(none)");

        static bool log_callback_installed = false;
        if (!log_callback_installed) {
            log_add_callback(StructuredLogCallback, nullptr, LOG_TRACE);
            SDL_LogSetOutputFunction(SdlLogOutput, nullptr);
            log_callback_installed = true;
        }

        psxe_config_t* cfg = psxe_cfg_create();
        if (!cfg) {
            return 1;
        }

        psxe_cfg_init(cfg);
        psxe_cfg_load_defaults(cfg);
        psxe_cfg_load(cfg, argc_, const_cast<const char**>(argv_));
        settings_ = BuildSettings(cfg, cli_);
        psxe_cfg_destroy(cfg);
        syncPlatformLibraries(false, false);

        applyLoggingSettings("startup");
#if defined(USE_HARDWARE)
        if (const char* env_backend = std::getenv("ARMSX_GPU_BACKEND")) {
            psxe_diag_breadcrumbf(
                "GPU backend env override=%s resolved=%s",
                env_backend,
                GpuBackendTitle(settings_.gpu_backend)
            );
            psxe_diag_logf(
                "diag",
                "GPU backend env override=%s resolved=%s",
                env_backend,
                GpuBackendTitle(settings_.gpu_backend)
            );
        }
#endif
        psxe_diag_breadcrumbf("Settings loaded model=%s region=%s logging_enabled=%s log_level=%d",
            settings_.model.c_str(),
            settings_.region.c_str(),
            settings_.logging_enabled ? "true" : "false",
            settings_.log_level);

        installCrashHandlers();

        vr_mode_ = !external_window_ && !external_renderer_ && armsx::vr::Requested();
        if (vr_mode_) {
            // No touchscreen in a headset: the Touch controllers drive the pad and the UI.
            settings_.mobile_controls = false;
            psxe_diag_logf("vr", "Quest VR mode requested");
        }

        if (!initializeSdl() || !initializeWindowAndRenderer() || !initializeFsui()) {
            shutdown();
            return 1;
        }

        logRendererBootstrap("frontend-init", kUiFrameRate);
        refreshGameList(true);

        g_active_app = this;
        initializeWebLaunchSupport();
        consumePendingLaunchArguments();

        if (cli_.has_boot_request) {
            if (pending_cli_launch_.has_value()) {
                launchSession(*pending_cli_launch_, false);
            } else if (!pending_cli_argument_.empty()) {
                pending_error_dialog_ = "Unsupported launch path or URI.";
            }
        }

        consumePendingLaunchArguments();

        if (!session_.valid()) {
            showLandingWindow();
        }

        fsui::SdlMainLoopCallbacks loop;
        loop.handle_event = [this](const SDL_Event& event) { handleEvent(event); };
        loop.run_frame = [this]() { runFrame(); };
        loop.should_continue = [this]() { return running_; };
        loop.on_exit = [this]() { shutdown(); };
        fsui::RunSdlMainLoop(loop);
        g_active_app = nullptr;

        return 0;
    }

    void writeCrashContext(const char* reason) const {
        const fsui::CurrentGameInfo info = session_.currentGameInfo();
        psxe_diag_logf("crash", "Crash reason: %s", reason ? reason : "(unknown)");
        psxe_diag_logf("crash", "Running game: %s", info.has_game ? info.title.c_str() : "(none)");
        psxe_diag_logf("crash", "Game path: %s", info.path.empty() ? "(none)" : info.path.string().c_str());
        psxe_diag_logf("crash", "BIOS override: %s", settings_.bios_override.empty() ? "(none)" : settings_.bios_override.c_str());
        psxe_diag_logf("crash", "BIOS folder: %s", settings_.bios_search.c_str());
        psxe_diag_logf("crash", "Model=%s Region=%s Expansion=%s",
            settings_.model.c_str(),
            settings_.region.c_str(),
            settings_.exp_path.empty() ? "(none)" : settings_.exp_path.c_str());
        logRendererBootstrap("crash", currentTargetFrameRate());
        logCpuState();
        psxe_diag_dump_breadcrumbs();
    }

  private:
    void initializeWebLaunchSupport() {
#if defined(__EMSCRIPTEN__)
        static bool initialized = false;
        if (initialized) {
            return;
        }

        initialized = true;
        EM_ASM({
            try {
                const searchParams = new URLSearchParams(window.location.search);
                let launchUri = searchParams.get('uri') || '';
                if (!launchUri && window.location.hash) {
                    const hash = window.location.hash.startsWith('#') ? window.location.hash.substring(1) : window.location.hash;
                    launchUri = new URLSearchParams(hash).get('uri') || '';
                }

                if (launchUri.startsWith('web+armsx:')) {
                    launchUri = 'armsx:' + launchUri.substring('web+armsx:'.length);
                }

                if (window.location.protocol === 'https:' && typeof navigator !== 'undefined' &&
                    typeof navigator.registerProtocolHandler === 'function') {
                    try {
                        navigator.registerProtocolHandler(
                            'web+armsx',
                            window.location.origin + window.location.pathname + '?uri=%s',
                            'ARMSX'
                        );
                    } catch (error) {
                        console.warn('ARMSX protocol handler registration skipped', error);
                    }
                }

                if (launchUri && typeof Module !== 'undefined' && typeof Module.ccall === 'function') {
                    Module.ccall('psxe_enqueue_launch_argument', null, ['string'], [launchUri]);
                }
            } catch (error) {
                console.warn('ARMSX web launch bootstrap failed', error);
            }
        });
#endif
    }

    void requestWebFiles(bool directory) {
#if defined(__EMSCRIPTEN__)
        if (directory) {
            EM_ASM({ window.ARMSXWebFiles.openDirectory(); });
        } else {
            EM_ASM({ window.ARMSXWebFiles.openFiles(); });
        }
#else
        (void)directory;
#endif
    }

    void queueLaunchArgument(std::string_view argument, bool close_ui, const char* error_message = "Unsupported launch path or URI.") {
        const LaunchRequest request = LaunchForArgument(argument);
        psxe_diag_logf("launch", "Launch argument=%s kind=%s", std::string(argument).c_str(), LaunchKindTitle(request.kind));
        if (request.kind == LaunchKind::None) {
            pending_error_dialog_ = error_message;
            if (!session_.valid()) {
                showLandingWindow();
            }
            return;
        }

        queueLaunchRequest(request, close_ui);
    }

    void consumePendingLaunchArguments() {
        for (const std::string& message : DrainWebErrors()) {
            pending_error_dialog_ = message;
            if (!session_.valid()) {
                showLandingWindow();
            }
        }
        for (const std::string& argument : DrainPendingLaunchArguments()) {
            queueLaunchArgument(argument, true);
        }
    }

    void queueLaunchRequest(const LaunchRequest& request, bool close_ui) {
        if (request.kind == LaunchKind::None) {
            return;
        }

        deferred_launch_ = request;
        close_ui_after_launch_ = close_ui;
    }

    void queueLaunchSelection(const std::string& path, std::optional<LaunchKind> forced_kind = std::nullopt) {
        const std::optional<std::string> selection = NormalizedPickerSelection(path);
        if (!selection.has_value()) {
            return;
        }

        const LaunchRequest request = LaunchForArgument(*selection, forced_kind);
        if (request.kind != LaunchKind::None) {
            queueLaunchRequest(request, true);
        }
    }

    void queueDiscSwapSelection(const std::string& path) {
        const std::optional<std::string> selection = NormalizedPickerSelection(path);
        if (!selection.has_value()) {
            return;
        }

        deferred_change_disc_ = std::filesystem::path(*selection);
    }

    Uint32 managedRendererFlags(bool vsync_enabled) const {
        if (vr_mode_) {
            // Frames go to the headset through an offscreen texture, so there is no window present
            // to synchronize, and the XR copy needs a GL renderer.
            return SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE;
        }
        Uint32 renderer_flags = settings_.gpu_backend == GpuBackend::SDLAccelerated
            ? SDL_RENDERER_ACCELERATED
            : SDL_RENDERER_SOFTWARE;
#if !defined(__EMSCRIPTEN__)
        if (vsync_enabled) {
            renderer_flags |= SDL_RENDERER_PRESENTVSYNC;
        }
#endif
        return renderer_flags;
    }

#ifdef USE_HARDWARE
    bool hardwareRendererAvailable() const {
        if (!owns_renderer_ || external_renderer_ || !renderer_ || vr_mode_) {
            return false;
        }
        return SupportsHardwareGpuBackend();
    }
#endif

    bool createManagedRenderer(bool vsync_enabled) {
        renderer_ = SDL_CreateRenderer(window_, -1, managedRendererFlags(vsync_enabled));
        owns_renderer_ = renderer_ != nullptr;

        if (!renderer_ && settings_.gpu_backend == GpuBackend::SDLAccelerated) {
            psxe_diag_logf(
                "renderer",
                "SDL accelerated renderer unavailable error=%s; retrying software renderer.",
                SDL_GetError()
            );
            Uint32 fallback_flags = SDL_RENDERER_SOFTWARE;
#if !defined(__EMSCRIPTEN__)
            if (vsync_enabled) {
                fallback_flags |= SDL_RENDERER_PRESENTVSYNC;
            }
#endif
            renderer_ = SDL_CreateRenderer(window_, -1, fallback_flags);
            owns_renderer_ = renderer_ != nullptr;
        }

        if (!renderer_) {
            psxe_diag_logf(
                "renderer",
                "Renderer initialization failed requested_vsync=%s error=%s",
                vsync_enabled ? "true" : "false",
                SDL_GetError()
            );
            return false;
        }

        SDL_RendererInfo info{};
        if (SDL_GetRendererInfo(renderer_, &info) == 0) {
            psxe_diag_logf(
                "renderer",
                "Selected SDL renderer driver=%s flags=%s",
                info.name ? info.name : "(unknown)",
                RendererFlagsTitle(info.flags).c_str()
            );
        }

        managed_renderer_vsync_ = vsync_enabled;
        return true;
    }

    void shutdownFsuiFrontend() {
        if (fsui_initialized_) {
            fsui::Shutdown(true);
            fsui_initialized_ = false;
        }

        ImGuiFullscreen::ClearFileSelectorExtraRoots();

        imgui_backend_.shutdown();

        if (ImGui::GetCurrentContext()) {
            ImGui::DestroyContext();
        }
    }

    void refreshFileSelectorRoots() {
        std::vector<ImGuiFullscreen::FileSelectorRootEntry> roots{
            ImGuiFullscreen::FileSelectorRootEntry{
                .title = "App Folder",
                .path = DefaultBrowseDirectory().string(),
            },
        };
#if defined(__ANDROID__)
        if (vr_mode_) {
            // The Quest build holds all-files access, so shared storage -- where files copied onto
            // the headset over USB land -- is browsable directly.
            const char* external = std::getenv("EXTERNAL_STORAGE");
            roots.push_back(ImGuiFullscreen::FileSelectorRootEntry{
                .title = "Internal Storage",
                .path = (external && external[0]) ? external : "/storage/emulated/0",
            });
        }
#endif

        ImGuiFullscreen::SetFileSelectorExtraRoots(std::move(roots));
    }

    void showLandingWindow() {
        ui_window_state_ = FsuiWindowState::Landing;
        fsui::ShowLandingWindow();
    }

    void showStartGameWindow() {
        ui_window_state_ = FsuiWindowState::StartGame;
        fsui::ShowStartGameWindow();
    }

    void showExitWindow() {
        ui_window_state_ = FsuiWindowState::Exit;
        fsui::ShowExitWindow();
    }

    void showGameListWindow() {
        ui_window_state_ = FsuiWindowState::GameList;
        fsui::ShowGameListWindow();
    }

    void switchToSettingsWindow() {
        ui_window_state_ = FsuiWindowState::Settings;
        fsui::SwitchToSettings();
    }

    void returnToMainWindow() {
        ui_window_state_ = session_.valid() ? FsuiWindowState::None : FsuiWindowState::Landing;
        fsui::ReturnToMainWindow();
    }

    void showPauseMenuWindow() {
        ui_window_state_ = FsuiWindowState::PauseMenu;
        fsui::OpenPauseMenu();
    }

    void restoreUiWindow() {
        switch (ui_window_state_) {
            case FsuiWindowState::Landing:
                showLandingWindow();
                break;
            case FsuiWindowState::StartGame:
                showStartGameWindow();
                break;
            case FsuiWindowState::Exit:
                showExitWindow();
                break;
            case FsuiWindowState::GameList:
                showGameListWindow();
                break;
            case FsuiWindowState::Settings:
                switchToSettingsWindow();
                break;
            case FsuiWindowState::PauseMenu:
                showPauseMenuWindow();
                break;
            case FsuiWindowState::None:
            default:
                break;
        }
    }

    bool recreateManagedRenderer(bool desired_vsync) {
        // The XR thread's GL context shares SDL's; a new renderer would leave it behind.
        if (!owns_renderer_ || external_renderer_ || !window_ || vr_mode_) {
            return false;
        }

        const bool had_session = session_.valid();
        const bool was_paused = had_session ? session_.paused() : false;
        const FsuiWindowState restore_window = ui_window_state_;
        const bool previous_vsync = managed_renderer_vsync_;
        const bool should_be_paused_after_restore = had_session && (was_paused || restore_window != FsuiWindowState::None);

        auto restore_previous_renderer = [&](const char* phase, const char* message) {
            settings_.vsync_enabled = previous_vsync;
            SaveSettings(settings_);

            if (renderer_) {
                SDL_DestroyRenderer(renderer_);
                renderer_ = nullptr;
            }
            owns_renderer_ = false;

            if (!createManagedRenderer(previous_vsync) || !initializeFsui()) {
                psxe_diag_logf("renderer", "Renderer recovery failed phase=%s error=%s", phase ? phase : "(none)", SDL_GetError());
                running_ = false;
                return false;
            }

            if (had_session) {
                session_.rebindRenderer(renderer_, settings_);
                session_.setPaused(should_be_paused_after_restore);
            }

            ui_window_state_ = restore_window;
            restoreUiWindow();
            resetFramePacing("renderer-recovery");
            logRendererBootstrap(phase, currentTargetFrameRate());
            pending_error_dialog_ = message;
            return false;
        };

        if (had_session) {
            session_.setPaused(true);
        }

        shutdownFsuiFrontend();

        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }
        owns_renderer_ = false;

        if (!createManagedRenderer(desired_vsync)) {
            return restore_previous_renderer("renderer-recreate-recover", "Failed to apply the requested VSync mode; restored the previous renderer.");
        }

        if (!initializeFsui()) {
            return restore_previous_renderer("renderer-recreate-recover", "Failed to rebuild the UI after changing VSync; restored the previous renderer.");
        }

        if (had_session) {
            session_.rebindRenderer(renderer_, settings_);
            session_.setPaused(should_be_paused_after_restore);
        }

        ui_window_state_ = restore_window;
        restoreUiWindow();
        resetFramePacing("renderer-recreate");
        logRendererBootstrap("renderer-recreate", currentTargetFrameRate());
        return true;
    }

    void installCrashHandlers() {
        static bool installed = false;
        if (installed) {
            return;
        }

        installed = true;

        std::set_terminate([]() {
            ReportNativeCrash("std::terminate");
        });

        auto signal_handler = [](int signal_value) {
            switch (signal_value) {
                case SIGABRT:
                    ReportNativeCrash("SIGABRT");
                    break;
                case SIGSEGV:
                    ReportNativeCrash("SIGSEGV");
                    break;
                case SIGILL:
                    ReportNativeCrash("SIGILL");
                    break;
                case SIGFPE:
                    ReportNativeCrash("SIGFPE");
                    break;
                default:
                    ReportNativeCrash("signal");
                    break;
            }
        };

        std::signal(SIGABRT, signal_handler);
        std::signal(SIGSEGV, signal_handler);
        std::signal(SIGILL, signal_handler);
        std::signal(SIGFPE, signal_handler);

#if defined(_WIN32) && !defined(UWP_TARGET)
        SetUnhandledExceptionFilter(&WindowsUnhandledExceptionFilter);
#endif
    }

    double currentTargetFrameRate() const {
        if (session_.valid() && !session_.paused()) {
            return session_.targetFrameRate();
        }

        return kUiFrameRate;
    }

    // In VR, the frame rate is nudged by at most +-0.5% so the queued audio stays near
    // kVrAudioTargetMs. Emulation is timed by the CPU clock and playback by the audio hardware's,
    // and the two drift apart: without this the queue slowly runs dry (clicks as SDL pads with
    // silence) or overfills (and is flushed).
    double pacedFrameRate() const {
        const double rate = currentTargetFrameRate();
        if (!vr_mode_ || !session_.valid() || session_.paused() || session_.fastForwardEnabled()) {
            return rate;
        }
        const double queued_ms = session_.queuedAudioMs();
        if (queued_ms < 0.0) {
            return rate;
        }
        const double error = std::clamp((queued_ms - kVrAudioTargetMs) / kVrAudioTargetMs, -1.0, 1.0);
        return rate * (1.0 - error * kVrAudioMaxRateAdjust);
    }

    // Comfortably above one Android output buffer (2048 frames, ~46 ms), which is pulled at once.
    static constexpr double kVrAudioTargetMs = 80.0;
    static constexpr double kVrAudioMaxRateAdjust = 0.005;

    void updateFramePeriod(double frame_rate) {
#if !defined(__EMSCRIPTEN__)
        const double safe_rate = std::max(frame_rate, 1.0);
        const uint64_t frequency = SDL_GetPerformanceFrequency();
        const uint64_t ticks = std::max<uint64_t>(
            1,
            static_cast<uint64_t>(std::llround(static_cast<double>(frequency) / safe_rate))
        );

        // Small adjustments (the VR audio rate control below) keep the running deadline; only a
        // real change of target, such as NTSC to PAL, restarts the pacing.
        const bool small_change = frame_period_ticks_ != 0 &&
            std::llabs(static_cast<long long>(ticks) - static_cast<long long>(frame_period_ticks_)) * 50 <
                static_cast<long long>(frame_period_ticks_);
        if (frame_period_ticks_ != ticks && small_change) {
            frame_period_ticks_ = ticks;
        } else if (frame_period_ticks_ != ticks) {
            frame_period_ticks_ = ticks;
            next_frame_deadline_ = 0;
            psxe_diag_breadcrumbf(
                "Frame pacing target updated fps=%.2f period_ms=%.3f",
                safe_rate,
                CounterTicksToMilliseconds(frame_period_ticks_)
            );
        }
#else
        (void)frame_rate;
#endif
    }

    void waitForFrameDeadline(double frame_rate) {
#if !defined(__EMSCRIPTEN__)
        updateFramePeriod(frame_rate);

        const uint64_t frequency = SDL_GetPerformanceFrequency();
        const uint64_t slack_ticks = std::max<uint64_t>(1, frequency / 2000u);
        uint64_t now = SDL_GetPerformanceCounter();

        if (next_frame_deadline_ == 0) {
            next_frame_deadline_ = now;
            return;
        }

        if (frame_period_ticks_ != 0 && now > (next_frame_deadline_ + (frame_period_ticks_ * 2u))) {
            psxe_diag_breadcrumbf(
                "Frame pacing resync lateness_ms=%.3f",
                CounterTicksToMilliseconds(now - next_frame_deadline_)
            );
            next_frame_deadline_ = now;
            return;
        }

        while (now + slack_ticks < next_frame_deadline_) {
            const double wait_ms = CounterTicksToMilliseconds(next_frame_deadline_ - now);
            if (wait_ms > 1.5) {
                SDL_Delay(static_cast<Uint32>(wait_ms - 1.0));
            } else {
                std::this_thread::yield();
            }
            now = SDL_GetPerformanceCounter();
        }

        while (now < next_frame_deadline_) {
            std::this_thread::yield();
            now = SDL_GetPerformanceCounter();
        }
#else
        (void)frame_rate;
#endif
    }

    void advanceFrameDeadline() {
#if !defined(__EMSCRIPTEN__)
        if (frame_period_ticks_ == 0) {
            return;
        }

        if (next_frame_deadline_ == 0) {
            next_frame_deadline_ = SDL_GetPerformanceCounter();
        }

        next_frame_deadline_ += frame_period_ticks_;
#endif
    }

    void resetFramePacing(const char* reason) {
#if !defined(__EMSCRIPTEN__)
        next_frame_deadline_ = 0;
        frame_period_ticks_ = 0;
        if (reason && reason[0]) {
            psxe_diag_breadcrumbf("Frame pacing reset reason=%s", reason);
        }
#else
        (void)reason;
#endif
    }

    std::filesystem::path diagnosticsLogPath() const {
        const char* live_path = psxe_diag_log_path();
        if (live_path && live_path[0]) {
            return std::filesystem::path(live_path);
        }

        return DefaultDiagnosticsLogPath();
    }

    void applyLoggingSettings(const char* reason) {
        const bool enable_logs = settings_.logging_enabled;
        settings_.quiet = !enable_logs;

        if (!enable_logs && psxe_diag_is_enabled()) {
            psxe_diag_logf("diag", "Debug logging disabled reason=%s", reason ? reason : "(none)");
        }

        psxe_diag_set_enabled(enable_logs ? 1 : 0);
        log_set_level(settings_.log_level);
        log_set_quiet(settings_.quiet ? 1 : 0);

        if (enable_logs) {
            psxe_diag_logf(
                "diag",
                "Debug logging enabled reason=%s level=%s path=%s",
                reason ? reason : "(none)",
                log_level_string(settings_.log_level),
                diagnosticsLogPath().string().c_str()
            );
        }
    }

    void logRendererBootstrap(const char* phase, double frame_rate) const {
        if (!renderer_) {
            return;
        }

        SDL_RendererInfo info{};
        SDL_GetRendererInfo(renderer_, &info);

        int output_width = 0;
        int output_height = 0;
        SDL_GetRendererOutputSize(renderer_, &output_width, &output_height);

        int refresh_rate = 0;
        SDL_DisplayMode display_mode{};
        if (window_) {
            const int display_index = SDL_GetWindowDisplayIndex(window_);
            if (display_index >= 0 && SDL_GetCurrentDisplayMode(display_index, &display_mode) == 0) {
                refresh_rate = display_mode.refresh_rate;
            }
        }

        const char* timing_title = session_.valid() ? session_.timingModeTitle() : "UI";
        psxe_diag_logf(
            "renderer",
            "phase=%s source=%s driver=%s flags=%s output=%dx%d refresh_hz=%d timing=%s target_fps=%.2f frame_period_ms=%.3f requested_vsync=%s",
            phase ? phase : "(none)",
            external_renderer_ ? "external" : "internal",
            info.name ? info.name : "(unknown)",
            RendererFlagsTitle(info.flags).c_str(),
            output_width,
            output_height,
            refresh_rate,
            timing_title,
            frame_rate,
            frame_rate > 0.0 ? (1000.0 / frame_rate) : 0.0,
            (owns_renderer_ && !external_renderer_) ? (managed_renderer_vsync_ ? "true" : "false") : "host-controlled"
        );
    }

    void logUiRendererState(const char* stage) const {
        if (!renderer_) {
            return;
        }

        int window_width = 0;
        int window_height = 0;
        if (window_) {
            SDL_GetWindowSize(window_, &window_width, &window_height);
        }

        int output_width = 0;
        int output_height = 0;
        SDL_GetRendererOutputSize(renderer_, &output_width, &output_height);

        SDL_Rect viewport = {0, 0, 0, 0};
        SDL_Rect clip_rect = {0, 0, 0, 0};
        int logical_width = 0;
        int logical_height = 0;
        float scale_x = 0.0f;
        float scale_y = 0.0f;

        SDL_RenderGetViewport(renderer_, &viewport);
        SDL_RenderGetClipRect(renderer_, &clip_rect);
        SDL_RenderGetLogicalSize(renderer_, &logical_width, &logical_height);
        SDL_RenderGetScale(renderer_, &scale_x, &scale_y);

        const SDL_Texture* target = SDL_GetRenderTarget(renderer_);
        const Uint32 window_flags = window_ ? SDL_GetWindowFlags(window_) : 0u;
        const bool clip_enabled = SDL_RenderIsClipEnabled(renderer_) == SDL_TRUE;
        const bool integer_scale = SDL_RenderGetIntegerScale(renderer_) == SDL_TRUE;

        float imgui_display_x = 0.0f;
        float imgui_display_y = 0.0f;
        float imgui_scale_x = 0.0f;
        float imgui_scale_y = 0.0f;
        float imgui_font_scale = 0.0f;
        int imgui_backend_flags = 0;
        int imgui_config_flags = 0;
        if (ImGui::GetCurrentContext()) {
            const ImGuiIO& io = ImGui::GetIO();
            imgui_display_x = io.DisplaySize.x;
            imgui_display_y = io.DisplaySize.y;
            imgui_scale_x = io.DisplayFramebufferScale.x;
            imgui_scale_y = io.DisplayFramebufferScale.y;
            imgui_font_scale = io.FontGlobalScale;
            imgui_backend_flags = static_cast<int>(io.BackendFlags);
            imgui_config_flags = static_cast<int>(io.ConfigFlags);
        }

        psxe_diag_logf(
            "ui",
            "%s window=%dx%d window_flags=0x%x output=%dx%d target=%p viewport=%d,%d %dx%d clip=%d,%d %dx%d scale=(%f,%f) logical=%dx%d clip_enabled=%s integer_scale=%s imgui_display=(%.1f,%.1f) imgui_fb_scale=(%.2f,%.2f) imgui_font_scale=%.3f backend_flags=0x%x config_flags=0x%x session_hw=%s texture=%dx%d format=%s",
            stage ? stage : "(state)",
            window_width,
            window_height,
            window_flags,
            output_width,
            output_height,
            (const void*)target,
            viewport.x,
            viewport.y,
            viewport.w,
            viewport.h,
            clip_rect.x,
            clip_rect.y,
            clip_rect.w,
            clip_rect.h,
            scale_x,
            scale_y,
            logical_width,
            logical_height,
            clip_enabled ? "true" : "false",
            integer_scale ? "true" : "false",
            imgui_display_x,
            imgui_display_y,
            imgui_scale_x,
            imgui_scale_y,
            imgui_font_scale,
            imgui_backend_flags,
            imgui_config_flags,
#ifdef USE_HARDWARE
            session_.valid() ? (session_.hardwareBackendActive() ? "true" : "false") : "false",
            session_.valid() ? session_.textureWidth() : 0,
            session_.valid() ? session_.textureHeight() : 0,
            session_.valid() ? SDL_GetPixelFormatName(session_.textureFormat()) : "(none)"
#else
            "false",
            0,
            0,
            "(none)"
#endif
        );
    }

    void logCpuState() const {
        if (!session_.valid() || !session_.psx() || !session_.psx()->cpu) {
            psxe_diag_logf("crash", "CPU state unavailable.");
            return;
        }

        const psx_cpu_t* cpu = session_.psx()->cpu;
        psxe_diag_logf("crash", "r0=%08x at=%08x v0=%08x v1=%08x", cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3]);
        psxe_diag_logf("crash", "a0=%08x a1=%08x a2=%08x a3=%08x", cpu->r[4], cpu->r[5], cpu->r[6], cpu->r[7]);
        psxe_diag_logf("crash", "t0=%08x t1=%08x t2=%08x t3=%08x", cpu->r[8], cpu->r[9], cpu->r[10], cpu->r[11]);
        psxe_diag_logf("crash", "t4=%08x t5=%08x t6=%08x t7=%08x", cpu->r[12], cpu->r[13], cpu->r[14], cpu->r[15]);
        psxe_diag_logf("crash", "s0=%08x s1=%08x s2=%08x s3=%08x", cpu->r[16], cpu->r[17], cpu->r[18], cpu->r[19]);
        psxe_diag_logf("crash", "s4=%08x s5=%08x s6=%08x s7=%08x", cpu->r[20], cpu->r[21], cpu->r[22], cpu->r[23]);
        psxe_diag_logf("crash", "t8=%08x t9=%08x k0=%08x k1=%08x", cpu->r[24], cpu->r[25], cpu->r[26], cpu->r[27]);
        psxe_diag_logf("crash", "gp=%08x sp=%08x fp=%08x ra=%08x", cpu->r[28], cpu->r[29], cpu->r[30], cpu->r[31]);
        psxe_diag_logf(
            "crash",
            "pc=%08x next=%08x saved=%08x hi=%08x lo=%08x epc=%08x opcode=%08x",
            cpu->pc,
            cpu->next_pc,
            cpu->saved_pc,
            cpu->hi,
            cpu->lo,
            cpu->cop0_r[COP0_EPC],
            cpu->opcode
        );
    }

    bool initializeSdl() {
        const Uint32 required = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER;
        psxe_diag_breadcrumbf("Initializing SDL subsystems flags=0x%x", required);

        if (SDL_WasInit(0) == 0) {
            owns_sdl_ = true;
            if (SDL_Init(required) != 0) {
                psxe_diag_logf("sdl", "SDL_Init failed: %s", SDL_GetError());
                return false;
            }
        } else if (SDL_InitSubSystem(required) != 0) {
            psxe_diag_logf("sdl", "SDL_InitSubSystem failed: %s", SDL_GetError());
            return false;
        }

        if (argc_ > 1) {
            for (int index = 1; index < argc_; index++) {
                const std::string_view arg(argv_[index] ? argv_[index] : "");

                auto set_boot_argument = [&](std::string_view value, std::optional<LaunchKind> forced_kind = std::nullopt) {
                    pending_cli_argument_ = std::string(value);
                    pending_cli_launch_ = LaunchForArgument(value, forced_kind);
                };

                if (arg == "--cdrom") {
                    if ((index + 1) < argc_) {
                        set_boot_argument(argv_[index + 1] ? argv_[index + 1] : "", LaunchKind::Disc);
                        index++;
                    }
                } else if (arg == "-x" || arg == "--exe") {
                    if ((index + 1) < argc_) {
                        set_boot_argument(argv_[index + 1] ? argv_[index + 1] : "", LaunchKind::Exe);
                        index++;
                    }
                } else if (arg.starts_with("--cdrom=")) {
                    set_boot_argument(arg.substr(8), LaunchKind::Disc);
                } else if (arg.starts_with("--exe=")) {
                    set_boot_argument(arg.substr(6), LaunchKind::Exe);
                } else if (!arg.empty() && arg[0] != '-') {
                    set_boot_argument(arg);
                }
            }
        }

        return true;
    }

    bool initializeWindowAndRenderer() {
        if (external_window_) {
            window_ = external_window_;
        } else {
            int window_width = 1280;
            int window_height = 720;
#if defined(__EMSCRIPTEN__)
            double css_width = 0.0;
            double css_height = 0.0;
            if (emscripten_get_element_css_size("#canvas", &css_width, &css_height) == EMSCRIPTEN_RESULT_SUCCESS &&
                css_width > 0.0 && css_height > 0.0) {
                window_width = std::max(1, static_cast<int>(std::lround(css_width)));
                window_height = std::max(1, static_cast<int>(std::lround(css_height)));
            }
#endif
            Uint32 flags = 0;
            if (SupportsManagedWindowSizing()) {
                flags |= SDL_WINDOW_RESIZABLE;
            }
#if defined(__ANDROID__) || defined(IOS_TARGET)
            flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
            if (vr_mode_) {
                // An ES 3 context, which the XR thread shares and OpenXR requires. Only a window
                // created with SDL_WINDOW_OPENGL keeps these attributes; otherwise the GLES2
                // renderer recreates it with an ES 2 context.
                SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
                flags |= SDL_WINDOW_OPENGL;
            }
            window_ = SDL_CreateWindow(
                "ARMSX",
                SDL_WINDOWPOS_CENTERED,
                SDL_WINDOWPOS_CENTERED,
                window_width,
                window_height,
                flags
            );
            owns_window_ = window_ != nullptr;
        }

        if (!window_) {
            psxe_diag_logf("renderer", "Window initialization failed: %s", SDL_GetError());
            return false;
        }

        if (external_renderer_) {
            renderer_ = external_renderer_;
            owns_renderer_ = false;
        } else {
            renderer_ = nullptr;
            if (!createManagedRenderer(settings_.vsync_enabled)) {
                return false;
            }
        }

        if (!renderer_) {
            psxe_diag_logf("renderer", "Renderer initialization failed: %s", SDL_GetError());
            return false;
        }

        if (vr_mode_) {
            startVr();
        }

        return true;
    }

    bool initializeFsui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.IniFilename = nullptr;

        fsui::SdlImGuiBackendConfig backend_config;
        backend_config.window = window_;
        backend_config.sdl_renderer = renderer_;
        backend_config.renderer_backend = fsui::RendererBackend::SDLRenderer;
#if defined(__EMSCRIPTEN__)
        // SDL_Renderer on Emscripten renders correctly when the canvas backing store
        // matches the CSS size instead of a DPR-scaled framebuffer.
        backend_config.web_canvas.max_device_pixel_ratio = 1.0;
#endif

        if (!imgui_backend_.initialize(backend_config)) {
            return false;
        }

#if defined(__EMSCRIPTEN__)
        fsui::SyncSdl2WebCanvasLayout(window_, backend_config.web_canvas, true);
#endif

        fsui::FontStack fonts;
        if (!fsui::BuildDefaultFontStack(fsui::DescribeSdl2FontBootstrap(window_), fonts)) {
            return false;
        }

        fsui::UiContext context;
        context.window = window_;
        context.fonts = fonts;
        context.app_title = "ARMSX";
        context.app_icon_path = ResolveFsuiAppIconPath();
        context.platform_backend = fsui::PlatformBackendKind::SDL2;
        context.renderer_backend = fsui::RendererBackend::SDLRenderer;

        fsui::Host host;
        host.ui_state = &settings_.ui_state;
        host.has_running_game = [this]() { return session_.valid(); };
        host.get_current_game = [this]() { return session_.currentGameInfo(); };
        host.get_game_list = [this]() { return game_list_; };
        host.refresh_game_list = [this](bool full_rescan) { refreshGameList(full_rescan); };
        host.persist_ui_state = [this](bool reload) {
            if (reload) {
                refreshGameList(true);
            }
            SaveSettings(settings_);
        };
        host.set_paused = [this](bool paused) {
            if (!session_.valid()) {
                return;
            }
            session_.setPaused(paused);
            resetFramePacing(paused ? "fsui-pause" : "fsui-unpause");
            if (paused) {
                input_router_.onFsuiOpened();
            }
        };
        host.resume_game = [this]() {
            if (session_.valid()) {
                session_.setPaused(false);
                resetFramePacing("fsui-resume");
            }
        };
        host.reset_system = [this]() {
            if (session_.valid()) {
                session_.reset();
                resetFramePacing("fsui-reset");
            }
        };
        host.exit_to_library = [this]() { exitToLibrary(); };
        host.request_quit = [this]() { running_ = false; };
        host.request_classic_ui = []() {};
        host.close_selector = []() {};
        host.detect_prompt_icon_pack = []() { return fsui::DetectPromptIconPackFromSDL(); };
        host.detect_swap_north_west_gamepad_buttons = []() { return false; };
        host.runtime_overlay_options = fsui::RuntimeOverlayOptions{
            .show_inputs = true,
            .show_settings = true,
            .show_performance = true,
        };
        host.get_input_overlay_devices = [this]() { return input_router_.buildInputOverlay(); };
        host.get_settings_overlay_lines = [this]() { return session_.settingsOverlayLines(settings_); };
        host.get_performance_overlay_lines = [this]() { return session_.performanceOverlayLines(settings_); };
        host.get_landing_items = [this]() { return buildLandingItems(); };
        host.get_start_items = [this]() { return buildStartItems(); };
        host.get_exit_items = [this]() { return buildExitItems(); };
        host.get_pause_items = [this]() { return buildPauseItems(); };
        host.get_game_launch_options = [this](const fsui::GameEntry& entry) { return buildGameLaunchOptions(entry); };
        host.get_settings_pages = [this](fsui::SettingsScope scope) { return buildSettingsPages(scope); };

        context.host = std::move(host);
        refreshFileSelectorRoots();

        if (!fsui::Initialize(context)) {
            return false;
        }

        fsui_initialized_ = true;
        return true;
    }

    void shutdown() {
        psxe_diag_breadcrumbf("Frontend shutdown");
        input_router_.detach();
        session_.destroy();

        shutdownFsuiFrontend();

        armsx::vr::Stop();

        if (owns_renderer_ && renderer_) {
            SDL_DestroyRenderer(renderer_);
        }
        if (owns_window_ && window_) {
            SDL_DestroyWindow(window_);
        }

        renderer_ = nullptr;
        window_ = nullptr;

        if (owns_sdl_) {
            SDL_Quit();
        }

        psxe_diag_shutdown();
    }

    void handleEvent(const SDL_Event& event) {
        imgui_backend_.processEvent(event);
        mobile_controls_.processEvent(event);

        if (event.type == SDL_QUIT) {
            running_ = false;
            return;
        }

        if (event.type == SDL_DROPFILE) {
            if (event.drop.file) {
                queueLaunchArgument(event.drop.file, true);
                SDL_free(event.drop.file);
            }
            return;
        }

        if (event.type == SDL_DROPCOMPLETE) {
            return;
        }

        const bool fsui_active = fsui::HasActiveWindow();
        input_router_.processEvent(event, session_.valid() ? &session_ : nullptr, fsui_active);
        if (input_router_.takeControllerDisconnected()) {
            mobile_controls_.onControllerDisconnected();
        }
        if (input_router_.takeControllerActivity()) {
            mobile_controls_.onControllerActivity(input_router_);
        }

        if (event.type == SDL_KEYDOWN && !fsui_active && session_.valid() && event.key.repeat == 0 && event.key.keysym.sym == SDLK_RETURN) {
            psx_exp2_atcons_put(session_.psx()->exp2, 13);
        }

        if (session_.valid() && !fsui_active && input_router_.takePauseRequest()) {
            openPauseMenu();
        }
    }

    void runFrame() {
        const bool fsui_was_active = fsui::HasActiveWindow();
        input_router_.tick(fsui_was_active);
        pollVr(fsui_was_active);
        if (!running_) {
            return;
        }
        syncPlatformLibraries(true, true);

        if (deferred_vsync_.has_value()) {
            const bool desired_vsync = *deferred_vsync_;
            deferred_vsync_.reset();

            if (owns_renderer_ && !external_renderer_ && desired_vsync != managed_renderer_vsync_) {
                recreateManagedRenderer(desired_vsync);
                if (!running_) {
                    return;
                }
            }
        }

        consumePendingLaunchArguments();

        waitForFrameDeadline(pacedFrameRate());
        const uint64_t frame_work_start = SDL_GetPerformanceCounter();

        std::uint32_t session_steps = 0;
        uint64_t emulate_ticks = 0;
        uint64_t texture_ticks = 0;
        if (session_.valid() && !session_.paused()) {
            const uint64_t emulate_start = SDL_GetPerformanceCounter();
            session_steps = session_.runFrame();
            const uint64_t texture_start = SDL_GetPerformanceCounter();
            session_.updateTexture(settings_);
            session_.finishHardwareFrame();
            emulate_ticks = texture_start - emulate_start;
            texture_ticks = SDL_GetPerformanceCounter() - texture_start;
        }

#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (session_.valid() && session_.hardwareBackendActive()) {
            logUiRendererState("ui-pre-imgui");
            psxe_diag_logf(
                "hw",
                "frame-present frame=%llu steps=%u paused=%s fast_forward=%s texture=%dx%d format=%s target_fps=%.3f",
                static_cast<unsigned long long>(session_.vblankCounter()),
                session_steps,
                session_.paused() ? "true" : "false",
                session_.fastForwardEnabled() ? "true" : "false",
                session_.textureWidth(),
                session_.textureHeight(),
                SDL_GetPixelFormatName(session_.textureFormat()),
                session_.targetFrameRate()
            );
        }
#endif
        imgui_backend_.newFrame();
        feedVrImGui(fsui::HasActiveWindow());
#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (session_.valid() && session_.hardwareBackendActive()) {
            logUiRendererState("ui-post-imgui-backend");
        }
#endif
        ImGui::NewFrame();

        if (session_.valid()) {
            session_.draw(settings_);
        }

        if (session_.valid() && settings_.mobile_controls && !fsui::HasActiveWindow()) {
            mobile_controls_.drawAndRoute(input_router_);
        } else {
            mobile_controls_.reset(input_router_);
        }

        if (pending_error_dialog_.has_value()) {
            ImGuiFullscreen::OpenInfoMessageDialog("ARMSX", *pending_error_dialog_);
            pending_error_dialog_.reset();
        }

        fsui::Render();

        for (const fsui::Command& command : fsui::ConsumeCommands()) {
            applyCommand(command);
        }

        applyDeferredActions();

        drawVrCursor();
        ImGui::Render();
        const uint64_t submit_start = SDL_GetPerformanceCounter();
        if (!renderVrFrame()) {
            imgui_backend_.renderDrawData(ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
        }
        const uint64_t submit_ticks = SDL_GetPerformanceCounter() - submit_start;

#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (session_.valid() && session_.hardwareBackendActive()) {
            logUiRendererState("ui-post-imgui-render");
        }
#endif

        const bool fsui_is_active = fsui::HasActiveWindow();

        if (fsui_was_active && !fsui_is_active) {
            ui_window_state_ = FsuiWindowState::None;
            if (session_.valid()) {
                input_router_.onFsuiClosed();
            }
        } else if (!fsui_was_active && fsui_is_active && session_.valid()) {
            input_router_.onFsuiOpened();
        }

        advanceFrameDeadline();
        if (vr_mode_ && VrPerfProbeEnabled()) {
            probeVrPerformance(frame_work_start, session_steps != 0, emulate_ticks, texture_ticks, submit_ticks);
        }
    }

    void applyCommand(const fsui::Command& command) {
        psxe_diag_breadcrumbf("Frontend command type=%d", static_cast<int>(command.type));

        switch (command.type) {
            case fsui::CommandType::LaunchPath:
                queueLaunchArgument(command.path.string(), true);
                break;

            case fsui::CommandType::RequestQuit:
                if (session_.valid()) {
                    exitToLibrary();
                } else {
                    running_ = false;
                }
                break;

            case fsui::CommandType::Resume:
                if (session_.valid()) {
                    // The pause menu's Resume item only sends this command; FSUI's window stays up
                    // until it is told to close. ReturnToMainWindow queues another Resume itself,
                    // which finds the window closed and ends here.
                    if (fsui::HasActiveWindow()) {
                        fsui::ReturnToMainWindow();
                    }
                    ui_window_state_ = FsuiWindowState::None;
                    session_.setPaused(false);
                    resetFramePacing("command-resume");
                }
                break;

            case fsui::CommandType::Reset:
                if (session_.valid()) {
                    session_.reset();
                    session_.setPaused(false);
                    resetFramePacing("command-reset");
                }
                break;

            case fsui::CommandType::ExitToLibrary:
                exitToLibrary();
                break;

            case fsui::CommandType::CloseSelector:
            case fsui::CommandType::ExitFullscreenUI:
            case fsui::CommandType::SwitchShell:
            case fsui::CommandType::Custom:
            default:
                break;
        }
    }

    void applyDeferredActions() {
        if (deferred_launch_.has_value()) {
            const LaunchRequest request = *deferred_launch_;
            deferred_launch_.reset();
            psxe_diag_breadcrumbf("Deferred launch kind=%s path=%s",
                LaunchKindTitle(request.kind),
                request.path.empty() ? "(none)" : request.path.string().c_str());
            if (request.kind != LaunchKind::None) {
                launchSession(request, close_ui_after_launch_);
            }
            close_ui_after_launch_ = false;
        }

        if (deferred_exit_to_library_) {
            deferred_exit_to_library_ = false;
            exitToLibrary();
        }

        if (deferred_reset_) {
            deferred_reset_ = false;
            if (session_.valid()) {
                session_.reset();
                session_.setPaused(false);
                resetFramePacing("deferred-reset");
            }
        }

        if (deferred_change_disc_.has_value()) {
            const std::filesystem::path path = *deferred_change_disc_;
            deferred_change_disc_.reset();

            if (path.empty()) {
                returnToMainWindow();
                return;
            }

            if (!session_.swapDisc(path)) {
                pending_error_dialog_ = "Failed to swap to the selected disc image.";
            } else {
                session_.setPaused(false);
                resetFramePacing("change-disc");
                refreshGameList(false);
                returnToMainWindow();
            }
        }

        if (deferred_state_action_.has_value()) {
            const StateAction action = *deferred_state_action_;
            deferred_state_action_.reset();
            applyStateAction(action);
        }

        if (deferred_screenshot_) {
            deferred_screenshot_ = false;

            const std::filesystem::path snap_dir = DefaultBrowseDirectory() / "snap";
            try {
                std::filesystem::create_directories(snap_dir);
                const std::time_t now = std::time(nullptr);
                char name[64] = {};
                std::strftime(name, sizeof(name), "armsx-%Y%m%d-%H%M%S.bmp", std::localtime(&now));
                if (!session_.saveScreenshot(snap_dir / name)) {
                    pending_error_dialog_ = "Failed to write the screenshot.";
                }
            } catch (...) {
                pending_error_dialog_ = "Failed to prepare the screenshot folder.";
            }
        }

        if (deferred_fast_forward_toggle_) {
            deferred_fast_forward_toggle_ = false;
            if (session_.valid()) {
                session_.setFastForwardEnabled(!session_.fastForwardEnabled());
                session_.setPaused(false);
                resetFramePacing("toggle-fast-forward");
                returnToMainWindow();
            }
        }
    }

    bool launchSession(const LaunchRequest& request, bool close_ui) {
        std::string error;
        psxe_diag_breadcrumbf("Launching session kind=%s path=%s close_ui=%s",
            LaunchKindTitle(request.kind),
            request.path.empty() ? "(none)" : request.path.string().c_str(),
            close_ui ? "true" : "false");

        if (!session_.create(renderer_, settings_, request, error)) {
            pending_error_dialog_ = error;
            psxe_diag_logf("launch", "Session launch failed kind=%s error=%s", LaunchKindTitle(request.kind), error.c_str());
            if (request.kind == LaunchKind::Bios) {
                showLandingWindow();
            } else {
                showGameListWindow();
            }
            return false;
        }

        applyVrStereo();
        input_router_.attach(session_.pad());
        mobile_controls_.showNow();
        resetFramePacing("launch-session");
        applyWindowMetrics();
        logRendererBootstrap("session-launch", session_.frameRate());
        refreshGameList(false);

        if (close_ui) {
            returnToMainWindow();
        }

        return true;
    }

    void exitToLibrary() {
        psxe_diag_breadcrumbf("Exit to library requested");
        input_router_.detach();
        session_.destroy();
        resetFramePacing("exit-to-library");
        showLandingWindow();
    }

    struct StateAction {
        bool saving = false;
        int slot = 1;
    };
    static constexpr int kStateSlots = 5;

    std::filesystem::path statePath(int slot) const {
        return DefaultBrowseDirectory() / "states" / (session_.stateKey() + ".slot" + std::to_string(slot) + ".state");
    }

    void openStateSlotDialog(bool saving) {
        if (!session_.valid()) {
            return;
        }
        ImGuiFullscreen::ChoiceDialogOptions options;
        for (int slot = 1; slot <= kStateSlots; slot++) {
            std::error_code ec;
            const std::filesystem::path path = statePath(slot);
            std::string label = "Slot " + std::to_string(slot) + "  -  ";
            if (std::filesystem::exists(path, ec)) {
                const std::time_t when = FileTimeToTimeT(std::filesystem::last_write_time(path, ec));
                char stamp[64] = {};
                std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", std::localtime(&when));
                label += stamp;
            } else {
                label += "empty";
            }
            options.emplace_back(label, false);
        }
        ImGuiFullscreen::OpenChoiceDialog(
            saving ? ICON_FA_SAVE " Save State" : ICON_FA_FOLDER_OPEN " Load State",
            false,
            std::move(options),
            [this, saving](int index, const std::string&, bool) {
                // Closing destroys this very callback: copy what it captured first.
                ArmsxApp* const app = this;
                const bool is_saving = saving;
                ImGuiFullscreen::CloseChoiceDialog();
                if (index < 0 || index >= kStateSlots) {
                    return;
                }
                // Applied between frames, like every other change to the running machine.
                app->deferred_state_action_ = StateAction{is_saving, index + 1};
            });
    }

    void applyStateAction(const StateAction& action) {
        if (!session_.valid()) {
            return;
        }
        const std::filesystem::path path = statePath(action.slot);
        if (action.saving) {
            if (!session_.saveState(path)) {
                pending_error_dialog_ = "Could not save the state to " + path.string() + ".";
            }
            return;
        }
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            pending_error_dialog_ = "Slot " + std::to_string(action.slot) + " is empty.";
            return;
        }
        if (!session_.loadState(path)) {
            pending_error_dialog_ = "Could not load this state. States only load in the build and on the kind of device that saved them.";
            return;
        }
        // Straight back into the game at the restored moment.
        returnToMainWindow();
        session_.setPaused(false);
        resetFramePacing("load-state");
    }

    void openPauseMenu() {
        if (!session_.valid()) {
            return;
        }

        session_.setPaused(true);
        resetFramePacing("open-pause-menu");
        input_router_.onFsuiOpened();
        showPauseMenuWindow();
    }

    bool syncPlatformLibraries(bool persist, bool refresh) {
        std::vector<PlatformLibraryRoot> roots;
        std::vector<PlatformLibraryGame> games;
        const std::uint64_t version = CopyPlatformLibrarySnapshot(roots, games);
        if (version == 0 || version == platform_library_version_) {
            return false;
        }

#if defined(__ANDROID__)
        auto remove_virtual_roots = [](std::vector<std::filesystem::path>& paths) {
            paths.erase(
                std::remove_if(paths.begin(), paths.end(), [](const std::filesystem::path& path) {
                    return IsAndroidVirtualPath(path.string());
                }),
                paths.end()
            );
        };
        remove_virtual_roots(settings_.ui_state.game_list_paths);
        remove_virtual_roots(settings_.ui_state.game_list_recursive_paths);
#endif

        platform_roots_ = std::move(roots);
        platform_games_ = std::move(games);
        for (const PlatformLibraryRoot& root : platform_roots_) {
            auto& paths = root.recursive
                ? settings_.ui_state.game_list_recursive_paths
                : settings_.ui_state.game_list_paths;
            const std::filesystem::path path(root.path);
            if (!PathListContains(paths, path)) {
                paths.push_back(path);
            }
        }
        platform_library_version_ = version;

        if (refresh) {
            refreshGameList(true);
        }
        if (persist) {
            SaveSettings(settings_);
        }
        return true;
    }

    void refreshGameList(bool full_rescan) {
        (void)full_rescan;
        game_list_.clear();
        std::vector<std::filesystem::path> seen_game_paths;
        std::vector<std::pair<std::filesystem::path, bool>> scan_roots;

        auto add_scan_root = [&](const std::filesystem::path& root, bool recursive) {
            if (root.empty() || IsAndroidVirtualPath(root.string())) {
                return;
            }

            for (auto& entry : scan_roots) {
                if (PathsMatch(entry.first, root)) {
                    entry.second = entry.second || recursive;
                    return;
                }
            }

            scan_roots.emplace_back(root, recursive);
        };

        auto scan_root = [&](const std::filesystem::path& root, bool recursive) {
            try {
                if (!std::filesystem::exists(root)) {
                    return;
                }

                auto append_entry = [&](const std::filesystem::directory_entry& item) {
                    if (!item.is_regular_file()) {
                        return;
                    }

                    const std::filesystem::path path = item.path();
                    if (!IsDiscPath(path) && !IsExePath(path)) {
                        return;
                    }
                    if (IsLikelyBiosImagePath(path, settings_)) {
                        return;
                    }
                    if (ToLower(path.extension().string()) == ".bin" && CueDirectoryReferencesImage(path)) {
                        return;
                    }
                    if (PathListContains(seen_game_paths, path)) {
                        return;
                    }
                    seen_game_paths.push_back(path);

                    fsui::GameEntry entry;
                    entry.path = path;
                    entry.type = IsExePath(path) ? fsui::GameEntryType::Homebrew : fsui::GameEntryType::Application;
                    entry.title = StemToTitle(path);
                    entry.title_sort = ToLower(entry.title);
                    entry.english_title = entry.title;
                    entry.title_id = NormalizeModel(entry.title);
                    entry.file_size = item.file_size();
                    entry.modified_time = FileTimeToTimeT(item.last_write_time());

                    const int region = RegionFromSettings(settings_);
                    if (region == CDR_REGION_EUROPE) {
                        entry.region = "Europe";
                        entry.region_flag_path = "icons/flags/eu.png";
                    } else if (region == CDR_REGION_JAPAN) {
                        entry.region = "Japan";
                        entry.region_flag_path = "icons/flags/jp.png";
                    } else {
                        entry.region = "North America";
                        entry.region_flag_path = "icons/flags/us.png";
                    }

                    game_list_.push_back(std::move(entry));
                };

                if (recursive) {
                    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
                        append_entry(item);
                    }
                } else {
                    for (const auto& item : std::filesystem::directory_iterator(root)) {
                        append_entry(item);
                    }
                }
            } catch (...) {
            }
        };

        for (const auto& root : settings_.ui_state.game_list_paths) {
            add_scan_root(root, false);
        }

        for (const auto& root : settings_.ui_state.game_list_recursive_paths) {
            add_scan_root(root, true);
        }

#if defined(UWP_TARGET)
        add_scan_root(DefaultBrowseDirectory(), true);
#endif

        for (const auto& [root, recursive] : scan_roots) {
            scan_root(root, recursive);
        }

        for (const PlatformLibraryGame& game : platform_games_) {
            const auto root = std::find_if(platform_roots_.begin(), platform_roots_.end(), [&](const PlatformLibraryRoot& candidate) {
                return PlatformPathBelongsToRoot(game.path, candidate.path, candidate.recursive);
            });
            if (root == platform_roots_.end()) {
                continue;
            }

            const std::filesystem::path path(game.path);
            if ((!IsDiscPath(path) && !IsExePath(path) && !IsZipPath(path)) ||
                IsLikelyBiosImagePath(path, settings_) ||
                PathListContains(seen_game_paths, path)) {
                continue;
            }
            seen_game_paths.push_back(path);

            fsui::GameEntry entry;
            entry.path = path;
            entry.type = IsExePath(path) ? fsui::GameEntryType::Homebrew : fsui::GameEntryType::Application;
            entry.title = game.label.empty() ? StemToTitle(path) : game.label;
            entry.title_sort = ToLower(entry.title);
            entry.english_title = entry.title;
            entry.title_id = NormalizeModel(entry.title);
            entry.file_size = game.size;
            entry.modified_time = game.modified;

            const int region = RegionFromSettings(settings_);
            if (region == CDR_REGION_EUROPE) {
                entry.region = "Europe";
                entry.region_flag_path = "icons/flags/eu.png";
            } else if (region == CDR_REGION_JAPAN) {
                entry.region = "Japan";
                entry.region_flag_path = "icons/flags/jp.png";
            } else {
                entry.region = "North America";
                entry.region_flag_path = "icons/flags/us.png";
            }
            game_list_.push_back(std::move(entry));
        }

        std::sort(game_list_.begin(), game_list_.end(), [](const fsui::GameEntry& left, const fsui::GameEntry& right) {
            if (left.title_sort == right.title_sort) {
                return left.path < right.path;
            }
            return left.title_sort < right.title_sort;
        });
    }

    std::vector<fsui::MenuItemDescriptor> buildLandingItems() {
        std::vector<fsui::MenuItemDescriptor> items;

        fsui::MenuItemDescriptor game_list;
        game_list.id = "game-list";
        game_list.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::GameList);
        game_list.title = "Game List";
        game_list.summary = "Browse your library folders and open a game.";
        game_list.on_activate = [this]() { showGameListWindow(); };
        items.push_back(std::move(game_list));

        fsui::MenuItemDescriptor start;
        start.id = "start-game";
        start.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartGame);
        start.title = "Start Game";
        start.summary = "Boot a disc, PS-X EXE, or the BIOS directly.";
        start.on_activate = [this]() { showStartGameWindow(); };
        items.push_back(std::move(start));

        fsui::MenuItemDescriptor settings;
        settings.id = "settings";
        settings.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::Settings);
        settings.title = "Settings";
        settings.summary = "Adjust ARMSX, BIOS, video, and library settings.";
        settings.on_activate = [this]() { switchToSettingsWindow(); };
        items.push_back(std::move(settings));

        fsui::MenuItemDescriptor exit;
        exit.id = "exit";
        exit.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::Exit);
        exit.title = "Exit";
        exit.summary = "Quit ARMSX.";
        exit.on_activate = [this]() { showExitWindow(); };
        items.push_back(std::move(exit));

        return items;
    }

    std::vector<fsui::MenuItemDescriptor> buildStartItems() {
        std::vector<fsui::MenuItemDescriptor> items;

        auto default_dir = initialBrowseDirectory();

        fsui::MenuItemDescriptor start_file;
        start_file.id = "start-file";
        start_file.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartFile);
        start_file.title = "Start File";
        start_file.summary = "Choose a disc image or PS-X EXE from your files.";
        start_file.on_activate = [this, default_dir]() {
#if defined(__EMSCRIPTEN__)
            requestWebFiles(false);
#else
            ImGuiFullscreen::OpenFileSelector(
                "Start File",
                false,
                [this](const std::string& path) {
                    queueLaunchSelection(path);
                },
                {".cue", ".bin", ".iso", ".img",
#ifdef USE_CHD
                 ".chd",
#endif
                 ".exe", ".ps-exe", ".psexe"},
                default_dir.string()
            );
#endif
        };
        items.push_back(std::move(start_file));

        if (!settings_.default_exe_path.empty()) {
            fsui::MenuItemDescriptor start_saved_exe;
            start_saved_exe.id = "start-default-exe";
            start_saved_exe.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartFile);
            start_saved_exe.title = "Start Saved PS-X EXE";
            start_saved_exe.summary = settings_.default_exe_path;
            start_saved_exe.on_activate = [this]() {
                deferred_launch_ = LaunchRequest{
                    .kind = LaunchKind::Exe,
                    .path = std::filesystem::path(settings_.default_exe_path),
                    .label = StemToTitle(settings_.default_exe_path),
                };
                close_ui_after_launch_ = true;
            };
            items.push_back(std::move(start_saved_exe));
        }

        fsui::MenuItemDescriptor start_disc;
        start_disc.id = "start-disc";
        start_disc.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartDisc);
        start_disc.title = "Start Disc";
        start_disc.summary = "Pick a disc image directly.";
        start_disc.on_activate = [this, default_dir]() {
#if defined(__EMSCRIPTEN__)
            requestWebFiles(false);
#else
            ImGuiFullscreen::OpenFileSelector(
                "Start Disc",
                false,
                [this](const std::string& path) {
                    queueLaunchSelection(path, LaunchKind::Disc);
                },
                {".cue", ".bin", ".iso", ".img",
#ifdef USE_CHD
                 ".chd",
#endif
                },
                default_dir.string()
            );
#endif
        };
        items.push_back(std::move(start_disc));

#if defined(__EMSCRIPTEN__)
        fsui::MenuItemDescriptor start_folder;
        start_folder.id = "start-web-folder";
        start_folder.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartFile);
        start_folder.title = "Open Game Folder";
        start_folder.summary = "Grant temporary access to a folder and start a discovered game.";
        start_folder.on_activate = [this]() { requestWebFiles(true); };
        items.push_back(std::move(start_folder));
#endif

        fsui::MenuItemDescriptor start_bios;
        start_bios.id = "start-bios";
        start_bios.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartBios);
        start_bios.title = "Start BIOS";
        start_bios.summary = "Boot into the BIOS without media.";
        start_bios.on_activate = [this]() {
            deferred_launch_ = LaunchRequest{.kind = LaunchKind::Bios, .path = {}, .label = "PlayStation BIOS"};
            close_ui_after_launch_ = true;
        };
        items.push_back(std::move(start_bios));

        fsui::MenuItemDescriptor back;
        back.id = "back";
        back.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::Back);
        back.title = "Back";
        back.summary = "Return to the main menu.";
        back.on_activate = [this]() { showLandingWindow(); };
        items.push_back(std::move(back));

        return items;
    }

    std::vector<fsui::MenuItemDescriptor> buildExitItems() {
        std::vector<fsui::MenuItemDescriptor> items;

        fsui::MenuItemDescriptor quit;
        quit.id = "quit";
        quit.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::Exit);
        quit.title = "Quit";
        quit.summary = "Close ARMSX.";
        quit.command = fsui::MakeRequestQuitCommand();
        items.push_back(std::move(quit));

        fsui::MenuItemDescriptor back;
        back.id = "back";
        back.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::Back);
        back.title = "Back";
        back.summary = "Return to the main menu.";
        back.on_activate = [this]() { showLandingWindow(); };
        items.push_back(std::move(back));

        return items;
    }

    std::vector<fsui::MenuItemDescriptor> buildPauseItems() {
        std::vector<fsui::MenuItemDescriptor> items;

        fsui::MenuItemDescriptor resume;
        resume.id = "resume";
        resume.title = ICON_FA_PLAY " Resume";
        resume.summary = "Return to gameplay.";
        resume.command = fsui::MakeResumeCommand();
        items.push_back(std::move(resume));

        fsui::MenuItemDescriptor reset;
        reset.id = "reset";
        reset.title = ICON_FA_SYNC " Reset";
        reset.summary = "Soft-reset the active session.";
        reset.command = fsui::MakeResetCommand();
        items.push_back(std::move(reset));

        fsui::MenuItemDescriptor change_disc;
        change_disc.id = "change-disc";
        change_disc.title = ICON_FA_COMPACT_DISC " Change Disc";
        change_disc.summary = "Swap to another disc image and resume.";
        change_disc.on_activate = [this]() {
            ImGuiFullscreen::OpenFileSelector(
                "Change Disc",
                false,
                [this](const std::string& path) { queueDiscSwapSelection(path); },
                {".cue", ".bin", ".iso", ".img",
#ifdef USE_CHD
                 ".chd",
#endif
                },
                initialBrowseDirectory().string()
            );
        };
        items.push_back(std::move(change_disc));

        fsui::MenuItemDescriptor save_state;
        save_state.id = "save-state";
        save_state.title = ICON_FA_SAVE " Save State";
        save_state.summary = "Save a snapshot of the game right now into one of five slots.";
        save_state.on_activate = [this]() { openStateSlotDialog(true); };
        items.push_back(std::move(save_state));

        fsui::MenuItemDescriptor load_state;
        load_state.id = "load-state";
        load_state.title = ICON_FA_FOLDER_OPEN " Load State";
        load_state.summary = "Continue from a snapshot saved earlier.";
        load_state.on_activate = [this]() { openStateSlotDialog(false); };
        items.push_back(std::move(load_state));

        fsui::MenuItemDescriptor screenshot;
        screenshot.id = "screenshot";
        screenshot.title = ICON_FA_CAMERA " Save Screenshot";
        screenshot.summary = "Save a BMP screenshot to the screenshot folder.";
        screenshot.on_activate = [this]() { deferred_screenshot_ = true; };
        items.push_back(std::move(screenshot));

        if (vr_mode_) {
            fsui::MenuItemDescriptor vr_settings;
            vr_settings.id = "vr-settings";
            vr_settings.title = ICON_FA_TV " VR Settings";
            vr_settings.summary = "Screen size, distance and curvature, 3D depth and pop-out.";
            vr_settings.on_activate = [this]() {
                pending_settings_page_restore_ = "VR";
                switchToSettingsWindow();
            };
            items.push_back(std::move(vr_settings));

            fsui::MenuItemDescriptor recenter;
            recenter.id = "vr-recenter";
            recenter.title = ICON_FA_BULLSEYE " Recenter Screen";
            recenter.summary = "Move the screen straight ahead of where you are looking now.";
            recenter.on_activate = []() { armsx::vr::Recenter(); };
            items.push_back(std::move(recenter));
        }

        fsui::MenuItemDescriptor settings;
        settings.id = "settings";
        settings.title = ICON_FA_SLIDERS_H " Settings";
        settings.summary = "Open the in-game settings pages.";
        settings.on_activate = [this]() { switchToSettingsWindow(); };
        items.push_back(std::move(settings));

        fsui::MenuItemDescriptor fast_forward;
        fast_forward.id = "fast-forward";
        fast_forward.title = ICON_FA_FAST_FORWARD " Fast Forward";
        fast_forward.summary = session_.fastForwardEnabled()
            ? "Current speed is 2x. Click to return to normal speed and resume gameplay."
            : "Current speed is normal. Click to resume gameplay at 2x.";
        fast_forward.on_activate = [this]() { deferred_fast_forward_toggle_ = true; };
        items.push_back(std::move(fast_forward));

        fsui::MenuItemDescriptor quit;
        quit.id = "quit-game";
        quit.title = ICON_FA_POWER_OFF " Quit Game";
        quit.summary = "Close the current game and return to the main menu.";
        quit.command = fsui::MakeExitToLibraryCommand();
        items.push_back(std::move(quit));

        return items;
    }

    std::vector<fsui::MenuItemDescriptor> buildGameLaunchOptions(const fsui::GameEntry&) {
        return {};
    }

    std::vector<fsui::SettingsPageDescriptor> buildSettingsPages(fsui::SettingsScope scope) {
        std::vector<fsui::SettingsPageDescriptor> pages;

        if (scope == fsui::SettingsScope::PerGame) {
            pages = buildPerGameSettingsPages();
        } else {
            pages = buildGlobalSettingsPages();
        }
        if (vr_mode_) {
            pages.push_back(buildVrSettingsPage(scope));
        }

        if (pending_settings_page_restore_.has_value()) {
            const std::string restore_id = *pending_settings_page_restore_;
            const auto match = std::find_if(pages.begin(), pages.end(), [&](const fsui::SettingsPageDescriptor& page) {
                return page.id == restore_id;
            });

            if (match != pages.end()) {
                std::rotate(pages.begin(), match, match + 1);
            }

            pending_settings_page_restore_.reset();
        }

        return pages;
    }

    std::vector<fsui::SettingsPageDescriptor> buildGlobalSettingsPages() {
        std::vector<fsui::SettingsPageDescriptor> pages;

        fsui::SettingsPageDescriptor bios_page;
        bios_page.id = "BIOS";
        bios_page.scope = fsui::SettingsScope::Global;
        bios_page.built_in_page = fsui::BuiltInSettingsPage::BIOS;
        bios_page.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

            fsui::SettingsRowDescriptor model;
            model.kind = fsui::SettingsRowKind::Choice;
            model.id = "model";
            model.title = ICON_FA_MICROCHIP " Model";
            model.summary = "Choose which BIOS model to use when a BIOS folder contains more than one.";
            model.value = settings_.model;
            model.dialog_title = model.title;
            model.choices = BuildModelChoices(settings_.model);
            model.on_choice = [this](int index) {
                const auto models = ModelChoices();
                if (index >= 0 && index < static_cast<int>(models.size())) {
                    settings_.model = models[static_cast<size_t>(index)];
                    SaveSettings(settings_);
                }
            };
            rows.push_back(std::move(model));

            fsui::SettingsRowDescriptor folder;
            folder.kind = fsui::SettingsRowKind::Action;
            folder.id = "bios-folder";
            folder.title = ICON_FA_FOLDER " BIOS Folder";
            if (settings_.bios_search.empty()) {
                folder.summary = AppendBrowseRootHint("Choose a folder that contains BIOS files.");
            } else {
                folder.summary = settings_.bios_search;
            }
            folder.on_activate = [this]() {
                ImGuiFullscreen::OpenFileSelector(
                    "BIOS Folder",
                    true,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.bios_search = *selection;
                            SaveSettings(settings_);
                        }
                    },
                    {},
                    browseDirectoryForPath(settings_.bios_search, true).string()
                );
            };
            rows.push_back(std::move(folder));

            fsui::SettingsRowDescriptor override;
            override.kind = fsui::SettingsRowKind::Action;
            override.id = "bios-override";
            override.title = ICON_FA_FILE " BIOS Override";
            override.summary = settings_.bios_override.empty() ? "Auto-select from the BIOS folder." : settings_.bios_override;
            override.on_activate = [this]() {
                const std::string browse_seed = settings_.bios_override.empty() ? settings_.bios_search : settings_.bios_override;
                ImGuiFullscreen::OpenFileSelector(
                    "BIOS Override",
                    false,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.bios_override = *selection;
                            SaveSettings(settings_);
                        }
                    },
                    {".bin", ".rom"},
                    browseDirectoryForPath(browse_seed, false).string()
                );
            };
            rows.push_back(std::move(override));

            fsui::SettingsRowDescriptor clear_override;
            clear_override.kind = fsui::SettingsRowKind::Action;
            clear_override.id = "clear-bios-override";
            clear_override.title = ICON_FA_TIMES " Clear BIOS Override";
            clear_override.summary = "Fall back to the BIOS folder + preferred model.";
            clear_override.enabled = !settings_.bios_override.empty();
            clear_override.on_activate = [this]() {
                settings_.bios_override.clear();
                SaveSettings(settings_);
            };
            rows.push_back(std::move(clear_override));

            return rows;
        };
        pages.push_back(std::move(bios_page));

        fsui::SettingsPageDescriptor emulation_page;
        emulation_page.id = "Emulation";
        emulation_page.scope = fsui::SettingsScope::Global;
        emulation_page.built_in_page = fsui::BuiltInSettingsPage::Emulation;
        emulation_page.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

            fsui::SettingsRowDescriptor cpu_engine;
            cpu_engine.kind = fsui::SettingsRowKind::Choice;
            cpu_engine.id = "cpu-engine";
            cpu_engine.title = ICON_FA_MICROCHIP " CPU Engine";
            cpu_engine.summary = "Use the cached interpreter for normal play or the reference interpreter for compatibility diagnostics. Applies on the next launch.";
            cpu_engine.value = CpuEngineTitle(settings_.cpu_engine);
            cpu_engine.dialog_title = cpu_engine.title;
            cpu_engine.choices = BuildCpuEngineChoices(settings_.cpu_engine);
            cpu_engine.on_choice = [this](int index) {
                settings_.cpu_engine = index == 1 ? PSX_CPU_INTERPRETER : PSX_CPU_CACHED_INTERPRETER;
                SaveSettings(settings_);
            };
            rows.push_back(std::move(cpu_engine));

            fsui::SettingsRowDescriptor region;
            region.kind = fsui::SettingsRowKind::Choice;
            region.id = "region";
            region.title = ICON_FA_GLOBE " Region";
            region.summary = "Choose which region the console should use when starting a disc.";
            region.value = ToLower(settings_.region);
            region.dialog_title = region.title;
            region.choices = BuildRegionChoices(settings_.region);
            region.on_choice = [this](int index) {
                static const std::array<const char*, 3> values = {"auto", "ntsc", "pal"};
                if (index >= 0 && index < static_cast<int>(values.size())) {
                    settings_.region = values[static_cast<size_t>(index)];
                    SaveSettings(settings_);
                }
            };
            rows.push_back(std::move(region));

            fsui::SettingsRowDescriptor expansion;
            expansion.kind = fsui::SettingsRowKind::Action;
            expansion.id = "expansion-rom";
            expansion.title = ICON_FA_MEMORY " Expansion ROM";
            expansion.summary = settings_.exp_path.empty() ? "No expansion ROM selected." : settings_.exp_path;
            expansion.on_activate = [this]() {
                ImGuiFullscreen::OpenFileSelector(
                    "Expansion ROM",
                    false,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.exp_path = *selection;
                            SaveSettings(settings_);
                        }
                    },
                    {".bin", ".rom"},
                    initialBrowseDirectory().string()
                );
            };
            rows.push_back(std::move(expansion));

            fsui::SettingsRowDescriptor default_exe;
            default_exe.kind = fsui::SettingsRowKind::Action;
            default_exe.id = "default-exe";
            default_exe.title = ICON_FA_FILE_CODE " Default PS-X EXE";
            default_exe.summary = settings_.default_exe_path.empty() ? "No default executable selected." : settings_.default_exe_path;
            default_exe.on_activate = [this]() {
                ImGuiFullscreen::OpenFileSelector(
                    "Default PS-X EXE",
                    false,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.default_exe_path = *selection;
                            SaveSettings(settings_);
                        }
                    },
                    {".exe", ".ps-exe", ".psexe"},
                    initialBrowseDirectory().string()
                );
            };
            rows.push_back(std::move(default_exe));

            return rows;
        };
        pages.push_back(std::move(emulation_page));

        fsui::SettingsPageDescriptor graphics_page;
        graphics_page.id = "Graphics";
        graphics_page.scope = fsui::SettingsScope::Global;
        graphics_page.built_in_page = fsui::BuiltInSettingsPage::Graphics;
        graphics_page.build_rows = [this]() { return buildGraphicsRows(SupportsManagedWindowSizing()); };
        pages.push_back(std::move(graphics_page));

        fsui::SettingsPageDescriptor folders_page;
        folders_page.id = "Folders";
        folders_page.scope = fsui::SettingsScope::Global;
        folders_page.built_in_page = fsui::BuiltInSettingsPage::Folders;
        folders_page.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

#if defined(UWP_TARGET)
            fsui::SettingsRowDescriptor browse_notice;
            browse_notice.kind = fsui::SettingsRowKind::Notice;
            browse_notice.id = "uwp-filesystem-roots";
            browse_notice.title = ICON_FA_FOLDER_OPEN " Mounted Drives";
            browse_notice.summary = "Drive roots are available in the file picker. Use Filesystem Roots or Parent Directory to switch drives.";
            rows.push_back(std::move(browse_notice));
#endif

            fsui::SettingsRowDescriptor add_folder;
            add_folder.kind = fsui::SettingsRowKind::Action;
            add_folder.id = "add-folder";
            add_folder.title = ICON_FA_FOLDER_PLUS " Add Library Folder";
#if defined(__ANDROID__) || defined(IOS_TARGET)
            add_folder.summary = "Grant persistent access to one game directory using the system folder picker.";
#else
            add_folder.summary = AppendBrowseRootHint("Scan only the selected directory.");
#endif
            add_folder.on_activate = [this]() {
                if (RequestPlatformLibraryDirectory(false)) {
                    return;
                }
                ImGuiFullscreen::OpenFileSelector(
                    "Add Library Folder",
                    true,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.ui_state.game_list_paths.emplace_back(*selection);
                            refreshGameList(true);
                            SaveSettings(settings_);
                        }
                    },
                    {},
                    initialBrowseDirectory().string()
                );
            };
            rows.push_back(std::move(add_folder));

            fsui::SettingsRowDescriptor add_recursive;
            add_recursive.kind = fsui::SettingsRowKind::Action;
            add_recursive.id = "add-recursive-folder";
            add_recursive.title = ICON_FA_FOLDER_PLUS " Add Recursive Folder";
#if defined(__ANDROID__) || defined(IOS_TARGET)
            add_recursive.summary = "Grant persistent access to a game directory and scan all of its subfolders.";
#else
            add_recursive.summary = AppendBrowseRootHint("Scan the selected directory and its children.");
#endif
            add_recursive.on_activate = [this]() {
                if (RequestPlatformLibraryDirectory(true)) {
                    return;
                }
                ImGuiFullscreen::OpenFileSelector(
                    "Add Recursive Library Folder",
                    true,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.ui_state.game_list_recursive_paths.emplace_back(*selection);
                            refreshGameList(true);
                            SaveSettings(settings_);
                        }
                    },
                    {},
                    initialBrowseDirectory().string()
                );
            };
            rows.push_back(std::move(add_recursive));

            fsui::SettingsRowDescriptor refresh;
            refresh.kind = fsui::SettingsRowKind::Action;
            refresh.id = "refresh-library";
            refresh.title = ICON_FA_SYNC " Refresh Library";
            refresh.summary = "Rescan every configured library folder.";
            refresh.on_activate = [this]() { refreshGameList(true); };
            rows.push_back(std::move(refresh));

            auto append_folder_rows = [&](const std::vector<std::filesystem::path>& folders, bool recursive) {
                for (size_t index = 0; index < folders.size(); index++) {
                    fsui::SettingsRowDescriptor row;
                    row.kind = fsui::SettingsRowKind::Action;
                    row.id = std::string(recursive ? "recursive-" : "folder-") + std::to_string(index);
                    row.title = WithHiddenId(
                        recursive ? ICON_FA_FOLDER_OPEN " Recursive Folder" : ICON_FA_FOLDER " Library Folder",
                        row.id
                    );
                    const std::string configured_path = folders[index].string();
                    const auto platform_root = std::find_if(platform_roots_.begin(), platform_roots_.end(), [&](const PlatformLibraryRoot& root) {
                        return root.path == configured_path;
                    });
                    row.summary = platform_root != platform_roots_.end() && !platform_root->label.empty()
                        ? platform_root->label + "\n" + configured_path
                        : configured_path;
                    row.on_activate = [this, index, recursive]() {
                        auto& list = recursive ? settings_.ui_state.game_list_recursive_paths : settings_.ui_state.game_list_paths;
                        if (index < list.size()) {
                            const std::string removed_path = list[index].string();
                            list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
                            NotifyPlatformLibraryDirectoryRemoved(removed_path);
                            refreshGameList(true);
                            SaveSettings(settings_);
                        }
                    };
                    rows.push_back(std::move(row));
                }
            };

            append_folder_rows(settings_.ui_state.game_list_paths, false);
            append_folder_rows(settings_.ui_state.game_list_recursive_paths, true);

            return rows;
        };
        pages.push_back(std::move(folders_page));

        fsui::SettingsPageDescriptor advanced_page;
        advanced_page.id = "Advanced";
        advanced_page.scope = fsui::SettingsScope::Global;
        advanced_page.built_in_page = fsui::BuiltInSettingsPage::Advanced;
        advanced_page.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

            fsui::SettingsRowDescriptor logging_enabled;
            logging_enabled.kind = fsui::SettingsRowKind::Toggle;
            logging_enabled.id = "logging-enabled";
            logging_enabled.title = ICON_FA_BUG " Debug Logging";
            logging_enabled.summary =
                "Write diagnostic logs for troubleshooting only. Leave this off during normal play because it can reduce performance.";
            logging_enabled.toggle_value = settings_.logging_enabled;
            logging_enabled.on_toggle = [this](bool value) {
                settings_.logging_enabled = value;
                applyLoggingSettings("settings-toggle");
                SaveSettings(settings_);
            };
            rows.push_back(std::move(logging_enabled));

            fsui::SettingsRowDescriptor mobile_controls;
            mobile_controls.kind = fsui::SettingsRowKind::Toggle;
            mobile_controls.id = "mobile-controls";
            mobile_controls.title = ICON_FA_GAMEPAD " ImGui Mobile Controls";
            mobile_controls.summary =
                "Show the multitouch PlayStation controls during play. They hide after three seconds idle or when a controller is used, and reappear on touch.";
            mobile_controls.toggle_value = settings_.mobile_controls;
            mobile_controls.on_toggle = [this](bool value) {
                settings_.mobile_controls = value;
                if (!value) {
                    mobile_controls_.reset(input_router_);
                } else {
                    mobile_controls_.showNow();
                }
                SaveSettings(settings_);
            };
            rows.push_back(std::move(mobile_controls));

            if (settings_.logging_enabled) {
                fsui::SettingsRowDescriptor log_level;
                log_level.kind = fsui::SettingsRowKind::Choice;
                log_level.id = "log-level";
                log_level.title = ICON_FA_TERMINAL " Log Level";
                log_level.summary = "Choose how much detail ARMSX writes to the debug log.";
                log_level.value = log_level_string(settings_.log_level);
                log_level.dialog_title = log_level.title;
                log_level.choices = BuildLogLevelChoices(settings_.log_level);
                log_level.on_choice = [this](int index) {
                    settings_.log_level = std::clamp(index, static_cast<int>(LOG_TRACE), static_cast<int>(LOG_FATAL));
                    applyLoggingSettings("settings-log-level");
                    SaveSettings(settings_);
                };
                rows.push_back(std::move(log_level));

                fsui::SettingsRowDescriptor log_path;
                log_path.kind = fsui::SettingsRowKind::Notice;
                log_path.id = "log-path";
                log_path.title = ICON_FA_FILE_ALT " Log File";
                log_path.summary = diagnosticsLogPath().string();
                rows.push_back(std::move(log_path));
            }

            fsui::SettingsRowDescriptor settings_path;
            settings_path.kind = fsui::SettingsRowKind::Notice;
            settings_path.id = "settings-path";
            settings_path.title = ICON_FA_FILE_ALT " Settings File";
            settings_path.summary = settings_.settings_path;
            rows.push_back(std::move(settings_path));

            return rows;
        };
        pages.push_back(std::move(advanced_page));

        return pages;
    }

    std::vector<fsui::SettingsPageDescriptor> buildPerGameSettingsPages() {
        std::vector<fsui::SettingsPageDescriptor> pages;

        fsui::SettingsPageDescriptor summary;
        summary.id = "Summary";
        summary.scope = fsui::SettingsScope::PerGame;
        summary.built_in_page = fsui::BuiltInSettingsPage::Summary;
        summary.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;
            const fsui::CurrentGameInfo info = session_.currentGameInfo();

            fsui::SettingsRowDescriptor title;
            title.kind = fsui::SettingsRowKind::Notice;
            title.id = "current-game";
            title.title = info.title;
            title.summary = info.path.empty() ? info.subtitle : info.path.string();
            rows.push_back(std::move(title));

            return rows;
        };
        pages.push_back(std::move(summary));

        fsui::SettingsPageDescriptor graphics;
        graphics.id = "Graphics";
        graphics.scope = fsui::SettingsScope::PerGame;
        graphics.built_in_page = fsui::BuiltInSettingsPage::Graphics;
        graphics.build_rows = [this]() { return buildGraphicsRows(false); };
        pages.push_back(std::move(graphics));

        fsui::SettingsPageDescriptor emulation;
        emulation.id = "Emulation";
        emulation.scope = fsui::SettingsScope::PerGame;
        emulation.built_in_page = fsui::BuiltInSettingsPage::Emulation;
        emulation.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

            fsui::SettingsRowDescriptor reset;
            reset.kind = fsui::SettingsRowKind::Action;
            reset.id = "reset-now";
            reset.title = ICON_FA_SYNC " Reset";
            reset.summary = "Soft-reset the active session.";
            reset.on_activate = [this]() { deferred_reset_ = true; };
            rows.push_back(std::move(reset));

            fsui::SettingsRowDescriptor disc;
            disc.kind = fsui::SettingsRowKind::Action;
            disc.id = "change-disc-now";
            disc.title = ICON_FA_COMPACT_DISC " Change Disc";
            disc.summary = "Swap to a different disc image.";
            disc.on_activate = [this]() {
                ImGuiFullscreen::OpenFileSelector(
                    "Change Disc",
                    false,
                [this](const std::string& path) { queueDiscSwapSelection(path); },
                {".cue", ".bin", ".iso", ".img",
#ifdef USE_CHD
                 ".chd",
#endif
                },
                initialBrowseDirectory().string()
            );
        };
            rows.push_back(std::move(disc));

            fsui::SettingsRowDescriptor screenshot;
            screenshot.kind = fsui::SettingsRowKind::Action;
            screenshot.id = "save-screenshot-now";
            screenshot.title = ICON_FA_CAMERA " Save Screenshot";
            screenshot.summary = "Write a BMP screenshot to the snapshot folder.";
            screenshot.on_activate = [this]() { deferred_screenshot_ = true; };
            rows.push_back(std::move(screenshot));

            fsui::SettingsRowDescriptor exit_library;
            exit_library.kind = fsui::SettingsRowKind::Action;
            exit_library.id = "exit-library-now";
            exit_library.title = ICON_FA_FOLDER_OPEN " Exit To Library";
            exit_library.summary = "Close the current game and return to the main menu.";
            exit_library.on_activate = [this]() { deferred_exit_to_library_ = true; };
            rows.push_back(std::move(exit_library));

            return rows;
        };
        pages.push_back(std::move(emulation));

        return pages;
    }

    std::vector<fsui::SettingsRowDescriptor> buildGraphicsRows(bool include_scale) {
        std::vector<fsui::SettingsRowDescriptor> rows;
#if defined(__EMSCRIPTEN__)
        const bool can_control_vsync = false;
#else
        const bool can_control_vsync = owns_renderer_ && !external_renderer_ && !vr_mode_;
#endif

#ifdef USE_HARDWARE
        {
            fsui::SettingsRowDescriptor gpu_backend;
            gpu_backend.kind = fsui::SettingsRowKind::Choice;
            gpu_backend.id = "gpu-backend";
            gpu_backend.title = ICON_FA_TACHOMETER_ALT " GPU Backend";
            gpu_backend.summary = hardwareRendererAvailable()
                ? "Choose SDL software or accelerated presentation. PlayStation GPU rasterization remains accuracy-first in software. Applies on the next launch."
                : "SDL renderer selection is controlled by the embedding host in this display mode.";
            gpu_backend.value = GpuBackendTitle(settings_.gpu_backend);
            gpu_backend.dialog_title = gpu_backend.title;
            gpu_backend.choices = BuildGpuBackendChoices(settings_.gpu_backend);
            gpu_backend.enabled = hardwareRendererAvailable();
            if (!gpu_backend.enabled) {
                gpu_backend.availability = fsui::AvailabilityMode::Disabled;
                gpu_backend.unavailable_title = gpu_backend.title;
                gpu_backend.unavailable_message = "This embedding host owns the SDL renderer.";
            }
            gpu_backend.on_choice = [this](int index) {
                settings_.gpu_backend = (index == 1) ? GpuBackend::SDLAccelerated : GpuBackend::Software;
                SaveSettings(settings_);
            };
            rows.push_back(std::move(gpu_backend));
        }
#endif

        if (include_scale) {
            fsui::SettingsRowDescriptor scale;
            scale.kind = fsui::SettingsRowKind::Choice;
            scale.id = "display-scale";
            scale.title = ICON_FA_EXPAND " Display Scale";
            scale.summary = "Resize the app window.";
            scale.value = std::to_string(settings_.scale) + "x";
            scale.dialog_title = scale.title;
            scale.choices = BuildScaleChoices(settings_.scale);
            scale.on_choice = [this](int index) {
                static const std::array<int, 6> values = {1, 2, 3, 4, 5, 6};
                if (index >= 0 && index < static_cast<int>(values.size())) {
                    settings_.scale = values[static_cast<size_t>(index)];
                    applyWindowMetrics();
                    SaveSettings(settings_);
                }
            };
            rows.push_back(std::move(scale));
        }

        fsui::SettingsRowDescriptor vsync;
        vsync.kind = fsui::SettingsRowKind::Toggle;
        vsync.id = "vsync";
        vsync.title = ICON_FA_SYNC " VSync";
#if defined(__EMSCRIPTEN__)
        vsync.summary = "Synchronize presentation to the display refresh rate. The browser controls this on web builds.";
#else
        vsync.summary = can_control_vsync
            ? "Synchronize presentation to the display refresh rate to reduce tearing. This applies immediately."
            : "Synchronize presentation to the display refresh rate. This renderer is host-controlled here.";
#endif
        vsync.toggle_value = settings_.vsync_enabled;
        vsync.enabled = can_control_vsync;
        vsync.on_toggle = [this](bool value) {
            settings_.vsync_enabled = value;
            pending_settings_page_restore_ = "Graphics";
            SaveSettings(settings_);
            deferred_vsync_ = value;
        };
        rows.push_back(std::move(vsync));

        fsui::SettingsRowDescriptor filter;
        filter.kind = fsui::SettingsRowKind::Toggle;
        filter.id = "texture-filter";
        filter.title = ICON_FA_ADJUST " Texture Scaling / Bilinear";
        filter.summary = "Use smoother filtering for the display output.";
        filter.toggle_value = settings_.texture_scale_mode;
        filter.on_toggle = [this](bool value) {
            settings_.texture_scale_mode = value;
            SaveSettings(settings_);
        };
        rows.push_back(std::move(filter));

        fsui::SettingsRowDescriptor presentation_upscale;
        presentation_upscale.kind = fsui::SettingsRowKind::Choice;
        presentation_upscale.id = "presentation-upscale";
        presentation_upscale.title = ICON_FA_MAGIC " SDL Quality Upscale";
        presentation_upscale.summary =
            "Build a higher-resolution SDL presentation texture without changing PlayStation rasterization.";
        presentation_upscale.value = std::to_string(settings_.presentation_upscale) + "x";
        presentation_upscale.dialog_title = presentation_upscale.title;
        presentation_upscale.choices = BuildPresentationUpscaleChoices(settings_.presentation_upscale);
        presentation_upscale.on_choice = [this](int index) {
            if (index >= 0 && index < 8) {
                settings_.presentation_upscale = index + 1;
                SaveSettings(settings_);
            }
        };
        rows.push_back(std::move(presentation_upscale));

        fsui::SettingsRowDescriptor stretch;
        stretch.kind = fsui::SettingsRowKind::Toggle;
        stretch.id = "stretch";
        stretch.title = ICON_FA_EXPAND_ARROWS_ALT " Stretch";
        stretch.summary = "Fill the window instead of preserving the original aspect ratio.";
        stretch.toggle_value = settings_.stretch_mode;
        stretch.on_toggle = [this](bool value) {
            settings_.stretch_mode = value;
            SaveSettings(settings_);
        };
        rows.push_back(std::move(stretch));

        fsui::SettingsRowDescriptor aspect;
        aspect.kind = fsui::SettingsRowKind::Choice;
        aspect.id = "aspect";
        aspect.title = ICON_FA_TV " Display Aspect";
        aspect.summary = "Choose the presentation aspect ratio.";
        aspect.value = AspectTitle(settings_.display_aspect);
        aspect.dialog_title = aspect.title;
        aspect.choices = BuildAspectChoices(settings_.display_aspect);
        aspect.on_choice = [this](int index) {
            settings_.display_aspect = std::clamp(index, 0, 2);
            applyWindowMetrics();
            SaveSettings(settings_);
        };
        rows.push_back(std::move(aspect));

        fsui::SettingsRowDescriptor upscale;
        upscale.kind = fsui::SettingsRowKind::Choice;
        upscale.id = "wide-upscale";
        upscale.title = ICON_FA_EXPAND " Wide Upscale";
        upscale.summary = "Choose the wide-mode output height.";
        upscale.value = UpscaleToString(settings_.upscale_height);
        upscale.dialog_title = upscale.title;
        upscale.choices = BuildUpscaleChoices(settings_.upscale_height);
        upscale.on_choice = [this](int index) {
            static const std::array<int, 5> heights = {480, 720, 1080, 1440, 2160};
            if (index >= 0 && index < static_cast<int>(heights.size())) {
                settings_.upscale_height = heights[static_cast<size_t>(index)];
                applyWindowMetrics();
                SaveSettings(settings_);
            }
        };
        rows.push_back(std::move(upscale));

        fsui::SettingsRowDescriptor debug_panel;
        debug_panel.kind = fsui::SettingsRowKind::Toggle;
        debug_panel.id = "debug-panel";
        debug_panel.title = ICON_FA_TACHOMETER_ALT " Debug Panel";
        debug_panel.summary = "Show the settings and performance overlays.";
        debug_panel.toggle_value = settings_.debug_panel;
        debug_panel.on_toggle = [this](bool value) {
            settings_.debug_panel = value;
            settings_.ui_state.show_settings_overlay = value;
            settings_.ui_state.show_performance_overlay = value;
            SaveSettings(settings_);
        };
        rows.push_back(std::move(debug_panel));

        if (session_.valid()) {
            fsui::SettingsRowDescriptor debug_view;
            debug_view.kind = fsui::SettingsRowKind::Toggle;
            debug_view.id = "debug-view";
            debug_view.title = ICON_FA_BUG " VRAM Debug View";
            debug_view.summary = "Show raw VRAM instead of the game image.";
            debug_view.toggle_value = session_.debugView();
            debug_view.on_toggle = [this](bool value) {
                session_.setDebugView(value);
                applyWindowMetrics();
            };
            rows.push_back(std::move(debug_view));
        }

        return rows;
    }

    void applyWindowMetrics() {
        if (!SupportsManagedWindowSizing() || !owns_window_ || !window_) {
            return;
        }

        int width = 1280;
        int height = 720;

        if (session_.valid()) {
            if (session_.debugView()) {
                width = PSX_GPU_FB_WIDTH;
                height = PSX_GPU_FB_HEIGHT;
            } else if (settings_.display_aspect == 2) {
                height = std::max(240, settings_.upscale_height);
                width = static_cast<int>((16.0f / 9.0f) * static_cast<float>(height));
            } else if (settings_.display_aspect == 1) {
                width = 320 * std::max(1, settings_.scale);
                height = width;
            } else {
                int base_width = 320;
                if (session_.psx()) {
                    const int display_width = static_cast<int>(psx_get_dmode_width(session_.psx()));
                    if (display_width == 256 || display_width == 320) {
                        base_width = display_width;
                    } else if (display_width == 368) {
                        base_width = 384;
                    }
                }

                width = base_width * std::max(1, settings_.scale);
                height = 240 * std::max(1, settings_.scale);
            }
        }
        SDL_SetWindowSize(window_, width, height);
    }

    std::filesystem::path initialBrowseDirectory() const {
        if (session_.valid()) {
            const fsui::CurrentGameInfo info = session_.currentGameInfo();
            if (!info.path.empty() && info.path.has_parent_path()) {
                return info.path.parent_path();
            }
        }

        if (!settings_.ui_state.game_list_paths.empty()) {
            return settings_.ui_state.game_list_paths.front();
        }

        if (!settings_.ui_state.game_list_recursive_paths.empty()) {
            return settings_.ui_state.game_list_recursive_paths.front();
        }

        return DefaultBrowseDirectory();
    }

    std::filesystem::path browseDirectoryForPath(const std::string& configured_path, bool expect_directory) const {
        std::error_code ec;

        if (!configured_path.empty()) {
            const std::filesystem::path candidate(configured_path);

            if (std::filesystem::exists(candidate, ec)) {
                if (expect_directory && std::filesystem::is_directory(candidate, ec)) {
                    return candidate;
                }

                if (!expect_directory) {
                    if (std::filesystem::is_directory(candidate, ec)) {
                        return candidate;
                    }

                    if (candidate.has_parent_path() && std::filesystem::exists(candidate.parent_path(), ec)) {
                        return candidate.parent_path();
                    }
                }
            }

            if (candidate.has_parent_path() && std::filesystem::exists(candidate.parent_path(), ec)) {
                return candidate.parent_path();
            }
        }

        return initialBrowseDirectory();
    }

    // ---- Quest VR --------------------------------------------------------------------------------

    static constexpr std::array<uint8_t, 4> kCentredAnalog = {0x80, 0x80, 0x80, 0x80};

    static uint32_t PadMaskFromVr(uint32_t buttons) {
        static constexpr std::array<std::pair<uint32_t, uint32_t>, 16> table = {{
            {armsx::vr::PAD_UP, PSXI_SW_SDA_PAD_UP},
            {armsx::vr::PAD_DOWN, PSXI_SW_SDA_PAD_DOWN},
            {armsx::vr::PAD_LEFT, PSXI_SW_SDA_PAD_LEFT},
            {armsx::vr::PAD_RIGHT, PSXI_SW_SDA_PAD_RIGHT},
            {armsx::vr::PAD_CROSS, PSXI_SW_SDA_CROSS},
            {armsx::vr::PAD_CIRCLE, PSXI_SW_SDA_CIRCLE},
            {armsx::vr::PAD_SQUARE, PSXI_SW_SDA_SQUARE},
            {armsx::vr::PAD_TRIANGLE, PSXI_SW_SDA_TRIANGLE},
            {armsx::vr::PAD_L1, PSXI_SW_SDA_L1},
            {armsx::vr::PAD_R1, PSXI_SW_SDA_R1},
            {armsx::vr::PAD_L2, PSXI_SW_SDA_L2},
            {armsx::vr::PAD_R2, PSXI_SW_SDA_R2},
            {armsx::vr::PAD_L3, PSXI_SW_SDA_L3},
            {armsx::vr::PAD_R3, PSXI_SW_SDA_R3},
            {armsx::vr::PAD_START, PSXI_SW_SDA_START},
            {armsx::vr::PAD_SELECT, PSXI_SW_SDA_SELECT},
        }};
        uint32_t mask = 0;
        for (const auto& [vr_bit, psx_bit] : table) {
            if (buttons & vr_bit) {
                mask |= psx_bit;
            }
        }
        return mask;
    }

    void startVr() {
        int width = 0;
        int height = 0;
        SDL_GetRendererOutputSize(renderer_, &width, &height);
        std::string error;
        if (!armsx::vr::Start(renderer_, width, height, settings_.vr_screen, error)) {
            psxe_diag_logf("vr", "VR mode unavailable: %s", error.c_str());
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ARMSX VR mode unavailable: %s", error.c_str());
            return;
        }
        vr_width_ = width;
        vr_height_ = height;
        psxe_diag_logf("vr", "VR mode active screen=%dx%d", width, height);

        // This thread runs the emulator; on a slow core heavy scenes miss 60 fps and the audio
        // queue drains.
        std::string pinning;
        armsx::vr::PinCurrentThreadToFastCores(pinning);
        psxe_diag_logf("vr", "emulator thread: %s", pinning.c_str());
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "ARMSX-PERF", "emulator thread: %s", pinning.c_str());
#endif
    }

    // Controllers -> pad, Menu tap -> pause menu, focus loss -> pause. Runs once per frame, before
    // the emulator steps.
    void pollVr(bool fsui_active) {
        if (!armsx::vr::Active()) {
            return;
        }
        armsx::vr::GetSnapshot(vr_snapshot_);
        if (vr_snapshot_.exit_requested) {
            // Quit from the system menu: the runtime is ending the session.
            running_ = false;
            return;
        }

        const bool focused = vr_snapshot_.focused;
        if (vr_focused_ && !focused && session_.valid() && !session_.paused() && !fsui_active) {
            // A system overlay is up or the headset came off: nobody is watching the game.
            openPauseMenu();
            fsui_active = true;
        }
        vr_focused_ = focused;
        if (!focused) {
            vr_mapper_.ResetHeld();
            input_router_.setVrPad(0, kCentredAnalog);
            return;
        }

        const double now = static_cast<double>(SDL_GetTicks64()) / 1000.0;
        const armsx::vr::MapperOutput out = vr_mapper_.Update(vr_snapshot_.controllers, now);
        if (out.recenter) {
            armsx::vr::Recenter();
        }

        bool in_game = session_.valid() && !fsui_active;
        if (out.open_menu) {
            if (in_game) {
                openPauseMenu();
                in_game = false;
            } else if (session_.valid() && ui_window_state_ == FsuiWindowState::PauseMenu) {
                // Menu toggles the pause menu, the way it opened it.
                returnToMainWindow();
                session_.setPaused(false);
                resetFramePacing("vr-menu-resume");
            }
        }

        if (!in_game) {
            vr_hold_until_release_ = true;
            input_router_.setVrPad(0, kCentredAnalog);
            return;
        }
        // The button that closed the menu (A on "Resume") must not also land in the game.
        if (vr_hold_until_release_) {
            if (out.pad.buttons != 0) {
                input_router_.setVrPad(0, kCentredAnalog);
                return;
            }
            vr_hold_until_release_ = false;
        }

        const armsx::vr::PadOutput& pad = out.pad;
        input_router_.setVrPad(PadMaskFromVr(pad.buttons), {
            armsx::vr::AnalogByte(pad.left_x, false),
            armsx::vr::AnalogByte(pad.left_y, true),
            armsx::vr::AnalogByte(pad.right_x, false),
            armsx::vr::AnalogByte(pad.right_y, true),
        });
    }

    // While FSUI is open, the controllers drive ImGui: the laser pointer of whichever hand last
    // pulled its trigger is the mouse, the trigger clicks, the right stick scrolls, and the face
    // buttons, grips and left stick are the gamepad navigation FSUI is built around. Runs between
    // the platform backend's NewFrame and ImGui::NewFrame. Only changes are sent, so a Bluetooth
    // pad paired to the headset keeps working alongside.
    void feedVrImGui(bool fsui_active) {
        if (!armsx::vr::Active()) {
            return;
        }
        ImGuiIO& io = ImGui::GetIO();
        // The frame textures are this size; keep ImGui's layout and pointer maths in step with
        // them even if the Android surface ever changes size.
        io.DisplaySize = ImVec2(static_cast<float>(vr_width_), static_cast<float>(vr_height_));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

        const bool ui_input = vr_focused_ && fsui_active;
        const armsx::vr::ControllerInput& in = vr_snapshot_.controllers;
        const auto hysteresis = [](bool was_down, float value) {
            return was_down ? value > 0.4f : value > 0.6f;
        };

        const float triggers[2] = {in.left_trigger, in.right_trigger};
        for (int hand = 0; hand < 2; hand++) {
            const bool was_down = vr_trigger_down_[hand];
            vr_trigger_down_[hand] = hysteresis(was_down, triggers[hand]);
            if (vr_trigger_down_[hand] && !was_down) {
                vr_pointer_hand_ = hand;
            }
        }
        const int other = 1 - vr_pointer_hand_;
        if (!vr_snapshot_.pointer[vr_pointer_hand_].valid && vr_snapshot_.pointer[other].valid &&
            !vr_trigger_down_[vr_pointer_hand_]) {
            vr_pointer_hand_ = other;
        }

        const armsx::vr::PointerHit& hit = vr_snapshot_.pointer[vr_pointer_hand_];
        const bool cursor_was_visible = vr_cursor_visible_;
        vr_cursor_visible_ = ui_input && hit.valid;
        if (vr_cursor_visible_) {
            vr_cursor_ = ImVec2(hit.u * io.DisplaySize.x, hit.v * io.DisplaySize.y);
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(vr_cursor_.x, vr_cursor_.y);
        } else if (cursor_was_visible) {
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }

        const bool click = ui_input && vr_trigger_down_[vr_pointer_hand_];
        if (click != vr_mouse_down_) {
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, click);
            vr_mouse_down_ = click;
        }

        if (ui_input && std::fabs(in.right_y) > 0.25f) {
            io.AddMouseWheelEvent(0.0f, in.right_y * 0.35f);
        }

        vr_nav_x_ = StickNavAxis(vr_nav_x_, in.left_x);
        vr_nav_y_ = StickNavAxis(vr_nav_y_, in.left_y);
        const std::array<std::pair<ImGuiKey, bool>, 10> keys = {{
            {ImGuiKey_GamepadFaceDown, in.a},
            {ImGuiKey_GamepadFaceRight, in.b},
            {ImGuiKey_GamepadFaceLeft, in.x},
            {ImGuiKey_GamepadFaceUp, in.y},
            {ImGuiKey_GamepadL1, in.left_grip > 0.6f},
            {ImGuiKey_GamepadR1, in.right_grip > 0.6f},
            {ImGuiKey_GamepadDpadUp, vr_nav_y_ > 0},
            {ImGuiKey_GamepadDpadDown, vr_nav_y_ < 0},
            {ImGuiKey_GamepadDpadLeft, vr_nav_x_ < 0},
            {ImGuiKey_GamepadDpadRight, vr_nav_x_ > 0},
        }};
        for (size_t i = 0; i < keys.size(); i++) {
            const bool down = ui_input && keys[i].second;
            const uint32_t bit = 1u << i;
            if (down != ((vr_keys_down_ & bit) != 0)) {
                io.AddKeyEvent(keys[i].first, down);
                vr_keys_down_ = down ? (vr_keys_down_ | bit) : (vr_keys_down_ & ~bit);
            }
        }
    }

    static int StickNavAxis(int state, float value) {
        if (state > 0) {
            return value > 0.4f ? 1 : 0;
        }
        if (state < 0) {
            return value < -0.4f ? -1 : 0;
        }
        return value > 0.6f ? 1 : (value < -0.6f ? -1 : 0);
    }

    // Off unless asked for, read once at startup: adb shell setprop log.tag.ARMSX-PERF I
    // (log.tag.* properties are readable by any app, unlike debug.*).
    static bool VrPerfProbeEnabled() {
#if defined(__ANDROID__)
        static const bool enabled = [] {
            char value[PROP_VALUE_MAX] = {};
            return __system_property_get("log.tag.ARMSX-PERF", value) > 0 && value[0] != '\0' &&
                value[0] != 'S';
        }();
        return enabled;
#else
        return false;
#endif
    }

    // Every two seconds in VR mode: emulated fps, frame work time and audio queue health, on
    // logcat (adb logcat -s ARMSX-PERF). Frame work is everything after the pacing wait.
    void probeVrPerformance(uint64_t frame_work_start, bool emulated, uint64_t emulate_ticks, uint64_t texture_ticks,
                            uint64_t submit_ticks) {
        const uint64_t now = SDL_GetPerformanceCounter();
        const double work_ms = CounterTicksToMilliseconds(now - frame_work_start);
        if (vr_probe_.window_start == 0) {
            vr_probe_ = VrProbe{};
            vr_probe_.window_start = now;
            vr_probe_.underruns_at_start = session_.valid() ? session_.audioUnderruns() : 0;
        }
        vr_probe_.frames++;
        vr_probe_.emulated += emulated ? 1 : 0;
        vr_probe_.work_total_ms += work_ms;
        vr_probe_.emulate_total_ms += CounterTicksToMilliseconds(emulate_ticks);
        vr_probe_.emulate_max_ms = std::max(vr_probe_.emulate_max_ms, CounterTicksToMilliseconds(emulate_ticks));
        vr_probe_.texture_total_ms += CounterTicksToMilliseconds(texture_ticks);
        vr_probe_.submit_total_ms += CounterTicksToMilliseconds(submit_ticks);
        vr_probe_.work_max_ms = std::max(vr_probe_.work_max_ms, work_ms);
        vr_probe_.slow_core_frames += armsx::vr::OnSlowCore() ? 1 : 0;
        vr_probe_.late_frames += work_ms > 1000.0 / std::max(currentTargetFrameRate(), 1.0) ? 1 : 0;
        const double queued = session_.valid() ? session_.queuedAudioMs() : -1.0;
        if (queued >= 0.0) {
            vr_probe_.audio_min_ms = std::min(vr_probe_.audio_min_ms, queued);
            vr_probe_.audio_max_ms = std::max(vr_probe_.audio_max_ms, queued);
        }

        const double window_ms = CounterTicksToMilliseconds(now - vr_probe_.window_start);
        if (window_ms < 2000.0) {
            return;
        }
        const uint64_t underruns = session_.valid() ? session_.audioUnderruns() : 0;
        const double frames = std::max<uint32_t>(vr_probe_.frames, 1u);
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "ARMSX-PERF",
#else
        psxe_diag_logf("perf",
#endif
            "emu_fps=%.2f loop_fps=%.2f work_avg=%.2fms work_max=%.2fms emulate_avg=%.2fms emulate_max=%.2fms "
            "texture3d_avg=%.2fms submit_avg=%.2fms (begin=%.2f draw=%.2f end=%.2f) ui_avg=%.2fms late=%u slow_core=%u audio_queue=%.0f..%.0fms underruns=%llu\n",
            vr_probe_.emulated * 1000.0 / window_ms,
            vr_probe_.frames * 1000.0 / window_ms,
            vr_probe_.work_total_ms / frames,
            vr_probe_.work_max_ms,
            vr_probe_.emulate_total_ms / frames,
            vr_probe_.emulate_max_ms,
            vr_probe_.texture_total_ms / frames,
            vr_probe_.submit_total_ms / frames,
            vr_probe_.begin_total_ms / frames,
            vr_probe_.draw_total_ms / frames,
            vr_probe_.end_total_ms / frames,
            (vr_probe_.work_total_ms - vr_probe_.emulate_total_ms - vr_probe_.texture_total_ms - vr_probe_.submit_total_ms) / frames,
            vr_probe_.late_frames,
            vr_probe_.slow_core_frames,
            vr_probe_.audio_min_ms > 1e8 ? -1.0 : vr_probe_.audio_min_ms,
            vr_probe_.audio_max_ms,
            static_cast<unsigned long long>(underruns - std::min(underruns, vr_probe_.underruns_at_start)));
        vr_probe_.window_start = 0;
    }

    void drawVrCursor() {
        if (!vr_cursor_visible_) {
            return;
        }
        ImDrawList* draw_list = ImGui::GetForegroundDrawList();
        const float radius = std::max(6.0f, ImGui::GetIO().DisplaySize.y / 110.0f);
        draw_list->AddCircleFilled(vr_cursor_, radius + 2.0f, IM_COL32(0, 0, 0, 170));
        draw_list->AddCircleFilled(vr_cursor_, radius, vr_mouse_down_ ? IM_COL32(110, 180, 255, 255) : IM_COL32(255, 255, 255, 240));
    }

    // Draws ImGui's frame into the next VR frame texture instead of presenting the window.
    // In 3D, the same ImGui frame is drawn once per eye, each time with that eye's game image in
    // the texture the frame places; FSUI and the cursor come out identical, on the screen plane.
    bool renderVrFrame() {
        const uint64_t begin_start = SDL_GetPerformanceCounter();
        armsx::vr::FrameTargets targets;
        if (!armsx::vr::BeginFrame(targets)) {
            return false;
        }
        const uint64_t draw_start = SDL_GetPerformanceCounter();
        const bool stereo = session_.valid() && session_.stereoReady();
        for (int eye = 0; eye < (stereo ? 2 : 1); eye++) {
            if (stereo && eye == 1) {
                SwapDrawTexture(session_.stereoTexture(0), session_.stereoTexture(1));
            }
            SDL_SetRenderTarget(renderer_, targets.eye[eye]);
            SDL_RenderSetScale(renderer_, 1.0f, 1.0f);
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
            SDL_RenderClear(renderer_);
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer_);
            SDL_RenderFlush(renderer_);
        }
        if (stereo) {
            SwapDrawTexture(session_.stereoTexture(1), session_.stereoTexture(0));
        }
        SDL_SetRenderTarget(renderer_, nullptr);
        const uint64_t end_start = SDL_GetPerformanceCounter();
        armsx::vr::EndFrame(stereo);
        const uint64_t end_done = SDL_GetPerformanceCounter();
        vr_probe_.begin_total_ms += CounterTicksToMilliseconds(draw_start - begin_start);
        vr_probe_.draw_total_ms += CounterTicksToMilliseconds(end_start - draw_start);
        vr_probe_.end_total_ms += CounterTicksToMilliseconds(end_done - end_start);
        return true;
    }

    // Points every draw command that shows `from` at `to` instead.
    static void SwapDrawTexture(SDL_Texture* from, SDL_Texture* to) {
        const ImTextureID from_id = reinterpret_cast<ImTextureID>(from);
        ImDrawData* draw_data = ImGui::GetDrawData();
        for (ImDrawList* list : draw_data->CmdLists) {
            for (ImDrawCmd& command : list->CmdBuffer) {
                if (!command.TexRef._TexData && command.TexRef._TexID == from_id) {
                    command.TexRef._TexID = reinterpret_cast<ImTextureID>(to);
                }
            }
        }
    }

    void applyVrScreen() {
        armsx::vr::SetScreenConfig(settings_.vr_screen);
        applyVrStereo();
        SaveSettings(settings_);
    }

    void applyVrStereo() {
        const bool enabled = armsx::vr::Active() && settings_.vr_depth_percent > 0;
        session_.setStereo(
            enabled,
            static_cast<float>(settings_.vr_depth_percent) / 100.0f,
            static_cast<float>(settings_.vr_popout),
            settings_.vr_screen.width_m);
    }

    fsui::SettingsPageDescriptor buildVrSettingsPage(fsui::SettingsScope scope) {
        fsui::SettingsPageDescriptor page;
        page.id = "VR";
        page.scope = scope;
        page.icon = ICON_FA_TV;
        page.title = "VR Screen";
        page.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

            static constexpr std::array<int, 7> widths_cm = {160, 200, 240, 280, 320, 400, 500};
            static constexpr std::array<int, 7> distances_cm = {100, 125, 150, 175, 200, 250, 300};
            const auto metres = [](int cm) {
                char text[16] = {};
                std::snprintf(text, sizeof(text), "%.2f m", static_cast<double>(cm) / 100.0);
                return std::string(text);
            };
            const auto choice_rows = [&](const auto& values, float current_m, std::string& value_title) {
                std::vector<std::string> titles;
                int selected = 0;
                int best = INT32_MAX;
                const int current_cm = static_cast<int>(std::lround(current_m * 100.0f));
                for (size_t i = 0; i < values.size(); i++) {
                    titles.push_back(metres(values[i]));
                    if (std::abs(values[i] - current_cm) < best) {
                        best = std::abs(values[i] - current_cm);
                        selected = static_cast<int>(i);
                    }
                }
                value_title = metres(current_cm);
                return BuildChoices(titles, selected);
            };

            fsui::SettingsRowDescriptor width;
            width.kind = fsui::SettingsRowKind::Choice;
            width.id = "vr-screen-width";
            width.title = ICON_FA_TV " Screen Width";
            width.summary = "How wide the virtual screen is, edge to edge.";
            width.dialog_title = width.title;
            width.choices = choice_rows(widths_cm, settings_.vr_screen.width_m, width.value);
            width.on_choice = [this](int index) {
                if (index >= 0 && index < static_cast<int>(widths_cm.size())) {
                    settings_.vr_screen.width_m = static_cast<float>(widths_cm[static_cast<size_t>(index)]) / 100.0f;
                    applyVrScreen();
                }
            };
            rows.push_back(std::move(width));

            fsui::SettingsRowDescriptor distance;
            distance.kind = fsui::SettingsRowKind::Choice;
            distance.id = "vr-screen-distance";
            distance.title = ICON_FA_RULER " Screen Distance";
            distance.summary = "How far the middle of the screen is from your eyes. A curved screen keeps every point at this distance.";
            distance.dialog_title = distance.title;
            distance.choices = choice_rows(distances_cm, settings_.vr_screen.distance_m, distance.value);
            distance.on_choice = [this](int index) {
                if (index >= 0 && index < static_cast<int>(distances_cm.size())) {
                    settings_.vr_screen.distance_m = static_cast<float>(distances_cm[static_cast<size_t>(index)]) / 100.0f;
                    applyVrScreen();
                }
            };
            rows.push_back(std::move(distance));

            fsui::SettingsRowDescriptor curved;
            curved.kind = fsui::SettingsRowKind::Toggle;
            curved.id = "vr-screen-curved";
            curved.title = ICON_FA_GLOBE " Curved Screen";
            curved.summary = "Wrap the screen around you so its edges are as close as its middle. Off shows a flat screen.";
            curved.toggle_value = settings_.vr_screen.curved;
            curved.on_toggle = [this](bool value) {
                settings_.vr_screen.curved = value;
                applyVrScreen();
            };
            rows.push_back(std::move(curved));

            static constexpr std::array<int, 7> depth_percents = {0, 25, 50, 75, 100, 125, 150};
            fsui::SettingsRowDescriptor depth;
            depth.kind = fsui::SettingsRowKind::Choice;
            depth.id = "vr-depth";
            depth.title = ICON_FA_EYE " 3D Depth";
            depth.summary =
                "Real stereoscopic depth from the game's own 3D geometry. 100% matches your eye distance; "
                "Off shows the game flat. HUDs, sprites and videos always sit on the screen.";
            depth.value = settings_.vr_depth_percent == 0 ? std::string("Off") : std::to_string(settings_.vr_depth_percent) + "%";
            depth.dialog_title = depth.title;
            {
                std::vector<std::string> titles;
                int selected = 0;
                for (size_t i = 0; i < depth_percents.size(); i++) {
                    titles.push_back(depth_percents[i] == 0 ? std::string("Off") : std::to_string(depth_percents[i]) + "%");
                    if (depth_percents[i] == settings_.vr_depth_percent) {
                        selected = static_cast<int>(i);
                    }
                }
                depth.choices = BuildChoices(titles, selected);
            }
            depth.on_choice = [this](int index) {
                if (index >= 0 && index < static_cast<int>(depth_percents.size())) {
                    settings_.vr_depth_percent = depth_percents[static_cast<size_t>(index)];
                    applyVrScreen();
                }
            };
            rows.push_back(std::move(depth));

            static constexpr std::array<int, 6> popouts = {1, 2, 3, 4, 6, 8};
            static constexpr std::array<const char*, 6> popout_titles = {
                "Behind the screen", "Low", "Medium", "High", "Very high", "Extreme",
            };
            fsui::SettingsRowDescriptor popout;
            popout.kind = fsui::SettingsRowKind::Choice;
            popout.id = "vr-popout";
            popout.title = ICON_FA_CHEVRON_UP " 3D Pop-Out";
            popout.summary =
                "How far the scene comes forward. Low keeps it behind the screen like a window; higher "
                "brings nearer objects out towards you. Games scale their worlds differently, so tune per game.";
            popout.dialog_title = popout.title;
            {
                std::vector<std::string> titles(popout_titles.begin(), popout_titles.end());
                int selected = 1;
                for (size_t i = 0; i < popouts.size(); i++) {
                    if (popouts[i] == settings_.vr_popout) {
                        selected = static_cast<int>(i);
                    }
                }
                popout.value = popout_titles[static_cast<size_t>(selected)];
                popout.choices = BuildChoices(titles, selected);
            }
            popout.on_choice = [this](int index) {
                if (index >= 0 && index < static_cast<int>(popouts.size())) {
                    settings_.vr_popout = popouts[static_cast<size_t>(index)];
                    applyVrScreen();
                }
            };
            rows.push_back(std::move(popout));

            fsui::SettingsRowDescriptor recenter;
            recenter.kind = fsui::SettingsRowKind::Action;
            recenter.id = "vr-recenter";
            recenter.title = ICON_FA_BULLSEYE " Recenter Screen";
            recenter.summary = "Move the screen straight ahead of where you are looking now. In game: hold Menu and pull the right trigger.";
            recenter.on_activate = []() { armsx::vr::Recenter(); };
            rows.push_back(std::move(recenter));

#if defined(__ANDROID__)
            fsui::SettingsRowDescriptor storage;
            storage.kind = fsui::SettingsRowKind::Action;
            storage.id = "vr-storage-access";
            storage.title = ICON_FA_FOLDER " Storage Access";
            storage.summary =
                "Let the file browser reach Internal Storage (Download and other folders) for BIOS files and games. "
                "Opens the system permission page; return to ARMSX afterwards.";
            storage.on_activate = []() { CallAndroidActivityVoidMethod("requestAllFilesAccess", "()V"); };
            rows.push_back(std::move(storage));
#endif

            fsui::SettingsRowDescriptor controls;
            controls.kind = fsui::SettingsRowKind::Notice;
            controls.id = "vr-controls";
            controls.title = ICON_FA_GAMEPAD " Touch Controls";
            controls.summary =
                "A B X Y: Cross Circle Square Triangle. Grips: L1 R1. Triggers: L2 R2. Left stick click: Start. "
                "Right stick click: Select. Left stick: analog, D-pad on a hard push. Menu: pause menu. "
                "Menu + left/right stick click: L3/R3. Menu + B: left stick analog-only. In menus, point and pull a trigger.";
            rows.push_back(std::move(controls));

            return rows;
        };
        return page;
    }

    struct VrProbe {
        uint64_t window_start = 0;
        uint32_t frames = 0;
        uint32_t emulated = 0;
        uint32_t late_frames = 0;
        uint32_t slow_core_frames = 0;
        double work_total_ms = 0.0;
        double work_max_ms = 0.0;
        double emulate_total_ms = 0.0;
        double emulate_max_ms = 0.0;
        double texture_total_ms = 0.0;
        double submit_total_ms = 0.0;
        double begin_total_ms = 0.0;
        double draw_total_ms = 0.0;
        double end_total_ms = 0.0;
        double audio_min_ms = 1e9;
        double audio_max_ms = 0.0;
        uint64_t underruns_at_start = 0;
    };

    bool vr_mode_ = false;
    VrProbe vr_probe_{};
    int vr_width_ = 0;
    int vr_height_ = 0;
    armsx::vr::Snapshot vr_snapshot_{};
    armsx::vr::QuestPadMapper vr_mapper_{};
    bool vr_focused_ = false;
    bool vr_hold_until_release_ = true;
    bool vr_trigger_down_[2] = {false, false};
    int vr_pointer_hand_ = 1;
    bool vr_cursor_visible_ = false;
    ImVec2 vr_cursor_{};
    bool vr_mouse_down_ = false;
    int vr_nav_x_ = 0;
    int vr_nav_y_ = 0;
    uint32_t vr_keys_down_ = 0;

    int argc_ = 0;
    const char* const* argv_ = nullptr;
    SDL_Window* external_window_ = nullptr;
    SDL_Renderer* external_renderer_ = nullptr;
    CliFlags cli_{};
    FrontendSettings settings_{};
    bool running_ = true;
    bool owns_sdl_ = false;
    bool owns_window_ = false;
    bool owns_renderer_ = false;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    fsui::SdlImGuiBackend imgui_backend_{};
    ArmsxSession session_{};
    GameplayInputRouter input_router_{};
    ImGuiMobileControls mobile_controls_{};
    std::vector<fsui::GameEntry> game_list_{};
    std::vector<PlatformLibraryRoot> platform_roots_{};
    std::vector<PlatformLibraryGame> platform_games_{};
    std::uint64_t platform_library_version_ = 0;
    std::optional<std::string> pending_error_dialog_{};
    std::optional<LaunchRequest> deferred_launch_{};
    std::optional<std::filesystem::path> deferred_change_disc_{};
    std::optional<bool> deferred_vsync_{};
    std::optional<StateAction> deferred_state_action_{};
    std::optional<std::string> pending_settings_page_restore_{};
    std::optional<LaunchRequest> pending_cli_launch_{};
    std::string pending_cli_argument_;
    bool close_ui_after_launch_ = false;
    bool deferred_exit_to_library_ = false;
    bool deferred_reset_ = false;
    bool deferred_screenshot_ = false;
    bool deferred_fast_forward_toggle_ = false;
    bool fsui_initialized_ = false;
    FsuiWindowState ui_window_state_ = FsuiWindowState::None;
    bool managed_renderer_vsync_ = DefaultVsyncEnabled();
    uint64_t next_frame_deadline_ = 0;
    uint64_t frame_period_ticks_ = 0;
};

std::string ModuleNameFromPath(const char* path) {
    if (!path || !path[0]) {
        return "(unknown)";
    }

    return std::filesystem::path(path).filename().string();
}

void WriteNativeStackTraceImpl() {
#if defined(_WIN32)
    void* frames[64] = {};
    const USHORT frame_count = CaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    psxe_diag_logf("crash", "Native stack trace (%u frames):", static_cast<unsigned int>(frame_count));

#if !defined(UWP_TARGET)
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);

    char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
#endif

    for (USHORT index = 0; index < frame_count; index++) {
        const DWORD64 address = static_cast<DWORD64>(reinterpret_cast<uintptr_t>(frames[index]));
        HMODULE module = nullptr;
        char module_path[MAX_PATH] = {};
        DWORD64 module_offset = 0;

        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(frames[index]),
                &module) != 0) {
            GetModuleFileNameA(module, module_path, static_cast<DWORD>(std::size(module_path)));
            module_offset = address - static_cast<DWORD64>(reinterpret_cast<uintptr_t>(module));
        }

#if !defined(UWP_TARGET)
        DWORD64 displacement = 0;
        if (SymFromAddr(process, address, &displacement, symbol) != 0) {
            psxe_diag_logf(
                "crash",
                "  #%u %p %s!%s+0x%llx module+0x%llx",
                static_cast<unsigned int>(index),
                frames[index],
                module_path[0] ? ModuleNameFromPath(module_path).c_str() : "(unknown)",
                symbol->Name,
                static_cast<unsigned long long>(displacement),
                static_cast<unsigned long long>(module_offset)
            );
            continue;
        }
#endif

        psxe_diag_logf(
            "crash",
            "  #%u %p %s+0x%llx",
            static_cast<unsigned int>(index),
            frames[index],
            module_path[0] ? ModuleNameFromPath(module_path).c_str() : "(unknown)",
            static_cast<unsigned long long>(module_offset)
        );
    }

#if !defined(UWP_TARGET)
    SymCleanup(process);
#endif
#elif defined(PSXE_HAS_EXECINFO)
    void* frames[64] = {};
    const int frame_count = backtrace(frames, static_cast<int>(std::size(frames)));
    psxe_diag_logf("crash", "Native stack trace (%d frames):", frame_count);

    for (int index = 0; index < frame_count; index++) {
        Dl_info info{};
        if (dladdr(frames[index], &info) != 0 && info.dli_fname) {
            const uintptr_t symbol_offset =
                info.dli_saddr
                    ? (reinterpret_cast<uintptr_t>(frames[index]) - reinterpret_cast<uintptr_t>(info.dli_saddr))
                    : 0u;
            psxe_diag_logf(
                "crash",
                "  #%d %p %s %s+0x%zx",
                index,
                frames[index],
                ModuleNameFromPath(info.dli_fname).c_str(),
                info.dli_sname ? info.dli_sname : "(unknown)",
                symbol_offset
            );
        } else {
            psxe_diag_logf("crash", "  #%d %p", index, frames[index]);
        }
    }
#else
    psxe_diag_logf("crash", "Native stack trace unavailable on this platform build.");
#endif
}

[[noreturn]] void ReportNativeCrash(const char* reason) {
    if (g_crash_reporting.exchange(true)) {
        std::_Exit(1);
    }

    psxe_diag_logf("crash", "Native crash captured: %s", reason ? reason : "(unknown)");

    if (g_active_app) {
        g_active_app->writeCrashContext(reason);
    } else {
        psxe_diag_logf("crash", "No active app context available.");
        psxe_diag_dump_breadcrumbs();
    }

    WriteNativeStackTraceImpl();
    psxe_diag_shutdown();
    std::_Exit(1);
}

} // namespace

extern "C" void psxe_diag_write_native_stacktrace(void) {
    WriteNativeStackTraceImpl();
}

extern "C" int psxe_run(int argc, const char* argv[], void* external_window, void* external_renderer) {
    ArmsxApp app(argc, argv, external_window, external_renderer);
    return app.run();
}

extern "C" PSXE_API void psxe_enqueue_launch_argument(const char* argument) {
    if (!argument || !*argument) {
        return;
    }

    EnqueuePendingLaunchArgument(argument);
}

extern "C" PSXE_API void psxe_wasm_on_file(const char* path) {
    if (!path || !*path) {
        return;
    }

    psxe_enqueue_launch_argument(path);
}

extern "C" PSXE_API void psxe_wasm_on_error(const char* message) {
    const char* text = message && *message ? message : "Unknown browser file error.";
    psxe_diag_logf("web", "Browser file access failed: %s", text);
    EnqueueWebError(text);
}

extern "C" PSXE_API void psxe_set_library_directory_picker_callback(
    PlatformDirectoryPickerCallback callback,
    void* userdata
) {
    g_platform_directory_picker_callback = callback;
    g_platform_directory_picker_userdata = userdata;
}

extern "C" PSXE_API void psxe_set_library_directory_removed_callback(
    PlatformDirectoryRemovedCallback callback,
    void* userdata
) {
    g_platform_directory_removed_callback = callback;
    g_platform_directory_removed_userdata = userdata;
}

extern "C" PSXE_API void psxe_register_platform_library_directory(
    const char* path,
    const char* label,
    int recursive
) {
    RegisterPlatformLibraryRoot(
        path ? path : "",
        label ? label : "",
        recursive != 0
    );
}

#if defined(__ANDROID__)
extern "C" JNIEXPORT void JNICALL Java_com_nanodata_armsx_EmulatorActivity_nativeEnqueueLaunchArgument(
    JNIEnv* env,
    jclass,
    jstring argument
) {
    if (!env || !argument) {
        return;
    }

    const char* utf = env->GetStringUTFChars(argument, nullptr);
    if (!utf) {
        return;
    }

    psxe_enqueue_launch_argument(utf);
    env->ReleaseStringUTFChars(argument, utf);
}

extern "C" JNIEXPORT void JNICALL Java_com_nanodata_armsx_EmulatorActivity_nativeSetPlatformLibrarySnapshot(
    JNIEnv* env,
    jclass,
    jobjectArray root_paths,
    jobjectArray root_labels,
    jbooleanArray root_recursive,
    jobjectArray game_paths,
    jobjectArray game_labels,
    jlongArray game_sizes,
    jlongArray game_modified
) {
    if (!env) {
        return;
    }

    auto array_string = [&](jobjectArray values, jsize index) -> std::string {
        if (!values || index < 0 || index >= env->GetArrayLength(values)) {
            return {};
        }
        auto value = static_cast<jstring>(env->GetObjectArrayElement(values, index));
        if (!value) {
            return {};
        }
        const char* utf = env->GetStringUTFChars(value, nullptr);
        std::string result = utf ? utf : "";
        if (utf) {
            env->ReleaseStringUTFChars(value, utf);
        }
        env->DeleteLocalRef(value);
        return result;
    };

    std::vector<PlatformLibraryRoot> roots;
    const jsize root_count = root_paths ? env->GetArrayLength(root_paths) : 0;
    std::vector<jboolean> recursive(static_cast<size_t>(root_count), JNI_TRUE);
    if (root_recursive && root_count > 0) {
        const jsize count = std::min(root_count, env->GetArrayLength(root_recursive));
        env->GetBooleanArrayRegion(root_recursive, 0, count, recursive.data());
    }
    roots.reserve(static_cast<size_t>(root_count));
    for (jsize index = 0; index < root_count; index++) {
        std::string path = array_string(root_paths, index);
        if (!path.empty()) {
            roots.push_back(PlatformLibraryRoot{
                .path = std::move(path),
                .label = array_string(root_labels, index),
                .recursive = recursive[static_cast<size_t>(index)] == JNI_TRUE,
            });
        }
    }

    std::vector<PlatformLibraryGame> games;
    const jsize game_count = game_paths ? env->GetArrayLength(game_paths) : 0;
    std::vector<jlong> sizes(static_cast<size_t>(game_count), 0);
    std::vector<jlong> modified(static_cast<size_t>(game_count), 0);
    if (game_sizes && game_count > 0) {
        env->GetLongArrayRegion(game_sizes, 0, std::min(game_count, env->GetArrayLength(game_sizes)), sizes.data());
    }
    if (game_modified && game_count > 0) {
        env->GetLongArrayRegion(game_modified, 0, std::min(game_count, env->GetArrayLength(game_modified)), modified.data());
    }
    games.reserve(static_cast<size_t>(game_count));
    for (jsize index = 0; index < game_count; index++) {
        std::string path = array_string(game_paths, index);
        if (!path.empty()) {
            games.push_back(PlatformLibraryGame{
                .path = std::move(path),
                .label = array_string(game_labels, index),
                .size = sizes[static_cast<size_t>(index)] > 0
                    ? static_cast<std::uint64_t>(sizes[static_cast<size_t>(index)])
                    : 0,
                .modified = modified[static_cast<size_t>(index)] > 0
                    ? static_cast<std::time_t>(modified[static_cast<size_t>(index)] / 1000)
                    : 0,
            });
        }
    }
    SetPlatformLibrarySnapshot(std::move(roots), std::move(games));
}
#endif

extern "C" PSXE_API int external_main(int argc, const char* argv[], void* external_window, void* external_renderer) {
    return psxe_run(argc, argv, external_window, external_renderer);
}

#if defined(__ANDROID__)
extern "C" PSXE_API int armsx_android_main(int argc, char* argv[]) {
    psxe_platform_set_fopen_callback(AndroidPlatformFopen, nullptr);
    return psxe_run(argc, const_cast<const char**>(argv), nullptr, nullptr);
}
#endif

#ifndef __DLL_BUILD
int main(int argc, const char* argv[]) {
    return external_main(argc, argv, nullptr, nullptr);
}
#endif
