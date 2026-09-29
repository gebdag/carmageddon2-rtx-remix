#include "std_include.hpp"
#include "imgui.hpp"
#include "frame_pacer.hpp"

#include "imgui_internal.h"
#include "renderer.hpp"
#include "tracer.hpp"
#include "diagnostics.hpp"
#include "headlights.hpp"
#include "sun.hpp"
#include "time_of_day.hpp"
#include "street_lights.hpp"
#include "lens_lights.hpp"
#include "brender_inject.hpp"
#include "shared/common/imgui_helper.hpp"
#include "shared/common/config.hpp"
#include "shared/common/remix_api.hpp"

// Allow us to directly call the ImGui WndProc function.
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#define CENTER_URL(text, link)					\
	ImGui::SetCursorForCenteredText((text));	\
	ImGui::TextURL((text), (link), true);

#define SPACEY16 ImGui::Spacing(0.0f, 16.0f);
#define SPACEY8 ImGui::Spacing(0.0f, 8.0f);

namespace comp
{
	WNDPROC g_game_wndproc = nullptr;
	
	LRESULT __stdcall wnd_proc_hk(HWND window, UINT message_type, WPARAM wparam, LPARAM lparam)
	{
		if (message_type != WM_MOUSEMOVE && message_type != WM_NCMOUSEMOVE)
		{
			if (imgui::get()->input_message(message_type, wparam, lparam)) {
			//	return true;
			}
		}

		// if your game has issues with floating cursors
		/*if (message_type == WM_KILLFOCUS)
		{
			uint32_t counter = 0u;
			while (::ShowCursor(TRUE) < 0 && ++counter < 3) {}
			ClipCursor(NULL);
		}*/

		//printf("MSG 0x%x -- w: 0x%x -- l: 0x%x\n", message_type, wparam, lparam);
		return CallWindowProc(g_game_wndproc, window, message_type, wparam, lparam);
	}

	bool imgui::input_message(const UINT message_type, const WPARAM wparam, const LPARAM lparam)
	{
		// F is unbound in the game's default key layout (H, the obvious choice, is the
		// horn in every layout). Bit 30 of lparam is set on auto-repeat; one press is one step.
		if (message_type == WM_KEYDOWN && wparam == 'F' && !(lparam & (1 << 30))
			&& !shared::globals::imgui_wants_text_input)
		{
			if (const auto lights = headlights::get(); lights) {
				lights->cycle_mode();
			}
		}

		if (message_type == WM_KEYUP && wparam == VK_F4) 
		{
			const auto& io = ImGui::GetIO();
			if (!io.MouseDown[1]) {
				shared::globals::imgui_menu_open = !shared::globals::imgui_menu_open;
			} else {
				ImGui_ImplWin32_WndProcHandler(shared::globals::main_window, message_type, wparam, lparam);
			}
		}

		if (shared::globals::imgui_menu_open)
		{
			//auto& io = ImGui::GetIO();
			ImGui_ImplWin32_WndProcHandler(shared::globals::main_window, message_type, wparam, lparam);
		} else {
			shared::globals::imgui_allow_input_bypass = false; // always reset if there is no imgui window open
		}

		return shared::globals::imgui_menu_open;
	}

	// ------

	void imgui::tab_about()
	{
		if (tex_addons::icon)
		{
			const float cursor_y = ImGui::GetCursorPosY();
			ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() * 0.85f, 24));
			ImGui::Image((ImTextureID)tex_addons::icon, ImVec2(48.0f, 48.0f), ImVec2(0.03f, 0.03f), ImVec2(0.96f, 0.96f));
			ImGui::SetCursorPosY(cursor_y);
		}

		ImGui::Spacing(0.0f, 20.0f);

		ImGui::CenterText("remix-comp-proxy");
		ImGui::CenterText("DX9 proxy framework for RTX Remix compatibility mods");

		ImGui::Spacing(0.0f, 24.0f);
		ImGui::CenterText("current version");

		const char* version_str = shared::utils::va("%d.%d.%d :: %s",
			COMP_MOD_VERSION_MAJOR, COMP_MOD_VERSION_MINOR, COMP_MOD_VERSION_PATCH, __DATE__);
		ImGui::CenterText(version_str);

#if DEBUG
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.64f, 0.23f, 0.18f, 1.0f));
		ImGui::CenterText("DEBUG BUILD");
		ImGui::PopStyleColor();
