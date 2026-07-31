#include "std_include.hpp"
#include "config.hpp"

namespace shared::common
{
	config& config::get()
	{
		static config instance;
		return instance;
	}

	void config::load(const std::string& path)
	{
		ini_path_ = path;

		// Check if the INI file actually exists — GetPrivateProfileInt silently
		// returns defaults for missing files, making it look like settings are ignored.
		if (GetFileAttributesA(ini_path_.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			log("Config", std::format("INI NOT FOUND: {} — using all defaults!", ini_path_),
				LOG_TYPE::LOG_TYPE_ERROR, true);
			loaded_ = false;
			return;
		}

		loaded_ = true;
		parse_all();
	}

	int config::get_int(const char* section, const char* key, int default_val) const
	{
		if (!loaded_) return default_val;
		return GetPrivateProfileIntA(section, key, default_val, ini_path_.c_str());
	}

	std::string config::get_string(const char* section, const char* key, const char* default_val) const
	{
		if (!loaded_) return default_val;
		char buf[512];
		GetPrivateProfileStringA(section, key, default_val, buf, sizeof(buf), ini_path_.c_str());
		return buf;
	}

	float config::get_float(const char* section, const char* key, float default_val) const
	{
		auto str = get_string(section, key, "");
		if (str.empty()) return default_val;
		try { return std::stof(str); }
		catch (...) { return default_val; }
	}

	bool config::get_bool(const char* section, const char* key, bool default_val) const
	{
		return get_int(section, key, default_val ? 1 : 0) != 0;
	}

	void config::parse_all()
	{
		// [Remix]
		remix.enabled = get_bool("Remix", "Enabled", true);
		remix.dll_name = get_string("Remix", "DLLName", "d3d9_remix.dll");

		// [Chain]
		chain.preload = get_string("Chain", "PreLoad", "");
		chain.postload = get_string("Chain", "PostLoad", "");

		// [FFP]
		ffp.enabled = get_bool("FFP", "Enabled", true);
		ffp.albedo_stage = get_int("FFP", "AlbedoStage", 0);
		if (ffp.albedo_stage < 0 || ffp.albedo_stage > 7)
			ffp.albedo_stage = 0;

		// [Skinning]
		skinning.enabled = get_bool("Skinning", "Enabled", false);

		// [Diagnostics]
		diagnostics.enabled = get_bool("Diagnostics", "Enabled", true);
		diagnostics.auto_capture = get_bool("Diagnostics", "AutoCapture", true);
		diagnostics.delay_ms = get_int("Diagnostics", "DelayMs", 50000);
		diagnostics.log_frames = get_int("Diagnostics", "LogFrames", 3);
		diagnostics.log_draw_calls = get_bool("Diagnostics", "LogDrawCalls", true);
		diagnostics.log_vs_constants = get_bool("Diagnostics", "LogVSConstants", true);
		diagnostics.log_vertex_data = get_bool("Diagnostics", "LogVertexData", true);
		diagnostics.log_declarations = get_bool("Diagnostics", "LogDeclarations", true);
		diagnostics.log_textures = get_bool("Diagnostics", "LogTextures", true);
		diagnostics.log_present_info = get_bool("Diagnostics", "LogPresentInfo", true);

		// [Tracer]
		tracer.backtrace_depth = get_int("Tracer", "BacktraceDepth", 8);
		tracer.output_dir = get_string("Tracer", "OutputDir", "captures");

		// [Culling]
		culling.far_plane = get_float("Culling", "FarPlane", 0.0f);
		culling.disable_frustum = get_bool("Culling", "DisableFrustum", true);
		culling.bubble_radius = get_float("Culling", "BubbleRadius", 0.0f);

		// [Optimization]
		optimization.static_world = get_bool("Optimization", "StaticWorld", true);
		optimization.suppress_game_render = get_bool("Optimization", "SuppressGameRender", true);

		// [Effects]
		effects.translucent_pass = get_bool("Effects", "TranslucentPass", true);
		effects.texture_transform = get_bool("Effects", "TextureTransform", true);
		effects.sparks = get_bool("Effects", "Sparks", true);
		effects.decal_offset = get_float("Effects", "DecalOffset", 0.02f);
		effects.spark_width = get_float("Effects", "SparkWidth", 0.004f);

		log("Config", std::format("Loaded from: {}", ini_path_));
		log("Config", std::format("FFP={} AlbedoStage={}", ffp.enabled ? 1 : 0, ffp.albedo_stage));
		if (skinning.enabled)
			log("Config", "Skinning ENABLED", LOG_TYPE::LOG_TYPE_WARN);
	}
}
