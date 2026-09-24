#include "std_include.hpp"
#include "headlights.hpp"

#include "shared/common/remix_api.hpp"

namespace comp
{
	namespace
	{
		constexpr const char* INI_NAME = "carma2-headlights.ini";
		constexpr const char* INI_SECTION = "Headlights";

		// A car's drawn box only changes when it is crushed or sheds a part.
		constexpr uint32_t BOUNDS_LIFETIME_FRAMES = 120;

		constexpr uint32_t MAX_TREE_ACTORS = 256;
		constexpr uint32_t MAX_TREE_DEPTH = 8;

		constexpr auto NOTICE_DURATION = std::chrono::milliseconds(2000);

		constexpr float DEG_TO_RAD = 3.14159265f / 180.0f;
		constexpr float PI = 3.14159265f;

		enum br_transform_type : uint16_t
		{
			BR_TRANSFORM_MATRIX34 = 0,
			BR_TRANSFORM_MATRIX34_LP = 1,
			BR_TRANSFORM_IDENTITY = 6,
		};

		const char* mode_name(const headlights::mode m)
		{
			switch (m)
			{
			case headlights::mode::player: return "player car";
			case headlights::mode::all_cars: return "all cars";
			default: return "off";
			}
		}

		constexpr game::br_matrix34 IDENTITY34 = { {
			{ 1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f },
		} };

		// BRender vectors are rows: p' = p * M, with the translation in row 3.
		void transform_point(const game::br_matrix34& m, const float p[3], float out[3])
		{
			for (int i = 0; i < 3; ++i) {
				out[i] = p[0] * m.m[0][i] + p[1] * m.m[1][i] + p[2] * m.m[2][i] + m.m[3][i];
			}
		}

		void transform_direction(const game::br_matrix34& m, const float d[3], float out[3])
		{
			for (int i = 0; i < 3; ++i) {
				out[i] = d[0] * m.m[0][i] + d[1] * m.m[1][i] + d[2] * m.m[2][i];
			}
		}

		// a then b
		game::br_matrix34 concatenate(const game::br_matrix34& a, const game::br_matrix34& b)
		{
			game::br_matrix34 out{};
			for (int row = 0; row < 4; ++row)
			{
				for (int col = 0; col < 3; ++col)
				{
					out.m[row][col] = a.m[row][0] * b.m[0][col] + a.m[row][1] * b.m[1][col]
						+ a.m[row][2] * b.m[2][col] + (row == 3 ? b.m[3][col] : 0.0f);
				}
			}
			return out;
		}

		/*
		 * An actor's transform as a matrix. Every br_transform variant keeps its translation
		 * where a matrix keeps row 3, so the ones that are not matrices still place the actor;
		 * their rotation is dropped, which only loosens a bounding box.
		 */
		game::br_matrix34 local_matrix(const game::br_actor* actor)
		{
			if (actor->t_type <= BR_TRANSFORM_MATRIX34_LP) {
				return actor->t;
			}

			game::br_matrix34 out = IDENTITY34;
			if (actor->t_type != BR_TRANSFORM_IDENTITY) {
				std::memcpy(out.m[3], actor->t.m[3], sizeof(out.m[3]));
			}
			return out;
		}

		struct box
		{
			float min[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
			float max[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

			bool empty() const { return min[0] > max[0]; }

			void add(const float p[3])
			{
				for (int i = 0; i < 3; ++i)
				{
					min[i] = std::min(min[i], p[i]);
					max[i] = std::max(max[i], p[i]);
				}
			}
		};

		void add_model_bounds(box& out, const game::br_model* model, const game::br_matrix34& to_car)
		{
			for (int corner = 0; corner < 8; ++corner)
			{
				const float p[3] = {
					(corner & 1) ? model->bounds_max.v[0] : model->bounds_min.v[0],
					(corner & 2) ? model->bounds_max.v[1] : model->bounds_min.v[1],
					(corner & 4) ? model->bounds_max.v[2] : model->bounds_min.v[2],
				};

				float in_car[3];
				transform_point(to_car, p, in_car);
				out.add(in_car);
			}
		}

		// Everything drawn by `actor`, its siblings and their subtrees, in the car's own space.
		void add_tree_bounds(box& out, const game::br_actor* actor, const game::br_matrix34& parent_to_car,
			const uint32_t depth, uint32_t& budget)
		{
			for (; actor && budget; actor = actor->next)
			{
				if (!game::can_read(actor, sizeof(*actor))) {
					return;
				}
				--budget;

				if (actor->render_style == game::BR_RSTYLE_NONE) {
					continue;
				}

				const game::br_matrix34 to_car = concatenate(local_matrix(actor), parent_to_car);

				if (actor->type == 1 && game::can_read(actor->model, sizeof(game::br_model))) {
					add_model_bounds(out, actor->model, to_car);
				}

				if (actor->children && depth < MAX_TREE_DEPTH) {
					add_tree_bounds(out, actor->children, to_car, depth + 1, budget);
				}
			}
		}

		float length(const float v[3])
		{
			return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		}
	}

