#include "std_include.hpp"
#include "free_camera.hpp"

namespace comp
{
	namespace
	{
		constexpr float SPEED = 3.0f;              // world units per second (a unit is about 6.9 m)
		constexpr float FAST = 4.0f;               // Shift
		constexpr float SLOW = 0.25f;              // Ctrl
		constexpr float TURN_PER_PIXEL = 0.004f;   // radians
		constexpr float PITCH_LIMIT = 1.55f;       // just short of straight up or down
		constexpr float MAX_STEP_SECONDS = 0.1f;   // a hitch must not throw the camera across the level
		constexpr int NOTICE_MS = 1500;

		bool key_down(const int vk)
		{
			return (GetAsyncKeyState(vk) & 0x8000) != 0;
		}
	}

	void free_camera::toggle()
	{
		m_active = !m_active;
		m_placed = false;
		m_turning = false;
		m_notice_pending = true;
		shared::common::log("FreeCamera", m_active ? "free camera on (F3)" : "free camera off (F3)");
	}

	void free_camera::post_notice()
	{
		if (!m_notice_pending) {
			return;
		}
		m_notice_pending = false;
		game::show_headup_message(m_active ? "Free camera on" : "Free camera off", NOTICE_MS);
	}

	/*
	 * BRender's view space looks down -Z with +Y up and +X right, and its vectors are rows:
	 * p' = p * M. The camera's axes are the rows of camera-to-world, so world-to-view holds
	 * them as columns, with the position folded into the translation row.
	 */
	void free_camera::rebuild_view()
	{
		const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
		const float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
		const float forward[3] = { sy * cp, sp, -cy * cp };
		const float right[3] = { cy, 0.0f, sy };
		const float up[3] = {
			right[1] * forward[2] - right[2] * forward[1],
			right[2] * forward[0] - right[0] * forward[2],
			right[0] * forward[1] - right[1] * forward[0],
		};
		const float back[3] = { -forward[0], -forward[1], -forward[2] };
		const float* axes[3] = { right, up, back };

		for (int axis = 0; axis < 3; ++axis)
		{
			for (int i = 0; i < 3; ++i) {
				m_world_to_view.m[i][axis] = axes[axis][i];
			}
			m_world_to_view.m[3][axis] = -(m_position[0] * axes[axis][0] + m_position[1] * axes[axis][1]
				+ m_position[2] * axes[axis][2]);
		}
	}

	void free_camera::update(const game::br_matrix34& game_camera_to_world)
	{
		LARGE_INTEGER now{}, frequency{};
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);
		const float seconds = m_last_tick.QuadPart
			? std::min(MAX_STEP_SECONDS, static_cast<float>(now.QuadPart - m_last_tick.QuadPart) / static_cast<float>(frequency.QuadPart))
			: 0.0f;
		m_last_tick = now;

		if (!m_active) {
			return;
		}

		if (!m_placed)
		{
			// Row 2 of camera-to-world is the camera's back.
			const auto& m = game_camera_to_world.m;
			std::memcpy(m_position, m[3], sizeof(m_position));
			const float forward[3] = { -m[2][0], -m[2][1], -m[2][2] };
			m_yaw = std::atan2(forward[0], -forward[2]);
			m_pitch = std::asin(std::clamp(forward[1], -1.0f, 1.0f));
			m_placed = true;
		}

		const bool focused = GetForegroundWindow() == shared::globals::main_window
			&& !shared::globals::imgui_menu_open;
		if (focused)
		{
			// Turning: the mouse while the right button is held.
			POINT cursor{};
			GetCursorPos(&cursor);
			if (key_down(VK_RBUTTON))
			{
				if (m_turning)
				{
					m_yaw += static_cast<float>(cursor.x - m_last_cursor.x) * TURN_PER_PIXEL;
					m_pitch = std::clamp(m_pitch - static_cast<float>(cursor.y - m_last_cursor.y) * TURN_PER_PIXEL,
						-PITCH_LIMIT, PITCH_LIMIT);
				}
				m_turning = true;
			}
			else {
				m_turning = false;
			}
			m_last_cursor = cursor;

			// Moving: along the view for W/S, across it for A/D, straight up and down for Q/E.
			float speed = SPEED * seconds;
			if (key_down(VK_SHIFT)) { speed *= FAST; }
			if (key_down(VK_CONTROL)) { speed *= SLOW; }

			const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
			const float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
			const float forward[3] = { sy * cp, sp, -cy * cp };
			const float right[3] = { cy, 0.0f, sy };
			const float along = (key_down('W') ? 1.0f : 0.0f) - (key_down('S') ? 1.0f : 0.0f);
			const float across = (key_down('D') ? 1.0f : 0.0f) - (key_down('A') ? 1.0f : 0.0f);
			const float rise = (key_down('E') ? 1.0f : 0.0f) - (key_down('Q') ? 1.0f : 0.0f);
			for (int i = 0; i < 3; ++i) {
				m_position[i] += (forward[i] * along + right[i] * across) * speed;
			}
			m_position[1] += rise * speed;
		}
		else {
			m_turning = false;
		}

		rebuild_view();
	}

	free_camera::free_camera()
	{
		p_this = this;
		shared::common::log("FreeCamera", "F3 toggles the free camera.", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}
}
