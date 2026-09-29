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

			// A car with its own headlight geometry moves and turns each beam with its lamp as
			// the car is crushed. Stored in remix-comp-proxy.ini ([Lights] HeadlightsFollowDamage).
			bool follow_damage = true;

			// lamp
			float colour[3] = { 1.0f, 0.93f, 0.80f };
			float brightness = 2.0f;        // radiance times emitter area, so the radius only softens shadows
			float emitter_radius = 0.012f;  // world units; a car is roughly 0.4 wide
			float volumetric_scale = 1.0f;

			// everyone else
			float other_brightness = 1.0f;  // scale on brightness for cars that are not the player's
			float other_range = 12.0f;      // world units from the camera beyond which other cars stay dark
			bool wasted_stay_lit = false;

			// civilian traffic (the drones), in 'all cars' mode; the game draws only those near the camera
			float civilian_brightness = 0.35f;  // scale on brightness
			float civilian_range = 20.0f;       // world units from the camera

			bool operator==(const settings&) const = default;
		};

		// F: off -> player -> all cars -> off
		void cycle_mode();

		// Once per race-view submit, with the camera's position in world space.
		void on_race_frame(const float camera_pos[3]);

		// A frame that is not a race frame shows no headlights.
		void on_frame_without_race();

		// Whether `actor` is part of a car whose headlights were on at the last race frame.
		bool lights_actor(const game::br_actor* actor) const;

		// Where a lamp sits on `car` by the beam placement: the front corners (side 0 left,
		// 1 right) facing forward, or with `rear` the matching points at the back facing
		// back. World space. False when the car cannot be measured.
		bool lamp_mount(const game::race_car& car, int side, bool rear, float position[3], float facing[3]);

		// A night race turns every car's lights on; the mode from before comes back when it
		// ends, unless the player changed it in between.
		void set_night(bool night);

		void draw_menu();

		// Shows a mode change in the game's own message box. Called from Present, where the
		// game is between frames and the heads-up table is not being walked.
		void post_mode_notice();

	private:
		struct car_bounds
		{
			float min[3];
			float max[3];
			const game::br_actor* model;
			uint32_t measured_frame;
		};

		// How far a side's lamp has moved and turned since its first sight, in the car's own
		// space: the rigid motion that best carries its lens vertices from then to now.
		struct lamp_damage
		{
			const car_bounds* bounds;   // the box the beam was placed against then
			float moved[3];             // of the lenses' centre
			float rotation[3][3];       // v' = R v, limited to MAX_LAMP_TURN
			float angle;                // radians, before the limit
		};

		// One side's lamp lenses as first drawn this race, undamaged, and the car's box then.
		// Once a lamp has one, its beam is always placed against it: `last` is the latest fit,
		// held through any frame whose lens cannot be matched.
		struct lamp_origin
		{
			std::vector<std::array<float, 3>> points;
			car_bounds bounds;
			lamp_damage last;
		};

		struct lamp
		{
			remixapi_LightHandle handle = nullptr;
			uint32_t incarnation = 0;   // part of the hash, bumped when its light is destroyed
			bool drawn_this_frame = false;
		};

		void load();
		void save();

		static bool remix_lights_available();
		const car_bounds* measure(const game::race_car& car);
		// Every race car's lamps at their first sight this race, whatever the mode: cars start
		// whole, and one crushed before its lights come on still has its undamaged shape.
		void remember_lamp_origins();

		// Null model or too few points: the car has no lamp geometry on that side.
		std::optional<lamp_damage> lamp_damage_on(const game::race_car& car, int side,
			const game::br_model* model, const std::vector<std::array<float, 3>>& points);
		void describe_lamp(const game::race_car& car, const car_bounds& bounds, int side, float brightness,
			const lamp_damage* damage);
		void destroy_all();

		static std::string ini_path();
		static uint64_t lamp_key(const void* car_spec, int side);

		settings m_settings{};
		settings m_saved{};
		mode m_mode = mode::off;
		bool m_night = false;
		bool m_night_switched = false;   // m_mode was set by set_night, not by the player
		mode m_mode_before_night = mode::off;

		std::unordered_map<const void*, car_bounds> m_bounds;
		std::unordered_map<uint64_t, lamp> m_lamps;
		std::map<std::tuple<const void*, int, const void*>, lamp_origin> m_lamp_origins;   // car spec, side, model
		std::unordered_set<const game::br_actor*> m_lit_masters;   // master actors of the lit cars
		std::vector<game::race_car> m_cars;

		uint32_t m_frame = 0;
		uint32_t m_lit_cars = 0;
		std::string m_player_status;
		bool m_create_failed = false;

		bool m_notice_pending = false;
	};
}
