#pragma once

namespace comp
{
	/*
	 * Two Remix spot lights on the nose of each car.
	 *
	 * The port is lit by the sky and the sun, neither of which reaches into a tunnel or an
	 * indoor section, so those path-trace to black. Headlights are the light a driver would
	 * expect to have there. They exist only on the Remix side: sphere lights with
	 * cone shaping, created through the Remix API and re-described every race frame from
	 * each car's master actor, which is the one matrix the game keeps car-to-world.
	 *
	 * Cars differ too much in size for one fixed mounting point, so a lamp is placed
	 * relative to the box around everything the car's actor tree draws: a fraction of the
	 * half width out from the centre line, a fraction of the height up, and just clear of
	 * the front face so the body does not shadow its own lamp. Just clear and no more: the
	 * game keeps the car out of the scenery, and nothing keeps a lamp out of it, so a lamp
	 * held well ahead of the bumper is buried by the first slope the nose meets.
	 */
	class headlights final : public shared::common::loader::component_module
	{
	public:
		headlights();
		~headlights() { p_this = nullptr; }

		static inline headlights* p_this = nullptr;
		static headlights* get() { return p_this; }

		enum class mode : int
		{
			off = 0,
			player = 1,
			all_cars = 2,
		};

		struct settings
		{
			mode start_mode = mode::off;

			// placement, relative to the car's drawn bounds
			float spacing = 0.68f;          // 0..1 of the half width, out from the centre line
			float height = 0.55f;           // 0..1 of the car's height, up from its wheels
			float forward = 0.01f;          // world units ahead of the front face

			// beam
			float pitch_down = 4.0f;        // degrees
			float toe_out = 1.5f;           // degrees each lamp turns away from the centre line
			float cone_angle = 28.9f;       // degrees, axis to edge
			float cone_softness = 0.30f;
			float focus = 0.0f;

			// lamp
			float colour[3] = { 1.0f, 0.93f, 0.80f };
			float brightness = 10.06f;      // radiance times emitter area, so the radius only softens shadows
			float emitter_radius = 0.012f;  // world units; a car is roughly 0.4 wide
			float volumetric_scale = 1.0f;

			// everyone else
			float other_brightness = 1.0f;  // scale on brightness for cars that are not the player's
			float other_range = 12.0f;      // world units from the camera beyond which other cars stay dark
			bool wasted_stay_lit = false;

			bool operator==(const settings&) const = default;
		};

		// H: off -> player -> all cars -> off
		void cycle_mode();

		// Once per race-view submit, with the camera's position in world space.
		void on_race_frame(const float camera_pos[3]);

		// A frame that is not a race frame shows no headlights.
		void on_frame_without_race();

		void draw_menu();
		void draw_mode_notice();

	private:
		struct car_bounds
		{
			float min[3];
			float max[3];
			const game::br_actor* model;
			uint32_t measured_frame;
		};

		struct lamp
		{
			remixapi_LightHandle handle = nullptr;
			bool drawn_this_frame = false;
		};

		void load();
		void save();

		static bool remix_lights_available();
		const car_bounds* measure(const game::race_car& car);
		void describe_lamp(const game::race_car& car, const car_bounds& bounds, int side, float brightness);
		void destroy_all();

		static std::string ini_path();
		static uint64_t lamp_key(const void* car_spec, int side);

		settings m_settings{};
		settings m_saved{};
		mode m_mode = mode::off;

		std::unordered_map<const void*, car_bounds> m_bounds;
		std::unordered_map<uint64_t, lamp> m_lamps;
		std::vector<game::race_car> m_cars;

		uint32_t m_frame = 0;
		uint32_t m_lit_cars = 0;
		std::string m_player_status;
		bool m_create_failed = false;

		std::chrono::steady_clock::time_point m_notice_until{};
	};
}
