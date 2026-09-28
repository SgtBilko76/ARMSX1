#pragma once

// Quest Touch controllers -> PlayStation pad. Pure logic, no OpenXR, SDL or emulator types, so it
// builds and is tested on a desktop compiler (tests/quest_pad_mapper_test.cpp).
//
// The layout is ARMSX2's, so both emulators feel the same in the headset. The Touch pair has 11
// buttons and two sticks; a DualShock needs 16 buttons and two sticks. The missing ones live
// behind the left Menu button:
//
//   A / B / X / Y                  Cross / Circle / Square / Triangle
//   Grips / Triggers               L1 R1 / L2 R2
//   Left stick click               Start
//   Right stick click              Select
//   Sticks                         left / right analog; a hard push on the left stick is the D-pad
//   Menu tap                       open the ARMSX pause menu
//   Menu held + left stick click   L3
//   Menu held + right stick click  R3
//   Menu held + B                  left stick: analog + D-pad on hard push (default), or analog only
//   Menu held + right trigger      recenter the screen
//
// Menu is on the left controller under the left thumb, so every chord partner is on the right.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace armsx::vr
{
	enum PadButton : std::uint32_t
	{
		PAD_UP = 1u << 0,
		PAD_DOWN = 1u << 1,
		PAD_LEFT = 1u << 2,
		PAD_RIGHT = 1u << 3,
		PAD_CROSS = 1u << 4,
		PAD_CIRCLE = 1u << 5,
		PAD_SQUARE = 1u << 6,
		PAD_TRIANGLE = 1u << 7,
		PAD_L1 = 1u << 8,
		PAD_R1 = 1u << 9,
		PAD_L2 = 1u << 10,
		PAD_R2 = 1u << 11,
		PAD_L3 = 1u << 12,
		PAD_R3 = 1u << 13,
		PAD_START = 1u << 14,
		PAD_SELECT = 1u << 15,
	};

	struct ControllerInput
	{
		bool a = false, b = false, x = false, y = false, menu = false;
		bool left_stick_click = false, right_stick_click = false;
		float left_trigger = 0.0f, right_trigger = 0.0f;
		float left_grip = 0.0f, right_grip = 0.0f;
		// OpenXR convention: +x right, +y up.
		float left_x = 0.0f, left_y = 0.0f, right_x = 0.0f, right_y = 0.0f;
	};

	struct PadOutput
	{
		std::uint32_t buttons = 0; // PadButton bits
		// -1..1, +x right, +y up, after the deadzone. Zero on an axis the D-pad has taken over.
		float left_x = 0.0f, left_y = 0.0f, right_x = 0.0f, right_y = 0.0f;
	};

	struct MapperOutput
	{
		PadOutput pad;
		bool recenter = false;
		bool open_menu = false;
	};

	// PlayStation analog byte for a -1..1 stick axis: 0x00 left/up, 0x80 centre, 0xFF right/down.
	inline std::uint8_t AnalogByte(float value, bool invert)
	{
		const float v = std::clamp(invert ? -value : value, -1.0f, 1.0f);
		return static_cast<std::uint8_t>(std::lround((v + 1.0f) * 127.5f));
	}

	class QuestPadMapper
	{
	public:
		static constexpr float kStickDeadzone = 0.12f;
		static constexpr float kPressThreshold = 0.6f;
		static constexpr float kReleaseThreshold = 0.4f;
		// Left-stick D-pad: a HARD push, with hysteresis. Pressed past kStickDpadPress, released only
		// back under kStickDpadRelease, so a push hovering near the edge cannot chatter into a run of
		// presses that skips menu entries.
		static constexpr float kStickDpadPress = 0.75f;
		static constexpr float kStickDpadRelease = 0.55f;
		// A Menu TAP opens the pause menu. Decided on RELEASE: until then the press could still turn
		// into a chord, and a chord must not also open the menu. A long press does nothing.
		static constexpr double kMenuTapSeconds = 0.5;

		MapperOutput Update(const ControllerInput& in, double now)
		{
			MapperOutput out;

			if (in.menu && !m_menu_down)
			{
				m_menu_down = true;
				m_menu_chorded = false;
				m_menu_pressed_at = now;
			}
			else if (!in.menu && m_menu_down)
			{
				m_menu_down = false;
				if (!m_menu_chorded && now - m_menu_pressed_at < kMenuTapSeconds)
					out.open_menu = true;
			}
			const bool shift = m_menu_down;

			const bool rt_down = Hysteresis(m_rt_down, in.right_trigger);

			// A button pressed while Menu is held belongs to the chord until it is released, even if
			// Menu is let go first -- otherwise releasing Menu early would fire Circle/R2.
			if (Claim(m_b_claimed, in.b, m_prev_b, shift))
			{
				m_menu_chorded = true;
				m_dpad_mode = !m_dpad_mode;
			}
			if (Claim(m_rt_claimed, rt_down, m_prev_rt, shift))
			{
				m_menu_chorded = true;
				out.recenter = true;
			}
			// The stick clicks are Start and Select; the pad's own L3/R3 ride the Menu chord.
			if (Claim(m_l3_claimed, in.left_stick_click, m_prev_l3, shift))
				m_menu_chorded = true;
			if (Claim(m_r3_claimed, in.right_stick_click, m_prev_r3, shift))
				m_menu_chorded = true;

			std::uint32_t buttons = 0;
			const auto set = [&buttons](std::uint32_t bit, bool down) {
				if (down)
					buttons |= bit;
			};
			set(PAD_CROSS, in.a);
			set(PAD_CIRCLE, in.b && !m_b_claimed);
			set(PAD_SQUARE, in.x);
			set(PAD_TRIANGLE, in.y);
			set(PAD_L1, Hysteresis(m_lg_down, in.left_grip));
			set(PAD_R1, Hysteresis(m_rg_down, in.right_grip));
			set(PAD_L2, Hysteresis(m_lt_down, in.left_trigger));
			set(PAD_R2, rt_down && !m_rt_claimed);
			set(PAD_L3, m_l3_claimed);
			set(PAD_R3, m_r3_claimed);
			set(PAD_START, in.left_stick_click && !m_l3_claimed);
			set(PAD_SELECT, in.right_stick_click && !m_r3_claimed);

			PadOutput& pad = out.pad;
			pad.left_x = Deadzone(in.left_x);
			pad.left_y = Deadzone(in.left_y);
			pad.right_x = Deadzone(in.right_x);
			pad.right_y = Deadzone(in.right_y);

			// Left stick: analog, and by default a hard push is the D-pad INSTEAD on that axis. Most
			// PS1 games only read the D-pad, so the stick has to reach it -- but menus that read both
			// would move two entries per push if the stick sent both at once.
			if (m_dpad_mode)
			{
				m_left_dpad_x = StickDpadAxis(m_left_dpad_x, in.left_x);
				m_left_dpad_y = StickDpadAxis(m_left_dpad_y, in.left_y);
				if (m_left_dpad_x != 0)
				{
					set(m_left_dpad_x > 0 ? PAD_RIGHT : PAD_LEFT, true);
					pad.left_x = 0.0f;
				}
				if (m_left_dpad_y != 0)
				{
					set(m_left_dpad_y > 0 ? PAD_UP : PAD_DOWN, true);
					pad.left_y = 0.0f;
				}
			}
			else
			{
				m_left_dpad_x = 0;
				m_left_dpad_y = 0;
			}

			pad.buttons = buttons;
			return out;
		}

		// Forget held buttons (focus lost, menu opened) but keep the user's D-pad mode choice.
		void ResetHeld()
		{
			const bool dpad_mode = m_dpad_mode;
			*this = QuestPadMapper();
			m_dpad_mode = dpad_mode;
		}

		bool DpadMode() const { return m_dpad_mode; }

	private:
		static float Deadzone(float v)
		{
			const float m = std::fabs(v);
			if (m <= kStickDeadzone)
				return 0.0f;
			return std::copysign(std::min(1.0f, (m - kStickDeadzone) / (1.0f - kStickDeadzone)), v);
		}

		static bool Hysteresis(bool& state, float value)
		{
			state = state ? (value > kReleaseThreshold) : (value > kPressThreshold);
			return state;
		}

		// Returns true on the press that starts a claim.
		static bool Claim(bool& claimed, bool pressed, bool& prev, bool shift)
		{
			const bool edge = pressed && !prev;
			prev = pressed;
			if (!pressed)
				claimed = false;
			else if (edge && shift)
			{
				claimed = true;
				return true;
			}
			return false;
		}

		// One axis of the left-stick D-pad: -1, 0 or +1, with hysteresis around the current state.
		static int StickDpadAxis(int state, float value)
		{
			if (state > 0)
				return (value > kStickDpadRelease) ? 1 : (value < -kStickDpadPress ? -1 : 0);
			if (state < 0)
				return (value < -kStickDpadRelease) ? -1 : (value > kStickDpadPress ? 1 : 0);
			return (value > kStickDpadPress) ? 1 : (value < -kStickDpadPress ? -1 : 0);
		}

		bool m_dpad_mode = true;
		int m_left_dpad_x = 0, m_left_dpad_y = 0;
		bool m_menu_down = false;
		bool m_menu_chorded = false;
		double m_menu_pressed_at = 0.0;

		bool m_rt_down = false, m_lt_down = false, m_rg_down = false, m_lg_down = false;
		bool m_prev_b = false, m_prev_rt = false, m_prev_l3 = false, m_prev_r3 = false;
		bool m_b_claimed = false, m_rt_claimed = false, m_l3_claimed = false, m_r3_claimed = false;
	};
} // namespace armsx::vr
