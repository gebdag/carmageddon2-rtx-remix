#pragma once

namespace comp
{
	/*
	 * Small Remix lights on car lamps: the headlight lenses while a car's headlights are on,
	 * the brake lenses in red while it brakes, the reverse lenses in white while it reverses.
	 * They sit just off the lens, so a glowing lens also lights the body around it. A headlight
	 * lens gets an unshaped sphere: its beam is the headlights' job, and what the lens light
	 * adds is the glow on the bodywork next to the lamp. Brake and reverse lights are shaped,
	 * since nothing else lights behind the car. They sit at the lens and face straight back
	 * from the car, level with the ground, and their lift moves them out along that line:
	 * a rear lens wraps round the body's corners, and its own normal would lift the light
	 * into the neighbouring panels.
	 *
	 * The lamps come from each car's own lens geometry: the capture reports every run drawn
	 * with a headlight texture ([Lights] HeadlightTextures) or with a material the game
	 * animates from the car's brake or reverse bit (a texturebits funk), one spot per lamp.
	 * Cars whose lamps are painted into the body have no such runs; they get lens lights
	 * where the headlight beams sit, and brake and reverse lights at the matching points at
	 * the back.
	 *
	 * Brake and reverse follow the car's light bits (tCar_spec + 0x18CC), the bits the game's
	 * own brake-light textures follow, so the light and the lens change together.
	 */
	class lens_lights final : public shared::common::loader::component_module
	{
	public:
		lens_lights();
		~lens_lights() { p_this = nullptr; }

		static inline lens_lights* p_this = nullptr;
		static lens_lights* get() { return p_this; }

		enum class role : uint8_t
		{
			head,
			brake,
			reverse,
		};

		struct settings
		{
			bool enabled = true;
			float head_colour[3] = { 1.0f, 0.93f, 0.80f };
			float head_brightness = 0.0001f;     // radiance times emitter area, as the headlights
			float head_radius = 0.02f;           // the headlight lens glow is wider than a brake lamp's
			float head_lift = 0.04f;
			float brake_colour[3] = { 1.0f, 0.05f, 0.02f };
			float brake_brightness = 0.005f;
			float reverse_colour[3] = { 1.0f, 1.0f, 1.0f };
			float reverse_brightness = 0.005f;
			float radius = 0.008f;               // brake and reverse; world units, a lens is a few hundredths across
			float lift = 0.04f;                  // brake and reverse: out of the lens along its facing, clear of the body
			float cone_angle = 80.0f;            // degrees, axis to edge; brake and reverse only
			float cone_softness = 0.5f;
			float range = 12.0f;                 // world units from the camera; the player's car always
			float civilian_scale = 0.5f;         // on the headlight lens brightness, for civilian traffic

			bool operator==(const settings&) const = default;
		};

		// From the capture: one lamp of a race car drawn this scene, in world space.
		//
		// Args:
		//   master: the car's master actor, which identifies the car.
		//   id: the lamp, stable from frame to frame for as long as the car's model is.
		//   intact: false for a smashed lamp. It stays dark, and it still stands for the car's
		//     own lamp of that role, so the painted-lamp fallback does not light it instead.
		//   points: a head lamp's lens vertices, world space, in the model's source order.
		void note(const game::br_actor* master, const game::br_model* model, role r, uint64_t id,
			const float position[3], const float facing[3], bool intact,
			std::vector<std::array<float, 3>> points = {});

		// The car's own head lamps drawn this scene on `side` of its centre line (0 left,
		// 1 right, as the beams are placed). Cars whose lamps are painted into the body have
		// none. All of them smashed puts that side's beam out.
		//
		// `points` are the lens vertices of the side's lamps on one model (the first drawn
		// with any there), in the car's own space and a fixed order. A crushed car's geometry
		// is rebuilt, so they move with the damage.
		struct head_lamp_side
		{
			int total = 0;
			int intact = 0;
			const game::br_model* model = nullptr;
			std::vector<std::array<float, 3>> points;
		};
		head_lamp_side head_lamps(const game::br_actor* master, int side) const;

		// At the start of every race scene, before the capture reports this scene's lamps.
		void begin_scene() { m_spots.clear(); }

		// Once per race-view submit, after the capture and after the headlights, whose lit
		// cars decide which headlight lenses glow.
		void on_race_frame(const float camera_pos[3]);

		void on_frame_without_race();

		void draw_menu();

	private:
		struct spot
		{
			const game::br_actor* master;
			const game::br_model* model;
			role r;
			uint64_t id;
			float position[3];
			float facing[3];
			bool intact;
			std::vector<std::array<float, 3>> points;
		};

		struct lamp
		{
			remixapi_LightHandle handle = nullptr;
			uint32_t incarnation = 0;   // part of the hash, bumped when its light is destroyed
			bool drawn = false;
		};

		void light(uint64_t key, role r, float scale, const float position[3], const float facing[3]);
		void destroy_all();
		void load();
		void save();

		settings m_settings{};
		settings m_saved{};
		std::vector<spot> m_spots;
		std::vector<game::race_car> m_cars;
		std::unordered_map<uint64_t, lamp> m_lamps;
		uint32_t m_lit = 0;
		bool m_create_failed = false;
	};
}