#endif

		SPACEY16;
		CENTER_URL("Vibe Reverse Engineering Toolkit", "https://github.com/Ekozmaster/Vibe-Reverse-Engineering");

		SPACEY16;
		ImGui::Separator();
		SPACEY16;

		ImGui::CenterText("Contributors");

		SPACEY8;

		// xoxor4d
		CENTER_URL("xoxor4d", "https://github.com/xoxor4d");
		ImGui::CenterText("Original remix-comp-base framework, D3D9 proxy architecture,");
		ImGui::CenterText("ImGui integration, module system");
		SPACEY8;
		CENTER_URL("Ko-Fi (xoxor4d)", "https://ko-fi.com/xoxor4d");
		ImGui::SameLine();
		ImGui::TextURL("Patreon", "https://patreon.com/xoxor4d", true);

		SPACEY16;

		// kim2091
		CENTER_URL("kim2091", "https://github.com/kim2091");
		ImGui::CenterText("FFP conversion, skinning module, diagnostic logging,");
		ImGui::CenterText("tracer integration, INI config, toolkit integration");
		SPACEY8;
		CENTER_URL("Ko-Fi (kim2091)", "https://ko-fi.com/kim20913944");

		SPACEY16;

		// momo5502
		CENTER_URL("momo5502", "https://github.com/momo5502");
		ImGui::CenterText("Initial codebase that remix-comp-base was built on");

		SPACEY16;
		ImGui::Separator();
		SPACEY16;

		ImGui::CenterText("Dependencies");

		SPACEY8;

		CENTER_URL("NVIDIA - RTX Remix", "https://github.com/NVIDIAGameWorks/rtx-remix");
		CENTER_URL("Dear ImGui", "https://github.com/ocornut/imgui");
		CENTER_URL("MinHook", "https://github.com/TsudaKageyu/minhook");

		SPACEY16;
		ImGui::CenterText("Thank you to all supporters and contributors!");
	}

	// draw imgui widget
	void imgui::ImGuiStats::draw_stats()
	{
		if (!m_tracking_enabled) {
			return;
		}

		for (const auto& p : m_stat_list)
		{
			if (p.second) {
				display_single_stat(p.first, *p.second);
			}
			else {
				ImGui::Spacing(0, 4);
			}
		}
	}

	void imgui::ImGuiStats::display_single_stat(const char* name, const StatObj& stat)
	{
		switch (stat.get_mode())
		{
		case StatObj::Mode::Single:
			ImGui::Text("%s", name);
			ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.65f);
			ImGui::Text("%d total", stat.get_total());
			break;

		case StatObj::Mode::ConditionalCheck:
			ImGui::Text("%s", name);
			ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.65f);
			ImGui::Text("%d total, %d successful", stat.get_total(), stat.get_successful());
			break;

		default:
			throw std::runtime_error("Uncovered Mode in StatObj");
		}
	}

	void dev_debug_container()
	{
		SPACEY16;
		const auto& im = imgui::get();

		if (ImGui::CollapsingHeader("Temp Debug Values"))
		{
			SPACEY8;
			ImGui::DragFloat3("Debug Vector", &im->m_debug_vector.x, 0.01f, 0, 0, "%.6f");
			ImGui::DragFloat3("Debug Vector 2", &im->m_debug_vector2.x, 0.1f, 0, 0, "%.6f");
			ImGui::DragFloat3("Debug Vector 3", &im->m_debug_vector3.x, 0.1f, 0, 0, "%.6f");
			ImGui::DragFloat3("Debug Vector 4", &im->m_debug_vector4.x, 0.1f, 0, 0, "%.6f");
			ImGui::DragFloat3("Debug Vector 5", &im->m_debug_vector5.x, 0.1f, 0, 0, "%.6f");

			ImGui::Checkbox("Debug Bool 1", &im->m_dbg_debug_bool01);
			ImGui::Checkbox("Debug Bool 2", &im->m_dbg_debug_bool02);
			ImGui::Checkbox("Debug Bool 3", &im->m_dbg_debug_bool03);
			ImGui::Checkbox("Debug Bool 4", &im->m_dbg_debug_bool04);
			ImGui::Checkbox("Debug Bool 5", &im->m_dbg_debug_bool05);
			ImGui::Checkbox("Debug Bool 6", &im->m_dbg_debug_bool06);
			ImGui::Checkbox("Debug Bool 7", &im->m_dbg_debug_bool07);
			ImGui::Checkbox("Debug Bool 8", &im->m_dbg_debug_bool08);
			ImGui::Checkbox("Debug Bool 9", &im->m_dbg_debug_bool09);

			ImGui::DragInt("Debug Int 1", &im->m_dbg_int_01, 0.01f);
			ImGui::DragInt("Debug Int 2", &im->m_dbg_int_02, 0.01f);
			ImGui::DragInt("Debug Int 3", &im->m_dbg_int_03, 0.01f);
			ImGui::DragInt("Debug Int 4", &im->m_dbg_int_04, 0.01f);
			ImGui::DragInt("Debug Int 5", &im->m_dbg_int_05, 0.01f);
			SPACEY8;
		}

		if (ImGui::CollapsingHeader("Fake Camera"))
		{
			SPACEY8;
			ImGui::Checkbox("Use Fake Camera", &im->m_dbg_use_fake_camera);
			ImGui::BeginDisabled(!im->m_dbg_use_fake_camera);
			{
				ImGui::SliderFloat3("Camera Position (X, Y, Z)", im->m_dbg_camera_pos, -10000.0f, 10000.0f);
				ImGui::SliderFloat("Yaw (Y-axis)", &im->m_dbg_camera_yaw, -180.0f, 180.0f);
				ImGui::SliderFloat("Pitch (X-axis)", &im->m_dbg_camera_pitch, -90.0f, 90.0f);

				// Projection matrix adjustments
				ImGui::SliderFloat("FOV", &im->m_dbg_camera_fov, 1.0f, 180.0f);
				ImGui::SliderFloat("Aspect Ratio", &im->m_dbg_camera_aspect, 0.2f, 3.555f);
				ImGui::SliderFloat("Near Plane", &im->m_dbg_camera_near_plane, 0.1f, 1000.0f);
				ImGui::SliderFloat("Far Plane", &im->m_dbg_camera_far_plane, 1.0f, 100000.0f);

				ImGui::EndDisabled();
			}
			SPACEY8;
		}

		if (ImGui::CollapsingHeader("Statistics ..."))
		{
			SPACEY8;
			im->m_stats.enable_tracking(true);
			im->m_stats.draw_stats();
			SPACEY8;
		} else {
			im->m_stats.enable_tracking(false);
		}
	}

	void imgui::tab_dev()
	{
		dev_debug_container();
	}

	void imgui::tab_lights()
	{
		if (const auto lights = headlights::get(); lights) {
			lights->draw_menu();
		}
		if (const auto lens = lens_lights::get(); lens)
		{
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
			lens->draw_menu();
		}
		if (const auto lamps = street_lights::get(); lamps)
		{
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
			lamps->draw_menu();
		}
	}

	void imgui::tab_sun()
	{
		if (const auto light = sun::get(); light) {
			light->draw_menu();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		sky_section();
	}

	void imgui::sky_section()
	{
		auto& config = shared::common::config::get();
		auto& sky = config.sky;

		if (shared::common::remix_api::has_atmosphere())
		{
			int choice = sky.physical ? 0 : 1;
			ImGui::RadioButton("Physical sky (Numos)", &choice, 0); ImGui::SameLine();
			ImGui::RadioButton("Game's own sky", &choice, 1);
			sky.physical = choice == 0;
			ImGui::TextWrapped("Remix Plus: the runtime's physical sky with its own sun, clouds and haze, or the "
				"track's original sky as on stock Remix. Switch here rather than in Remix's own menu, so the "
				"proxy draws the game's sky and places the sun to match. [Sky] PhysicalSky.");
			ImGui::Spacing();
		}

		ImGui::Checkbox("Dynamic sky brightness", &sky.dynamic_brightness);
		ImGui::TextWrapped("Sets RTX Remix's sky brightness per track from how much light its sky throws on "
			"the ground, so bright skies do not flood the track and dark ones do not leave it dim. "
			"Night skies are left dark. Off, rtx.conf's sky brightness applies to every track.");

		if (time_of_day::physical_sky()) {
			ImGui::TextDisabled("The physical sky is lit by its own sun; this applies to the game's sky only.");
		}
		else if (const auto inject = brender_inject::get(); inject)
		{
			const auto lighting = inject->sky_lighting();
			if (std::isnan(lighting.brightness)) {
				ImGui::TextDisabled("No sky set yet (not in a race).");
			}
			else {
				ImGui::TextDisabled("This sky lights the ground at %.3f of a white sky; sky brightness %.2f",
					lighting.ground_light, lighting.brightness);
			}
		}

		save_row(sky.dynamic_brightness != m_saved_dynamic_sky || sky.physical != m_saved_physical_sky,
			[&] {
				config.set_bool("Sky", "DynamicBrightness", sky.dynamic_brightness);
				config.set_bool("Sky", "PhysicalSky", sky.physical);
				m_saved_dynamic_sky = sky.dynamic_brightness;
				m_saved_physical_sky = sky.physical;
			},
			[&] {
				sky.dynamic_brightness = config.get_bool("Sky", "DynamicBrightness", true);
				sky.physical = config.get_bool("Sky", "PhysicalSky", true);
				m_saved_dynamic_sky = sky.dynamic_brightness;
				m_saved_physical_sky = sky.physical;
			},
			[&] {
				sky.dynamic_brightness = true;
				sky.physical = true;
			});
	}

	// Save / Reload / Defaults for a tab whose settings live in remix-comp-proxy.ini. Changes
	// take effect at once and are written only when asked, as the other tabs do.
	void imgui::save_row(const bool dirty, const std::function<void()>& save,
		const std::function<void()>& reload, const std::function<void()>& defaults)
	{
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		if (ImGui::Button("Save to ini")) { save(); }
		ImGui::SameLine();
		if (ImGui::Button("Reload from ini")) { reload(); }
		ImGui::SameLine();
		if (ImGui::Button("Defaults")) { defaults(); }
		ImGui::SameLine();
		ImGui::TextDisabled(dirty ? "unsaved changes" : "remix-comp-proxy.ini");
	}

	void imgui::tab_effects()
	{
		auto& config = shared::common::config::get();
		auto& effects = config.effects;

		ImGui::Checkbox("Cull closed meshes", &effects.cull_closed_meshes);
		ImGui::TextWrapped(
			"Keeps backface culling on for closed meshes the game has made two-sided. "
			"A car door that flaps open is made two-sided; when it is a closed shell "
			"crushed flat, its paint and interior panel otherwise flicker against each other. "
			"Takes effect at once on cars and anything moving; baked scenery follows on the "
			"next track load. [Effects] CullClosedMeshes.");
		if (!effects.backface_culling) {
			ImGui::TextDisabled("Backface culling is off ([Effects] BackfaceCulling), so this does nothing.");
		}

		ImGui::Separator();

		auto& timing = config.timing;
		ImGui::Checkbox("Frame pacing", &timing.frame_pacing);
		ImGui::SliderFloat("Pacing fraction", &timing.frame_pacing_fraction, 0.5f, 1.0f, "%.2f of average");
		const auto& pacer = frame_pacer::get();
		ImGui::TextDisabled("Average frame %.1f ms, last hold %.1f ms", pacer.average_interval_ms(), pacer.last_wait_ms());
		ImGui::TextWrapped(
			"Holds a frame that arrives in a burst until the frames are evenly spaced. On a burst frame the "
			"game places the cars wrongly, which shakes the chase camera up and down on slopes (Bob Slay). "
			"Takes effect at once. [Timing] FramePacing, FramePacingFraction.");

		ImGui::Separator();

		ImGui::Checkbox("Additive car flames", &effects.additive_car_flames);
		ImGui::TextWrapped(
			"On: car flames (FLM01..FLM20) glow through the additive sprite path, like the "
			"explosions. Off: they stay solid cut-outs lit by the mod's emissive masks. "
			"Takes effect at once. [Effects] AdditiveCarFlames.");

		const bool dirty = effects.cull_closed_meshes != m_saved_effects.cull_closed_meshes
			|| effects.additive_car_flames != m_saved_effects.additive_car_flames;
		save_row(dirty,
			[&] {
				config.set_bool("Effects", "CullClosedMeshes", effects.cull_closed_meshes);
				config.set_bool("Effects", "AdditiveCarFlames", effects.additive_car_flames);
				m_saved_effects = { effects.cull_closed_meshes, effects.additive_car_flames };
			},
			[&] {
				effects.cull_closed_meshes = config.get_bool("Effects", "CullClosedMeshes", true);
				effects.additive_car_flames = config.get_bool("Effects", "AdditiveCarFlames", true);
				m_saved_effects = { effects.cull_closed_meshes, effects.additive_car_flames };
			},
			[&] {
				effects.cull_closed_meshes = true;
				effects.additive_car_flames = true;
			});
	}

	imgui::fog_settings imgui::fog_now()
	{
		const auto& effects = shared::common::config::get().effects;
		return { effects.fog, effects.fog_volumetrics, effects.fog_level_edge, effects.fog_distance, effects.fog_tint };
	}

	void imgui::tab_fog()
	{
		auto& config = shared::common::config::get();
		auto& effects = config.effects;
		const auto inject = brender_inject::get();
		const auto resync = [inject] { if (inject) { inject->resync_fog(); } };

		const game::scene_fog fog = game::read_scene_fog();
		if (fog.enabled)
		{
			ImGui::Text("This track's fog: colour %06X, %.1f to %.1f world units",
				fog.colour, fog.min_distance, fog.max_distance);
			if (inject && effects.fog)
			{
				const auto& handed = inject->handed_fog();
				ImGui::TextDisabled("Handed to Remix as %.1f to %.1f (x %.1f the track's own)%s",
					handed.start, handed.end, handed.end / fog.max_distance,
					effects.fog_level_edge && !handed.at_level_edge ? ", level not mapped yet" : "");
			}
		}
		else {
			ImGui::TextDisabled("No fog on this track (or not in a race).");
		}

		ImGui::Spacing();
		ImGui::Checkbox("Fog", &effects.fog);
		ImGui::SameLine();
		ImGui::TextDisabled("the track's depth cue, read by Remix's legacy fog remapping");

		if (ImGui::Checkbox("Fog colour in the volumetrics", &effects.fog_volumetrics)) {
			resync();
		}

		ImGui::BeginDisabled(!effects.fog);
		ImGui::Checkbox("Fog at the level's edge", &effects.fog_level_edge);
		ImGui::TextWrapped(effects.fog_level_edge
			? "The fog is full at the level's edge in the direction you are looking: its far side when looking "
			  "across the level, a nearby boundary when facing one. It never comes closer than the track's own fog."
			: "The track's own fog distances, as the game set them to hide where it stopped drawing.");

		ImGui::SliderFloat("Distance", &effects.fog_distance, 0.5f, 10.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
		ImGui::TextWrapped(effects.fog_level_edge
			? "Scales the level-edge distance: 1 is full fog exactly at the level's far side, below 1 "
			  "fogs it out sooner, above 1 leaves it showing."
			: "How far you see through the fog, as a multiple of the track's own. 2 sees twice as far.");

		if (ImGui::SliderFloat("Tint", &effects.fog_tint, 0.0f, 0.5f, "%.2f")) {
			resync();
		}
		ImGui::TextWrapped("How much of the fog's hue colours what you see through it. "
			"High values darken coloured fog, since the sky is the main light.");
		ImGui::EndDisabled();

		const fog_settings now = fog_now();
		save_row(!(now == m_saved_fog),
			[&] {
				config.set_bool("Effects", "Fog", effects.fog);
				config.set_bool("Effects", "FogVolumetrics", effects.fog_volumetrics);
				config.set_bool("Effects", "FogLevelEdge", effects.fog_level_edge);
				config.set_float("Effects", "FogDistance", effects.fog_distance);
				config.set_float("Effects", "FogTint", effects.fog_tint);
				m_saved_fog = now;
			},
			[&] {
				effects.fog = config.get_bool("Effects", "Fog", true);
				effects.fog_volumetrics = config.get_bool("Effects", "FogVolumetrics", true);
				effects.fog_level_edge = config.get_bool("Effects", "FogLevelEdge", true);
				effects.fog_distance = config.get_float("Effects", "FogDistance", 1.0f);
				effects.fog_tint = config.get_float("Effects", "FogTint", 0.08f);
				m_saved_fog = fog_now();
				resync();
			},
			[&] {
				effects.fog_level_edge = true;
				effects.fog_distance = 1.0f;
				effects.fog_tint = 0.08f;
				resync();
			});
	}

	void imgui::tab_conversion()
	{
		const auto inject = brender_inject::get();
		if (!inject) {
			ImGui::TextDisabled("The BRender injection is not running.");
			return;
		}

		bool converting = inject->conversion_enabled();
		if (ImGui::Checkbox("Conversion", &converting)) {
			inject->set_conversion(converting);
		}

		ImGui::TextWrapped(
			"On: the race is rebuilt as path-traced geometry, with the sky, the sun and the headlights. "
			"Off: the game renders its own frame, with its own culling and draw distance, as it would "
			"without this proxy. Switching back on rebuilds the static world, so expect a moment of "
			"extra load. Every start is converted; this is not saved.");
	}

	// -----------

	void tab_tracer()
	{
		auto* t = tracer::get();
		if (!t)
		{
			ImGui::TextColored(ImVec4(1, 0, 0, 1), "Tracer module not loaded");
			return;
		}

		SPACEY16;

		// Status
		if (t->is_capturing())
		{
			ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "CAPTURING");
			ImGui::SameLine();
			ImGui::Text("Frame %d / %d  (%d calls)",
				t->frames_captured(), t->frames_to_capture(), t->sequence());
			ImGui::ProgressBar(static_cast<float>(t->frames_captured()) / t->frames_to_capture());
		}
		else if (t->is_waiting())
		{
			float remaining = t->delay_remaining();
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "WAITING");
			ImGui::SameLine();
			ImGui::Text("Capture starts in %.1fs", remaining);
		}
		else
		{
			ImGui::Text("Status: IDLE");
		}

		SPACEY8;
		ImGui::Separator();
		SPACEY8;

		// Controls
		static int frames = 2;
		ImGui::SliderInt("Frames to capture", &frames, 1, 10);

		static int bt_depth = 8;
		ImGui::SliderInt("Backtrace depth", &bt_depth, 0, 16);

		static float delay_seconds = 0.0f;
		ImGui::SliderFloat("Delay (seconds)", &delay_seconds, 0.0f, 30.0f, "%.1f");

		SPACEY8;

		// Category filters
		if (ImGui::TreeNode("Capture Filters"))
		{
			uint32_t mask = t->category_mask();
			auto flag = [&](const char* label, trace_category cat)
			{
				bool on = (mask & cat) != 0;
				if (ImGui::Checkbox(label, &on))
					mask = on ? (mask | cat) : (mask & ~cat);
			};

			flag("Draw calls",  TRACE_DRAW);
			flag("State",       TRACE_STATE);
			flag("Shaders",     TRACE_SHADERS);
			flag("Textures",    TRACE_TEXTURES);
			flag("Transforms",  TRACE_TRANSFORMS);
			flag("Vertex setup", TRACE_VERTEX);
			flag("Resources",   TRACE_RESOURCES);
			flag("Scene flow",  TRACE_SCENE);
			flag("Get* calls",  TRACE_GETTERS);
			flag("Misc",        TRACE_MISC);

			if (mask != t->category_mask())
				t->set_category_mask(mask);

			ImGui::SameLine();
			if (ImGui::SmallButton("All"))
				t->set_category_mask(TRACE_ALL);
			ImGui::SameLine();
			if (ImGui::SmallButton("Default"))
				t->set_category_mask(TRACE_DEFAULT);

			ImGui::TreePop();
		}

		SPACEY8;

		// Editable filename
		static char filename_buf[256] = {};
		static bool filename_initialized = false;
		if (!filename_initialized || (!t->is_capturing() && filename_buf[0] == '\0'))
		{
			auto default_name = tracer::generate_default_filename();
			strncpy_s(filename_buf, default_name.c_str(), sizeof(filename_buf) - 1);
			filename_initialized = true;
		}

		ImGui::InputText("Filename", filename_buf, sizeof(filename_buf));
		ImGui::SameLine();
		ImGui::TextDisabled(".jsonl");

		SPACEY8;

		ImGui::BeginDisabled(t->is_capturing() || t->is_waiting());
		if (ImGui::Button("Start Capture", ImVec2(200, 0)))
		{
			t->set_backtrace_depth(bt_depth);
			std::string fname = filename_buf;
			if (fname.empty())
				fname = tracer::generate_default_filename();
			t->start_capture_delayed(frames, fname, delay_seconds);
			filename_buf[0] = '\0';
			filename_initialized = false;
		}
		ImGui::EndDisabled();

		ImGui::SameLine();

		ImGui::BeginDisabled(!t->is_capturing() && !t->is_waiting());
		if (ImGui::Button(t->is_waiting() ? "Cancel" : "Stop Capture", ImVec2(200, 0)))
		{
			if (t->is_waiting())
				t->cancel_delayed();
			else
				t->stop_capture();
		}
		ImGui::EndDisabled();

		// Last capture info
		SPACEY16;
		if (!t->last_capture_path().empty())
		{
			ImGui::Separator();
			SPACEY8;
			ImGui::Text("Last capture:");
			ImGui::Text("  File: %s", t->last_capture_path().c_str());
			ImGui::Text("  Size: %.1f KB", t->last_capture_size() / 1024.0);
			ImGui::Text("  Records: %d", t->last_capture_records());
			ImGui::Text("  Output dir: %s", t->output_dir().c_str());
		}
	}

	void tab_diagnostics()
	{
		auto* d = diagnostics::get();
		if (!d)
		{
			ImGui::TextColored(ImVec4(1, 0, 0, 1), "Diagnostics module not loaded");
			ImGui::TextWrapped("Set [Diagnostics] Enabled=1 in remix-comp-proxy.ini to enable.");
			return;
		}

		SPACEY16;

		// Status
		if (d->is_capturing())
		{
			ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "CAPTURING");
			ImGui::SameLine();
			ImGui::Text("Frame %d / %d", d->frames_captured(), d->frames_to_capture());
			ImGui::ProgressBar(static_cast<float>(d->frames_captured()) / d->frames_to_capture());
		}
		else
		{
			ImGui::Text("Status: IDLE");
		}

		SPACEY8;
		ImGui::Separator();
		SPACEY8;

		// Capture controls
		static int diag_frames = 3;
		ImGui::SliderInt("Frames to capture", &diag_frames, 1, 30);

		SPACEY8;

		ImGui::BeginDisabled(d->is_capturing());
		if (ImGui::Button("Start Capture", ImVec2(200, 0)))
			d->start_capture(diag_frames);
		ImGui::EndDisabled();

		ImGui::SameLine();

		ImGui::BeginDisabled(!d->is_capturing());
		if (ImGui::Button("Stop Capture", ImVec2(200, 0)))
			d->stop_capture();
		ImGui::EndDisabled();

		// Last capture info
		if (!d->last_log_path().empty())
		{
			SPACEY8;
			ImGui::Text("Last log: %s", d->last_log_path().c_str());
		}

		SPACEY16;
		ImGui::Separator();
		SPACEY8;

		// Log category checkboxes
		ImGui::Text("Log Categories:");
		SPACEY8;

		ImGui::Checkbox("Draw Calls", &d->cat_draw_calls);
		ImGui::SameLine(220);
		ImGui::TextDisabled("DIP/DP params, strides, decl flags");

		ImGui::Checkbox("VS Constants", &d->cat_vs_constants);
		ImGui::SameLine(220);
		ImGui::TextDisabled("Register writes, matrix values");

		ImGui::Checkbox("Vertex Data", &d->cat_vertex_data);
		ImGui::SameLine(220);
		ImGui::TextDisabled("Raw vertex bytes for early draws");

		ImGui::Checkbox("Declarations", &d->cat_declarations);
		ImGui::SameLine(220);
		ImGui::TextDisabled("Vertex element breakdown");

		ImGui::Checkbox("Textures", &d->cat_textures);
		ImGui::SameLine(220);
		ImGui::TextDisabled("Stage bindings, unique counts");

		ImGui::Checkbox("Present Info", &d->cat_present_info);
		ImGui::SameLine(220);
		ImGui::TextDisabled("Frame summary, VS regs written");
	}

	// -----------

	void imgui::devgui()
	{
		ImGui::SetNextWindowSize(ImVec2(900, 800), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Remix Comp - FFP Proxy", &shared::globals::imgui_menu_open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollWithMouse))
		{
			ImGui::End();
			return;
		}

		m_im_window_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow);
		m_im_window_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);

