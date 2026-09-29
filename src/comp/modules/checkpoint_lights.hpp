#pragma once

namespace comp
{
	/*
	 * A faint Remix light under each checkpoint arch, optionally green over the one to cross next.
	 *
	 * The arches are faces of the track's own ground models (findings 75), so there is no
	 * actor to hang a light on. The capture hands over the world-space vertices of every
	 * face drawn with an arch material; each race gate (gCurrent_race's checkpoint quads)
	 * then takes the arch vertices standing in its plane. Their lowest and highest points
	 * are the ground and the top beam, and their extent along the gate is the arch's width.
	 * The light hangs just under the beam, aimed at the road between the posts.
	 *
	 * The next checkpoint is gCheckpoint; the game enforces the order only in a
	 * single-player normal or checkpoint race, so that is also the only time one arch is
	 * singled out.
	 */
	class checkpoint_lights final : public shared::common::loader::component_module
	{
	public:
		checkpoint_lights();
		~checkpoint_lights() { p_this = nullptr; }

		static inline checkpoint_lights* p_this = nullptr;
		static checkpoint_lights* get() { return p_this; }

		struct settings
		{
			bool enabled = true;
			float colour[3] = { 1.0f, 0.85f, 0.65f };      // a warm, faint white
			float brightness = 0.35f;                      // radiance times emitter area
			bool green_next = false;                       // the next checkpoint in next_colour
			float next_colour[3] = { 0.2f, 1.0f, 0.35f };
			float next_brightness = 0.6f;
			float radius = 0.03f;                          // world units
			float cone_angle = 75.0f;                      // degrees, axis to edge
			float cone_softness = 0.5f;
			float height = 0.85f;                          // of the arch's height, up from the ground

			bool operator==(const settings&) const = default;
		};

		// Whether the capture should scan `model` for arch faces: once per model per track.
		bool wants_model(const game::br_model* model) const { return !m_scanned.contains(model); }

		// From the capture: the world-space vertices of `model`'s arch faces, possibly none.
		void note_model(const game::br_model* model, std::vector<std::array<float, 3>>&& points);

		// Whether a material draws checkpoint arches.
		static bool is_arch_material(const char* identifier);

		// Once per race-view submit, after the capture.
		void on_race_frame();

		// Leaving the race view. The arches found stay until the checkpoints change.
		void on_frame_without_race();

		void draw_menu();

	private:
		struct arch
		{
			float position[3];   // where the light hangs
			bool found = false;
			remixapi_LightHandle handle = nullptr;
			uint32_t incarnation = 0;
			bool next = false;   // described as the next checkpoint
		};

		bool track_changed();
		void locate_arches();
		int next_checkpoint() const;
		void describe(size_t index, arch& a, bool next);
		void put_out(arch& a);
		void destroy_all();
		void load();
		void save();
		static std::string ini_path();

		settings m_settings{};
		settings m_saved{};
		settings m_described{};

		std::unordered_set<const game::br_model*> m_scanned;
		std::vector<std::array<float, 3>> m_points;
		bool m_points_changed = false;
		std::vector<arch> m_arches;
		std::vector<float> m_signature;   // the checkpoint gates the arches were found for

		uint32_t m_generation = 0;
		uint32_t m_lit = 0;
		bool m_create_failed = false;
	};
}
