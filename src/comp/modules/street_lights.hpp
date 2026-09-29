#pragma once

namespace comp
{
	/*
	 * Remix lights on the city's street furniture, for the night races.
	 *
	 * The lamps and signals are ordinary scenery actors: the city places 86 copies of one
	 * street lamp model (&02lamp.act, a post 1.42 units tall with an arm reaching 0.71 units
	 * over the street) and 49 of one traffic light (&03traffic.act, a signal head at the end
	 * of a 0.79 unit arm). The capture reports every one it draws with its model-to-world
	 * matrix. A street lamp gets a sphere light just below its head, shaped into a downward
	 * cone. A traffic light gets a smaller, fainter one under its signal head, cycling green,
	 * amber, red as US signals do. Where each sits is a model-space setting, so it follows
	 * every copy. All are lit only while the race is a night race (time_of_day.hpp).
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
			float position[3] = { 0.0f, 1.33f, 0.60f }; // model space: just under the lamp head

			// Traffic lights glow rather than light the street: fainter, smaller, and pooling
			// on the road under the signal head.
			bool signals = true;
			float signal_brightness = 0.25f;
			float signal_radius = 0.01f;                // a lens is ~0.025 across
			float signal_cone_angle = 40.0f;
			float signal_cone_softness = 0.5f;
			float signal_position[3] = { 0.0f, 1.0f, 0.755f };   // model space: just under the head

			// A small light in front of the lit lens itself, so the lens lights the head around it.
			float signal_lens_brightness = 0.05f;
			float signal_lens_radius = 0.008f;
			float green_seconds = 8.0f;
			float amber_seconds = 3.0f;

			bool operator==(const settings&) const = default;
		};

		// From the capture: a model drawn in the race view, with its placement.
		void on_model_drawn(const game::br_actor* actor, const game::br_model* model,
			const game::br_matrix34& model_to_world);

		// Once per race-view submit, after the capture: lights every lamp and signal drawn
		// this scene when `night`, and none otherwise.
		void on_race_frame(bool night);

		// Leaving the race view: lamp actors are recycled by the next track.
		void on_frame_without_race();

		void draw_menu();

		// What the lens of the signal drawn by `actor` shows now: 1 red, 2 amber, 3 green, or
		// 0 when the actor is not a traffic light. Signals cycle day and night; only their
		// Remix lights are kept for the night races.
		int signal_lens(const game::br_actor* actor) const;

		enum class lamp_style : uint8_t
		{
			street,
			signal,
		};

		// A lamp model and which way its light shines, in model space.
		struct lamp_kind
		{
			const char* model;
			lamp_style style;
			float direction[3];   // cone axis
		};

	private:
		// What a lamp shows. Street lamps are always `lit`.
		enum class aspect : uint8_t
		{
			lit,
			red,
			amber,
			green,
		};

		struct lamp
		{
			const lamp_kind* kind = nullptr;
			remixapi_LightHandle handle = nullptr;
			remixapi_LightHandle lens_handle = nullptr;   // signals: the light at the lit lens
			uint32_t lens_incarnation = 0;                // as incarnation, for the lens light
			game::br_matrix34 model_to_world{};
			uint32_t incarnation = 0;   // part of the hash, bumped when its light is destroyed
			aspect shown = aspect::lit;
			bool drawn = false;
			bool placed = true;   // new or moved: describe again
		};

		const lamp_kind* kind_of(const game::br_model* model);
		aspect aspect_of(const lamp& l, double seconds) const;
		void describe(const game::br_actor* actor, lamp& l, aspect shown);
		void describe_lens(const game::br_actor* actor, lamp& l, aspect shown);
		bool create_light(uint64_t hash, const float position[3], const float direction[3], float radius,
			const float colour[3], float brightness, float cone_angle, float cone_softness,
			remixapi_LightHandle& handle);
		void put_out(lamp& l);
		void put_out_lens(lamp& l);
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
		uint32_t m_lit_lamps = 0;
		uint32_t m_lit_signals = 0;
		bool m_create_failed = false;
	};
}
