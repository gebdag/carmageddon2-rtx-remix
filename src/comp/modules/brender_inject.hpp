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

		void begin_scene(game::br_actor* world, game::br_actor* camera);
		void capture_camera();
		// True when the game's own render of this model is provably redundant: it is drawn
		// from a live sealed chunk, or it is a spark line the billboards replace. Dynamic
		// models return false even when injected -- not everything a model draws passes
		// through this hook (pedestrian limbs are drawn inside the ped's render call), so
		// their rasterized stream must keep running.
		bool capture_model(game::br_actor* actor, game::br_model* model,
		                   game::br_material* fallback_material, uint32_t style);
		void end_scene();

		// Called from the BrModelUpdate detour while the authored face array is still alive.
		void learn_materials(game::br_model* model);

		// BrModelUpdate rebuilds the prepared block -- car damage deformation does this every
		// few frames -- so any geometry cached from the old contents must be dropped, and any
		// actor baked with the old shape demoted back to the dynamic path.
		void invalidate_geometry(game::br_model* model);

		// Called from the BrMaterialUpdate detour. A material whose UV transform is animated
		// mid-race can never live in a sealed chunk; anything already baked with it demotes.
		void on_material_update(game::br_material* material, uint16_t flags);

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

		/*
		 * One run of the level's static geometry sharing a texture and blend mode.
		 *
		 * A chunk accumulates CPU-side while the level is being discovered, then seals:
		 * the data is uploaded once into buffers that are never modified again. Remix
		 * hashes a buffer when it first sees it and keeps the acceleration structure it
		 * builds for as long as the contents hold still, so sealed chunks cost it nothing
		 * per frame -- which is exactly what the old rebuild-everything batches broke every
		 * time a new corner of the track forced a rebuild.
		 */
		struct static_chunk
		{
			IDirect3DTexture9* texture;
			bool has_alpha;
			bool sealed;
			std::vector<ffp_vertex> vertices;   // emptied on seal
			std::vector<uint16_t> indices;      // emptied on seal
			IDirect3DVertexBuffer9* vertex_buffer;
			IDirect3DIndexBuffer9* index_buffer;
			uint32_t vertex_count;
			uint32_t triangle_count;
		};

		// Where one baked actor's indices ended up, so it can be punched back out (the
		// range overwritten with degenerate triangles) if the actor turns out to move.
		struct baked_range
		{
			uint32_t chunk;
			uint32_t index_start;
			uint32_t index_count;
		};

		/*
		 * One actor's placement history. Keyed by actor rather than model because scenery
		 * is instanced -- one lamppost model, dozens of actors -- and a model-keyed record
		 * reads the second instance as the first one moving, which is why the previous
		 * merge never captured instanced scenery at all.
		 */
		struct actor_record
		{
			game::br_model* model;
			uint64_t placement;         // fingerprint of the actor's transform chain
			uint64_t placement_chain;   // node-address part alone, for drift diagnosis
			uint32_t sightings;
			bool baked;   // copied into a chunk, possibly one still accumulating
			bool live;    // its chunks are sealed and drawing; the game render is redundant
			std::vector<baked_range> ranges;
			std::vector<game::br_material*> materials;  // compared only, never dereferenced
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

		bool bake_actor(actor_record& record, game::br_model* model,
		                game::br_material* fallback_material, const game::br_matrix34& world);
		void append_part_to_chunk(const geometry_part& part,
		                          const std::vector<ffp_vertex>& vertices,
		                          const std::vector<uint32_t>& indices, actor_record& record);
		void punch_out(const actor_record& record);
		void demote_actor(game::br_actor* actor, bool permanent);
		void seal_chunks(IDirect3DDevice9* dev);
		void reset_static_world(const char* reason);
		void release_chunks();
		void release_geometry(model_geometry& geometry);
		void evict_stale_geometry();

		void note_untextured(const game::br_model* model, const game::br_material* material);
		void note_unsupported_style(const game::br_model* model, uint32_t style);

		// Geometry that reached the capture but did not make it to Remix, and is therefore
		// only ever rasterized by the game. This is the injection's coverage gap.
		void note_not_injected(const game::br_model* model, const char* reason);
		// Why an actor failed the placement check -- the difference between a mover and a
		// fingerprint that cannot hold still. Logged once per model name.
		void note_placement_drift(const game::br_model* model, const char* reason);
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

std::vector<static_chunk> m_chunks;

		// Index of the chunk currently accepting geometry for a (texture, blend) pair.
		// Keyed on the texture pointer with the blend bit folded in.
		std::unordered_map<uint64_t, size_t> m_open_chunks;

		// Every actor the race scene has walked, and the ones that have proved they move.
		// Anything that moves must never be baked -- it would leave a ghost behind.
		std::unordered_map<game::br_actor*, actor_record> m_actors;
		std::unordered_set<game::br_actor*> m_moving_actors;

		// Materials the funkotronic system has animated mid-race. Pointers are compared,
		// never dereferenced; a stale entry after a level change only costs one model its
		// bake, and the set is cleared with the rest of the static world.
		std::unordered_set<game::br_material*> m_animated_materials;

		// Scene stamp of each material's last UV-transform update. Animation means updates
		// in two different scenes; a load burst is many updates under one stamp.
		std::unordered_map<game::br_material*, uint32_t> m_material_update_scene;

		// Frames an actor must hold one placement before it counts as scenery. Cars fail on
		// their second frame and are never baked.
		static constexpr uint32_t STATIC_PROMOTE_SIGHTINGS = 3;

		// Scenes without a new promotion before the accumulated chunks seal. Sealing early
		// means sealing often, and every seal hands Remix new buffers to hash.
		static constexpr uint32_t STATIC_SEAL_QUIET_SCENES = 30;

		// Baked actors are kept for the whole race even while unseen: the city streams
		// scenery by zone, so absence means hidden, not gone -- and keeping hidden zones
		// resident is the point of the chunks. Everything that can genuinely vanish or
		// move mid-race (powerups, noncars, decals, peds, cars) is excluded from baking
		// instead. Race changes are detected by population takeover below.

		// Fresh actor records created this scene, and live ones walked this scene. A race
		// change reuses the world actor, so the pointer comparison alone never catches
		// it; what it cannot hide is a whole scene of never-seen actors landing while
		// none of the previously live ones are walked. A zone flood re-walks the current
		// zone's live actors, so it never matches.
		uint32_t m_fresh_this_scene = 0;
		uint32_t m_live_seen_this_scene = 0;

		// Chunk indices are 16-bit.
		static constexpr uint32_t CHUNK_VERTEX_LIMIT = 0xFFFFu;

		uint32_t m_last_promotion_scene = 0;
		bool m_have_unsealed = false;
		uint32_t m_live_actors = 0;
		uint32_t m_demotions = 0;
		uint32_t m_scene_models = 0;

		// The camera whose scene last submitted, i.e. the race view. Models seen under any
		// other camera belong to 3D HUD widgets: they are never suppressed and never enter
		// the placement history.
		game::br_actor* m_race_camera = nullptr;

		// The world actor of the submitting scene. A different world means a different
		// race, and every baked pointer from the old one is garbage.
		game::br_actor* m_world = nullptr;
		game::br_actor* m_submitted_world = nullptr;

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
		std::map<std::string, std::string> m_drift_models;

		// A scene with fewer models than this is a 3D HUD widget, not the race view.
		static constexpr size_t MIN_WORLD_SCENE_MODELS = 8;

		// Geometry untouched for this many scenes is released.
		static constexpr uint32_t GEOMETRY_EVICT_AFTER_SCENES = 900;

		struct frame_stats
		{
			uint32_t draws;
			uint32_t vertices;
			uint32_t models;
			uint32_t baked;
			uint32_t chunks;
			uint32_t demotions;
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
