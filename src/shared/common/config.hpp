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

			// Drops the game's own render of every model the proxy has already injected.
			// BRender transforms and lights on the CPU and nGlide rasterizes the result, and
			// Remix discards all of it as pre-transformed, so with the injection running that
			// work reaches nothing. Turn off when running without Remix: the injected
			// geometry is then the only thing left drawing the world.
			bool suppress_game_render = true;
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
		} effects;

	private:
		std::string ini_path_;
		bool loaded_ = false;

		void parse_all();
	};
}
