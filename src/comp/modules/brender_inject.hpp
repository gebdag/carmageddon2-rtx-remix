#pragma once

namespace comp
{
	/*
	 * Forwards Carmageddon 2's geometry to RTX Remix in model space.
	 *
	 * BRender transforms and lights on the CPU, so everything that reaches the device
	 * driver (Glide via nGlide, or the built-in Direct3D IM driver) is already in screen
	 * space. Remix rejects those draws outright — "using pre-transformed vertices which
	 * isn't currently supported" — and never finds a camera.
	 *
	 * This module taps the renderer one level higher. BrZbModelRender still holds the
	 * prepared, model-space vertex groups, and the renderer's accumulated model_to_view
	 * matrix is available from its dispatch table. Geometry is batched over the scene walk
	 * and replayed onto the D3D9 device that nGlide created (which is the Remix bridge),
	 * with WORLD / VIEW / PROJECTION supplied separately via SetTransform.
	 *
	 * Scene boundaries come from the incremental API (Begin/Add/End), which is what the race
	 * uses. The all-in-one BrZbSceneRender only ever draws HUD and menu overlays — flat
	 * z=0 glyph models with a camera at the origin — so those scenes are deliberately
	 * suppressed rather than fed to Remix as world geometry.
	 */
	class brender_inject final : public shared::common::loader::component_module
	{
	public:
		brender_inject();

		static inline brender_inject* p_this = nullptr;
		static brender_inject* get() { return p_this; }

		void begin_scene(game::br_actor* camera);
		void capture_camera();
		void capture_model(game::br_model* model, game::br_material* fallback_material);
		void end_scene();

		// Called from the BrModelUpdate detour while the authored face array is still alive.
		void learn_materials(game::br_model* model);

		void set_overlay_scene(const bool active) { m_overlay_scene = active; }

		bool m_enabled = true;
		uint32_t m_stat_draws = 0;
		uint32_t m_stat_vertices = 0;

	private:
		struct ffp_vertex
		{
			float x, y, z;
			float nx, ny, nz;
			float u, v;
		};

		struct batched_draw
		{
			D3DMATRIX world;
			const char* model_name;
			game::br_material* material;
			uint32_t vertex_offset;
			uint32_t vertex_count;
			uint32_t index_offset;
			uint32_t triangle_count;
		};

		void submit(IDirect3DDevice9* dev);
		bool build_projection(D3DMATRIX& out) const;
		struct texture_entry
		{
			IDirect3DTexture9* texture;
			bool has_alpha;
		};

		void note_untextured(const batched_draw& draw);
		void install_bounds_test_hook();
		void ensure_white_texture(IDirect3DDevice9* dev);
		texture_entry texture_for(IDirect3DDevice9* dev, game::br_material* material);
		IDirect3DTexture9* upload_pixelmap(IDirect3DDevice9* dev, const game::br_pixelmap* pm);

		bool m_bounds_hook_attempted = false;

		IDirect3DTexture9* m_white_texture = nullptr;
		IDirect3DVertexDeclaration9* m_vertex_decl = nullptr;
		uint32_t m_textures_ok = 0;
		uint32_t m_textures_failed = 0;

		// Keyed on the colour_map rather than the material, so materials sharing a texture
		// share one upload and Remix sees one stable hash for them.
		std::unordered_map<const game::br_pixelmap*, texture_entry> m_textures;
		std::set<uint32_t> m_unsupported_types;

		// A prepared group only records br_material::stored, so this maps that token — and
		// the material pointer itself, in case the group holds one — back to the material.
		std::unordered_map<uint32_t, game::br_material*> m_materials;

		// Models BrZbModelRender was called for that produced no draw.
		std::map<std::string, std::string> m_skipped_models;

		// Models that reached Remix without a texture, with the reason.
		std::map<std::string, std::string> m_untextured_models;

		std::vector<ffp_vertex> m_vertices;
		std::vector<uint16_t> m_indices;
		std::vector<batched_draw> m_draws;

		game::br_actor* m_camera = nullptr;
		game::br_matrix34 m_world_to_view{};
		game::br_matrix34 m_view_inverse{};
		bool m_camera_valid = false;

		// True while the all-in-one BrZbSceneRender is on the stack, i.e. a HUD/menu pass.
		bool m_overlay_scene = false;
		bool m_capturing = false;

		// A scene with fewer draws than this is a 3D HUD widget, not the race view.
		static constexpr size_t MIN_WORLD_SCENE_DRAWS = 24;

		uint32_t m_models_logged = 0;
		uint32_t m_scenes_logged = 0;
		uint32_t m_scenes_submitted = 0;
		bool m_warned_no_geometry = false;
		bool m_warned_no_device = false;
		bool m_warned_no_camera = false;
	};
}