#define ADD_TAB(NAME, FUNC) \
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_ChildBg));					\
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x + 12.0f, 8));	\
	if (ImGui::BeginTabItem(NAME)) {																		\
		ImGui::PopStyleVar(1);																				\
		if (ImGui::BeginChild("##child_" NAME, ImVec2(0, ImGui::GetContentRegionAvail().y - 38), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_AlwaysVerticalScrollbar )) {	\
			FUNC(); ImGui::EndChild();																		\
		} else {																							\
			ImGui::EndChild();																				\
		} ImGui::EndTabItem();																				\
	} else { ImGui::PopStyleVar(1); } ImGui::PopStyleColor();

		// ---------------------------------------

		// Aero-style frosted glass header gradient
		const auto col_top = ImGui::ColorConvertFloat4ToU32(ImVec4(0.22f, 0.38f, 0.62f, 0.58f));
		const auto col_bottom = ImGui::ColorConvertFloat4ToU32(ImVec4(0.10f, 0.20f, 0.38f, 0.72f));
		const auto col_border = ImGui::ColorConvertFloat4ToU32(ImVec4(0.30f, 0.48f, 0.72f, 0.38f));
		const auto pre_tabbar_spos = ImGui::GetCursorScreenPos() - ImGui::GetStyle().WindowPadding;

		ImGui::GetWindowDrawList()->AddRectFilledMultiColor(pre_tabbar_spos, pre_tabbar_spos + ImVec2(ImGui::GetWindowWidth(), 40.0f),
			col_top, col_top, col_bottom, col_bottom);

		ImGui::GetWindowDrawList()->AddLine(pre_tabbar_spos + ImVec2(0, 40.0f), pre_tabbar_spos + ImVec2(ImGui::GetWindowWidth(), 40.0f),
			col_border, 1.0f);

		ImGui::SetCursorScreenPos(pre_tabbar_spos + ImVec2(12,8));

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x + 12.0f, 8));
		ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.18f, 0.32f, 0.54f, 0.95f));
		if (ImGui::BeginTabBar("devgui_tabs"))
		{
			ImGui::PopStyleColor();
			ImGui::PopStyleVar(1);
			ADD_TAB("Lights", tab_lights);
			ADD_TAB("Sun", tab_sun);
			ADD_TAB("Fog", tab_fog);
			ADD_TAB("Effects", tab_effects);
			ADD_TAB("Conversion", tab_conversion);
			ADD_TAB("Diagnostics", tab_diagnostics);
			ADD_TAB("Tracer", tab_tracer);
			ADD_TAB("Dev", tab_dev);
			ADD_TAB("About", tab_about);
			ImGui::EndTabBar();
		}
		else {
			ImGui::PopStyleColor();
			ImGui::PopStyleVar(1);
		}
