#pragma once

namespace shared::common
{
	class config
	{
	public:
		static config& get();

		void load(const std::string& ini_path);
		bool is_loaded() const { return loaded_; }

		int get_int(const char* section, const char* key, int default_val) const;
		std::string get_string(const char* section, const char* key, const char* default_val) const;
		float get_float(const char* section, const char* key, float default_val) const;
		bool get_bool(const char* section, const char* key, bool default_val) const;

		// Writes a value back to the ini, so a setting changed at runtime survives a restart.
		void set_bool(const char* section, const char* key, bool value) const;

		struct ffp_settings
		{
			bool enabled = true;
			int albedo_stage = 0;
		} ffp;

		struct skinning_settings
		{
			bool enabled = false;
		} skinning;

		struct diagnostics_settings
		{
			bool enabled = true;
			bool auto_capture = true;
			int delay_ms = 50000;
			int log_frames = 3;

			// Log categories (defaults, overridable from ImGui at runtime)
			bool log_draw_calls = true;
			bool log_vs_constants = true;
			bool log_vertex_data = true;
			bool log_declarations = true;
			bool log_textures = true;
			bool log_present_info = true;
		} diagnostics;

		struct remix_settings
		{
			bool enabled = true;
			std::string dll_name = "d3d9_remix.dll";

			// Marks the end of the race submit as Remix's injection point with one
			// off-screen draw it classifies as UI. Remix otherwise injects at the first
			// draw that binds a texture tagged as UI, and composites every draw after
			// that point on top of the path-traced frame -- so where the game's own
			// raster stream ends up depends on which textures have been tagged and on
			// how nGlide batches. Pinning the point makes it independent of both.
			bool trigger_injection = true;
		} remix;

		struct chain_settings
		{
			std::string preload;   // semicolon-separated DLLs/ASIs loaded before d3d9 chain
			std::string postload;  // semicolon-separated DLLs/ASIs loaded after init
		} chain;

		struct tracer_settings
		{
			int backtrace_depth = 8;
			std::string output_dir = "captures";
		} tracer;

		struct optimization_settings
		{
			// Bakes scenery that holds one placement into per-texture chunks that are
			// uploaded once and never modified, so the whole level stays resident for
			// Remix at a few dozen draws instead of thousands.
			bool static_world = true;

			// Drops the game's own render of every model drawn from a live sealed chunk.
			// BRender transforms and lights those on the CPU and nGlide rasterizes the
			// result, all redundantly once the chunks carry them. Turn off when running
			// without Remix.
			bool suppress_game_render = true;

			// Extends that suppression to every model the injection captured in the race
			// view. Remix composites whatever nGlide draws after its injection point over
			// the path-traced frame, so a dynamic model's raster twin is not only wasted
			// CPU: it is the flat, env-mapped car window painted over the ray-traced glass.
			// Suppression rewrites the render style rather than skipping the render call,
			// so custom render callbacks still run and their draws are still captured.
			bool suppress_dynamics = true;
		} optimization;

		struct culling_settings
		{
			// Overrides br_camera::yon_z each frame. Carmageddon 2 ships 35 world units,
			// which is far too short for path tracing. 0 leaves the game's value alone.
			float far_plane = 0.0f;

			// Downgrades every frustum-rejected actor to a clipped pass instead, so the
			// entire level reaches the scene walk each frame. Path tracing needs the
			// geometry behind and beside the camera to occlude and bounce light; with the
			// game render suppressed, walking it costs almost nothing.
			bool disable_frustum = true;

			// Radius around the camera within which geometry is submitted even when the
			// frustum test rejects it. Superseded by disable_frustum; kept for runs that
			// want the game's culling mostly intact. 0 disables it.
			float bubble_radius = 0.0f;
		} culling;

		struct effects_settings
		{
			// Each of the three effect fixes can be switched off on its own, so a change in
			// frame rate can be attributed to one of them rather than to the set.

			// Split submission into an opaque pass and a translucent pass drawn afterwards
			// with depth writes off. Off restores a single pass with depth writes on and
			// blending toggled per run, which is what the proxy did before.
			bool translucent_pass = true;

			// Follow br_material::map_transform into the texture stage matrix, which is how
			// a car's rear light panel addresses one cell of its atlas. Off leaves the
			// stage transform disabled and shows all four light states at once.
			bool texture_transform = true;

			// Rebuild BR_RSTYLE_EDGES models as billboards. Off drops them, which is what
			// happens naturally otherwise: their faces are zero-area triangles.
			bool sparks = true;

			// Modulate the texture by BRender's authored vertex colour on prelit materials,
			// which is where a sprite's tint lives -- smoke carries no colour anywhere else.
			// Off leaves every surface the plain texture, so tinted sprites come out white.
			bool vertex_colour = true;

			// Follow br_material::opacity and the BRT_OPACITY tokens of its extra list into
			// the texture factor's alpha. This is how the game fades smoke as it disperses
			// and how overlay polys stay see-through. Off draws every surface fully opaque.
			bool material_opacity = true;

