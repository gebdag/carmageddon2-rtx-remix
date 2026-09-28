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

		// Every signal holds red for this long after the crossing direction turns red, so
		// no two directions ever show green or amber together.
		constexpr float ALL_RED_SECONDS = 1.0f;

		// US signal colours: red, the amber of an incandescent lens, and the blue-green of
		// modern signal green.
		constexpr float RED[3] = { 1.0f, 0.06f, 0.02f };
		constexpr float AMBER[3] = { 1.0f, 0.55f, 0.02f };
		constexpr float GREEN[3] = { 0.15f, 1.0f, 0.55f };

		/*
		 * The lamp models (newcity). Where each light sits is a setting; these are the cone axes.
		 *
		 * &02lamp.act: the head is a wedge at the end of the arm, z 0.49 to 0.71 and y 1.34 to
		 * 1.42, its underside sloping to ~1.37 at z 0.6. The light sits just under that, aimed
		 * down.
		 *
		 * &03traffic.act: the signal head is a box at the end of the arm, x +-0.031, y 1.02 to
		 * 1.241, z 0.724 to 0.787. Its lenses (TRAFFICL) are on the end facing back along the
		 * arm towards the pole, so a light aimed out of them lights only the pole; the glow is
		 * instead a pool on the road straight under the head.
		 */
		constexpr street_lights::lamp_kind LAMP_KINDS[] =
		{
			{ "&02lamp.act", street_lights::lamp_style::street, { 0.0f, -1.0f, 0.0f } },
			{ "&03traffic.act", street_lights::lamp_style::signal, { 0.0f, -1.0f, 0.0f } },
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

		double seconds_now()
		{
			using clock = std::chrono::steady_clock;
			static const clock::time_point start = clock::now();
			return std::chrono::duration<double>(clock::now() - start).count();
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
		read_float("PositionX", s.position[0], -5.0f, 5.0f);
		read_float("PositionY", s.position[1], -5.0f, 5.0f);
		read_float("PositionZ", s.position[2], -5.0f, 5.0f);

		s.signals = GetPrivateProfileIntA(INI_SECTION, "Signals", s.signals ? 1 : 0, path.c_str()) != 0;
		read_float("SignalBrightness", s.signal_brightness, 0.0f, 100000.0f);
		read_float("SignalEmitterRadius", s.signal_radius, 0.001f, 0.5f);
		read_float("SignalConeAngle", s.signal_cone_angle, 1.0f, 90.0f);
		read_float("SignalConeSoftness", s.signal_cone_softness, 0.0f, 1.0f);
		read_float("SignalPositionX", s.signal_position[0], -5.0f, 5.0f);
		read_float("SignalPositionY", s.signal_position[1], -5.0f, 5.0f);
		read_float("SignalPositionZ", s.signal_position[2], -5.0f, 5.0f);
		read_float("SignalGreenSeconds", s.green_seconds, 1.0f, 120.0f);
		read_float("SignalAmberSeconds", s.amber_seconds, 0.5f, 30.0f);

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
		ok &= write("PositionX", number(s.position[0]));
		ok &= write("PositionY", number(s.position[1]));
		ok &= write("PositionZ", number(s.position[2]));

		ok &= write("Signals", s.signals ? "1" : "0");
		ok &= write("SignalBrightness", number(s.signal_brightness));
		ok &= write("SignalEmitterRadius", number(s.signal_radius));
		ok &= write("SignalConeAngle", number(s.signal_cone_angle));
		ok &= write("SignalConeSoftness", number(s.signal_cone_softness));
		ok &= write("SignalPositionX", number(s.signal_position[0]));
		ok &= write("SignalPositionY", number(s.signal_position[1]));
		ok &= write("SignalPositionZ", number(s.signal_position[2]));
		ok &= write("SignalGreenSeconds", number(s.green_seconds));
		ok &= write("SignalAmberSeconds", number(s.amber_seconds));

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
		l.kind = kind;

		// A lamp that moved (knocked over) is described again.
		if (std::memcmp(&model_to_world, &l.model_to_world, sizeof(model_to_world)) != 0)
		{
			l.model_to_world = model_to_world;
			l.placed = true;
		}
		l.drawn = true;
	}

	/*
	 * One fixed cycle for the whole city, as a coordinated grid would run it. A signal facing
	 * mostly along world x runs half a cycle behind one facing along z, so the two directions
	 * at a crossing alternate: green, amber, then red while the crossing direction has its
	 * green and amber, each change separated by a moment of all-red.
	 */
	street_lights::aspect street_lights::aspect_of(const lamp& l, const double seconds) const
	{
		if (l.kind->style == lamp_style::street) {
			return aspect::lit;
		}

		// The lenses face model -z; which world axis that lies along decides the phase.
		constexpr float LENS_FACING[3] = { 0.0f, 0.0f, -1.0f };
		float facing[3];
		transform_direction(l.model_to_world, LENS_FACING, facing);

		const double half = m_settings.green_seconds + m_settings.amber_seconds + ALL_RED_SECONDS;
		const bool crossing = std::abs(facing[0]) > std::abs(facing[2]);
		const double t = std::fmod(seconds + (crossing ? half : 0.0), 2.0 * half);

		if (t < m_settings.green_seconds) {
			return aspect::green;
		}
		if (t < m_settings.green_seconds + m_settings.amber_seconds) {
			return aspect::amber;
		}
		return aspect::red;
	}

	void street_lights::describe(const game::br_actor* actor, lamp& l, const aspect shown)
	{
		const settings& s = m_settings;
		const bool signal = l.kind->style == lamp_style::signal;

		const float* colour = s.colour;
		switch (shown)
		{
		case aspect::red:   colour = RED;   break;
		case aspect::amber: colour = AMBER; break;
		case aspect::green: colour = GREEN; break;
		default: break;
		}

		float position[3], direction[3];
		transform_point(l.model_to_world, signal ? s.signal_position : s.position, position);
		transform_direction(l.model_to_world, l.kind->direction, direction);

		const float brightness = signal ? s.signal_brightness : s.brightness;
		const float radius = signal ? s.signal_radius : s.emitter_radius;
		const float radiance = brightness / (PI * radius * radius);

		remixapi_LightInfoSphereEXT sphere{};
		sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
		sphere.position = { position[0], position[1], position[2] };
		sphere.radius = radius;
		sphere.shaping_hasvalue = TRUE;
		sphere.shaping_value.direction = { direction[0], direction[1], direction[2] };
		sphere.shaping_value.coneAngleDegrees = signal ? s.signal_cone_angle : s.cone_angle;
		sphere.shaping_value.coneSoftness = signal ? s.signal_cone_softness : s.cone_softness;
		sphere.shaping_value.focusExponent = 0.0f;
		sphere.volumetricRadianceScale = 1.0f;

		remixapi_LightInfo info{};
		info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
		info.pNext = &sphere;
		info.hash = shared::utils::string_hash64(std::format("carma2-streetlight-{}-{:x}-{}",
			m_generation, reinterpret_cast<uintptr_t>(actor), l.incarnation));
		info.radiance = { colour[0] * radiance, colour[1] * radiance, colour[2] * radiance };
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
		l.shown = shown;
	}

	/*
	 * A lamp that should be dark has its light destroyed, not merely left undrawn: Remix Plus
	 * keeps an API light in the scene once it exists. The lamp's next light takes a new
	 * incarnation, because Remix Plus applies a destroy one scene frame late and would erase
	 * a light re-created under the old hash before then.
	 */
	void street_lights::put_out(lamp& l)
	{
		if (!l.handle) {
			return;
		}
		shared::common::remix_api::get().m_bridge.DestroyLight(l.handle);
		l.handle = nullptr;
		++l.incarnation;
	}

	void street_lights::on_race_frame(const bool night)
	{
		m_lit_lamps = 0;
		m_lit_signals = 0;

		if (remix_lights_available())
		{
			// A lamp is described again when a setting changes, when it moves, and when a
			// signal changes aspect; otherwise it is only drawn, which keeps it in this frame.
			const bool changed = !(m_described == m_settings);
			const auto& bridge = shared::common::remix_api::get().m_bridge;
			const double seconds = seconds_now();

			for (auto& [actor, l] : m_lamps)
			{
				const bool signal = l.kind->style == lamp_style::signal;
				const bool wanted = night && l.drawn && (signal
					? m_settings.signals && m_settings.signal_brightness > 0.0f
					: m_settings.enabled && m_settings.brightness > 0.0f);
				if (!wanted)
				{
					put_out(l);
					continue;
				}

				const aspect shown = aspect_of(l, seconds);
				if (changed || l.placed || !l.handle || shown != l.shown)
				{
					describe(actor, l, shown);
					l.placed = false;
				}
				if (l.handle)
				{
					bridge.DrawLightInstance(l.handle);
					++(signal ? m_lit_signals : m_lit_lamps);
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
		ImGui::TextDisabled("%u lamp(s) and %u traffic light(s) lit, %u known on this track",
			m_lit_lamps, m_lit_signals, static_cast<uint32_t>(m_lamps.size()));
		ImGui::TextWrapped("A light under every street lamp's head in the night races. "
			"A lamp is about 1.4 units tall; the cone spreads down from its head.");

		ImGui::ColorEdit3("Lamp colour", s.colour);
		ImGui::SliderFloat("Lamp brightness", &s.brightness, 0.05f, 2000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Lamp emitter radius", &s.emitter_radius, 0.005f, 0.2f, "%.3f units", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Lamp cone angle", &s.cone_angle, 10.0f, 90.0f, "%.1f deg");
		ImGui::SliderFloat("Lamp cone softness", &s.cone_softness, 0.0f, 1.0f, "%.2f");
		ImGui::DragFloat3("Lamp position", s.position, 0.002f, -5.0f, 5.0f, "%.3f");
		ImGui::TextDisabled("Model space: X across the arm, Y up, Z along the arm (the head is at Z 0.49 to 0.71).");

		ImGui::Spacing();
		ImGui::Checkbox("Traffic lights at night", &s.signals);
		ImGui::TextWrapped("A faint light under the head of every traffic light, cycling green, amber, "
			"red. Signals facing across each other run opposite phases.");
		ImGui::SliderFloat("Signal brightness", &s.signal_brightness, 0.01f, 100.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Signal emitter radius", &s.signal_radius, 0.002f, 0.05f, "%.3f units", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Signal cone angle", &s.signal_cone_angle, 10.0f, 90.0f, "%.1f deg");
		ImGui::SliderFloat("Signal cone softness", &s.signal_cone_softness, 0.0f, 1.0f, "%.2f");
		ImGui::DragFloat3("Signal position", s.signal_position, 0.002f, -5.0f, 5.0f, "%.3f");
		ImGui::TextDisabled("Model space: X across the arm, Y up, Z along the arm (the head is at Y 1.02 to 1.24, Z 0.72 to 0.79).");
		ImGui::SliderFloat("Green", &s.green_seconds, 1.0f, 60.0f, "%.1f s");
		ImGui::SliderFloat("Amber", &s.amber_seconds, 0.5f, 10.0f, "%.1f s");

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