	std::string headlights::ini_path()
	{
		return shared::globals::root_path + "\\" + INI_NAME;
	}

	uint64_t headlights::lamp_key(const void* car_spec, const int side)
	{
		return (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(car_spec)) << 1) | static_cast<uint64_t>(side);
	}

	// ------
	// settings

	void headlights::load()
	{
		const std::string path = ini_path();
		settings s{};

		const auto read_float = [&path](const char* key, float& value, const float lo, const float hi)
		{
			char buf[64];
			if (GetPrivateProfileStringA(INI_SECTION, key, "", buf, sizeof(buf), path.c_str()))
			{
				char* end = nullptr;
				const float parsed = std::strtof(buf, &end);
				if (end != buf) {
					value = std::clamp(parsed, lo, hi);
				}
			}
		};

		const auto read_bool = [&path](const char* key, const bool fallback) {
			return GetPrivateProfileIntA(INI_SECTION, key, fallback ? 1 : 0, path.c_str()) != 0;
		};

		const int start = GetPrivateProfileIntA(INI_SECTION, "StartMode", static_cast<int>(s.start_mode), path.c_str());
		s.start_mode = static_cast<mode>(std::clamp(start, 0, 2));

		read_float("Spacing", s.spacing, 0.0f, 1.5f);
		read_float("Height", s.height, 0.0f, 1.5f);
		read_float("Forward", s.forward, -0.5f, 1.0f);

		read_float("PitchDown", s.pitch_down, -30.0f, 45.0f);
		read_float("ToeOut", s.toe_out, -30.0f, 30.0f);
		read_float("ConeAngle", s.cone_angle, 1.0f, 90.0f);
		read_float("ConeSoftness", s.cone_softness, 0.0f, 1.0f);
		read_float("Focus", s.focus, 0.0f, 50.0f);

		read_float("ColourR", s.colour[0], 0.0f, 1.0f);
		read_float("ColourG", s.colour[1], 0.0f, 1.0f);
		read_float("ColourB", s.colour[2], 0.0f, 1.0f);
		read_float("Brightness", s.brightness, 0.0f, 100000.0f);
		read_float("EmitterRadius", s.emitter_radius, 0.001f, 0.2f);
		read_float("VolumetricScale", s.volumetric_scale, 0.0f, 10.0f);

		read_float("OtherCarsBrightness", s.other_brightness, 0.0f, 4.0f);
		read_float("OtherCarsRange", s.other_range, 0.0f, 1000.0f);
		s.wasted_stay_lit = read_bool("WastedStayLit", s.wasted_stay_lit);

		m_settings = s;
		m_saved = s;
	}