#undef ADD_TAB

		{
			ImGui::Separator();
			const char* movement_hint_str = "Hold Right Mouse to enable Game Input ";

			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
			{
				ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().ItemSpacing.y);
				const auto spos = ImGui::GetCursorScreenPos();
				ImGui::TextUnformatted(m_devgui_custom_footer_content.c_str());
				ImGui::SetCursorScreenPos(spos);
				m_devgui_custom_footer_content.clear();
			}

			const float hint_width = ImGui::CalcTextSize(movement_hint_str).x;
			ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionAvail().x - hint_width - 6.0f, ImGui::GetCursorPosY() + 2.0f));
			ImGui::TextUnformatted(movement_hint_str);
		}
		ImGui::PopStyleVar(1);
		ImGui::End();
	}

	/*
	 * Lays ImGui out in the back buffer's pixels. The Win32 backend sizes it from the window's
	 * client area, but nGlide renders at its own resolution and Present scales that into the
	 * window: at 1080p in a 4K window, the client area is twice the back buffer, and anything
	 * centred on it lands at the right edge of the picture. The cursor, which the backend
	 * reads in client pixels, is scaled the same way.
	 */
	static void match_display_to_back_buffer(const uint32_t width, const uint32_t height)
	{
		auto& io = ImGui::GetIO();
		const ImVec2 client = io.DisplaySize;
		if (width == 0 || height == 0 || client.x <= 0.0f || client.y <= 0.0f) {
			return;
		}
		if (client.x == static_cast<float>(width) && client.y == static_cast<float>(height)) {
			return;
		}

		io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));

		POINT cursor{};
		if (GetForegroundWindow() == shared::globals::main_window && GetCursorPos(&cursor)
			&& ScreenToClient(shared::globals::main_window, &cursor))
		{
			io.AddMousePosEvent(static_cast<float>(cursor.x) * width / client.x,
				static_cast<float>(cursor.y) * height / client.y);
		}
	}

	void imgui::on_present(const uint32_t width, const uint32_t height)
	{
		if (auto* im = imgui::get(); im)
		{
			if (const auto dev = shared::globals::d3d_device; dev)
			{
				if (!im->m_initialized_device)
				{
					//Sleep(1000);
					shared::common::log("ImGui", "ImGui_ImplDX9_Init");
					ImGui_ImplDX9_Init(dev);
					im->m_initialized_device = true;
				}

				// else so we render the first frame one frame later
				else if (im->m_initialized_device)
				{
					// handle srgb
					DWORD og_srgb_samp, og_srgb_write;
					dev->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &og_srgb_samp);
					dev->GetRenderState(D3DRS_SRGBWRITEENABLE, &og_srgb_write);
					dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 1);
					dev->SetRenderState(D3DRS_SRGBWRITEENABLE, 1);

					ImGui_ImplDX9_NewFrame();
					ImGui_ImplWin32_NewFrame();
					match_display_to_back_buffer(width, height);
					ImGui::NewFrame();

					auto& io = ImGui::GetIO();

					if (shared::globals::imgui_allow_input_bypass_timeout) {
						shared::globals::imgui_allow_input_bypass_timeout--;
					}

					shared::globals::imgui_wants_text_input = ImGui::GetIO().WantTextInput;

					if (shared::globals::imgui_menu_open) 
					{
						io.MouseDrawCursor = true;
						im->devgui();

						// ---
						// enable game input via right mouse button logic

						if (!im->m_im_window_hovered && io.MouseDown[1])
						{
							// reset stuck rmb if timeout is active 
							if (shared::globals::imgui_allow_input_bypass_timeout)
							{
								io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
								shared::globals::imgui_allow_input_bypass_timeout = 0u;
							}

							// enable game input if no imgui window is hovered and right mouse is held
							else
							{
								ImGui::SetWindowFocus(); // unfocus input text
								shared::globals::imgui_allow_input_bypass = true;
							}
						}

						// ^ wait until mouse is up
						else if (shared::globals::imgui_allow_input_bypass && !io.MouseDown[1] && !shared::globals::imgui_allow_input_bypass_timeout)
						{
							shared::globals::imgui_allow_input_bypass_timeout = 2u;
							shared::globals::imgui_allow_input_bypass = false;
						}
					}
					else 
					{
						io.MouseDrawCursor = false;
						shared::globals::imgui_allow_input_bypass_timeout = 0u;
						shared::globals::imgui_allow_input_bypass = false;
					}

					if (im->m_stats.is_tracking_enabled()) {
						im->m_stats.reset_stats();
					}

					shared::globals::imgui_is_rendering = true;
					ImGui::EndFrame();
					ImGui::Render();
					ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
					shared::globals::imgui_is_rendering = false;

					// restore
					dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, og_srgb_samp);
					dev->SetRenderState(D3DRS_SRGBWRITEENABLE, og_srgb_write);
				}
			}
		}
	}

	void imgui::theme()
	{
		ImGuiStyle& style = ImGui::GetStyle();
		style.Alpha = 1.0f;
		style.DisabledAlpha = 0.5f;

		style.WindowPadding = ImVec2(8.0f, 10.0f);
		style.FramePadding = ImVec2(14.0f, 6.0f);
		style.ItemSpacing = ImVec2(10.0f, 5.0f);
		style.ItemInnerSpacing = ImVec2(4.0f, 8.0f);
		style.IndentSpacing = 16.0f;
		style.ColumnsMinSpacing = 10.0f;
		style.ScrollbarSize = 14.0f;
		style.GrabMinSize = 10.0f;

		style.WindowBorderSize = 1.0f;
		style.ChildBorderSize = 1.0f;
		style.PopupBorderSize = 1.0f;
		style.FrameBorderSize = 1.0f;
		style.TabBorderSize = 0.0f;

		style.WindowRounding = 6.0f;
		style.ChildRounding = 3.0f;
		style.FrameRounding = 3.0f;
		style.PopupRounding = 3.0f;
		style.ScrollbarRounding = 3.0f;
		style.GrabRounding = 2.0f;
		style.TabRounding = 4.0f;

		style.CellPadding = ImVec2(5.0f, 4.0f);

		auto& colors = style.Colors;

		// Text
		colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
		colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.60f, 0.72f, 1.00f);

		// Window / panels — Vista Aero glass
		colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.14f, 0.88f);
		colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.09f, 0.12f, 0.78f);
		colors[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.12f, 0.16f, 0.92f);
		colors[ImGuiCol_Border] = ImVec4(0.25f, 0.40f, 0.65f, 0.42f);
		colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.04f, 0.30f);

		// Frames (inputs, sliders, checkboxes)
		colors[ImGuiCol_FrameBg] = ImVec4(0.05f, 0.08f, 0.16f, 0.85f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.28f, 0.48f, 0.70f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.35f, 0.58f, 0.80f);

		// Title bar — Vista blue gradient
		colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.14f, 0.28f, 0.95f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.22f, 0.42f, 0.98f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.07f, 0.12f, 0.22f, 0.80f);
		colors[ImGuiCol_MenuBarBg] = ImVec4(0.07f, 0.10f, 0.18f, 0.95f);

		// Accent controls — Vista blue highlights
		colors[ImGuiCol_CheckMark] = ImVec4(0.42f, 0.65f, 0.92f, 1.00f);
		colors[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.48f, 0.75f, 1.00f);
		colors[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.60f, 0.88f, 1.00f);

		// Buttons — glass with blue hover
		colors[ImGuiCol_Button] = ImVec4(0.10f, 0.18f, 0.32f, 0.85f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.35f, 0.58f, 0.90f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.25f, 0.42f, 0.68f, 0.95f);

		// Headers (collapsing headers, tree nodes)
		colors[ImGuiCol_Header] = ImVec4(0.12f, 0.20f, 0.35f, 0.80f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.35f, 0.58f, 0.85f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.42f, 0.68f, 0.95f);

		// Separators
		colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.34f, 0.55f, 0.35f);
		colors[ImGuiCol_SeparatorHovered] = ImVec4(0.28f, 0.45f, 0.70f, 0.60f);
		colors[ImGuiCol_SeparatorActive] = ImVec4(0.35f, 0.55f, 0.82f, 0.80f);

		// Resize grip
		colors[ImGuiCol_ResizeGrip] = ImVec4(0.20f, 0.35f, 0.58f, 0.30f);
		colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.30f, 0.48f, 0.75f, 0.65f);
		colors[ImGuiCol_ResizeGripActive] = ImVec4(0.40f, 0.60f, 0.88f, 0.85f);

		// Tabs — Vista glass panels
		colors[ImGuiCol_Tab] = ImVec4(0.10f, 0.18f, 0.32f, 0.80f);
		colors[ImGuiCol_TabHovered] = ImVec4(0.22f, 0.38f, 0.62f, 0.90f);
		colors[ImGuiCol_TabSelected] = ImVec4(0.18f, 0.32f, 0.54f, 0.95f);
		colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.40f, 0.62f, 0.90f, 0.85f);
		colors[ImGuiCol_TabDimmed] = ImVec4(0.07f, 0.12f, 0.22f, 0.75f);
		colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.14f, 0.24f, 0.40f, 0.85f);
		colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.28f, 0.45f, 0.70f, 0.50f);

		// Scrollbar
		colors[ImGuiCol_ScrollbarBg] = ImVec4(0.04f, 0.06f, 0.12f, 0.80f);
		colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.16f, 0.28f, 0.48f, 0.70f);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.24f, 0.40f, 0.64f, 0.85f);
		colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.30f, 0.48f, 0.75f, 0.95f);

		// Tables
		colors[ImGuiCol_TableHeaderBg] = ImVec4(0.10f, 0.18f, 0.30f, 0.90f);
		colors[ImGuiCol_TableBorderStrong] = ImVec4(0.20f, 0.32f, 0.52f, 0.40f);
		colors[ImGuiCol_TableBorderLight] = ImVec4(0.16f, 0.26f, 0.44f, 0.25f);
		colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.08f, 0.14f, 0.24f, 0.20f);

		// Selection, navigation, modals
		colors[ImGuiCol_TextSelectedBg] = ImVec4(0.20f, 0.35f, 0.58f, 0.55f);
		colors[ImGuiCol_DragDropTarget] = ImVec4(0.40f, 0.62f, 0.90f, 0.80f);
		colors[ImGuiCol_NavCursor] = ImVec4(0.35f, 0.55f, 0.82f, 0.80f);
		colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.40f, 0.62f, 0.90f, 0.60f);
		colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.03f, 0.04f, 0.08f, 0.50f);
		colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.03f, 0.04f, 0.08f, 0.60f);
	}
	
	imgui::imgui()
	{
		p_this = this;

		const auto& effects = shared::common::config::get().effects;
		m_saved_effects = { effects.cull_closed_meshes, effects.additive_car_flames };
		m_saved_fog = fog_now();
		m_saved_dynamic_sky = shared::common::config::get().sky.dynamic_brightness;
		m_saved_physical_sky = shared::common::config::get().sky.physical;

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		//ImGuiIO& io = ImGui::GetIO(); (void)io;
		//io.MouseDrawCursor = true;
		//io.ConfigFlags |= ImGuiConfigFlags_IsSRGB;
		//io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

		theme();

		ImGui_ImplWin32_Init(shared::globals::main_window);
		g_game_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(shared::globals::main_window, GWLP_WNDPROC, LONG_PTR(wnd_proc_hk)));

		// ---
		m_initialized = true;
		shared::common::log("ImGui", "Module initialized.", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}
}



