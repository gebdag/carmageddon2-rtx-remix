#pragma once
#include "sky_dome.hpp"

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

		void begin_scene(game::br_actor* world, game::br_actor* camera, game::br_pixelmap* colour);
		void capture_camera();
		// Captures one BrZbModelRender call of the race view. Returns true when the game's
		// own render of it is redundant and should be skipped: the model is drawn from a
		// live sealed chunk, it is a spark line the billboards replace, or it was injected
		// and the dynamics are suppressed. Models under any other camera -- the 3D HUD
		// widgets -- are never suppressed, since Remix never sees them.
		bool capture_model(game::br_actor* actor, game::br_model* model,
		                   game::br_material* fallback_material, uint32_t style);

		// Captures made this scene, so a caller can tell whether a custom render callback
		// rendered anything through the hook.
		uint32_t captures_this_scene() const { return m_captures; }
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

		// Called from the Frontend_Setup detour: the race is over, finished or abandoned.
		void on_frontend_entered();

		// Called from the BrModelUpdate detour. Only models rebuilt between leaving a race
		// and returning to one matter: they are what separates a track load from a pause.
		void note_model_rebuilt(game::br_model* model)
		{
			if (m_in_frontend) { m_frontend_models.insert(model); }
		}

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

			// BRender's authored vertex colour, and the surface colour outright for a
			// prelit material. White for everything else, so the texture stage's modulate
			// leaves lit geometry exactly as it was.
			uint32_t diffuse = 0xFFFFFFFFu;
		};

		/*
		 * One draw's worth of a model: the run of indices sharing a single material, as
		 * the model was when its geometry was built.
		 *
		 * The material state recorded here is what the static chunks bake: a chunk is
		 * copied once and never re-read. Dynamic draws do not trust it -- what a material
		 * looks like at the moment of a draw is resolved into a draw_state per capture.
		 */
		struct geometry_part
		{
			IDirect3DTexture9* texture;
			uint32_t index_start;
			uint32_t triangle_count;

			// Translucent runs are drawn in a later pass and their vertices carry a lift off
			// whatever surface they overlay, so this belongs to the geometry as built.
			bool has_alpha;
			uint8_t opacity;

			// Identity of the material this run came from. Only ever dereferenced from
			// resolve_draw_state, which runs inside the game's own render call while the
			// material is still alive; submit works purely off the resolved state.
			game::br_material* material;

			// The faces of this run carry no material of their own and take whatever
			// BrZbModelRender is handed, which is how the game re-skins one shared sprite
			// quad per particle: the material argument changes, the model does not.
			bool inherits_material;

			// The material asks for both sides to be drawn. Recorded here as well as in
			// the per-draw state because a sealed chunk draws under one cull mode.
			bool two_sided;
		};

		/*
		 * How a blended sprite gets its colour when Remix would otherwise light it.
		 *
		 * An additive blend (SRCALPHA, ONE) is emissive to Remix: the run glows with its
		 * texture x vertex colour x alpha and blocks nothing behind it. `unlit` pairs that
		 * with the ordinary alpha-blended draw, which darkens what is behind by the run's
		 * alpha; together they are BRender's unlit "colour over background" blend.
		 */
		enum class sprite_glow : uint8_t
		{
			lit,        // alpha blend only; Remix lights it
			additive,   // additive only
			unlit,      // alpha blend, then an additive copy
		};

		/*
		 * How one run looks in one particular draw.
		 *
		 * Everything here the game rewrites between draws of the same model without
		 * touching its geometry: the material handed to the render call, the pixelmap
		 * behind that material, its opacity and its UV transform. Sprite systems draw
		 * dozens of instances of one quad per scene, each with its own frame, so this
		 * cannot live on the geometry -- it is appended per capture and read by submit.
		 */
		struct draw_state
		{
			IDirect3DTexture9* texture;
			uint8_t opacity;

			// Belongs in the translucent pass: the material carries alpha, or the game has
			// faded it -- BRender turns blending on for anything below full opacity.
			bool blended;

			bool texture_transform_active;
			D3DMATRIX texture_transform;

			// The material asks for both sides to be drawn, so this run is exempt from
			// backface culling.
			bool two_sided;

			// Alpha blending on this run. Separate from `blended` because solid
			// translucency is submitted unblended but still cut out by its alpha.
			bool blend_enabled;
			bool alpha_tested;

			// How the run's own colour reaches Remix. Only ever other than `lit` on a
			// blended run.
			sprite_glow glow;
		};

		/*
		 * How a translucent run is submitted.
		 *
		 * BRender's translucency is a rasterizer instruction -- composite this surface
		 * over what is behind it -- and for sprites and decals that is still what it
		 * means. For solid geometry it is not: a car window or a water plane gets its
		 * transparency from the material Remix draws it with, and submitting the draw
		 * alpha-blended only costs it, because the runtime forces every blended draw to
		 * be double-sided (rtx_instance_manager.cpp:91) whatever cull mode it was given.
		 * A path tracer then finds an interface where the game has none.
		 */
		struct blend_plan
		{
			bool blended;       // drawn in the translucent pass, depth writes off
			bool blend_enabled;
			bool alpha_tested;
		};

		static blend_plan plan_blending(bool has_alpha, uint8_t opacity, bool solid);

		/*
		 * What a game object was when we cached something built from it.
		 *
		 * A br_model or br_pixelmap pointer is not an identity. The game frees a track's
		 * models and pixelmaps when the race ends, and the allocator hands the same
		 * addresses to the next track, so a surviving cache entry is not a stale miss -- it
		 * is a confident hit that returns the previous track's mesh or texture. Every entry
		 * therefore carries what it was built from and re-checks it on the way out.
		 */
		struct model_identity
		{
			const void* prepared;
			const void* vertices;
			const void* faces;
			uint16_t nvertices;
			uint16_t nfaces;

			bool operator==(const model_identity&) const = default;
		};

		struct pixelmap_identity
		{
			const void* pixels;
			const void* palette;
			uint32_t row_bytes;
			uint16_t width;
			uint16_t height;
			uint8_t type;

			bool operator==(const pixelmap_identity&) const = default;
		};

		static model_identity identify(const game::br_model* model);
		static pixelmap_identity identify(const game::br_pixelmap* pm);

		// A model's prepared geometry, uploaded once and reused. Static contents are what let
		// Remix keep the acceleration structure it builds instead of rebuilding every frame.
		struct model_geometry
		{
			model_identity identity;

			IDirect3DVertexBuffer9* vertex_buffer;
			IDirect3DIndexBuffer9* index_buffer;
			std::vector<geometry_part> parts;
			uint32_t vertex_count;
			uint32_t last_used_scene;

			// What the buffers were allocated to hold. A rebuild that needs the same sizes
			// refills them in place instead of trading them for an identical pair.
			uint32_t vertex_bytes;
			uint32_t index_bytes;

			// Geometry that is not one of the pooled sprite billboards or decal quads, so
			// its translucency describes a surface rather than a composite.
			bool solid;

			// One of the two models every smoke puff is drawn with.
			bool smoke;

			// BrModelUpdate can fire mid-scene, after this geometry is already queued for
			// submission. Marking instead of erasing keeps queued pointers valid; the
			// rebuild happens the next time the model is captured.
			bool dirty;

			// Scene this geometry was last pushed to the queue in. A rebuild inside that
			// same scene would rewrite data the queue is still pointing at, so it goes to
			// a transient copy instead. Starts on a value no scene counter reaches.
			uint32_t queued_scene = UINT32_MAX;
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

			// This draw's state for each of the geometry's parts, in order, starting at
			// m_draw_states[state_first].
			uint32_t state_first;

			// Which submission passes have anything to do for this draw, so the blended
			// pass can skip the overwhelming majority of models outright.
			bool has_opaque;
			bool has_blended;
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
			bool two_sided;
			uint8_t opacity;
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
			uint64_t placement;      // fingerprint of the actor's transform chain
			uint64_t parent_chain;   // node addresses, to tell a relink from a move
			uint32_t sightings;       // consecutive scenes holding this placement
			uint32_t last_seen_scene; // catches an actor re-placed between draws of one scene
			uint8_t bakes;            // times it has entered a chunk, capped
			bool bakeable;  // cleared for anything that can vanish, or that failed to bake
			bool demoted;   // has been unbaked at least once, so a later bake is a recovery
			bool noncar;    // bakes into the noncar chunks, apart from the pristine world
			bool baked;     // copied into a chunk, possibly one still accumulating
			bool live;      // its chunks are sealed and drawing; the game render is redundant
			std::vector<baked_range> ranges;
			std::vector<game::br_material*> materials;  // compared only, never dereferenced
		};

		// Where the static world loses geometry. Every eviction lands in exactly one bucket,
		// so a run says whether the world is shrinking because things moved, because the
		// game swapped a model, or because a material started animating -- distinctions the
		// old single counter could not make, and the reason a decaying world went unnoticed.
		struct demotion_tally
		{
			uint32_t moved = 0;
			uint32_t swapped = 0;
			uint32_t deformed = 0;
			uint32_t animated = 0;
			uint32_t instanced = 0;

			uint32_t total() const
			{
				return moved + swapped + deformed + animated + instanced;
			}
		};

		void submit(IDirect3DDevice9* dev);

		// Publishes the race's depth cue as fixed-function fog render states so Remix's
		// legacy fog remapping can pick it up from the injected draws.
		void apply_fog(IDirect3DDevice9* dev);

		// Retargets Remix's volumetric medium at the track's fog colour over the
		// bridge API, splitting a display fade colour into physically usable parts.
		// Returns false only when the bridge is not up yet, so the caller can retry.
		bool push_fog_to_remix(const game::scene_fog& fog) const;

		// False while the current depth cue still needs to reach Remix. The first fog
		// state of a session can beat the bridge initialization to the first submit,
		// so the push retries until it lands rather than firing once and being lost.
		bool m_remix_fog_synced = false;

		// Last fog state pushed to the device, so changes are logged once rather than
		// per scene. Colour 0xFFFFFFFF marks "nothing logged yet" — no real state matches,
		// because a br_colour never carries an alpha byte.
		game::scene_fog m_logged_fog{ false, 0xFFFFFFFFu, 0.0f, 0.0f };

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

		// Issues the one draw that tells Remix the path-traced scene is complete.
		void trigger_injection(IDirect3DDevice9* dev);

		// Accumulates whether the winding this module emits agrees with the normals the
		// game authored, which is the assumption resolve_cull_mode's answer rests on.
		void sample_winding(const std::vector<ffp_vertex>& vertices,
		                    const std::vector<uint32_t>& indices, size_t first_index);
		DWORD resolve_cull_mode();

		/*
		 * Reports the health of the normals being handed to Remix, once.
		 *
		 * They are forwarded verbatim from BRender's prepared vertices, and Remix shades
		 * with them rather than deriving its own -- so a model prepared without
		 * BR_MODU_VERTEX_NORMALS, or one whose normals were averaged across a material
		 * boundary, is invisible here but decides where every mirror points. Counting the
		 * degenerate and non-unit ones says whether the data is worth suspecting.
		 */
		void sample_normals(const std::vector<ffp_vertex>& vertices, size_t first_vertex,
		                    const game::br_model* model);

		void capture_lines(const game::br_model* model, const game::br_matrix34& model_to_world);
		uint32_t submit_lines(IDirect3DDevice9* dev);
		void log_spark_geometry(const float camera[3]);

		// A soft streak shading from one end colour to the other, one texture per pair the
		// emitter uses. Kept apart from the flat-colour swatches so sparks carry their own
		// Remix hashes and can be tagged emissive without dragging every red-painted chunk
		// of debris along.
		IDirect3DTexture9* spark_texture(IDirect3DDevice9* dev, uint32_t rgb_a, uint32_t rgb_b);

		// Resolves how each part of a model looks for the draw being captured -- the
		// material it inherits, the pixelmap behind it, its opacity and UV transform --
		// into this scene's draw_state pool, and records where in the queued entry.
		void resolve_draw_state(IDirect3DDevice9* dev, const model_geometry& geometry,
		                        game::br_material* fallback_material, queued_model& queued);

		// Chunks only ever hold solid geometry -- anything the game re-places or deletes
		// is barred from baking -- so their blending follows from the chunk alone.
		static blend_plan plan_blending(const static_chunk& chunk) {
			return plan_blending(chunk.has_alpha, chunk.opacity, true);
		}

		bool build_projection(D3DMATRIX& out) const;
		model_geometry* geometry_for(IDirect3DDevice9* dev, game::br_model* model,
		                             game::br_material* fallback_material);

		// Fills one model_geometry from a model's prepared block, reusing whatever buffers
		// it already holds when the new contents are the same size. Returns false and leaves
		// the entry drawable-free if the model has nothing to extract.
		bool build_geometry(IDirect3DDevice9* dev, game::br_model* model,
		                    game::br_material* fallback_material, model_geometry& into);

		// A copy of a model's geometry that lives for one scene, for the case where the game
		// rewrites a model between renders of it. Entries are recycled every scene rather
		// than reallocated: the smoke system alone would otherwise create and destroy
		// buffers for thirty-five quads a frame.
		model_geometry* transient_geometry(IDirect3DDevice9* dev, game::br_model* model,
		                                   game::br_material* fallback_material);
		void release_transient();

		// Walks a model's prepared groups into CPU-side vertices and per-texture index runs.
		// Shared by the per-model buffers and the merged static batches.
		bool extract_geometry(IDirect3DDevice9* dev, game::br_model* model,
		                      game::br_material* fallback_material,
		                      std::vector<ffp_vertex>& vertices,
		                      std::vector<geometry_part>& parts,
		                      std::vector<uint32_t>& indices);

		/*
		 * Why a model needed a draw of its own instead of coming from a sealed chunk.
		 *
		 * A dip is always a scene with a large dynamic population, and the population is
		 * the only thing the report could not break down: 841 queued models says nothing
		 * about whether they are cars, particles or scenery that failed to bake.
		 */
		enum class dynamic_reason : uint8_t
		{
			chunked,     // covered by a sealed chunk; never queued
			overlay,     // not the race view -- HUD widgets and menus
			callback,    // the model draws through its own render callback
			vanishing,   // pickup or decal quad: the game deletes it rather than moving it
			instanced,   // one actor re-placed between draws of a single scene
			unbakeable,  // extraction failed, or it has used up its bakes
			moving,      // its placement changed recently
			probation,   // holding still, not yet promoted
			unsealed,    // baked, but its chunk has not sealed yet
			count
		};

		static const char* dynamic_reason_name(dynamic_reason reason);

		void classify_actor(actor_record& record, const game::br_actor* actor,
		                    game::br_model* model) const;

		// Follows one actor's placement and keeps the chunks in step with it. Returns
		// `chunked` when the actor is already live in a sealed chunk, so the caller neither
		// queues it nor lets the game rasterize it; otherwise why it still needs a draw.
		dynamic_reason track_static_actor(game::br_actor* actor, game::br_model* model,
		                                  game::br_material* fallback_material,
		                                  const game::br_matrix34& world);

		bool bake_actor(actor_record& record, game::br_model* model,
		                game::br_material* fallback_material, const game::br_matrix34& world);
		void append_part_to_chunk(const geometry_part& part,
		                          const std::vector<ffp_vertex>& vertices,
		                          const std::vector<uint32_t>& indices, actor_record& record);
		void punch_out(const actor_record& record);

		// Takes an actor's geometry back out of the chunks and restarts its probation. The
		// record survives, so scenery that comes to rest -- a knocked lamppost, a settled
		// wreck -- rejoins the static world instead of costing a draw for the rest of the
		// race. Actors that must never bake are marked unbakeable rather than unbaked.
		// Returns whether there was a bake to take back, so callers can attribute the loss.
		bool unbake_actor(actor_record& record);
		void seal_chunks(IDirect3DDevice9* dev);
		void reset_static_world(const char* reason);

		/*
		 * Drops everything learned from the track that is ending.
		 *
		 * Every cache below is keyed on a game pointer -- br_model, br_pixelmap, the
		 * br_material::stored token -- and the game frees all three between races. The
		 * allocator then hands the same addresses to the next track, so a surviving entry
		 * is not stale in the harmless sense: it is a confident hit that returns the
		 * previous track's texture, geometry or material. That is one bug, and it shows up
		 * as misaligned textures, scenery from the last track, and a static world baked out
		 * of both.
		 */
		void resolve_frontend_return();

		/*
		 * Distinct models rebuilt while away that mean a level was loaded, not paused.
		 *
		 * The raw call count cannot be the signal: the frontend re-rebuilds the same few
		 * preview models every frame, so calls grow with time spent in the menu -- a pause
		 * measured 152 against real loads' 2141-3181, and a longer menu visit would have
		 * crossed any threshold. Distinct models measure what was read in: a load touches
		 * every model of the track, a menu the same handful over and over. Both decisions
		 * log the count so the margin stays visible.
		 */
		static constexpr size_t TRACK_LOAD_DISTINCT_MODELS = 400;

		bool m_in_frontend = false;
		std::unordered_set<game::br_model*> m_frontend_models;
		void release_chunks();
		void release_geometry(model_geometry& geometry);
		void evict_stale_geometry();

		void note_untextured(const game::br_model* model, const game::br_material* material);

		// Materials whose surface is shaded by prelit vertex colours or faded by opacity --
		// the two things that can change how already-working geometry looks.
		void note_shaded_material(const game::br_material* material, bool prelit, uint8_t opacity);
		void note_unsupported_style(const game::br_model* model, uint32_t style);

		// Names a surface the engine renders unshaded, and the texture behind it. Remix
		// ignores the per-draw D3DMATERIAL9 for emission, so a solid surface only glows
		// through a replacement keyed on the texture, and the list of textures worth
		// tagging is the useful output. Blended sprites are the exception: an additive
		// blend is emissive to Remix (see draw_state::emissive).
		void note_emissive_candidate(const game::br_material* material, const char* reason);
		std::map<std::string, std::string> m_emissive_materials;

		// How a blended run of this geometry and material gets its colour, and the log of
		// every sprite texture given a glow, once per name.
		sprite_glow classify_glow(const model_geometry& geometry, const game::br_material* material);
		std::set<std::string> m_glowing_sprites;
		void note_scene_target(const game::br_actor* camera, const game::br_pixelmap* colour);
		std::set<std::pair<const game::br_actor*, const game::br_pixelmap*>> m_scene_targets;

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

		// A pixelmap's pixels as tightly packed A8R8G8B8, whatever BRender type they are stored in.
		bool decode_pixelmap(const game::br_pixelmap* pm, std::vector<uint32_t>& argb);

		std::vector<queued_model> m_queue;
		std::vector<draw_state> m_draw_states;
		std::vector<line_segment> m_lines;
		std::vector<ffp_vertex> m_line_vertices;
		std::unordered_map<game::br_model*, model_geometry> m_geometry;

		// Per-scene copies for models the game rewrites between renders. A deque because the
		// queue holds pointers into it for the length of the scene.
		std::deque<model_geometry> m_transient;
		size_t m_transient_used = 0;

