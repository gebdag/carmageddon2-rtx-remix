#pragma once

namespace comp
{
	/*
	 * A free camera for the path-traced view (F3), in place of RTX Remix's own.
	 *
	 * Remix's free camera moves only the ray-traced view: the sky the proxy rasterizes is
	 * drawn with the game's camera, so it stays pinned to the screen while the world turns.
	 * This one replaces the view the proxy hands Remix and the one the sky dome is drawn
	 * with, so world and sky move together. The game's own camera is untouched: the proxy
	 * still recovers every model's world placement from it.
	 *
	 * W/A/S/D move, Q/E go down and up, Shift is faster, Ctrl slower; holding the right mouse
	 * button turns the view with the mouse. The game still gets its keys, so pause a replay
	 * to look around.
	 */
	class free_camera final : public shared::common::loader::component_module
	{
	public:
		free_camera();
		~free_camera() { p_this = nullptr; }

		static inline free_camera* p_this = nullptr;
		static free_camera* get() { return p_this; }

		void toggle();

		bool active() const { return m_active; }

		// Once per race-view submit, with the game camera's camera-to-world matrix: starts the
		// free camera where the game camera is, then moves it by this frame's input.
		void update(const game::br_matrix34& game_camera_to_world);

		// The free camera's world-to-view and its position; valid while active().
		const game::br_matrix34& world_to_view() const { return m_world_to_view; }
		const float* position() const { return m_position; }

		// Shows a toggle in the game's message box. Called from Present.
		void post_notice();

	private:
		void rebuild_view();

		bool m_active = false;
		bool m_placed = false;          // taken over the game camera's pose since switching on
		bool m_notice_pending = false;
		float m_position[3] = {};
		float m_yaw = 0.0f;             // radians; 0 looks down world -Z
		float m_pitch = 0.0f;
		POINT m_last_cursor{};
		bool m_turning = false;
		LARGE_INTEGER m_last_tick{};
		game::br_matrix34 m_world_to_view{};
	};
}
