// Quest VR mode (quest flavor): the whole frontend -- FSUI and the running game -- on a curved
// virtual screen in an immersive OpenXR session. See vr_host.h for the overall shape.
//
// Threads:
//   SDL main thread  draws each frame into one of three SDL target textures (BeginFrame/EndFrame)
//                    and publishes it with a GL fence. It never makes an OpenXR call.
//   XR thread        owns every OpenXR object and a second EGL context in SDL's share group. Once
//                    per headset frame it copies the newest published texture into the swapchain,
//                    syncs the controllers, and submits the screen layer.
//
// The structure follows ARMSX2's QuestXr.cpp (thread, loader init, session state machine, screen
// placement), which is where the ★ lessons below were learned on hardware.

#include "vr_host.h"

#if defined(__ANDROID__)

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace armsx::vr
{
namespace
{
	constexpr const char* kLogTag = "ARMSX-XR";
#define XR_LOG(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)
#define XR_ERR(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

	// One slot the XR thread may be reading, one holding the newest finished frame, and two free
	// for the main thread -- so that it can nearly always pick one whose previous copy into the
	// swapchain has already finished on the GPU, and never has to wait for the other thread.
	constexpr int kFrameSlots = 4;

	constexpr GLint kGlRgba8 = 0x8058;
	constexpr GLint kGlSrgb8Alpha8 = 0x8C43;

	// Copies one SDL frame texture into one eye's half of the swapchain image. SDL's pixels are
	// already sRGB-encoded bytes; the compositor treats a UNORM swapchain as linear light and would
	// brighten them a second time. So the swapchain is sRGB, the shader decodes each texel, and the
	// GPU's sRGB write encodes it back to the very same bytes. It also flips the image: SDL draws
	// render targets top row first, while a GL swapchain is sampled bottom-up.
	constexpr const char* kCopyVertexShader = R"(#version 300 es
out vec2 v_uv;
void main() {
    vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
    v_uv = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";

	constexpr const char* kCopyFragmentShader = R"(#version 300 es
precision highp float;
uniform sampler2D u_frame;
uniform bool u_decode_srgb;
in vec2 v_uv;
out vec4 o_color;
void main() {
    vec4 c = texture(u_frame, v_uv);
    if (u_decode_srgb) {
        c.rgb = mix(c.rgb / 12.92, pow((c.rgb + 0.055) / 1.055, vec3(2.4)), step(0.04045, c.rgb));
    }
    o_color = vec4(c.rgb, 1.0);
}
)";

	// Every OpenXR entry point this file uses. The loader is dlopen'ed, so all of them -- core ones
	// included -- come from xrGetInstanceProcAddr.
#define ARMSX_XR_GLOBAL_FUNCTIONS(X) \
	X(xrEnumerateInstanceExtensionProperties) \
	X(xrCreateInstance)

#define ARMSX_XR_INSTANCE_FUNCTIONS(X) \
	X(xrDestroyInstance) \
	X(xrResultToString) \
	X(xrGetSystem) \
	X(xrCreateSession) \
	X(xrDestroySession) \
	X(xrBeginSession) \
	X(xrEndSession) \
	X(xrPollEvent) \
	X(xrCreateReferenceSpace) \
	X(xrCreateActionSpace) \
	X(xrDestroySpace) \
	X(xrLocateSpace) \
	X(xrStringToPath) \
	X(xrCreateActionSet) \
	X(xrDestroyActionSet) \
	X(xrCreateAction) \
	X(xrSuggestInteractionProfileBindings) \
	X(xrAttachSessionActionSets) \
	X(xrSyncActions) \
	X(xrGetActionStateBoolean) \
	X(xrGetActionStateFloat) \
	X(xrGetActionStateVector2f) \
	X(xrEnumerateSwapchainFormats) \
	X(xrCreateSwapchain) \
	X(xrDestroySwapchain) \
	X(xrEnumerateSwapchainImages) \
	X(xrAcquireSwapchainImage) \
	X(xrWaitSwapchainImage) \
	X(xrReleaseSwapchainImage) \
	X(xrWaitFrame) \
	X(xrBeginFrame) \
	X(xrEndFrame) \
	X(xrGetOpenGLESGraphicsRequirementsKHR)

	struct XrApi
	{
		PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
		PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
#define ARMSX_XR_DECLARE(name) PFN_##name name = nullptr;
		ARMSX_XR_GLOBAL_FUNCTIONS(ARMSX_XR_DECLARE)
		ARMSX_XR_INSTANCE_FUNCTIONS(ARMSX_XR_DECLARE)
#undef ARMSX_XR_DECLARE
		// Optional extensions.
		PFN_xrEnumerateDisplayRefreshRatesFB xrEnumerateDisplayRefreshRatesFB = nullptr;
		PFN_xrGetDisplayRefreshRateFB xrGetDisplayRefreshRateFB = nullptr;
		PFN_xrRequestDisplayRefreshRateFB xrRequestDisplayRefreshRateFB = nullptr;
		PFN_xrPerfSettingsSetPerformanceLevelEXT xrPerfSettingsSetPerformanceLevelEXT = nullptr;
	};

	template <typename Fn>
	bool LoadFunction(const XrApi& api, XrInstance instance, const char* name, Fn& out)
	{
		PFN_xrVoidFunction function = nullptr;
		if (XR_FAILED(api.xrGetInstanceProcAddr(instance, name, &function)) || !function)
			return false;
		out = reinterpret_cast<Fn>(function);
		return true;
	}

	Quat ToQuat(const XrQuaternionf& q)
	{
		return {q.x, q.y, q.z, q.w};
	}

	Vec3 ToVec3(const XrVector3f& v)
	{
		return {v.x, v.y, v.z};
	}

	// One frame in flight: a texture per eye. A mono frame only uses the left one, and the XR
	// thread shows it to both eyes.
	struct FrameSlot
	{
		SDL_Texture* texture[2] = {nullptr, nullptr};
		GLuint gl_texture[2] = {0, 0};
		bool stereo = false;
		GLsync ready_fence = nullptr;    // main -> XR: drawing finished
		GLsync consumed_fence = nullptr; // XR -> main: copy into the swapchain finished
	};

	class QuestXr
	{
	public:
		QuestXr(JavaVM* vm, jobject activity, jobject app_context, EGLDisplay display, EGLConfig config,
			EGLContext shared_context, int width, int height, const ScreenConfig& screen)
			: m_vm(vm)
			, m_activity(activity)
			, m_app_context(app_context)
			, m_egl_display(display)
			, m_egl_config(config)
			, m_shared_context(shared_context)
			, m_width(width)
			, m_height(height)
		{
			m_screen.width_m = screen.width_m;
			m_screen.distance_m = screen.distance_m;
			m_screen.curved = screen.curved;
			m_screen.aspect = static_cast<float>(width) / static_cast<float>(height);
			m_pending_config = screen;
		}

		~QuestXr() { Stop(); }

		// Blocks until the XR thread has an OpenXR session and swapchain, or has given up.
		bool Start(std::string& error)
		{
			m_thread = std::thread(&QuestXr::ThreadMain, this);
			std::unique_lock<std::mutex> lock(m_init_mutex);
			m_init_cv.wait(lock, [this] { return m_init_done; });
			if (!m_init_ok)
			{
				error = m_error;
				lock.unlock();
				Stop();
				return false;
			}
			return true;
		}

		void Stop()
		{
			m_quit.store(true);
			if (m_thread.joinable())
				m_thread.join();
		}

		jobject Activity() const { return m_activity; }

		std::array<FrameSlot, kFrameSlots>& Slots() { return m_slots; }

		// Picks a slot to draw into. `consumed` is non-null only when no free slot's previous copy
		// has finished yet; the caller must then make its GL commands wait for it.
		int AcquireWriteSlot(GLsync& consumed)
		{
			std::lock_guard<std::mutex> lock(m_frame_mutex);
			int fallback = -1;
			for (int i = 0; i < kFrameSlots; i++)
			{
				if (i == m_ready_slot || i == m_reading_slot)
					continue;
				GLsync fence = m_slots[i].consumed_fence;
				// A zero-timeout poll: never blocks. (glWaitSync, which should only make the GPU
				// wait, stalls the calling thread on some drivers.)
				const bool finished = !fence ||
					glClientWaitSync(fence, 0, 0) != GL_TIMEOUT_EXPIRED;
				if (finished)
				{
					if (fence)
						glDeleteSync(fence);
					m_slots[i].consumed_fence = nullptr;
					consumed = nullptr;
					m_writing_slot = i;
					return i;
				}
				if (fallback < 0)
					fallback = i;
			}
			if (fallback >= 0)
			{
				consumed = m_slots[fallback].consumed_fence;
				m_slots[fallback].consumed_fence = nullptr;
				m_writing_slot = fallback;
			}
			return fallback; // never -1: at most two of the slots are taken
		}

		void PublishWriteSlot(GLsync ready, bool stereo)
		{
			GLsync dropped = nullptr;
			{
				std::lock_guard<std::mutex> lock(m_frame_mutex);
				if (m_writing_slot < 0)
				{
					dropped = ready;
				}
				else
				{
					// An older frame the XR thread never picked up is simply superseded.
					if (m_ready_slot >= 0)
					{
						dropped = m_slots[m_ready_slot].ready_fence;
						m_slots[m_ready_slot].ready_fence = nullptr;
					}
					m_slots[m_writing_slot].ready_fence = ready;
					m_slots[m_writing_slot].stereo = stereo;
					m_ready_slot = m_writing_slot;
					m_writing_slot = -1;
				}
			}
			if (dropped)
				glDeleteSync(dropped);
		}

		void GetSnapshot(Snapshot& out)
		{
			std::lock_guard<std::mutex> lock(m_snapshot_mutex);
			out = m_snapshot;
		}

		void SetScreenConfig(const ScreenConfig& config)
		{
			std::lock_guard<std::mutex> lock(m_config_mutex);
			m_pending_config = config;
			m_config_dirty = true;
		}

		void Recenter() { m_recenter.store(true); }

	private:
		struct Actions
		{
			XrActionSet set = XR_NULL_HANDLE;
			XrAction a = XR_NULL_HANDLE, b = XR_NULL_HANDLE, x = XR_NULL_HANDLE, y = XR_NULL_HANDLE;
			XrAction menu = XR_NULL_HANDLE;
			XrAction left_stick_click = XR_NULL_HANDLE, right_stick_click = XR_NULL_HANDLE;
			XrAction left_trigger = XR_NULL_HANDLE, right_trigger = XR_NULL_HANDLE;
			XrAction left_grip = XR_NULL_HANDLE, right_grip = XR_NULL_HANDLE;
			XrAction left_stick = XR_NULL_HANDLE, right_stick = XR_NULL_HANDLE;
			XrAction left_aim = XR_NULL_HANDLE, right_aim = XR_NULL_HANDLE;
		};

		void FinishInit(bool ok)
		{
			std::lock_guard<std::mutex> lock(m_init_mutex);
			m_init_ok = ok;
			m_init_done = true;
			m_init_cv.notify_all();
		}

		void ThreadMain()
		{
			JNIEnv* env = nullptr;
			if (m_vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
			{
				m_error = "AttachCurrentThread failed";
				FinishInit(false);
				return;
			}

			const bool ok = InitEgl() && LoadLoader() && InitInstance(env) && InitSession() && InitActions() && InitSwapchain();
			if (!ok)
				XR_ERR("init failed: %s", m_error.c_str());
			FinishInit(ok);

			if (ok)
			{
				while (!m_quit.load())
				{
					PollEvents();
					if (!m_session_running)
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(20));
						continue;
					}
					RunFrame();
				}
			}

			Destroy();
			m_vm->DetachCurrentThread();
		}

		bool Fail(const char* what, XrResult result = XR_SUCCESS)
		{
			m_error = what;
			if (result != XR_SUCCESS && m_instance != XR_NULL_HANDLE && m_xr.xrResultToString)
			{
				char name[XR_MAX_RESULT_STRING_SIZE] = {};
				m_xr.xrResultToString(m_instance, result, name);
				m_error += std::string(" (") + name + ")";
			}
			else if (result != XR_SUCCESS)
			{
				m_error += " (XrResult " + std::to_string(static_cast<int>(result)) + ")";
			}
			return false;
		}

		// A context of our own in SDL's share group: SDL's frame textures and fences are visible
		// here, while the framebuffers and GL state this thread touches never disturb SDL's.
		bool InitEgl()
		{
			const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
			m_egl_context = eglCreateContext(m_egl_display, m_egl_config, m_shared_context, context_attribs);
			if (m_egl_context == EGL_NO_CONTEXT)
				return Fail("eglCreateContext (shared with SDL) failed");

			if (!eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_egl_context))
			{
				// No EGL_KHR_surfaceless_context: a tiny pbuffer instead.
				const EGLint pbuffer_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
				m_egl_surface = eglCreatePbufferSurface(m_egl_display, m_egl_config, pbuffer_attribs);
				if (m_egl_surface == EGL_NO_SURFACE)
					return Fail("eglCreatePbufferSurface failed");
				if (!eglMakeCurrent(m_egl_display, m_egl_surface, m_egl_surface, m_egl_context))
					return Fail("eglMakeCurrent failed");
			}
			return true;
		}

		bool LoadLoader()
		{
			// Packaged into the quest APK from the Khronos loader .aar; absent from the phone APK.
			static void* s_loader = nullptr;
			if (!s_loader)
				s_loader = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
			if (!s_loader)
				return Fail("libopenxr_loader.so is not packaged in this APK");
			m_xr.xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(s_loader, "xrGetInstanceProcAddr"));
			if (!m_xr.xrGetInstanceProcAddr)
				return Fail("OpenXR loader has no xrGetInstanceProcAddr");

#define ARMSX_XR_LOAD_GLOBAL(name) \
	if (!LoadFunction(m_xr, XR_NULL_HANDLE, #name, m_xr.name)) \
		return Fail("OpenXR loader is missing " #name);
			ARMSX_XR_GLOBAL_FUNCTIONS(ARMSX_XR_LOAD_GLOBAL)
#undef ARMSX_XR_LOAD_GLOBAL
			if (!LoadFunction(m_xr, XR_NULL_HANDLE, "xrInitializeLoaderKHR", m_xr.xrInitializeLoaderKHR))
				return Fail("OpenXR loader has no xrInitializeLoaderKHR");
			return true;
		}

		bool InitInstance(JNIEnv* env)
		{
			(void)env;
			// xrInitializeLoaderKHR may only succeed once per process.
			//
			// ★ The context handed over here must outlive EVERY session: Meta's loader keeps it and
			// reaches for it again later. Passing the ACTIVITY aborted ARMSX2 on the second entry
			// into VR ("JNI DETECTED ERROR: java_class == null in GetMethodID"). The application
			// context lives as long as the process.
			static bool s_loader_initialized = false;
			if (!s_loader_initialized)
			{
				XrLoaderInitInfoAndroidKHR loader_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
				loader_info.applicationVM = m_vm;
				loader_info.applicationContext = m_app_context;
				const XrResult res = m_xr.xrInitializeLoaderKHR(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader_info));
				if (XR_FAILED(res))
					return Fail("xrInitializeLoaderKHR failed", res);
				s_loader_initialized = true;
			}

			uint32_t ext_count = 0;
			XrResult res = m_xr.xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
			if (XR_FAILED(res))
				return Fail("no OpenXR runtime", res);
			std::vector<XrExtensionProperties> props(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
			m_xr.xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, props.data());
			const auto has_ext = [&props](const char* name) {
				return std::any_of(props.begin(), props.end(), [name](const XrExtensionProperties& p) {
					return std::strcmp(p.extensionName, name) == 0;
				});
			};

			std::vector<const char*> extensions = {
				XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
				XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
			};
			for (const char* ext : extensions)
			{
				if (!has_ext(ext))
				{
					m_error = std::string("runtime lacks ") + ext;
					return false;
				}
			}
			const auto enable_optional = [&](const char* name) {
				if (!has_ext(name))
					return false;
				extensions.push_back(name);
				return true;
			};
			// Curved screen. Without it the screen is submitted as a flat quad instead.
			m_has_cylinder_ext = enable_optional(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
			m_has_refresh_rate_ext = enable_optional(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
			m_has_perf_settings_ext = enable_optional(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
			// PICO controllers: the bytedance/* profiles below only exist with this.
			enable_optional(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);

			XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
			android_info.applicationVM = m_vm;
			android_info.applicationActivity = m_activity;

			XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
			create_info.next = &android_info;
			std::strncpy(create_info.applicationInfo.applicationName, "ARMSX", XR_MAX_APPLICATION_NAME_SIZE - 1);
			create_info.applicationInfo.applicationVersion = 1;
			std::strncpy(create_info.applicationInfo.engineName, "ARMSX", XR_MAX_ENGINE_NAME_SIZE - 1);
			create_info.applicationInfo.engineVersion = 1;
			create_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
			create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
			create_info.enabledExtensionNames = extensions.data();
			res = m_xr.xrCreateInstance(&create_info, &m_instance);
			if (XR_FAILED(res))
				return Fail("xrCreateInstance failed", res);

#define ARMSX_XR_LOAD_INSTANCE(name) \
	if (!LoadFunction(m_xr, m_instance, #name, m_xr.name)) \
		return Fail("OpenXR runtime is missing " #name);
			ARMSX_XR_INSTANCE_FUNCTIONS(ARMSX_XR_LOAD_INSTANCE)
#undef ARMSX_XR_LOAD_INSTANCE
			if (m_has_perf_settings_ext)
				m_has_perf_settings_ext = LoadFunction(m_xr, m_instance, "xrPerfSettingsSetPerformanceLevelEXT", m_xr.xrPerfSettingsSetPerformanceLevelEXT);
			if (m_has_refresh_rate_ext)
			{
				m_has_refresh_rate_ext =
					LoadFunction(m_xr, m_instance, "xrEnumerateDisplayRefreshRatesFB", m_xr.xrEnumerateDisplayRefreshRatesFB) &&
					LoadFunction(m_xr, m_instance, "xrGetDisplayRefreshRateFB", m_xr.xrGetDisplayRefreshRateFB) &&
					LoadFunction(m_xr, m_instance, "xrRequestDisplayRefreshRateFB", m_xr.xrRequestDisplayRefreshRateFB);
			}

			XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
			system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
			res = m_xr.xrGetSystem(m_instance, &system_info, &m_system);
			if (XR_FAILED(res))
				return Fail("no head-mounted display", res);

			// Required before xrCreateSession.
			XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
			res = m_xr.xrGetOpenGLESGraphicsRequirementsKHR(m_instance, m_system, &requirements);
			if (XR_FAILED(res))
				return Fail("xrGetOpenGLESGraphicsRequirementsKHR failed", res);
			return true;
		}

		bool InitSession()
		{
			XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
			binding.display = m_egl_display;
			binding.config = m_egl_config;
			binding.context = m_egl_context;

			XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
			session_info.next = &binding;
			session_info.systemId = m_system;
			XrResult res = m_xr.xrCreateSession(m_instance, &session_info, &m_session);
			if (XR_FAILED(res))
				return Fail("xrCreateSession failed", res);

			XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
			space_info.poseInReferenceSpace.orientation.w = 1.0f;
			space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
			res = m_xr.xrCreateReferenceSpace(m_session, &space_info, &m_local_space);
			if (XR_FAILED(res))
				return Fail("LOCAL space failed", res);
			space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
			res = m_xr.xrCreateReferenceSpace(m_session, &space_info, &m_view_space);
			if (XR_FAILED(res))
				return Fail("VIEW space failed", res);
			return true;
		}

		bool InitActions()
		{
			XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
			std::strncpy(set_info.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
			std::strncpy(set_info.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
			XrResult res = m_xr.xrCreateActionSet(m_instance, &set_info, &m_actions.set);
			if (XR_FAILED(res))
				return Fail("xrCreateActionSet failed", res);

			struct ActionDef
			{
				XrAction* action;
				const char* name;
				XrActionType type;
				const char* path;
			};
			const ActionDef defs[] = {
				{&m_actions.a, "button_a", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/a/click"},
				{&m_actions.b, "button_b", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/b/click"},
				{&m_actions.x, "button_x", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/x/click"},
				{&m_actions.y, "button_y", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/y/click"},
				{&m_actions.menu, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/menu/click"},
				{&m_actions.left_stick_click, "left_stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/thumbstick/click"},
				{&m_actions.right_stick_click, "right_stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/thumbstick/click"},
				{&m_actions.left_trigger, "left_trigger", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/left/input/trigger/value"},
				{&m_actions.right_trigger, "right_trigger", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/right/input/trigger/value"},
				{&m_actions.left_grip, "left_grip", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/left/input/squeeze/value"},
				{&m_actions.right_grip, "right_grip", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/right/input/squeeze/value"},
				{&m_actions.left_stick, "left_stick", XR_ACTION_TYPE_VECTOR2F_INPUT, "/user/hand/left/input/thumbstick"},
				{&m_actions.right_stick, "right_stick", XR_ACTION_TYPE_VECTOR2F_INPUT, "/user/hand/right/input/thumbstick"},
				{&m_actions.left_aim, "left_aim", XR_ACTION_TYPE_POSE_INPUT, "/user/hand/left/input/aim/pose"},
				{&m_actions.right_aim, "right_aim", XR_ACTION_TYPE_POSE_INPUT, "/user/hand/right/input/aim/pose"},
			};

			std::vector<XrActionSuggestedBinding> bindings;
			for (const ActionDef& def : defs)
			{
				XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
				action_info.actionType = def.type;
				std::strncpy(action_info.actionName, def.name, XR_MAX_ACTION_NAME_SIZE - 1);
				std::strncpy(action_info.localizedActionName, def.name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
				res = m_xr.xrCreateAction(m_actions.set, &action_info, def.action);
				if (XR_FAILED(res))
					return Fail("xrCreateAction failed", res);

				XrPath path = XR_NULL_PATH;
				m_xr.xrStringToPath(m_instance, def.path, &path);
				bindings.push_back({*def.action, path});
			}

			// A runtime rejects the profiles it does not implement (Meta has no bytedance/*, PICO no
			// oculus/*), so each is suggested on its own and only having NONE accepted is fatal.
			const char* const profiles[] = {
				"/interaction_profiles/oculus/touch_controller",
				"/interaction_profiles/bytedance/pico4_controller",
				"/interaction_profiles/bytedance/pico_neo3_controller",
			};
			int accepted = 0;
			for (const char* profile : profiles)
			{
				XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
				if (XR_FAILED(m_xr.xrStringToPath(m_instance, profile, &suggested.interactionProfile)))
					continue;
				suggested.suggestedBindings = bindings.data();
				suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
				if (XR_SUCCEEDED(m_xr.xrSuggestInteractionProfileBindings(m_instance, &suggested)))
				{
					accepted++;
					XR_LOG("controller profile accepted: %s", profile);
				}
			}
			if (accepted == 0)
				return Fail("the runtime accepted none of this build's controller profiles");

			const XrAction aims[2] = {m_actions.left_aim, m_actions.right_aim};
			for (int hand = 0; hand < 2; hand++)
			{
				XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
				space_info.action = aims[hand];
				space_info.poseInActionSpace.orientation.w = 1.0f;
				res = m_xr.xrCreateActionSpace(m_session, &space_info, &m_aim_space[hand]);
				if (XR_FAILED(res))
					return Fail("xrCreateActionSpace failed", res);
			}

			XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
			attach.countActionSets = 1;
			attach.actionSets = &m_actions.set;
			res = m_xr.xrAttachSessionActionSets(m_session, &attach);
			if (XR_FAILED(res))
				return Fail("xrAttachSessionActionSets failed", res);
			return true;
		}

		bool InitSwapchain()
		{
			uint32_t count = 0;
			m_xr.xrEnumerateSwapchainFormats(m_session, 0, &count, nullptr);
			std::vector<int64_t> formats(count);
			m_xr.xrEnumerateSwapchainFormats(m_session, count, &count, formats.data());
			if (formats.empty())
				return Fail("runtime offers no swapchain formats");
			// sRGB, so the compositor shows SDL's sRGB-encoded pixels as they are (see
			// kCopyFragmentShader). A UNORM swapchain is taken as linear light and comes out too bright.
			int64_t format = formats.front();
			if (std::find(formats.begin(), formats.end(), kGlSrgb8Alpha8) != formats.end())
				format = kGlSrgb8Alpha8;
			else if (std::find(formats.begin(), formats.end(), kGlRgba8) != formats.end())
				format = kGlRgba8;
			m_srgb_swapchain = (format == kGlSrgb8Alpha8);
			XR_LOG("swapchain format 0x%llx (%s)", static_cast<unsigned long long>(format), m_srgb_swapchain ? "sRGB" : "linear");

			XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
			info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
			info.format = format;
			info.sampleCount = 1;
			// Both eyes side by side: left half for the left eye, right half for the right.
			info.width = static_cast<uint32_t>(m_width * 2);
			info.height = static_cast<uint32_t>(m_height);
			info.faceCount = 1;
			info.arraySize = 1;
			info.mipCount = 1;
			XrResult res = m_xr.xrCreateSwapchain(m_session, &info, &m_swapchain);
			if (XR_FAILED(res))
				return Fail("xrCreateSwapchain failed", res);

			m_xr.xrEnumerateSwapchainImages(m_swapchain, 0, &count, nullptr);
			std::vector<XrSwapchainImageOpenGLESKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
			res = m_xr.xrEnumerateSwapchainImages(m_swapchain, count, &count,
				reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
			if (XR_FAILED(res))
				return Fail("xrEnumerateSwapchainImages failed", res);
			for (const XrSwapchainImageOpenGLESKHR& image : images)
				m_swapchain_images.push_back(image.image);
			m_swapchain_fbos.assign(m_swapchain_images.size(), 0);
			return InitCopyProgram();
		}

		static GLuint CompileShader(GLenum type, const char* source)
		{
			const GLuint shader = glCreateShader(type);
			glShaderSource(shader, 1, &source, nullptr);
			glCompileShader(shader);
			GLint ok = GL_FALSE;
			glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
			if (!ok)
			{
				char log[512] = {};
				glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
				XR_ERR("shader compile failed: %s", log);
				glDeleteShader(shader);
				return 0;
			}
			return shader;
		}

		bool InitCopyProgram()
		{
			const GLuint vs = CompileShader(GL_VERTEX_SHADER, kCopyVertexShader);
			const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kCopyFragmentShader);
			if (!vs || !fs)
				return Fail("frame copy shader failed to compile");
			m_copy_program = glCreateProgram();
			glAttachShader(m_copy_program, vs);
			glAttachShader(m_copy_program, fs);
			glLinkProgram(m_copy_program);
			glDeleteShader(vs);
			glDeleteShader(fs);
			GLint ok = GL_FALSE;
			glGetProgramiv(m_copy_program, GL_LINK_STATUS, &ok);
			if (!ok)
				return Fail("frame copy shader failed to link");
			glUseProgram(m_copy_program);
			glUniform1i(glGetUniformLocation(m_copy_program, "u_frame"), 0);
			glUniform1i(glGetUniformLocation(m_copy_program, "u_decode_srgb"), m_srgb_swapchain ? 1 : 0);
			glGenVertexArrays(1, &m_copy_vao);
			// Its own sampler, so SDL's textures keep their filtering parameters untouched.
			glGenSamplers(1, &m_copy_sampler);
			glSamplerParameteri(m_copy_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glSamplerParameteri(m_copy_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glSamplerParameteri(m_copy_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glSamplerParameteri(m_copy_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			return true;
		}

		void Destroy()
		{
			if (m_egl_context != EGL_NO_CONTEXT)
			{
				for (GLuint& fbo : m_swapchain_fbos)
				{
					if (fbo)
						glDeleteFramebuffers(1, &fbo);
					fbo = 0;
				}
				if (m_copy_program)
					glDeleteProgram(m_copy_program);
				if (m_copy_vao)
					glDeleteVertexArrays(1, &m_copy_vao);
				if (m_copy_sampler)
					glDeleteSamplers(1, &m_copy_sampler);
			}

			if (m_swapchain != XR_NULL_HANDLE)
				m_xr.xrDestroySwapchain(m_swapchain);
			for (XrSpace& space : m_aim_space)
			{
				if (space != XR_NULL_HANDLE)
					m_xr.xrDestroySpace(space);
			}
			if (m_local_space != XR_NULL_HANDLE)
				m_xr.xrDestroySpace(m_local_space);
			if (m_view_space != XR_NULL_HANDLE)
				m_xr.xrDestroySpace(m_view_space);
			if (m_actions.set != XR_NULL_HANDLE)
				m_xr.xrDestroyActionSet(m_actions.set);
			if (m_session != XR_NULL_HANDLE)
				m_xr.xrDestroySession(m_session);
			if (m_instance != XR_NULL_HANDLE)
				m_xr.xrDestroyInstance(m_instance);

			if (m_egl_context != EGL_NO_CONTEXT)
			{
				glFinish();
				eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
				if (m_egl_surface != EGL_NO_SURFACE)
					eglDestroySurface(m_egl_display, m_egl_surface);
				eglDestroyContext(m_egl_display, m_egl_context);
				// ★ No eglTerminate: the display is process-wide and SDL's context lives on it.
			}
		}

		void PollEvents()
		{
			XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
			while (m_xr.xrPollEvent(m_instance, &event) == XR_SUCCESS)
			{
				switch (event.type)
				{
					case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
						OnSessionStateChanged(reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state);
						break;
					case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
						RequestExit();
						break;
					case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
						// The user long-pressed the Meta button to recenter: follow them.
						m_screen_placed = false;
						break;
					default:
						break;
				}
				event.type = XR_TYPE_EVENT_DATA_BUFFER;
				event.next = nullptr;
			}
		}

		void OnSessionStateChanged(XrSessionState state)
		{
			XR_LOG("session state %d", static_cast<int>(state));
			switch (state)
			{
				case XR_SESSION_STATE_READY:
				{
					XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
					begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					const XrResult res = m_xr.xrBeginSession(m_session, &begin);
					if (XR_FAILED(res))
					{
						XR_ERR("xrBeginSession failed: %d", static_cast<int>(res));
						RequestExit();
						break;
					}
					m_session_running = true;
					m_screen_placed = false;
					PickRefreshRate();
					RequestPerformanceLevels();
					break;
				}
				case XR_SESSION_STATE_STOPPING:
					m_xr.xrEndSession(m_session);
					m_session_running = false;
					break;
				case XR_SESSION_STATE_EXITING:
				case XR_SESSION_STATE_LOSS_PENDING:
					RequestExit();
					break;
				default:
					break;
			}

			// FOCUSED is the only state with controller input. Anything else means a system overlay
			// is up or the headset is off, and the game should not run unseen.
			std::lock_guard<std::mutex> lock(m_snapshot_mutex);
			m_snapshot.session_running = m_session_running;
			m_snapshot.focused = (state == XR_SESSION_STATE_FOCUSED);
			if (!m_snapshot.focused)
			{
				m_snapshot.controllers = ControllerInput();
				m_snapshot.pointer[0] = PointerHit();
				m_snapshot.pointer[1] = PointerHit();
			}
		}

		void RequestExit()
		{
			std::lock_guard<std::mutex> lock(m_snapshot_mutex);
			m_snapshot.exit_requested = true;
		}

		// 120 Hz shows a 60 fps (NTSC) game with every frame held exactly twice; 72 or 90 Hz would
		// judder. Without 120 on offer, the highest rate keeps head motion smoothest.
		void PickRefreshRate()
		{
			if (!m_has_refresh_rate_ext)
				return;
			uint32_t count = 0;
			if (XR_SUCCEEDED(m_xr.xrEnumerateDisplayRefreshRatesFB(m_session, 0, &count, nullptr)) && count > 0)
			{
				std::vector<float> rates(count);
				m_xr.xrEnumerateDisplayRefreshRatesFB(m_session, count, &count, rates.data());
				float chosen = 0.0f;
				for (float rate : rates)
				{
					if (std::fabs(rate - 120.0f) < 0.5f)
					{
						chosen = rate;
						break;
					}
					chosen = std::max(chosen, rate);
				}
				if (chosen > 0.0f)
					m_xr.xrRequestDisplayRefreshRateFB(m_session, chosen);
			}
			float current = 0.0f;
			if (XR_SUCCEEDED(m_xr.xrGetDisplayRefreshRateFB(m_session, &current)))
				XR_LOG("display refresh rate %.1f Hz", current);
		}

		// The emulator is one busy CPU thread; the GPU only composites a single layer.
		void RequestPerformanceLevels()
		{
			if (!m_has_perf_settings_ext)
				return;
			const XrResult cpu = m_xr.xrPerfSettingsSetPerformanceLevelEXT(m_session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT,
				XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
			const XrResult gpu = m_xr.xrPerfSettingsSetPerformanceLevelEXT(m_session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT,
				XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT);
			XR_LOG("performance levels: cpu sustained-high %s, gpu sustained-low %s",
				XR_SUCCEEDED(cpu) ? "ok" : "refused", XR_SUCCEEDED(gpu) ? "ok" : "refused");
		}

		void ApplyPendingConfig()
		{
			std::lock_guard<std::mutex> lock(m_config_mutex);
			if (!m_config_dirty)
				return;
			m_config_dirty = false;
			m_screen.width_m = std::clamp(m_pending_config.width_m, 0.5f, 8.0f);
			m_screen.distance_m = std::clamp(m_pending_config.distance_m, 0.5f, 6.0f);
			m_screen.curved = m_pending_config.curved;
			// The two layer types anchor differently, and the distance is baked into the pose.
			m_screen_placed = false;
		}

		void RunFrame()
		{
			XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
			XrFrameState frame_state{XR_TYPE_FRAME_STATE};
			if (XR_FAILED(m_xr.xrWaitFrame(m_session, &wait_info, &frame_state)))
				return;
			XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
			if (XR_FAILED(m_xr.xrBeginFrame(m_session, &begin_info)))
				return;

			ApplyPendingConfig();
			if (m_recenter.exchange(false))
				m_screen_placed = false;
			if (!m_screen_placed)
				PlaceScreen(frame_state.predictedDisplayTime);

			UpdateInput(frame_state.predictedDisplayTime);
			CopyNewestFrame();

			XrCompositionLayerQuad quads[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
			XrCompositionLayerCylinderKHR cylinders[2] = {
				{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}, {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}};
			const XrCompositionLayerBaseHeader* layers[2] = {};
			uint32_t layer_count = 0;
			if (frame_state.shouldRender && m_has_image && m_screen_placed)
			{
				const Quat q = YawQuat(m_screen.yaw);
				const XrQuaternionf yaw{q.x, q.y, q.z, q.w};
				const Vec3& head = m_screen.head;
				// The same screen, in the same place, for both eyes -- only the half of the swapchain
				// each one samples differs. The compositor projects each layer for its eye, which puts
				// the screen at a real distance in the room; the two halves differing is the game's
				// own depth (stereo_reproject.h).
				for (uint32_t eye = 0; eye < 2; eye++)
				{
					XrSwapchainSubImage sub_image{};
					sub_image.swapchain = m_swapchain;
					sub_image.imageRect.offset = {static_cast<int32_t>(eye) * m_width, 0};
					sub_image.imageRect.extent = {m_width, m_height};
					sub_image.imageArrayIndex = 0;
					const XrEyeVisibility visibility = (eye == 0) ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;

					if (m_screen.curved && m_has_cylinder_ext)
					{
						XrCompositionLayerCylinderKHR& cylinder = cylinders[eye];
						cylinder.space = m_local_space;
						cylinder.eyeVisibility = visibility;
						cylinder.subImage = sub_image;
						cylinder.pose.orientation = yaw;
						cylinder.pose.position = {head.x, head.y, head.z};
						cylinder.radius = m_screen.distance_m;
						cylinder.centralAngle = m_screen.CentralAngle();
						cylinder.aspectRatio = m_screen.aspect;
						layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder);
					}
					else
					{
						XrCompositionLayerQuad& quad = quads[eye];
						quad.space = m_local_space;
						quad.eyeVisibility = visibility;
						quad.subImage = sub_image;
						const Vec3 centre = m_screen.QuadCentre();
						quad.pose.orientation = yaw;
						quad.pose.position = {centre.x, centre.y, centre.z};
						quad.size = {m_screen.width_m, m_screen.QuadHeight()};
						layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
					}
				}
			}

			XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
			end_info.displayTime = frame_state.predictedDisplayTime;
			end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			end_info.layerCount = layer_count;
			end_info.layers = layer_count ? layers : nullptr;
			m_xr.xrEndFrame(m_session, &end_info);
		}

		// Straight ahead of the head at eye height, upright (yaw only), so looking down at the
		// controllers while recentering does not leave the screen tilted into the floor.
		void PlaceScreen(XrTime time)
		{
			XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
			if (XR_FAILED(m_xr.xrLocateSpace(m_view_space, m_local_space, time, &location)))
				return;
			const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
			if ((location.locationFlags & needed) != needed)
				return;

			m_screen.yaw = YawOf(ToQuat(location.pose.orientation));
			m_screen.head = ToVec3(location.pose.position);
			m_screen_placed = true;
		}

		bool GetBool(XrAction action)
		{
			XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
			info.action = action;
			XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
			return XR_SUCCEEDED(m_xr.xrGetActionStateBoolean(m_session, &info, &state)) && state.isActive && state.currentState;
		}

		float GetFloat(XrAction action)
		{
			XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
			info.action = action;
			XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
			return (XR_SUCCEEDED(m_xr.xrGetActionStateFloat(m_session, &info, &state)) && state.isActive) ? state.currentState : 0.0f;
		}

		XrVector2f GetVector(XrAction action)
		{
			XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
			info.action = action;
			XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
			if (XR_SUCCEEDED(m_xr.xrGetActionStateVector2f(m_session, &info, &state)) && state.isActive)
				return state.currentState;
			return {0.0f, 0.0f};
		}

		void UpdateInput(XrTime time)
		{
			bool focused = false;
			{
				std::lock_guard<std::mutex> lock(m_snapshot_mutex);
				focused = m_snapshot.focused;
			}
			if (!focused)
				return;

			XrActiveActionSet active{m_actions.set, XR_NULL_PATH};
			XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
			sync.countActiveActionSets = 1;
			sync.activeActionSets = &active;
			if (XR_FAILED(m_xr.xrSyncActions(m_session, &sync)))
				return;

			ControllerInput in;
			in.a = GetBool(m_actions.a);
			in.b = GetBool(m_actions.b);
			in.x = GetBool(m_actions.x);
			in.y = GetBool(m_actions.y);
			in.menu = GetBool(m_actions.menu);
			in.left_stick_click = GetBool(m_actions.left_stick_click);
			in.right_stick_click = GetBool(m_actions.right_stick_click);
			in.left_trigger = GetFloat(m_actions.left_trigger);
			in.right_trigger = GetFloat(m_actions.right_trigger);
			in.left_grip = GetFloat(m_actions.left_grip);
			in.right_grip = GetFloat(m_actions.right_grip);
			const XrVector2f left = GetVector(m_actions.left_stick);
			const XrVector2f right = GetVector(m_actions.right_stick);
			in.left_x = left.x;
			in.left_y = left.y;
			in.right_x = right.x;
			in.right_y = right.y;

			PointerHit hits[2];
			if (m_screen_placed)
			{
				for (int hand = 0; hand < 2; hand++)
				{
					XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
					if (XR_FAILED(m_xr.xrLocateSpace(m_aim_space[hand], m_local_space, time, &location)))
						continue;
					const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
					if ((location.locationFlags & needed) != needed)
						continue;
					const Vec3 direction = Rotate(ToQuat(location.pose.orientation), {0.0f, 0.0f, -1.0f});
					hits[hand] = m_screen.Intersect(ToVec3(location.pose.position), direction);
				}
			}

			std::lock_guard<std::mutex> lock(m_snapshot_mutex);
			m_snapshot.controllers = in;
			m_snapshot.pointer[0] = hits[0];
			m_snapshot.pointer[1] = hits[1];
			m_snapshot.sequence++;
		}

		// Copies the newest frame the main thread published into the next swapchain image. Nothing
		// new means the swapchain keeps its last image, which the compositor shows again.
		void CopyNewestFrame()
		{
			int slot_index = -1;
			GLsync ready = nullptr;
			{
				std::lock_guard<std::mutex> lock(m_frame_mutex);
				if (m_ready_slot < 0)
					return;
				slot_index = m_ready_slot;
				m_ready_slot = -1;
				m_reading_slot = slot_index;
				ready = m_slots[slot_index].ready_fence;
				m_slots[slot_index].ready_fence = nullptr;
			}
			FrameSlot& slot = m_slots[slot_index];

			if (ready)
			{
				glWaitSync(ready, 0, GL_TIMEOUT_IGNORED);
				glDeleteSync(ready);
			}

			uint32_t image_index = 0;
			XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait.timeout = XR_INFINITE_DURATION;
			const bool acquired = XR_SUCCEEDED(m_xr.xrAcquireSwapchainImage(m_swapchain, &acquire, &image_index)) &&
				XR_SUCCEEDED(m_xr.xrWaitSwapchainImage(m_swapchain, &wait));

			if (acquired && image_index < m_swapchain_images.size())
			{
				GLuint& draw_fbo = m_swapchain_fbos[image_index];
				if (!draw_fbo)
				{
					glGenFramebuffers(1, &draw_fbo);
					glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
					glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
						m_swapchain_images[image_index], 0);
				}
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
				glUseProgram(m_copy_program);
				glBindVertexArray(m_copy_vao);
				glActiveTexture(GL_TEXTURE0);
				glBindSampler(0, m_copy_sampler);

				for (int eye = 0; eye < 2; eye++)
				{
					// A mono frame shows its one image to both eyes.
					const int source = slot.stereo ? eye : 0;
					glBindTexture(GL_TEXTURE_2D, slot.gl_texture[source]);
					glViewport(eye * m_width, 0, m_width, m_height);
					glDrawArrays(GL_TRIANGLES, 0, 3);
				}
				glBindTexture(GL_TEXTURE_2D, 0);
				glBindVertexArray(0);
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
			}

			GLsync consumed = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			glFlush();

			if (acquired)
			{
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				m_xr.xrReleaseSwapchainImage(m_swapchain, &release);
				m_has_image = true;
			}

			GLsync stale = nullptr;
			{
				std::lock_guard<std::mutex> lock(m_frame_mutex);
				stale = slot.consumed_fence;
				slot.consumed_fence = consumed;
				m_reading_slot = -1;
			}
			if (stale)
				glDeleteSync(stale);
		}

		JavaVM* m_vm;
		jobject m_activity;    // global ref, owned by Start()/Stop() below
		jobject m_app_context; // global ref, lives for the whole process
		std::thread m_thread;
		std::atomic<bool> m_quit{false};
		std::string m_error;

		std::mutex m_init_mutex;
		std::condition_variable m_init_cv;
		bool m_init_done = false;
		bool m_init_ok = false;

		EGLDisplay m_egl_display;
		EGLConfig m_egl_config;
		EGLContext m_shared_context;
		EGLContext m_egl_context = EGL_NO_CONTEXT;
		EGLSurface m_egl_surface = EGL_NO_SURFACE;

		XrApi m_xr;
		XrInstance m_instance = XR_NULL_HANDLE;
		XrSystemId m_system = XR_NULL_SYSTEM_ID;
		XrSession m_session = XR_NULL_HANDLE;
		XrSpace m_local_space = XR_NULL_HANDLE;
		XrSpace m_view_space = XR_NULL_HANDLE;
		XrSpace m_aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
		Actions m_actions;
		bool m_has_cylinder_ext = false;
		bool m_has_refresh_rate_ext = false;
		bool m_has_perf_settings_ext = false;
		bool m_session_running = false;

		int m_width;
		int m_height;
		XrSwapchain m_swapchain = XR_NULL_HANDLE;
		std::vector<GLuint> m_swapchain_images;
		std::vector<GLuint> m_swapchain_fbos;
		bool m_has_image = false;
		bool m_srgb_swapchain = false;
		GLuint m_copy_program = 0;
		GLuint m_copy_vao = 0;
		GLuint m_copy_sampler = 0;

		ScreenGeometry m_screen;
		bool m_screen_placed = false;
		std::atomic<bool> m_recenter{false};
		std::mutex m_config_mutex;
		ScreenConfig m_pending_config;
		bool m_config_dirty = false;

		std::mutex m_snapshot_mutex;
		Snapshot m_snapshot;

		std::mutex m_frame_mutex;
		std::array<FrameSlot, kFrameSlots> m_slots{};
		int m_ready_slot = -1;
		int m_reading_slot = -1;
		int m_writing_slot = -1;
	};

	struct VrState
	{
		std::unique_ptr<QuestXr> xr;
		SDL_Renderer* renderer = nullptr;
		bool frame_open = false;
	};

	VrState& State()
	{
		static VrState state;
		return state;
	}

	bool ChooseSharedConfig(EGLDisplay display, EGLContext context, EGLConfig& out)
	{
		EGLint config_id = 0;
		if (!eglQueryContext(display, context, EGL_CONFIG_ID, &config_id))
			return false;
		const EGLint attribs[] = {EGL_CONFIG_ID, config_id, EGL_NONE};
		EGLint count = 0;
		return eglChooseConfig(display, attribs, &out, 1, &count) && count == 1;
	}

	GLuint GlTextureName(SDL_Texture* texture)
	{
		if (SDL_GL_BindTexture(texture, nullptr, nullptr) != 0)
			return 0;
		GLint name = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &name);
		SDL_GL_UnbindTexture(texture);
		return static_cast<GLuint>(name);
	}

	void DestroySlots(std::array<FrameSlot, kFrameSlots>& slots)
	{
		for (FrameSlot& slot : slots)
		{
			if (slot.ready_fence)
				glDeleteSync(slot.ready_fence);
			if (slot.consumed_fence)
				glDeleteSync(slot.consumed_fence);
			for (SDL_Texture* texture : slot.texture)
			{
				if (texture)
					SDL_DestroyTexture(texture);
			}
			slot = FrameSlot();
		}
	}
} // namespace

bool Requested()
{
	const char* value = std::getenv("ARMSX_VR");
	return value && value[0] == '1';
}

bool Start(SDL_Renderer* renderer, int width, int height, const ScreenConfig& config, std::string& error)
{
	VrState& state = State();
	if (state.xr)
		return true;
	if (!renderer || width <= 0 || height <= 0)
	{
		error = "no renderer";
		return false;
	}

	SDL_RendererInfo info{};
	if (SDL_GetRendererInfo(renderer, &info) != 0 || !info.name || std::strcmp(info.name, "opengles2") != 0)
	{
		error = std::string("VR needs the SDL opengles2 renderer, got ") + (info.name ? info.name : "(none)");
		return false;
	}

	// SDL makes its context current when it creates the renderer and never switches it away on
	// this thread.
	const EGLDisplay display = eglGetCurrentDisplay();
	const EGLContext context = eglGetCurrentContext();
	EGLConfig egl_config = nullptr;
	if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT || !ChooseSharedConfig(display, context, egl_config))
	{
		error = "SDL's EGL context is not current";
		return false;
	}

	JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
	jobject activity_local = static_cast<jobject>(SDL_AndroidGetActivity());
	if (!env || !activity_local)
	{
		error = "no Android activity";
		return false;
	}
	JavaVM* vm = nullptr;
	env->GetJavaVM(&vm);

	static jobject s_app_context = nullptr;
	if (!s_app_context)
	{
		jclass activity_class = env->GetObjectClass(activity_local);
		jmethodID get_app_context = env->GetMethodID(activity_class, "getApplicationContext", "()Landroid/content/Context;");
		jobject app_context = get_app_context ? env->CallObjectMethod(activity_local, get_app_context) : nullptr;
		env->DeleteLocalRef(activity_class);
		if (env->ExceptionCheck())
		{
			env->ExceptionDescribe();
			env->ExceptionClear();
		}
		if (!app_context)
		{
			env->DeleteLocalRef(activity_local);
			error = "no application context";
			return false;
		}
		s_app_context = env->NewGlobalRef(app_context);
		env->DeleteLocalRef(app_context);
	}
	jobject activity = env->NewGlobalRef(activity_local);
	env->DeleteLocalRef(activity_local);

	auto xr = std::make_unique<QuestXr>(vm, activity, s_app_context, display, egl_config, context, width, height, config);
	for (FrameSlot& slot : xr->Slots())
	{
		for (int eye = 0; eye < 2; eye++)
		{
			slot.texture[eye] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, width, height);
			slot.gl_texture[eye] = slot.texture[eye] ? GlTextureName(slot.texture[eye]) : 0;
		}
		if (!slot.gl_texture[0] || !slot.gl_texture[1])
		{
			error = std::string("VR frame texture failed: ") + SDL_GetError();
			DestroySlots(xr->Slots());
			env->DeleteGlobalRef(activity);
			return false;
		}
	}
	// The XR context must see complete textures before it attaches them.
	glFinish();

	if (!xr->Start(error))
	{
		DestroySlots(xr->Slots());
		env->DeleteGlobalRef(activity);
		return false;
	}

	state.renderer = renderer;
	state.xr = std::move(xr);
	XR_LOG("VR started: %dx%d screen, %.2f m wide at %.2f m, %s", width, height, config.width_m, config.distance_m,
		config.curved ? "curved" : "flat");
	return true;
}

void Stop()
{
	VrState& state = State();
	if (!state.xr)
		return;
	state.xr->Stop();
	DestroySlots(state.xr->Slots());
	if (JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv()))
		env->DeleteGlobalRef(state.xr->Activity());
	state.xr.reset();
	state.renderer = nullptr;
	state.frame_open = false;
}

bool Active()
{
	return State().xr != nullptr;
}

bool BeginFrame(FrameTargets& out)
{
	VrState& state = State();
	if (!state.xr)
		return false;
	GLsync consumed = nullptr;
	const int index = state.xr->AcquireWriteSlot(consumed);
	if (index < 0)
		return false;
	if (consumed)
	{
		// Queued on the GPU ahead of SDL's draw calls, which only reach GL when SDL flushes.
		glWaitSync(consumed, 0, GL_TIMEOUT_IGNORED);
		glDeleteSync(consumed);
	}
	const FrameSlot& slot = state.xr->Slots()[static_cast<size_t>(index)];
	out.eye[0] = slot.texture[0];
	out.eye[1] = slot.texture[1];
	state.frame_open = true;
	return true;
}

void EndFrame(bool stereo)
{
	VrState& state = State();
	if (!state.xr || !state.frame_open)
		return;
	SDL_RenderFlush(state.renderer);
	GLsync ready = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush();
	state.xr->PublishWriteSlot(ready, stereo);
	state.frame_open = false;
}

void GetSnapshot(Snapshot& out)
{
	VrState& state = State();
	if (!state.xr)
	{
		out = Snapshot();
		return;
	}
	state.xr->GetSnapshot(out);
}

void SetScreenConfig(const ScreenConfig& config)
{
	if (State().xr)
		State().xr->SetScreenConfig(config);
}

void Recenter()
{
	if (State().xr)
		State().xr->Recenter();
}
} // namespace armsx::vr

#else // !__ANDROID__

namespace armsx::vr
{
bool Requested() { return false; }
bool Start(SDL_Renderer*, int, int, const ScreenConfig&, std::string& error)
{
	error = "VR mode is only available in the Meta Quest build";
	return false;
}
void Stop() {}
bool Active() { return false; }
bool BeginFrame(FrameTargets&) { return false; }
void EndFrame(bool) {}
void GetSnapshot(Snapshot& out) { out = Snapshot(); }
void SetScreenConfig(const ScreenConfig&) {}
void Recenter() {}
} // namespace armsx::vr

#endif
