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
		// True when the model reached Remix, either as a queued draw, a merged static batch or
		// a line segment. The caller uses it to decide whether the game still needs to draw it.
		bool capture_model(game::br_model* model, game::br_material* fallback_material, uint32_t style);
		void end_scene();

		// Called from the BrModelUpdate detour while the authored face array is still alive.
		void learn_materials(game::br_model* model);

		// BrModelUpdate rebuilds the prepared block -- car damage deformation does this every
		// few frames -- so any geometry cached from the old contents must be dropped.
		void invalidate_geometry(game::br_model* model);

		void set_overlay_scene(const bool active) { m_overlay_scene = active; }

		/*
		 * Where a scene's time goes, split by who spends it.
		 *
		 * submit_ms on its own covered around a twentieth of the frame, so every earlier
		 * optimization pass was judged against a number that could not have moved. The hooks
		 * accumulate into this as they run and submit reports it.
		 */
		struct scene_profile
		{
			int64_t capture_ticks;      // our BrZbModelRender tap
			int64_t bounds_ticks;       // our addition to the renderer's bounds test
			int64_t game_render_ticks;  // the original BrZbModelRender: software T&L then Glide
			int64_t scene_end_ticks;    // BrZbSceneRenderEnd: bucket sort, rasterize, nGlide
			uint32_t model_updates;     // BrModelUpdate calls the scene made
			uint32_t rebuilds;          // models whose geometry was re-extracted and re-uploaded
			uint32_t glide_draws;       // draws the device took this frame before ours
		};

		scene_profile& profile() { return m_profile; }

		/*
		 * Time spent between one race scene closing and the next one opening.
		 *
		 * scene_profile is reset per scene, which is the wrong granularity for anything that
		 * happens outside the race scene -- the HUD and menu passes go through the all-in-one
		 * BrZbSceneRender, run BRender's software renderer in full and push their own draws
		 * across the bridge. This accumulates across scenes and is consumed by submit.
		 */
		void add_overlay_ticks(const int64_t ticks) { m_overlay_ticks += ticks; }

	private:
		struct ffp_vertex
		{
			float x, y, z;
			float nx, ny, nz;
			float u, v;
		};

		// One draw's worth of a model: the run of indices sharing a single material.
		// The texture is resolved when the geometry is built rather than per draw, so
		// submit never dereferences a br_material that the game may since have freed.
		struct geometry_part
		{
			IDirect3DTexture9* texture;
			uint32_t index_start;
			uint32_t triangle_count;

			// Translucent runs are drawn in a later pass and their vertices carry a lift off
			// whatever surface they overlay, so this belongs to the geometry as built.
			bool has_alpha;

			// Identity of the material this run came from. Only ever dereferenced from
			// refresh_part_state, which runs inside the game's own render call while the
			// material is still alive; submit works purely off the resolved state below.
			game::br_material* material;

			// Resolved afresh every time the model is captured -- the funkotronic system
			// animates br_material::map_transform while a race is running.
			bool texture_transform_active;
			D3DMATRIX texture_transform;
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

			// What the buffers were allocated to hold. A rebuild that needs the same sizes
			// refills them in place instead of trading them for an identical pair.
			uint32_t vertex_bytes;
			uint32_t index_bytes;

			// Which submission passes have anything to do for this model, so the blended
			// pass can skip the overwhelming majority of models outright.
			bool has_opaque;
			bool has_blended;

			// BrModelUpdate can fire mid-scene, after this geometry is already queued for
			// submission. Marking instead of erasing keeps queued pointers valid; the
			// rebuild happens the next time the model is captured.
			bool dirty;
		};

		// One BRender line segment in world space. BRender draws EDGES-style models by
		// walking face edges, and Carmageddon 2's spark model is a single face with two
		// coincident indices — a zero-area triangle, which a real rasterizer discards. The
		// segments are collected here and expanded into camera-facing quads at submit time.
		// The two ends carry different colours -- a spark runs yellow to red -- so both are
		// kept and the streak is shaded between them.
		struct line_segment
		{
			float a[3];
			float b[3];
			uint32_t rgb_a;
			uint32_t rgb_b;
		};

		struct queued_model
		{
			const model_geometry* geometry;
			D3DMATRIX world;
			const char* model_name;
		};

		// The level's static geometry, merged across models into one buffer per texture.
		// Track pieces are world-space children with an identity transform, so they can all
		// share a draw; that collapses thousands of per-model draws into a few dozen and
		// gives Remix an acceleration structure that never has to be rebuilt.
		struct static_batch
		{
			IDirect3DTexture9* texture;
			bool has_alpha;
			IDirect3DVertexBuffer9* vertex_buffer;
			IDirect3DIndexBuffer9* index_buffer;
			uint32_t vertex_count;
			uint32_t triangle_count;
		};

		void submit(IDirect3DDevice9* dev);

		enum class pass_kind
		{
			opaque,    // depth writes on, blending off
			blended,   // drawn after opaque, depth writes off, alpha tested
			combined,  // everything in scene-walk order, blending toggled per run
		};

		// Opaque geometry goes down first, then everything translucent with depth writes
		// off, so a decal's fully transparent texels can no longer occlude the road under
		// it. Returns the number of draws issued.
		uint32_t draw_pass(IDirect3DDevice9* dev, pass_kind kind);

		void capture_lines(const game::br_model* model, const game::br_matrix34& model_to_world);
		uint32_t submit_lines(IDirect3DDevice9* dev);
		void log_spark_geometry(const float camera[3]);

		// A soft streak shading from one end colour to the other, one texture per pair the
		// emitter uses. Kept apart from the flat-colour swatches so sparks carry their own
		// Remix hashes and can be tagged emissive without dragging every red-painted chunk
		// of debris along.
		IDirect3DTexture9* spark_texture(IDirect3DDevice9* dev, uint32_t rgb_a, uint32_t rgb_b);

		// Re-resolves the material state that the game animates: translucency and the UV
		// transform that picks a cell out of a texture atlas.
		void refresh_part_state(model_geometry& geometry) const;

		bool build_projection(D3DMATRIX& out) const;
		model_geometry* geometry_for(IDirect3DDevice9* dev, game::br_model* model,
		                             game::br_material* fallback_material);

		// Walks a model's prepared groups into CPU-side vertices and per-texture index runs.
		// Shared by the per-model buffers and the merged static batches.
		bool extract_geometry(IDirect3DDevice9* dev, game::br_model* model,
		                      game::br_material* fallback_material,
		                      std::vector<ffp_vertex>& vertices,
		                      std::vector<geometry_part>& parts,
		                      std::vector<uint32_t>& indices);

		bool bake_static_model(game::br_model* model, game::br_material* fallback_material,
		                       const game::br_matrix34& world);
		void forget_static_model(game::br_model* model);
		void rebuild_static_batches(IDirect3DDevice9* dev);
		void release_static_batches();
		void release_geometry(model_geometry& geometry);
		void evict_stale_geometry();

		void note_untextured(const game::br_model* model, const game::br_material* material);
		void note_unsupported_style(const game::br_model* model, uint32_t style);

		// Geometry that reached the capture but did not make it to Remix, and is therefore
		// only ever rasterized by the game. This is the injection's coverage gap.
		void note_not_injected(const game::br_model* model, const char* reason);
		void install_bounds_test_hook();
		void ensure_white_texture(IDirect3DDevice9* dev);
		IDirect3DTexture9* solid_colour_texture(IDirect3DDevice9* dev, uint32_t rgb);
		void note_flat_colour(const game::br_model* model, const game::br_material* material);
		IDirect3DTexture9* texture_for(IDirect3DDevice9* dev, const game::br_material* material);
		IDirect3DTexture9* upload_pixelmap(IDirect3DDevice9* dev, const game::br_pixelmap* pm);

		std::vector<queued_model> m_queue;
		std::vector<line_segment> m_lines;
		std::vector<ffp_vertex> m_line_vertices;
		std::unordered_map<game::br_model*, model_geometry> m_geometry;

		// One placement of a model that has held still long enough to be considered scenery.
		// The geometry is extracted and the transform baked in once, at promotion time, and
		// the game's own memory is never read again: models get freed between races, and a
		// rebuild must not depend on them still being alive.
		struct static_instance
		{
			game::br_model* model;   // identity only, never dereferenced
			std::vector<ffp_vertex> vertices;
			std::vector<geometry_part> parts;
			std::vector<uint32_t> indices;
		};

		struct placement_record
		{
			game::br_matrix34 world;
			uint32_t sightings;
			bool baked;
		};

		std::vector<static_batch> m_static_batches;
		std::unordered_map<uint64_t, static_instance> m_static_models;

		// Where each model was last seen, and the models that have since turned up somewhere
		// else. Anything that moves must never be baked -- it would leave a ghost behind.
		std::unordered_map<game::br_model*, placement_record> m_placements;
		std::unordered_set<game::br_model*> m_moving_models;
		bool m_static_dirty = false;
		bool m_static_urgent = false;

		// Frames a model must hold one placement before it counts as scenery. Cars fail on
		// their second frame and are never baked.
		static constexpr uint32_t STATIC_PROMOTE_SIGHTINGS = 3;
		uint32_t m_static_rebuilt_scene = 0;
		uint32_t m_scene_models = 0;

		// Rebuilding walks every static model, so discoveries are batched up rather than
		// triggering a rebuild each time a new corner of the track comes into view.
		static constexpr uint32_t STATIC_REBUILD_INTERVAL_SCENES = 120;

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

		// Built on the first submit and re-captured each scene thereafter.
		IDirect3DStateBlock9* m_saved_state = nullptr;
		uint32_t m_textures_ok = 0;
		uint32_t m_textures_failed = 0;

		// Keyed on the colour_map rather than the material, so materials sharing a texture
		// share one upload and Remix sees one stable hash for them.
		std::unordered_map<const game::br_pixelmap*, IDirect3DTexture9*> m_textures;
		std::set<uint32_t> m_unsupported_types;
		std::set<uint32_t> m_unsupported_styles;

		// 1x1 swatches for materials BRender colours flat instead of texturing.
		std::unordered_map<uint32_t, IDirect3DTexture9*> m_colour_textures;
		uint32_t m_flat_probes = 0;

		// Streak swatches, keyed on both end colours and never shared with the flat colours.
		std::unordered_map<uint64_t, IDirect3DTexture9*> m_spark_textures;
		bool m_logged_spark_geometry = false;

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
			uint32_t segments;
			uint32_t model_updates;
			uint32_t rebuilds;
			uint32_t glide_draws;
			double capture_ms;
			double bounds_ms;
			double game_render_ms;
			double scene_end_ms;
			double submit_ms;
			double present_ms;
			double overlay_ms;
			double frame_ms;
			uint32_t frame_draws;
		};

		void log_performance(const frame_stats& stats);

		uint32_t m_scenes_submitted = 0;
		int64_t m_last_scene_ticks = 0;
		double m_ticks_per_ms = 0.0;
		frame_stats m_worst{};
		scene_profile m_profile{};
		int64_t m_overlay_ticks = 0;
	};
}
