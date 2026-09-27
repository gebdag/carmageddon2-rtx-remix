#include "std_include.hpp"
#include "street_lights.hpp"

#include "shared/common/remix_api.hpp"

namespace comp
{
	namespace
	{
		constexpr const char* INI_NAME = "carma2-streetlights.ini";
		constexpr const char* INI_SECTION = "StreetLights";
		constexpr float PI = 3.14159265f;

		/*
		 * The lamp models and where each one's light sits. &02lamp.act (newcity): the head is
		 * a wedge at the end of the arm, z 0.49 to 0.71 and y 1.34 to 1.42 in model space, its
		 * underside sloping to ~1.37 at z 0.6. The light sits just under that, aimed down.
		 */
		constexpr street_lights::lamp_kind LAMP_KINDS[] =
		{
			{ "&02lamp.act", { 0.0f, 1.33f, 0.60f }, { 0.0f, -1.0f, 0.0f } },
		};

		bool remix_lights_available()
		{
			using shared::common::remix_api;
			if (!remix_api::is_initialized()) {
				return false;
			}
			const auto& bridge = remix_api::get().m_bridge;
			return bridge.CreateLight && bridge.DestroyLight && bridge.DrawLightInstance;
		}

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
			const float length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
			if (length > 1e-6f) {
				for (int i = 0; i < 3; ++i) { out[i] /= length; }
			}
		}
	}

	std::string street_lights::ini_path()
	{
		return shared::globals::root_path + "\\" + INI_NAME;
	}

	// ------
	// settings

	void street_lights::load()
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

		s.enabled = GetPrivateProfileIntA(INI_SECTION, "Enabled", s.enabled ? 1 : 0, path.c_str()) != 0;
		read_float("ColourR", s.colour[0], 0.0f, 1.0f);
		read_float("ColourG", s.colour[1], 0.0f, 1.0f);
		read_float("ColourB", s.colour[2], 0.0f, 1.0f);
		read_float("Brightness", s.brightness, 0.0f, 100000.0f);
		read_float("EmitterRadius", s.emitter_radius, 0.001f, 0.5f);
		read_float("ConeAngle", s.cone_angle, 1.0f, 90.0f);
		read_float("ConeSoftness", s.cone_softness, 0.0f, 1.0f);

		m_settings = s;
		m_saved = s;
	}

	void street_lights::save()
	{
		const std::string path = ini_path();
		const settings& s = m_settings;

		const auto write = [&path](const char* key, const std::string& value) {
			return WritePrivateProfileStringA(INI_SECTION, key, value.c_str(), path.c_str()) != FALSE;
		};
		const auto number = [](const float value) { return std::format("{:.4f}", value); };

		bool ok = write("Enabled", s.enabled ? "1" : "0");
		ok &= write("ColourR", number(s.colour[0]));
		ok &= write("ColourG", number(s.colour[1]));
		ok &= write("ColourB", number(s.colour[2]));
		ok &= write("Brightness", number(s.brightness));
		ok &= write("EmitterRadius", number(s.emitter_radius));
		ok &= write("ConeAngle", number(s.cone_angle));
		ok &= write("ConeSoftness", number(s.cone_softness));

		if (ok)
		{
			m_saved = s;
			shared::common::log("StreetLights", std::format("saved {}", path));
		}
		else
		{
			shared::common::log("StreetLights", std::format("could not write {}", path),
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
	}

	// ------
	// lamps

	const street_lights::lamp_kind* street_lights::kind_of(const game::br_model* model)
	{
		if (const auto it = m_kinds.find(model); it != m_kinds.end()) {
			return it->second;
		}

		const lamp_kind* kind = nullptr;
		if (model->identifier && game::can_read(model->identifier, 1))
		{
			for (const auto& entry : LAMP_KINDS)
			{
				if (_stricmp(entry.model, model->identifier) == 0) {
					kind = &entry;
				}
			}
		}

		m_kinds[model] = kind;
		return kind;
	}

	void street_lights::on_model_drawn(const game::br_actor* actor, const game::br_model* model,
		const game::br_matrix34& model_to_world)
	{
		const lamp_kind* kind = kind_of(model);
		if (!kind) {
			return;
		}

		lamp& l = m_lamps[actor];
		float position[3], direction[3];
		transform_point(model_to_world, kind->head, position);
		transform_direction(model_to_world, kind->direction, direction);

		// A lamp that moved (knocked over) is described again.
		if (!std::equal(std::begin(position), std::end(position), std::begin(l.position))
			|| !std::equal(std::begin(direction), std::end(direction), std::begin(l.direction)))
		{
			std::copy(std::begin(position), std::end(position), std::begin(l.position));
			std::copy(std::begin(direction), std::end(direction), std::begin(l.direction));
			l.placed = true;
		}
		l.drawn = true;
	}

	void street_lights::describe(const game::br_actor* actor, lamp& l)
	{
		const settings& s = m_settings;
		const float radiance = s.brightness / (PI * s.emitter_radius * s.emitter_radius);

		remixapi_LightInfoSphereEXT sphere{};
		sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
		sphere.position = { l.position[0], l.position[1], l.position[2] };
		sphere.radius = s.emitter_radius;
		sphere.shaping_hasvalue = TRUE;
		sphere.shaping_value.direction = { l.direction[0], l.direction[1], l.direction[2] };
		sphere.shaping_value.coneAngleDegrees = s.cone_angle;
		sphere.shaping_value.coneSoftness = s.cone_softness;
		sphere.shaping_value.focusExponent = 0.0f;
		sphere.volumetricRadianceScale = 1.0f;

		remixapi_LightInfo info{};
		info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
		info.pNext = &sphere;
		info.hash = shared::utils::string_hash64(std::format("carma2-streetlight-{}-{:x}",
			m_generation, reinterpret_cast<uintptr_t>(actor)));
		info.radiance = { s.colour[0] * radiance, s.colour[1] * radiance, s.colour[2] * radiance };
		info.isDynamic = TRUE;

		remixapi_LightHandle handle = l.handle;
		if (shared::common::remix_api::get().m_bridge.CreateLight(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			if (!m_create_failed)
			{
				m_create_failed = true;
				shared::common::log("StreetLights", "Remix refused a street light (CreateLight failed)",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
			return;
		}
		l.handle = handle;
	}

	void street_lights::on_race_frame(const bool night)
	{
		m_lit = 0;
		const bool lit = night && m_settings.enabled && m_settings.brightness > 0.0f && remix_lights_available();

		if (lit)
		{
			// Every lamp is described again when a setting changes; otherwise each is only
			// drawn, which is what keeps it in this frame.
			const bool changed = !(m_described == m_settings);
			const auto& bridge = shared::common::remix_api::get().m_bridge;

			for (auto& [actor, l] : m_lamps)
			{
				if (!l.drawn) {
					continue;
				}
				if (changed || l.placed || !l.handle)
				{
					describe(actor, l);
					l.placed = false;
				}
				if (l.handle)
				{
					bridge.DrawLightInstance(l.handle);
					++m_lit;
				}
			}
			m_described = m_settings;
		}

		for (auto& [actor, l] : m_lamps) {
			l.drawn = false;
		}
	}

	void street_lights::destroy_all()
	{
		if (shared::common::remix_api::is_initialized())
		{
			const auto& bridge = shared::common::remix_api::get().m_bridge;
			for (auto& [actor, l] : m_lamps)
			{
				if (l.handle) {
					bridge.DestroyLight(l.handle);
				}
			}
		}
		m_lamps.clear();
		m_kinds.clear();
		m_described = {};
		++m_generation;
	}

	void street_lights::on_frame_without_race()
	{
		destroy_all();
	}

	// ------
	// menu

	void street_lights::draw_menu()
	{
		settings& s = m_settings;

		ImGui::Checkbox("Street lights at night", &s.enabled);
		ImGui::TextDisabled("%u lamp(s) lit, %u known on this track", m_lit, static_cast<uint32_t>(m_lamps.size()));
		ImGui::TextWrapped("A light under every street lamp's head in the night races. "
			"A lamp is about 1.4 units tall; the cone spreads down from its head.");

		ImGui::ColorEdit3("Lamp colour", s.colour);
		ImGui::SliderFloat("Lamp brightness", &s.brightness, 0.5f, 2000.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Lamp emitter radius", &s.emitter_radius, 0.005f, 0.2f, "%.3f units", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Lamp cone angle", &s.cone_angle, 10.0f, 90.0f, "%.1f deg");
		ImGui::SliderFloat("Lamp cone softness", &s.cone_softness, 0.0f, 1.0f, "%.2f");

		ImGui::Spacing();
		const bool dirty = !(m_settings == m_saved);
		if (ImGui::Button("Save street lights to ini")) {
			save();
		}
		ImGui::SameLine();
		if (ImGui::Button("Reload street lights")) {
			load();
		}
		ImGui::SameLine();
		if (ImGui::Button("Street light defaults")) {
			m_settings = settings{};
		}
		ImGui::SameLine();
		ImGui::TextDisabled(dirty ? "unsaved changes" : INI_NAME);
	}

	street_lights::street_lights()
	{
		p_this = this;
		load();
	}
}