std::vector<static_chunk> m_chunks;

		// Index of the chunk currently accepting geometry for a (texture, blend) pair.
		// Keyed on the texture pointer with the blend bit folded in.
		std::unordered_map<uint64_t, size_t> m_open_chunks;

		// Every actor the race scene has walked. A mover keeps its record with the bake
		// taken back out, so the set of things that may bake is decided by what each actor
		// is doing now, not by a one-way blacklist that only ever grew.
		std::unordered_map<game::br_actor*, actor_record> m_actors;

		// Materials the funkotronic system has animated mid-race. Pointers are compared,
		// never dereferenced; a stale entry after a level change only costs one model its
		// bake, and the set is cleared with the rest of the static world.
		std::unordered_set<game::br_material*> m_animated_materials;

		// What a material looked like at its last appearance-changing update. Animation
		// means such updates in two different scenes; a load burst is many under one stamp.
		struct material_state
		{
			uint32_t scene = 0;
			uint8_t opacity = 0;
		};
		std::unordered_map<game::br_material*, material_state> m_material_state;

		// Frames an actor must hold one placement before it counts as scenery. Cars fail on
		// their second frame and are never baked; the same count is what a demoted actor
		// serves before it may bake again.
		static constexpr uint32_t STATIC_PROMOTE_SIGHTINGS = 3;

		// What a demoted actor must hold instead, and how many times it may ever bake.
		// A rebake appends a second copy to the chunks and leaves the first as dead
		// vertices, so an actor that keeps changing its mind has to be cut off: the point
		// of recovery is the lamppost that is knocked over once and then lies there.
		static constexpr uint32_t STATIC_REBAKE_SIGHTINGS = 120;
		static constexpr uint8_t STATIC_MAX_BAKES = 3;

		// Scenes without a new promotion before the accumulated chunks seal. Sealing early
		// means sealing often, and every seal hands Remix new buffers to hash.
		static constexpr uint32_t STATIC_SEAL_QUIET_SCENES = 30;

		// Baked actors are kept for the whole race even while unseen: the city streams
		// scenery by zone, so absence means hidden, not gone -- and keeping hidden zones
		// resident is the point of the chunks. Everything that can genuinely vanish or
		// move mid-race (powerups, noncars, decals, peds, cars) is excluded from baking
		// instead. Race changes are detected by population takeover below.


		// This scene's queued models, split by why the chunks could not carry them.
		std::array<uint32_t, static_cast<size_t>(dynamic_reason::count)> m_dynamic_reasons{};

		// Chunk indices are 16-bit.
		static constexpr uint32_t CHUNK_VERTEX_LIMIT = 0xFFFFu;

		uint32_t m_last_promotion_scene = 0;
		bool m_have_unsealed = false;
		uint32_t m_live_actors = 0;
		demotion_tally m_demotions;

		// Relinks that did not move anything, so the bake survived them. Under the old
		// fingerprint every one of these evicted an actor for good.
		uint32_t m_relinks_absorbed = 0;

		// Actors promoted after an earlier demotion -- scenery that came to rest.
		uint32_t m_repromotions = 0;
		uint32_t m_scene_models = 0;
		uint32_t m_captures = 0;

		// Triangles whose emitted winding agrees (or does not) with the authored normals.
		uint32_t m_winding_agree = 0;
		uint32_t m_winding_disagree = 0;
		bool m_winding_reported = false;

		// Triangles to sample before naming the cull mode. A model's own faces can
		// disagree -- authored geometry is not always consistent -- so the answer is the
		// majority over many models rather than the first one seen.
		static constexpr uint32_t WINDING_SAMPLES = 4096;

		// Normal health, over the same kind of bounded sample.
		uint32_t m_normals_sampled = 0;
		uint32_t m_normals_degenerate = 0;    // no direction at all
		uint32_t m_normals_unnormalized = 0;  // a direction, but not unit length
		bool m_normals_reported = false;
		std::string m_worst_normal_model;
		static constexpr uint32_t NORMAL_SAMPLES = 8192;

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

		// The track's horizon as a sky Remix rasterizes, drawn first in every race submit.
		void draw_sky(IDirect3DDevice9* dev);
		sky_dome m_sky;

		// Built on the first submit and re-captured each scene thereafter.
		IDirect3DStateBlock9* m_saved_state = nullptr;
		uint32_t m_textures_ok = 0;
		uint32_t m_textures_failed = 0;

		// Keyed on the colour_map rather than the material, so materials sharing a texture
		// share one upload and Remix sees one stable hash for them.
		struct cached_texture
		{
			IDirect3DTexture9* texture;
			pixelmap_identity identity;
		};
		std::unordered_map<const game::br_pixelmap*, cached_texture> m_textures;
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
		std::map<std::string, std::string> m_shaded_materials;

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
			demotion_tally demotions;
			uint32_t relinks_absorbed;
			uint32_t repromotions;
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
			uint32_t scene;
			std::array<uint32_t, static_cast<size_t>(dynamic_reason::count)> dynamic_reasons{};
		};

		void log_performance(const frame_stats& stats);
		void log_frame_stats(const char* label, const frame_stats& stats);

		uint32_t m_scenes_submitted = 0;

		/*
		 * Race scene walks begun, which is not the same as scenes submitted.
		 *
		 * m_scenes_submitted only advances when a submit runs to completion, and a scene
		 * whose camera has no usable projection -- the pause menu is one -- is walked in
		 * full and then dropped. Anything that means "this scene" has to count walks, or a
		 * dropped submit leaves two consecutive walks sharing a number and every actor in
		 * the level reads as drawn twice in one scene.
		 */
		uint32_t m_scene_walks = 0;
		int64_t m_last_scene_ticks = 0;
		double m_ticks_per_ms = 0.0;
		frame_stats m_window_worst{};
		scene_profile m_profile{};
		int64_t m_overlay_ticks = 0;
	};
}
