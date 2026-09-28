#pragma once

// The seam between the Quest OpenXR layer (vr_openxr.cpp) and the frontend in main.cpp.
//
// VR mode keeps the frontend exactly as it is -- FSUI and the game are still drawn with the SDL
// GLES2 renderer -- but into an offscreen SDL target texture instead of the window. A dedicated XR
// thread with its own EGL context (shared with SDL's) copies each finished texture into an OpenXR
// swapchain and submits it as a curved cylinder layer. The compositor draws that layer once per
// eye, so the screen sits in the room with real stereoscopic depth, and the headset's refresh rate
// never paces the emulator.
//
// Every call here is made from the SDL main thread. Off Android they are inert stubs.

#include <SDL.h>

#include <cstdint>
#include <string>

#include "quest_pad_mapper.h"
#include "screen_geometry.h"

namespace armsx::vr
{
	// Placement of the virtual screen. Stored in settings.toml [vr].
	struct ScreenConfig
	{
		float width_m = 2.4f;    // chord width of the screen
		float distance_m = 1.5f; // from the eyes to the middle of the screen
		bool curved = true;      // cylinder layer; flat quad when false
	};

	// Latest state from the XR thread, copied out under its lock.
	struct Snapshot
	{
		bool session_running = false;
		bool focused = false;          // controller input only arrives while focused
		bool exit_requested = false;   // the runtime is closing the app (Quit from the system menu)
		ControllerInput controllers;
		PointerHit pointer[2];         // 0 = left hand, 1 = right hand
		std::uint64_t sequence = 0;    // bumped on every XR frame that synced input
	};

	// True when this process was started as the Quest build (ARMSX_VR=1, set by the quest
	// flavor's activity before SDL starts).
	bool Requested();

	// Starts the XR thread against the renderer's current EGL context. `width` x `height` is the
	// resolution of the virtual screen. On failure, returns false and fills `error`.
	bool Start(SDL_Renderer* renderer, int width, int height, const ScreenConfig& config, std::string& error);

	// Joins the XR thread and releases every XR, EGL and SDL object it owns. Must run before the
	// renderer is destroyed.
	void Stop();

	bool Active();

	// Where this frame should be drawn: one texture per eye, each the virtual screen's resolution.
	// A mono frame draws only eye[0]. The textures are free: the XR thread has finished reading
	// their previous contents. Returns false when VR is not active.
	struct FrameTargets
	{
		SDL_Texture* eye[2] = {nullptr, nullptr};
	};
	bool BeginFrame(FrameTargets& out);

	// Hands the textures from BeginFrame() to the XR thread. Call after the frame's SDL draw calls,
	// with the render target already reset to the window. `stereo` false shows eye[0] to both eyes.
	void EndFrame(bool stereo);

	void GetSnapshot(Snapshot& out);

	void SetScreenConfig(const ScreenConfig& config);

	// Place the screen straight ahead of the current head pose again.
	void Recenter();
} // namespace armsx::vr
