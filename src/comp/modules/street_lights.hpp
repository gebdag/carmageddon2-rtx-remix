#pragma once

namespace comp
{
	/*
	 * A Remix light under the head of every street lamp, for the night races.
	 *
	 * The lamps are ordinary scenery actors: the city places 86 copies of one lamp model
	 * (&02lamp.act, a post 1.42 units tall with an arm reaching 0.71 units over the street).
	 * The capture reports every lamp it draws with its model-to-world matrix, and each gets
	 * a sphere light just below its head, shaped into a downward cone, drawn only while the
	 * race is a night race (time_of_day.hpp).
	 */
	class street_lights final : public shared::common::loader::component_module
	{
	public:
		street_lights();
		~street_lights() { p_this = nullptr; }

		static inline street_lights* p_this = nullptr;
		static street_lights* get() { return p_this; }

		struct settings
		{
			bool enabled = true;
			float colour[3] = { 1.0f, 0.78f, 0.52f };   // sodium-warm
			float brightness = 1.0f;                    // radiance times emitter area, as the headlights
			float emitter_radius = 0.03f;               // world units; the lamp head is ~0.11 wide
			float cone_angle = 70.0f;                   // degrees, axis to edge
			float cone_softness = 0.4f;

			bool operator==(const settings&) const = default;
		};

		// From the capture: a model drawn in the race view, with its placement.
		void on_model_drawn(const game::br_actor* actor, const game::br_model* model,
			const game::br_matrix34& model_to_world);

		// Once per race-view submit, after the capture: lights every lamp drawn this scene
		// when `night`, and none otherwise.
		void on_race_frame(bool night);

		// Leaving the race view: lamp actors are recycled by the next track.
		void on_frame_without_race();

		void draw_menu();

		// A lamp model and where its light sits, in model space.
		struct lamp_kind
		{
			const char* model;
			float head[3];
			float direction[3];   // cone axis
		};

	private:
		struct lamp
		{
			remixapi_LightHandle handle = nullptr;
			float position[3] = {};
			float direction[3] = {};
			bool drawn = false;
			bool placed = true;   // new or moved: describe again
		};

		const lamp_kind* kind_of(const game::br_model* model);
		void describe(const game::br_actor* actor, lamp& l);
		void destroy_all();
		void load();
		void save();
		static std::string ini_path();

		settings m_settings{};
		settings m_saved{};
		settings m_described{};

		std::unordered_map<const game::br_model*, const lamp_kind*> m_kinds;
		std::unordered_map<const game::br_actor*, lamp> m_lamps;

		// Part of every light's hash, bumped whenever the lamps are dropped. Remix Plus
		// applies a destroy one scene frame late, so a light re-created under its old hash
		// before then would be erased; a new generation never shares a hash with the old.
		uint32_t m_generation = 0;
		uint32_t m_lit = 0;
		bool m_create_failed = false;
	};
}