	void headlights::save()
	{
		const std::string path = ini_path();
		const settings& s = m_settings;

		const auto write = [&path](const char* key, const std::string& value) {
			return WritePrivateProfileStringA(INI_SECTION, key, value.c_str(), path.c_str()) != FALSE;
		};
		const auto number = [](const float value) { return std::format("{:.4f}", value); };

		bool ok = write("StartMode", std::to_string(static_cast<int>(s.start_mode)));
		ok &= write("Spacing", number(s.spacing));
		ok &= write("Height", number(s.height));
		ok &= write("Forward", number(s.forward));
		ok &= write("PitchDown", number(s.pitch_down));
		ok &= write("ToeOut", number(s.toe_out));
		ok &= write("ConeAngle", number(s.cone_angle));
		ok &= write("ConeSoftness", number(s.cone_softness));
		ok &= write("Focus", number(s.focus));
		ok &= write("ColourR", number(s.colour[0]));
		ok &= write("ColourG", number(s.colour[1]));
		ok &= write("ColourB", number(s.colour[2]));
		ok &= write("Brightness", number(s.brightness));
		ok &= write("EmitterRadius", number(s.emitter_radius));
		ok &= write("VolumetricScale", number(s.volumetric_scale));
		ok &= write("OtherCarsBrightness", number(s.other_brightness));
		ok &= write("OtherCarsRange", number(s.other_range));
		ok &= write("WastedStayLit", s.wasted_stay_lit ? "1" : "0");

		if (ok)
		{
			m_saved = s;
			shared::common::log("Headlights", std::format("saved {}", path));
		}
		else
		{
			shared::common::log("Headlights", std::format("could not write {}", path),
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
	}

	// ------
	// mode

	void headlights::cycle_mode()
	{
		m_mode = static_cast<mode>((static_cast<int>(m_mode) + 1) % 3);
		m_notice_until = std::chrono::steady_clock::now() + NOTICE_DURATION;
		shared::common::log("Headlights", std::format("headlights: {}", mode_name(m_mode)));
	}

	// ------
	// remix

	bool headlights::remix_lights_available()
	{
		using shared::common::remix_api;

		if (!remix_api::is_initialized()) {
			return false;
		}

		const auto& bridge = remix_api::get().m_bridge;
		return bridge.CreateLight && bridge.DestroyLight && bridge.DrawLightInstance;
	}

	void headlights::destroy_all()
	{
		if (m_lamps.empty()) {
			return;
		}

		if (shared::common::remix_api::is_initialized())
		{
			const auto& bridge = shared::common::remix_api::get().m_bridge;
			for (auto& [key, l] : m_lamps)
			{
				if (l.handle) {
					bridge.DestroyLight(l.handle);
				}
			}
		}

		m_lamps.clear();
		m_lit_cars = 0;
	}

	const headlights::car_bounds* headlights::measure(const game::race_car& car)
	{
		auto& cached = m_bounds[car.spec];
		if (cached.model == car.model && cached.measured_frame
			&& m_frame - cached.measured_frame < BOUNDS_LIFETIME_FRAMES)
		{
			return &cached;
		}

		box measured;
		uint32_t budget = MAX_TREE_ACTORS;
		add_tree_bounds(measured, car.master->children, IDENTITY34, 0, budget);

		if (measured.empty())
		{
			m_bounds.erase(car.spec);
			return nullptr;
		}

		std::memcpy(cached.min, measured.min, sizeof(cached.min));
		std::memcpy(cached.max, measured.max, sizeof(cached.max));
		cached.model = car.model;
		cached.measured_frame = m_frame;
		return &cached;
	}

	/*
	 * Remix has no call that moves a light. It derives the handle from the hash in the info
	 * and overwrites a matching entry, so describing the lamp again under the same hash is
	 * the update -- destroying it first would blink it out for a frame.
	 */
	void headlights::describe_lamp(const game::race_car& car, const car_bounds& bounds, const int side,
		const float brightness)
	{
		const settings& s = m_settings;
		const float sign = side == 0 ? -1.0f : 1.0f;

		// A car faces down its local -Z, so the front face of the box is min z.
		const float centre_x = 0.5f * (bounds.min[0] + bounds.max[0]);
		const float half_width = 0.5f * (bounds.max[0] - bounds.min[0]);
		const float local_pos[3] = {
			centre_x + sign * s.spacing * half_width,
			bounds.min[1] + s.height * (bounds.max[1] - bounds.min[1]),
			bounds.min[2] - s.forward,
		};

		const float pitch = s.pitch_down * DEG_TO_RAD;
		const float yaw = sign * s.toe_out * DEG_TO_RAD;
		const float local_dir[3] = {
			std::sin(yaw) * std::cos(pitch),
			-std::sin(pitch),
			-std::cos(yaw) * std::cos(pitch),
		};

		const game::br_matrix34& car_to_world = car.master->t;

		float pos[3];
		float dir[3];
		transform_point(car_to_world, local_pos, pos);
		transform_direction(car_to_world, local_dir, dir);

		const float dir_length = length(dir);
		if (!(dir_length > 1e-6f)) {
			return;
		}

		const float radiance = brightness / (PI * s.emitter_radius * s.emitter_radius);

		remixapi_LightInfoSphereEXT sphere{};
		sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
		sphere.position = { pos[0], pos[1], pos[2] };
		sphere.radius = s.emitter_radius;
		sphere.shaping_hasvalue = TRUE;
		sphere.shaping_value.direction = { dir[0] / dir_length, dir[1] / dir_length, dir[2] / dir_length };
		sphere.shaping_value.coneAngleDegrees = s.cone_angle;
		sphere.shaping_value.coneSoftness = s.cone_softness;
		sphere.shaping_value.focusExponent = s.focus;
		sphere.volumetricRadianceScale = s.volumetric_scale;

		const uint64_t key = lamp_key(car.spec, side);

		remixapi_LightInfo info{};
		info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
		info.pNext = &sphere;
		info.hash = shared::utils::string_hash64(std::format("carma2-headlight-{:x}", key));
		info.radiance = { s.colour[0] * radiance, s.colour[1] * radiance, s.colour[2] * radiance };

		// Remix puts a light it takes for static to sleep; a lamp on a parked car would be one.
		info.isDynamic = TRUE;

		const auto& bridge = shared::common::remix_api::get().m_bridge;
		lamp& l = m_lamps[key];

		remixapi_LightHandle handle = l.handle;
		if (bridge.CreateLight(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			if (!m_create_failed)
			{
				m_create_failed = true;
				shared::common::log("Headlights", "Remix refused a headlight (CreateLight failed)",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
			return;
		}

		l.handle = handle;
		l.drawn_this_frame = true;
		bridge.DrawLightInstance(handle);
	}

	void headlights::on_race_frame(const float camera_pos[3])
	{
		++m_frame;

		if (m_mode == mode::off)
		{
			destroy_all();
			return;
		}

		if (!remix_lights_available()) {
			return;
		}

		game::collect_race_cars(m_cars);

		for (auto& [key, l] : m_lamps) {
			l.drawn_this_frame = false;
		}

		m_lit_cars = 0;
		m_player_status = m_cars.empty() ? "no cars: the game is not in a race" : "not found";
		for (const auto& car : m_cars)
		{
			if (!car.is_player && m_mode != mode::all_cars) {
				continue;
			}

			// Why a car is dark is the first thing to know when its lights go missing, and
			// the player's car is the one being watched.
			const auto skip = [this, &car](const char* reason)
			{
				if (car.is_player) {
					m_player_status = reason;
				}
			};

			if (car.knackered && !m_settings.wasted_stay_lit)
			{
				skip("dark: the game has it flagged as wasted");
				continue;
			}
			if (car.master->render_style == game::BR_RSTYLE_NONE)
			{
				skip("dark: the game is not drawing the car");
				continue;
			}
			if (car.master->t_type > BR_TRANSFORM_MATRIX34_LP)
			{
				skip("dark: the car's actor carries no matrix");
				continue;
			}

			float brightness = m_settings.brightness;
			if (!car.is_player)
			{
				const float to_car[3] = {
					car.master->t.m[3][0] - camera_pos[0],
					car.master->t.m[3][1] - camera_pos[1],
					car.master->t.m[3][2] - camera_pos[2],
				};
				if (length(to_car) > m_settings.other_range) {
					continue;
				}
				brightness *= m_settings.other_brightness;
			}

			if (!(brightness > 0.0f)) {
				continue;
			}

			const car_bounds* bounds = measure(car);
			if (!bounds)
			{
				skip("dark: nothing under the car's actor could be measured");
				continue;
			}

			if (car.is_player)
			{
				m_player_status = std::format("lit - car {:.2f} x {:.2f} x {:.2f} units",
					bounds->max[0] - bounds->min[0], bounds->max[1] - bounds->min[1],
					bounds->max[2] - bounds->min[2]);
			}

			describe_lamp(car, *bounds, 0, brightness);
			describe_lamp(car, *bounds, 1, brightness);
			++m_lit_cars;
		}

		const auto& bridge = shared::common::remix_api::get().m_bridge;
		std::erase_if(m_lamps, [&bridge](auto& entry)
		{
			lamp& l = entry.second;
			if (l.drawn_this_frame) {
				return false;
			}
			if (l.handle) {
				bridge.DestroyLight(l.handle);
			}
			return true;
		});
	}

	void headlights::on_frame_without_race()
	{
		// Car specs and actors are recycled by the next race, so nothing measured in this
		// one may be carried into it.
		destroy_all();
		m_bounds.clear();
	}

	// ------
	// menu

	void headlights::draw_mode_notice()
	{
		if (std::chrono::steady_clock::now() >= m_notice_until) {
			return;
		}

		const std::string text = std::format("Headlights: {}", mode_name(m_mode));
		const ImVec2 size = ImGui::CalcTextSize(text.c_str());
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		const ImVec2 pos((display.x - size.x) * 0.5f, display.y * 0.12f);

		auto* draw_list = ImGui::GetForegroundDrawList();
		draw_list->AddRectFilled(pos - ImVec2(12.0f, 6.0f), pos + size + ImVec2(12.0f, 6.0f),
			IM_COL32(0, 0, 0, 170), 4.0f);
		draw_list->AddText(pos, IM_COL32(255, 238, 200, 255), text.c_str());
	}

	void headlights::draw_menu()
	{
		settings& s = m_settings;

		ImGui::TextUnformatted("H cycles: off -> player car -> all cars -> off");

		if (!shared::common::remix_api::is_initialized())
		{
			if (shared::common::remix_api::gave_up()) {
				ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
					"Remix API not available - .trex/bridge.conf needs 'exposeRemixApi = True'");
			}
			else {
				ImGui::TextDisabled("The Remix API starts with the first race frame.");
			}
		}
		else
		{
			ImGui::TextDisabled("%s: %u car(s) lit, %u Remix light(s)", shared::common::remix_api::runtime(),
				m_lit_cars, static_cast<uint32_t>(m_lamps.size()));
			if (m_mode != mode::off) {
				ImGui::TextDisabled("Player car: %s", m_player_status.c_str());
			}
		}

		ImGui::Spacing();

		int current = static_cast<int>(m_mode);
		ImGui::RadioButton("Off", &current, 0); ImGui::SameLine();
		ImGui::RadioButton("Player car", &current, 1); ImGui::SameLine();
		ImGui::RadioButton("All cars", &current, 2);
		m_mode = static_cast<mode>(current);

		int start = static_cast<int>(s.start_mode);
		if (ImGui::Combo("Mode at game start", &start, "Off\0Player car\0All cars\0")) {
			s.start_mode = static_cast<mode>(start);
		}

		if (ImGui::CollapsingHeader("Mounting", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::TextDisabled("Relative to each car's own size, so one setting fits an Eagle and a dump truck.");
			ImGui::SliderFloat("Spacing", &s.spacing, 0.0f, 1.2f, "%.2f of half width");
			ImGui::SliderFloat("Height", &s.height, 0.0f, 1.2f, "%.2f of body height");
			ImGui::SliderFloat("Ahead of nose", &s.forward, -0.3f, 0.5f, "%.3f units");
		}

		if (ImGui::CollapsingHeader("Beam", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::SliderFloat("Pitch down", &s.pitch_down, -15.0f, 30.0f, "%.1f deg");
			ImGui::SliderFloat("Toe out", &s.toe_out, -15.0f, 15.0f, "%.1f deg");
			ImGui::SliderFloat("Cone angle", &s.cone_angle, 5.0f, 90.0f, "%.1f deg");
			ImGui::SliderFloat("Cone softness", &s.cone_softness, 0.0f, 1.0f, "%.2f");
			ImGui::SliderFloat("Focus", &s.focus, 0.0f, 20.0f, "%.1f");
		}

		if (ImGui::CollapsingHeader("Lamp", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::ColorEdit3("Colour", s.colour);
			ImGui::SliderFloat("Brightness", &s.brightness, 0.05f, 5000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
			ImGui::SliderFloat("Emitter radius", &s.emitter_radius, 0.002f, 0.1f, "%.3f units", ImGuiSliderFlags_Logarithmic);
			ImGui::SliderFloat("Volumetric glow", &s.volumetric_scale, 0.0f, 5.0f, "%.2f");
			ImGui::TextDisabled("A car is about 0.4 units wide. The radius softens shadows; it does not change brightness.");
		}

		if (ImGui::CollapsingHeader("Other cars", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::SliderFloat("Brightness scale", &s.other_brightness, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat("Range from camera", &s.other_range, 1.0f, 100.0f, "%.1f units", ImGuiSliderFlags_Logarithmic);
			ImGui::Checkbox("Wasted cars stay lit", &s.wasted_stay_lit);
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		const bool dirty = !(m_settings == m_saved);
		if (ImGui::Button("Save to ini")) {
			save();
		}
		ImGui::SameLine();
		if (ImGui::Button("Reload from ini")) {
			load();
		}
		ImGui::SameLine();
		if (ImGui::Button("Defaults")) {
			m_settings = settings{};
		}
		ImGui::SameLine();
		ImGui::TextDisabled(dirty ? "unsaved changes" : INI_NAME);
	}

	// ------

	headlights::headlights()
	{
		p_this = this;

		load();
		m_mode = m_settings.start_mode;

		shared::common::log("Headlights", std::format("Module initialized, start mode: {}.", mode_name(m_mode)),
			shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}
}