			// Submit the game's own facing rule instead of drawing everything
			// double-sided. BRender rejects back faces, so a car body -- and the glass in
			// it -- is a shell of one-sided polygons; handing a path tracer both sides
			// gives it an interface the game does not have, which is where ray-traced
			// glass turns to noise. Materials flagged to be seen from both sides, and the
			// spark billboards, stay double-sided.
			bool backface_culling = true;

			// Keep culling on for closed meshes the game has made two-sided. The game sets
			// TWO_SIDED on a car part's materials the moment it flaps open, which an open
			// sheet needs but a closed shell does not -- and a closed shell crushed flat
			// (the Thunderbucket's doors) then shows its paint and its interior fighting in
			// one plane. Only meaningful with backface_culling on.
			bool cull_closed_meshes = true;

			// Submit solid translucent surfaces -- glass, water, anything whose texture
			// carries alpha and that is not a sprite or a decal -- as ordinary geometry
			// with an alpha test rather than as blended draws. Remix forces every blended
			// draw double-sided whatever cull mode it was given, and a replaced
			// translucent material owns how much light passes through anyway, so the
			// blending buys nothing and costs single-sidedness. Sprites and decals, which
			// nothing replaces, keep their blending.
			bool solid_translucency = true;

			// Publish the race's depth-cue state as D3D9 fixed-function fog render
			// states on the injected draws. Remix's legacy fog remapping reads exactly
			// these (D3DRS_FOGENABLE / FOGCOLOR / FOGSTART / FOGEND) to derive its
			// volumetric transmittance colour and distance, so this is what carries a
			// track's red haze or white-out into the path tracer.
			bool fog = true;

			// Retarget Remix's volumetric medium at the track's fog colour through the
			// bridge API. The authored colour is a display fade target, not a medium
			// colour: fed to Remix raw, a dark cue absorbs the sky (the port's only
			// light) to black, and a saturated medium scatters the *complementary*
			// hue. The proxy therefore splits it -- saturation into the scattering
			// albedo (the fog's own glow), a whisper of hue into the transmittance.
			bool fog_volumetrics = true;

			// How much of the fog hue the transmittance colour may carry, 0..0.5.
			// Sky light survives transmittance^5 whatever the distances are set to,
			// so even 0.2 leaves the weakest channel only ~4% of the sky.
			float fog_tint = 0.08f;

			// Distance translucent surfaces are lifted along their normals. BRender kept
			// tyre tracks, shadows and impact smears out of the road by drawing them in
			// depth-sorted order; a path tracer has no draw order, so co-planar decals
			// have to be separated geometrically instead. World units. 0 disables it.
			float decal_offset = 0.02f;

			// Width of the camera-facing quad a BRender line segment expands into, as a
			// fraction of the distance from the camera to that segment. BRender drew these
			// as one-pixel screen-space lines, so scaling with distance is what keeps their
			// apparent thickness constant; a fixed world width turns sparks struck against
			// the player's own bodywork into slabs. Roughly 0.001 per pixel at a 60 degree
			// vertical field of view.
			float spark_width = 0.004f;

			// Submit the game's full-bright sprites -- explosion fire, the powerup sparkle,
			// the car flames -- with an additive blend (SRCALPHA, ONE) instead of an alpha
			// blend. Remix reads that blend mode as emissive and lights the sprite from its
			// own texture, which is what BRender did: these materials are built unlit
			// (flags exactly BR_MATF_ALWAYS_VISIBLE). Under an alpha blend Remix lights
			// them from the scene instead, so fire is dark wherever the road is.
			bool emissive_sprites = true;

			// Treat the car flames (FLM01..FLM20) as sprites, so they go out blended and take
			// the additive path above. Off leaves them the alpha-tested surfaces the game's quad
			// is otherwise submitted as, which is what a mod's emissive mask needs: Remix
			// replaces the emission of any blended draw.
			bool additive_car_flames = true;

			// Pixelmap name prefixes of full-bright sprites that are drawn unlit instead of
			// additively. Blood clouds (BIGBL01..05) come out of the same sprite pool as the
			// fire, but blood covers what is behind it rather than glowing over it.
			std::vector<std::string> emissive_sprite_exclude = { "BIGBL" };

			// Draw smoke and the excluded full-bright sprites twice: the ordinary alpha
			// blend, which darkens what is behind them, then an additive copy that adds
			// their own colour. Remix lights a tagged particle only from its volumetric
			// radiance cache, which holds no sky light, so without this they are near black.
			bool unlit_sprites = true;

			// Scale on the additive copy's alpha, 0..1: how brightly the unlit sprites show
			// their own colour. rtx.emissiveBlendOverrideEmissiveIntensity scales it too.
			float unlit_sprite_brightness = 1.0f;
		} effects;

		struct sky_settings
		{
			// Draw a sky Remix rasterizes itself: a sphere around the camera carrying a
			// panorama baked from the track's horizon texture. This is what lights the
			// track on a stock Remix runtime, whose sky is otherwise black. Turn it off where
			// the runtime supplies a sky of its own (a physical atmosphere).
			bool synthesize = true;
		} sky;

	private:
		std::string ini_path_;
		bool loaded_ = false;

		void parse_all();
	};
}
