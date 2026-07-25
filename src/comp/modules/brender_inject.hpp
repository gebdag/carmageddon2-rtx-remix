#pragma once

namespace comp
{
	/*
	 * Forwards Carmageddon 2's geometry to RTX Remix in model space.
	 *
	 * BRender transforms and lights on the CPU, so everything that reaches the device
	 * driver (Glide via nGlide, or the built-in Direct3D IM driver) is already in screen
	 * space. Remix rejects those draws outright -- "using pre-transformed vertices which
	 * isn't currently supported" -- and never finds a camera.
	 *
	 * This module taps the renderer one level higher. BrZbModelRender still holds the
	 * prepared, model-space vertex groups, and the renderer's accumulated model_to_view
	 * matrix is available from its dispatch table. Geometry is uploaded once per model into
	 * static buffers and replayed onto the D3D9 device that nGlide created (which is the
	 * Remix bridge), with WORLD / VIEW / PROJECTION supplied separately via SetTransform.
	 *
	 * Scene boundaries come from the incremental API (Begin/Add/End), which is what the race
	 * uses. The all-in-one BrZbSceneRender only draws HUD and menu overlays -- flat z=0 glyph
	 * models with a camera at the origin -- so those scenes are suppressed rather than fed to
	 * Remix as world geometry.
	 */
	class brender_inject final : public shared::common::loader::component_module
	{
	public:
		brender_inject();
		~brender_inject();

		static inline brender_inject* p_this = nullptr;
		static brender_inject* get() { return p_this; }

		void begin_scene(game::br_actor* camera);
		void capture_camera();
		void capture_model(game::br_model* model, game::br_material* fallback_material);
		void end_scene();

		// Called from the BrModelUpdate detour while the authored face array is still alive.
		void learn_materials(game::br_model* model);

		// BrModelUpdate rebuilds the prepared block -- car damage deformation does this every
		// few frames -- so any geometry cached from the old contents must be dropped.
		void invalidate_geometry(game::br_model* model);

		void set_overlay_scene(const bool active) { m_overlay_scene = active; }

	private:
		struct ffp_vertex
		{
			float x, y, z;
			float nx, ny, nz;
			float u, v;
		};

		// One draw's worth of a model: the run of indices sharing a single material.
		struct geometry_part
		{
			game::br_material* material;
			uint32_t index_start;
			uint32_t triangle_count;
		};

		// A model's prepared geometry, uploaded once and reused. Static contents are what let
		// Remix keep the acceleration structure it builds instead of rebuilding every frame.
		struct model_geometry
		{
			IDirect3DVertexBuffer9* vertex_buffer;
			IDirect3DIndexBuffer9* index_buffer;
			std::vector<geometry_part> parts;
			uint32_t vertex_count;
			uint32_t last_used_scene;
		};

		struct queued_model
		{
			const model_geometry* geometry;
			D3DMATRIX world;
			const char* model_name;
		};

		struct texture_entry
		{
			IDirect3DTexture9* texture;
			bool has_alpha;
		};

		void submit(IDirect3DDevice9* dev);
		bool build_projection(D3DMATRIX& out) const;
		const model_geometry* geometry_for(IDirect3DDevice9* dev, game::br_model* model,
		                                   game::br_material* fallback_material);
		void release_geometry(model_geometry& geometry);
		void evict_stale_geometry();

		void note_untextured(const queued_model& queued, game::br_material* material);
		void install_bounds_test_hook();
		void ensure_white_texture(IDirect3DDevice9* dev);
		texture_entry texture_for(IDirect3DDevice9* dev, game::br_material* material);
		IDirect3DTexture9* upload_pixelmap(IDirect3DDevice9* dev, const game::br_pixelmap* pm);

		std::vector<queued_model> m_queue;
		std::unordered_map<game::br_model*, model_geometry> m_geometry;

		game::br_actor* m_camera = nullptr;
		game::br_matrix34 m_world_to_view{};
		game::br_matrix34 m_view_inverse{};
		bool m_camera_valid = false;

		// True while the all-in-one BrZbSceneRender is on the stack, i.e. a HUD/menu pass.
		bool m_overlay_scene = false;
		bool m_capturing = false;
		bool m_bounds_hook_attempted = false;

		IDirect3DTexture9* m_white_texture = nullptr;
		IDirect3DVertexDeclaration9* m_vertex_decl = nullptr;
		uint32_t m_textures_ok = 0;
		uint32_t m_textures_failed = 0;

		// Keyed on the colour_map rather than the material, so materials sharing a texture
		// share one upload and Remix sees one stable hash for them.
		std::unordered_map<const game::br_pixelmap*, texture_entry> m_textures;
		std::set<uint32_t> m_unsupported_types;

		// A prepared group only records br_material::stored, so this maps that token -- and
		// the material pointer itself, in case the group holds one -- back to the material.
		std::unordered_map<uint32_t, game::br_material*> m_materials;

		std::map<std::string, std::string> m_skipped_models;
		std::map<std::string, std::string> m_untextured_models;

		// A scene with fewer models than this is a 3D HUD widget, not the race view.
		static constexpr size_t MIN_WORLD_SCENE_MODELS = 8;

		// Geometry untouched for this many scenes is released.
		static constexpr uint32_t GEOMETRY_EVICT_AFTER_SCENES = 900;

		struct frame_stats
		{
			uint32_t draws;
			uint32_t vertices;
			uint32_t models;
			double submit_ms;
			double frame_ms;
		};

		void log_performance(const frame_stats& stats);

		uint32_t m_scenes_submitted = 0;
		int64_t m_last_scene_ticks = 0;
		double m_ticks_per_ms = 0.0;
		frame_stats m_worst{};
	};
}
