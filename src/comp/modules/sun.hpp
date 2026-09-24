#pragma once

namespace comp
{
	/*
	 * The game's sun, as a Remix distant light.
	 *
	 * Carmageddon 2 lights every track with one directional light that LoadInLight
	 * (0x0048F2E0) builds at start-up: rotated -60 degrees about X, then +30 about Y, and
	 * never moved again. That is a sun 60 degrees above the horizon, turned 30 degrees from
	 * +Z towards +X, the same on every track. Remix never sees it -- BRender lights on the
	 * CPU -- so without this the port is lit by the sky alone.
	 *
	 * The defaults reproduce the game's direction and its white colour. Brightness is
	 * Remix's distant-light radiance, which Remix scales so that 1.0 lights a white surface
	 * facing the sun to full brightness, the same as a legacy D3D9 directional light.
	 */
	class sun final : public shared::common::loader::component_module
	{
	public:
		sun();
		~sun() { p_this = nullptr; }

		static inline sun* p_this = nullptr;
		static sun* get() { return p_this; }

		struct settings
		{
			bool enabled = true;

			float elevation = 60.0f;        // degrees above the horizon
			float azimuth = 30.0f;          // degrees from +Z towards +X
			float colour[3] = { 1.0f, 1.0f, 1.0f };
			float brightness = 1.0f;
			float angular_diameter = 0.5f;  // degrees; widens the penumbra of every shadow
			float volumetric_scale = 1.0f;

			bool operator==(const settings&) const = default;
		};

		// Once per race-view submit, ahead of its draws.
		void on_race_frame();

		// A frame that is not a race frame has no sun.
		void on_frame_without_race();

		void draw_menu();

	private:
		void load();
		void save();
		void destroy();

		static std::string ini_path();

		settings m_settings{};
		settings m_saved{};

		remixapi_LightHandle m_handle = nullptr;
		settings m_described{};         // what m_handle was created from
		bool m_create_failed = false;
	};
}
