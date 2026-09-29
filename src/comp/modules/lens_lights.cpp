#include "std_include.hpp"
#include "lens_lights.hpp"
#include "headlights.hpp"

#include "shared/common/remix_api.hpp"

namespace comp
{
	namespace
	{
		constexpr const char* INI_NAME = "carma2-headlights.ini";
		constexpr const char* INI_SECTION = "LensLights";
		constexpr float PI = 3.14159265f;

		// Lamps placed where the headlight beams sit, for cars without lens geometry; their
		// ids live outside the range a model-derived lamp id can take.
		constexpr uint64_t MOUNT_ID = 0xFFFF'FFFF'0000'0000ull;

		bool remix_lights_available()
		{
			using shared::common::remix_api;
			if (!remix_api::is_initialized()) {
				return false;
			}
			const auto& bridge = remix_api::get().m_bridge;
			return bridge.CreateLight && bridge.DestroyLight && bridge.DrawLightInstance;
		}

		uint64_t mix(uint64_t x)
		{
			x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
			x ^= x >> 27; x *= 0x94D049BB133111EBull;
			return x ^ (x >> 31);
		}

		// Into a car's own space. Its matrix is a rotation and a translation; dividing by each
		// row's squared length also undoes a uniform scale.
		void to_car(const game::br_matrix34& t, const float v[3], const bool point, float out[3])
		{
			const float d[3] = {
				point ? v[0] - t.m[3][0] : v[0],
				point ? v[1] - t.m[3][1] : v[1],
				point ? v[2] - t.m[3][2] : v[2],
			};
			for (int row = 0; row < 3; ++row)
			{
				const float* axis = t.m[row];
				const float length_sq = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
				out[row] = length_sq > 1e-12f ? (d[0] * axis[0] + d[1] * axis[1] + d[2] * axis[2]) / length_sq : 0.0f;
			}
		}

		uint64_t lamp_key(const game::br_actor* master, const lens_lights::role r, const uint64_t id)
		{
			return mix(mix(reinterpret_cast<uintptr_t>(master)) ^ mix(id) ^ static_cast<uint64_t>(r));
		}

		uint8_t bit(const lens_lights::role r) { return static_cast<uint8_t>(1u << static_cast<int>(r)); }

		// Straight back from the car in world space, with the up/down part dropped: a car faces
		// down its local -Z, so its back is matrix row 2. False when the car points straight up
		// or down and has no level back.
		bool level_rear_direction(const game::race_car& car, float out[3])
		{
			const auto& back = car.master->t.m[2];
			const float length = std::sqrt(back[0] * back[0] + back[2] * back[2]);
			if (!(length > 1e-4f)) {
				return false;
			}
			out[0] = back[0] / length;
			out[1] = 0.0f;
			out[2] = back[2] / length;
			return true;
		}
	}

	// ------
	// settings

	void lens_lights::load()
	{
		const std::string path = shared::globals::root_path + "\\" + INI_NAME;
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
		const auto read_colour = [&read_float](const char* key, float (&colour)[3])
		{
			read_float((std::string(key) + "R").c_str(), colour[0], 0.0f, 1.0f);
			read_float((std::string(key) + "G").c_str(), colour[1], 0.0f, 1.0f);
			read_float((std::string(key) + "B").c_str(), colour[2], 0.0f, 1.0f);
		};

		s.enabled = GetPrivateProfileIntA(INI_SECTION, "Enabled", s.enabled ? 1 : 0, path.c_str()) != 0;
		read_colour("HeadColour", s.head_colour);
		read_float("HeadBrightness", s.head_brightness, 0.0f, 100000.0f);
		read_float("HeadRadius", s.head_radius, 0.001f, 1.0f);
		read_float("HeadLift", s.head_lift, 0.0f, 1.0f);
		read_colour("BrakeColour", s.brake_colour);
		read_float("BrakeBrightness", s.brake_brightness, 0.0f, 100000.0f);
		read_colour("ReverseColour", s.reverse_colour);
		read_float("ReverseBrightness", s.reverse_brightness, 0.0f, 100000.0f);
		read_float("Radius", s.radius, 0.001f, 0.2f);
		read_float("Lift", s.lift, 0.0f, 0.1f);
		read_float("ConeAngle", s.cone_angle, 1.0f, 90.0f);
		read_float("ConeSoftness", s.cone_softness, 0.0f, 1.0f);
		read_float("Range", s.range, 0.0f, 1000.0f);
		read_float("CivilianScale", s.civilian_scale, 0.0f, 4.0f);

		m_settings = s;
		m_saved = s;
	}

	void lens_lights::save()
	{
		const std::string path = shared::globals::root_path + "\\" + INI_NAME;
		const settings& s = m_settings;

		const auto write = [&path](const std::string& key, const std::string& value) {
			return WritePrivateProfileStringA(INI_SECTION, key.c_str(), value.c_str(), path.c_str()) != FALSE;
		};
		const auto number = [](const float value) { return std::format("{:.6g}", value); };
		const auto colour = [&](const char* key, const float (&c)[3]) {
			return write(std::string(key) + "R", number(c[0])) && write(std::string(key) + "G", number(c[1]))
				&& write(std::string(key) + "B", number(c[2]));
		};

		bool ok = write("Enabled", s.enabled ? "1" : "0");
		ok &= colour("HeadColour", s.head_colour);
		ok &= write("HeadBrightness", number(s.head_brightness));
		ok &= write("HeadRadius", number(s.head_radius));
		ok &= write("HeadLift", number(s.head_lift));
		ok &= colour("BrakeColour", s.brake_colour);
		ok &= write("BrakeBrightness", number(s.brake_brightness));
		ok &= colour("ReverseColour", s.reverse_colour);
		ok &= write("ReverseBrightness", number(s.reverse_brightness));
		ok &= write("Radius", number(s.radius));
		ok &= write("Lift", number(s.lift));
		ok &= write("ConeAngle", number(s.cone_angle));
		ok &= write("ConeSoftness", number(s.cone_softness));
		ok &= write("Range", number(s.range));
		ok &= write("CivilianScale", number(s.civilian_scale));

		if (ok)
		{
			m_saved = s;
			shared::common::log("LensLights", std::format("saved {}", path));
		}
		else
		{
			shared::common::log("LensLights", std::format("could not write {}", path),
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
	}

	// ------
	// lamps

	void lens_lights::note(const game::br_actor* master, const game::br_model* model, const role r, const uint64_t id,
		const float position[3], const float facing[3], const bool intact, std::vector<std::array<float, 3>> points)
	{
		spot s{ master, model, r, id };
		s.points = std::move(points);
		std::memcpy(s.position, position, sizeof(s.position));
		std::memcpy(s.facing, facing, sizeof(s.facing));
		s.intact = intact;
		m_spots.push_back(s);
	}

	lens_lights::head_lamp_side lens_lights::head_lamps(const game::br_actor* master, const int side) const
	{
		head_lamp_side out;
		for (const auto& s : m_spots)
		{
			if (s.master != master || s.r != role::head) {
				continue;
			}
			float position[3];
			to_car(master->t, s.position, true, position);
			if ((position[0] < 0.0f) != (side == 0)) {
				continue;
			}
			++out.total;
			out.intact += s.intact ? 1 : 0;

			if (!out.model) {
				out.model = s.model;
			}
			if (s.model != out.model) {
				continue;
			}
			for (const auto& p : s.points) {
				to_car(master->t, p.data(), true, out.points.emplace_back().data());
			}
		}
		return out;
	}

	void lens_lights::light(const uint64_t key, const role r, const float scale, const float position[3], const float facing[3])
	{
		const settings& s = m_settings;
		const float* colour = r == role::head ? s.head_colour : r == role::brake ? s.brake_colour : s.reverse_colour;
		const float brightness = scale
			* (r == role::head ? s.head_brightness : r == role::brake ? s.brake_brightness : s.reverse_brightness);
		if (!(brightness > 0.0f)) {
			return;
		}

		const float length = std::sqrt(facing[0] * facing[0] + facing[1] * facing[1] + facing[2] * facing[2]);
		if (!(length > 1e-6f)) {
			return;
		}
		const float dir[3] = { facing[0] / length, facing[1] / length, facing[2] / length };
		const float radius = r == role::head ? s.head_radius : s.radius;
		const float lift = r == role::head ? s.head_lift : s.lift;
		const float radiance = brightness / (PI * radius * radius);

		remixapi_LightInfoSphereEXT sphere{};
		sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
		sphere.position = { position[0] + dir[0] * lift, position[1] + dir[1] * lift, position[2] + dir[2] * lift };
		sphere.radius = radius;
		sphere.shaping_hasvalue = r == role::head ? FALSE : TRUE;
		sphere.shaping_value.direction = { dir[0], dir[1], dir[2] };
		sphere.shaping_value.coneAngleDegrees = s.cone_angle;
		sphere.shaping_value.coneSoftness = s.cone_softness;
		sphere.shaping_value.focusExponent = 0.0f;
		sphere.volumetricRadianceScale = 1.0f;

		lamp& l = m_lamps[key];

		// Remix Plus applies a destroy one scene frame late, so a lamp that went out and comes
		// back takes a new incarnation rather than its old hash.
		remixapi_LightInfo info{};
		info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
		info.pNext = &sphere;
		info.hash = mix(key ^ (static_cast<uint64_t>(l.incarnation) << 48)) | 1;
		info.radiance = { colour[0] * radiance, colour[1] * radiance, colour[2] * radiance };
		info.isDynamic = TRUE;

		const auto& bridge = shared::common::remix_api::get().m_bridge;
		remixapi_LightHandle handle = l.handle;
		if (bridge.CreateLight(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			if (!m_create_failed)
			{
				m_create_failed = true;
				shared::common::log("LensLights", "Remix refused a lens light (CreateLight failed)",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
			return;
		}
		l.handle = handle;
		l.drawn = true;
		bridge.DrawLightInstance(handle);
		++m_lit;
	}

	void lens_lights::on_race_frame(const float camera_pos[3])
	{
		m_lit = 0;
		for (auto& [key, l] : m_lamps) {
			l.drawn = false;
		}

		const auto lights = headlights::get();
		if (m_settings.enabled && lights && remix_lights_available())
		{
			game::collect_race_cars(m_cars);
			for (const auto& car : m_cars)
			{
				if (car.knackered || car.master->render_style == game::BR_RSTYLE_NONE) {
					continue;
				}
				if (!car.is_player)
				{
					const float d[3] = { car.master->t.m[3][0] - camera_pos[0], car.master->t.m[3][1] - camera_pos[1],
						car.master->t.m[3][2] - camera_pos[2] };
					if (std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) > m_settings.range) {
						continue;
					}
				}

				// Civilian traffic has no light bits: its headlights are all it shows.
				const auto spec = static_cast<const uint8_t*>(car.spec);
				const int bits = !car.civilian && game::can_read(spec + game::CAR_LIGHT_BITS, sizeof(int))
					? *reinterpret_cast<const int*>(spec + game::CAR_LIGHT_BITS) : 0;
				const float scale = car.civilian ? m_settings.civilian_scale : 1.0f;
				uint8_t wanted = 0;
				if (lights->lights_actor(car.master)) { wanted |= bit(role::head); }
				if (bits & game::CAR_LIGHT_BIT_BRAKE) { wanted |= bit(role::brake); }
				if (bits & game::CAR_LIGHT_BIT_REVERSE) { wanted |= bit(role::reverse); }
				if (!wanted) {
					continue;
				}

				// Brake and reverse lenses wrap round the body's corners and tilt with its panels,
				// so their own normals lift a light into the neighbouring bodywork. Those lights
				// face straight back from the car instead, level with the ground.
				float rear[3];
				const bool has_rear = level_rear_direction(car, rear);

				uint8_t covered = 0;
				for (const auto& s : m_spots)
				{
					if (s.master != car.master) {
						continue;
					}
					covered |= bit(s.r);
					if (!s.intact || !(wanted & bit(s.r))) {
						continue;
					}
					const bool rear_lamp = s.r != role::head;
					if (rear_lamp && !has_rear) {
						continue;
					}
					light(lamp_key(car.master, s.r, s.id), s.r, scale, s.position, rear_lamp ? rear : s.facing);
				}

				// Lamps painted into the body: where the beams sit, and the same points at the back.
				for (const role r : { role::head, role::brake, role::reverse })
				{
					if (!(wanted & bit(r)) || (covered & bit(r))) {
						continue;
					}
					for (int side = 0; side < 2; ++side)
					{
						const bool rear_lamp = r != role::head;
						float position[3], facing[3];
						if (lights->lamp_mount(car, side, rear_lamp, position, facing) && (!rear_lamp || has_rear)) {
							light(lamp_key(car.master, r, MOUNT_ID | static_cast<uint64_t>(side)), r, scale, position,
								rear_lamp ? rear : facing);
						}
					}
				}
			}
		}

		if (!remix_lights_available()) {
			return;
		}
		const auto& bridge = shared::common::remix_api::get().m_bridge;
		for (auto& [key, l] : m_lamps)
		{
			if (!l.drawn && l.handle)
			{
				bridge.DestroyLight(l.handle);
				l.handle = nullptr;
				++l.incarnation;
			}
		}
	}

	void lens_lights::destroy_all()
	{
		if (remix_lights_available())
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
		m_spots.clear();
		m_lit = 0;
	}

	void lens_lights::on_frame_without_race()
	{
		destroy_all();
	}

	// ------
	// menu

	void lens_lights::draw_menu()
	{
		settings& s = m_settings;

		ImGui::Checkbox("Lens lights", &s.enabled);
		ImGui::TextDisabled("%u lens light(s) lit", m_lit);
		ImGui::TextWrapped("Small lights on the car lamps themselves: the headlight lenses while the headlights are "
			"on, the brake lenses while braking, the reverse lenses while reversing. Cars with lamps painted into "
			"the body get them where the beams sit and at the matching points at the back. Headlight lens lights "
			"are unshaped spheres that light the bodywork around the lamp; brake and reverse face out of the lens.");

		ImGui::ColorEdit3("Headlight lens colour", s.head_colour);
		ImGui::SliderFloat("Headlight lens brightness", &s.head_brightness, 0.00001f, 0.1f, "%.5f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Headlight lens radius", &s.head_radius, 0.002f, 0.3f, "%.3f units", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Headlight lens lift", &s.head_lift, 0.0f, 0.3f, "%.3f units");
		ImGui::ColorEdit3("Brake colour", s.brake_colour);
		ImGui::SliderFloat("Brake brightness", &s.brake_brightness, 0.0f, 5.0f, "%.4f", ImGuiSliderFlags_Logarithmic);
		ImGui::ColorEdit3("Reverse colour", s.reverse_colour);
		ImGui::SliderFloat("Reverse brightness", &s.reverse_brightness, 0.0f, 5.0f, "%.4f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Brake/reverse radius", &s.radius, 0.002f, 0.05f, "%.3f units", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Brake/reverse lift", &s.lift, 0.0f, 0.1f, "%.3f units");
		ImGui::SliderFloat("Brake/reverse cone angle", &s.cone_angle, 10.0f, 90.0f, "%.1f deg");
		ImGui::SliderFloat("Brake/reverse cone softness", &s.cone_softness, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Lens light range", &s.range, 1.0f, 100.0f, "%.1f units");
		ImGui::SliderFloat("Civilian cars scale", &s.civilian_scale, 0.0f, 2.0f, "%.2f");

		ImGui::Spacing();
		const bool dirty = !(m_settings == m_saved);
		if (ImGui::Button("Save lens lights to ini")) {
			save();
		}
		ImGui::SameLine();
		if (ImGui::Button("Reload lens lights")) {
			load();
		}
		ImGui::SameLine();
		if (ImGui::Button("Lens light defaults")) {
			m_settings = settings{};
		}
		ImGui::SameLine();
		ImGui::TextDisabled(dirty ? "unsaved changes" : INI_NAME);
	}

	lens_lights::lens_lights()
	{
		p_this = this;
		load();
	}
}
