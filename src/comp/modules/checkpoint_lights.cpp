#include "std_include.hpp"
#include "checkpoint_lights.hpp"

#include "shared/common/remix_api.hpp"

namespace comp
{
	namespace
	{
		constexpr const char* INI_NAME = "carma2-streetlights.ini";
		constexpr const char* INI_SECTION = "Checkpoints";
		constexpr float PI = 3.14159265f;

		// Every track draws its arches with "checkpoint" except Junkyard, whose arches are its
		// posts (findings 75). Nothing else on a track uses these, bar a few stray checkpoint
		// faces well away from any gate.
		constexpr const char* ARCH_MATERIALS[] = {
			"checkpoint", "post", "postb", "postc", "postd", "poste", "postei", "postf", "postfb", "postg", "posth",
		};

		// How far an arch vertex may sit from its gate: out of the gate's plane, and beyond
		// the gate's ends along it -- where a gate is narrower than its arch, the posts stand
		// up to ~1.4 past the gate's ends.
		constexpr float ARCH_PLANE_TOLERANCE = 1.0f;
		constexpr float ARCH_END_TOLERANCE = 2.5f;

		bool remix_lights_available()
		{
			using shared::common::remix_api;
			if (!remix_api::is_initialized()) {
				return false;
			}
			const auto& bridge = remix_api::get().m_bridge;
			return bridge.CreateLight && bridge.DestroyLight && bridge.DrawLightInstance;
		}

		int read_int(const uint32_t addr) {
			return *reinterpret_cast<const int*>(game::rebase(addr));
		}

		// Gate i's quad corners, or null when the race holds no such gate.
		const float* gate_corners(const int index)
		{
			const auto entry = reinterpret_cast<const uint8_t*>(game::rebase(game::ADDR_g_checkpoints))
				+ static_cast<size_t>(index) * game::CHECKPOINT_STRIDE;
			const auto corners = reinterpret_cast<const float*>(entry + game::CHECKPOINT_QUAD_CORNERS);
			return game::can_read(corners, sizeof(float) * 12) ? corners : nullptr;
		}

		int checkpoint_count()
		{
			const int count = read_int(game::ADDR_g_checkpoint_count);
			return count > 0 && count <= static_cast<int>(game::MAX_CHECKPOINTS) ? count : 0;
		}
	}

	std::string checkpoint_lights::ini_path()
	{
		return shared::globals::root_path + "\\" + INI_NAME;
	}

	bool checkpoint_lights::is_arch_material(const char* identifier)
	{
		return identifier && std::any_of(std::begin(ARCH_MATERIALS), std::end(ARCH_MATERIALS),
			[identifier](const char* name) { return _stricmp(name, identifier) == 0; });
	}

	// ------
	// settings

	void checkpoint_lights::load()
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
		const auto read_colour = [&read_float](const char* key, float (&c)[3])
		{
			read_float((std::string(key) + "R").c_str(), c[0], 0.0f, 1.0f);
			read_float((std::string(key) + "G").c_str(), c[1], 0.0f, 1.0f);
			read_float((std::string(key) + "B").c_str(), c[2], 0.0f, 1.0f);
		};

		s.enabled = GetPrivateProfileIntA(INI_SECTION, "Enabled", s.enabled ? 1 : 0, path.c_str()) != 0;
		read_colour("Colour", s.colour);
		read_float("Brightness", s.brightness, 0.0f, 100000.0f);
		s.green_next = GetPrivateProfileIntA(INI_SECTION, "GreenNext", s.green_next ? 1 : 0, path.c_str()) != 0;
		read_colour("NextColour", s.next_colour);
		read_float("NextBrightness", s.next_brightness, 0.0f, 100000.0f);
		read_float("EmitterRadius", s.radius, 0.001f, 0.5f);
		read_float("ConeAngle", s.cone_angle, 1.0f, 90.0f);
		read_float("ConeSoftness", s.cone_softness, 0.0f, 1.0f);
		read_float("Height", s.height, 0.0f, 1.0f);

		m_settings = s;
		m_saved = s;
	}

	void checkpoint_lights::save()
	{
		const std::string path = ini_path();
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
		ok &= colour("Colour", s.colour);
		ok &= write("Brightness", number(s.brightness));
		ok &= write("GreenNext", s.green_next ? "1" : "0");
		ok &= colour("NextColour", s.next_colour);
		ok &= write("NextBrightness", number(s.next_brightness));
		ok &= write("EmitterRadius", number(s.radius));
		ok &= write("ConeAngle", number(s.cone_angle));
		ok &= write("ConeSoftness", number(s.cone_softness));
		ok &= write("Height", number(s.height));

		if (ok)
		{
			m_saved = s;
			shared::common::log("Checkpoints", std::format("saved {}", path));
		}
		else
		{
			shared::common::log("Checkpoints", std::format("could not write {}", path),
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
	}

	// ------
	// arches

	void checkpoint_lights::note_model(const game::br_model* model, std::vector<std::array<float, 3>>&& points)
	{
		m_scanned.insert(model);
		if (points.empty()) {
			return;
		}
		m_points.insert(m_points.end(), points.begin(), points.end());
		m_points_changed = true;
	}

	/*
	 * Whether the race's checkpoints differ from the ones the arches were found for, which
	 * is what a new track looks like from here. The pause menu keeps them, so its round
	 * trip keeps the arches too.
	 */
	bool checkpoint_lights::track_changed()
	{
		std::vector<float> signature;
		const int count = checkpoint_count();
		signature.push_back(static_cast<float>(count));
		for (int i = 0; i < count; ++i)
		{
			if (const float* c = gate_corners(i)) {
				signature.insert(signature.end(), c, c + 12);
			}
		}
		if (signature == m_signature) {
			return false;
		}
		m_signature = std::move(signature);
		return true;
	}

	void checkpoint_lights::locate_arches()
	{
		const int count = checkpoint_count();
		m_arches.resize(static_cast<size_t>(count));

		for (int i = 0; i < count; ++i)
		{
			arch& a = m_arches[static_cast<size_t>(i)];
			const float* c = gate_corners(i);
			if (!c) {
				continue;
			}

			// Corners 0 and 1 are one end of the gate, 2 and 3 the other.
			const float end_a[2] = { (c[0] + c[3]) * 0.5f, (c[2] + c[5]) * 0.5f };
			const float end_b[2] = { (c[6] + c[9]) * 0.5f, (c[8] + c[11]) * 0.5f };
			const float mid[2] = { (end_a[0] + end_b[0]) * 0.5f, (end_a[1] + end_b[1]) * 0.5f };
			float along[2] = { end_b[0] - end_a[0], end_b[1] - end_a[1] };
			const float length = std::sqrt(along[0] * along[0] + along[1] * along[1]);
			if (!(length > 1e-4f)) {
				continue;
			}
			along[0] /= length;
			along[1] /= length;
			const float across[2] = { -along[1], along[0] };
			const float reach = length * 0.5f + ARCH_END_TOLERANCE;

			float ground = FLT_MAX, top = -FLT_MAX, lo = FLT_MAX, hi = -FLT_MAX;
			for (const auto& p : m_points)
			{
				const float dx = p[0] - mid[0];
				const float dz = p[2] - mid[1];
				const float t = dx * along[0] + dz * along[1];
				if (std::abs(dx * across[0] + dz * across[1]) > ARCH_PLANE_TOLERANCE || std::abs(t) > reach) {
					continue;
				}
				ground = std::min(ground, p[1]);
				top = std::max(top, p[1]);
				lo = std::min(lo, t);
				hi = std::max(hi, t);
			}

			const bool found = ground < top;
			if (!found) {
				continue;
			}
			const float centre = (lo + hi) * 0.5f;
			const float position[3] = {
				mid[0] + along[0] * centre,
				ground + (top - ground) * m_settings.height,
				mid[1] + along[1] * centre,
			};

			// A light that moved must be described again.
			if (!a.found || std::memcmp(a.position, position, sizeof(position)) != 0)
			{
				std::memcpy(a.position, position, sizeof(position));
				put_out(a);
			}
			a.found = true;
		}
	}

	int checkpoint_lights::next_checkpoint() const
	{
		if (!m_settings.green_next || read_int(game::ADDR_g_net_mode) != 0 || read_int(game::ADDR_g_race_over) != 0) {
			return -1;
		}

		const auto entry = *reinterpret_cast<const uint8_t* const*>(game::rebase(game::ADDR_g_race_entry));
		if (!game::can_read(entry + game::RACE_ENTRY_TYPE, sizeof(int))) {
			return -1;
		}
		const int type = *reinterpret_cast<const int*>(entry + game::RACE_ENTRY_TYPE);
		if (type != 0 && type != 3) {
			return -1;
		}
		return read_int(game::ADDR_g_next_checkpoint) - 1;
	}

	// ------
	// lights

	void checkpoint_lights::describe(const size_t index, arch& a, const bool next)
	{
		const settings& s = m_settings;
		const float* colour = next ? s.next_colour : s.colour;
		const float brightness = next ? s.next_brightness : s.brightness;
		const float radiance = brightness / (PI * s.radius * s.radius);

		remixapi_LightInfoSphereEXT sphere{};
		sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
		sphere.position = { a.position[0], a.position[1], a.position[2] };
		sphere.radius = s.radius;
		sphere.shaping_hasvalue = TRUE;
		sphere.shaping_value.direction = { 0.0f, -1.0f, 0.0f };
		sphere.shaping_value.coneAngleDegrees = s.cone_angle;
		sphere.shaping_value.coneSoftness = s.cone_softness;
		sphere.shaping_value.focusExponent = 0.0f;
		sphere.volumetricRadianceScale = 1.0f;

		remixapi_LightInfo info{};
		info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
		info.pNext = &sphere;
		info.hash = shared::utils::string_hash64(std::format("carma2-checkpoint-{}-{}-{}",
			m_generation, index, a.incarnation));
		info.radiance = { colour[0] * radiance, colour[1] * radiance, colour[2] * radiance };
		info.isDynamic = TRUE;

		remixapi_LightHandle handle = a.handle;
		if (shared::common::remix_api::get().m_bridge.CreateLight(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			if (!m_create_failed)
			{
				m_create_failed = true;
				shared::common::log("Checkpoints", "Remix refused a checkpoint light (CreateLight failed)",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
			return;
		}
		a.handle = handle;
		a.next = next;
	}

	// Remix Plus applies a destroy one scene frame late, so a light that comes back takes a
	// new incarnation rather than its old hash.
	void checkpoint_lights::put_out(arch& a)
	{
		if (!a.handle) {
			return;
		}
		shared::common::remix_api::get().m_bridge.DestroyLight(a.handle);
		a.handle = nullptr;
		++a.incarnation;
	}

	void checkpoint_lights::destroy_all()
	{
		if (remix_lights_available())
		{
			for (auto& a : m_arches) {
				put_out(a);
			}
		}
		m_lit = 0;
	}

	void checkpoint_lights::on_race_frame()
	{
		m_lit = 0;

		if (track_changed())
		{
			destroy_all();
			m_arches.clear();
			m_points.clear();
			m_scanned.clear();
			++m_generation;
		}

		const bool wanted = m_settings.enabled && (m_settings.brightness > 0.0f || m_settings.next_brightness > 0.0f);
		if (!wanted || !remix_lights_available())
		{
			destroy_all();
			return;
		}

		const bool changed = !(m_described == m_settings);
		if (m_points_changed || changed)
		{
			locate_arches();
			m_points_changed = false;
		}

		const int next = next_checkpoint();
		const auto& bridge = shared::common::remix_api::get().m_bridge;
		for (size_t i = 0; i < m_arches.size(); ++i)
		{
			arch& a = m_arches[i];
			if (!a.found) {
				continue;
			}
			const bool is_next = static_cast<int>(i) == next;
			if ((is_next ? m_settings.next_brightness : m_settings.brightness) <= 0.0f)
			{
				put_out(a);
				continue;
			}
			if (changed || !a.handle || a.next != is_next) {
				describe(i, a, is_next);
			}
			if (a.handle)
			{
				bridge.DrawLightInstance(a.handle);
				++m_lit;
			}
		}
		m_described = m_settings;
	}

	void checkpoint_lights::on_frame_without_race()
	{
		destroy_all();
	}

	// ------
	// menu

	void checkpoint_lights::draw_menu()
	{
		settings& s = m_settings;

		ImGui::Checkbox("Checkpoint lights", &s.enabled);
		const auto found = std::count_if(m_arches.begin(), m_arches.end(), [](const arch& a) { return a.found; });
		ImGui::TextDisabled("%u lit; %d of %d arches found", m_lit, static_cast<int>(found), static_cast<int>(m_arches.size()));
		ImGui::TextWrapped("A faint light under every checkpoint arch, aimed at the road between its posts. Optionally "
			"the one to cross next glows green instead (single-player normal and checkpoint races).");

		ImGui::ColorEdit3("Arch colour", s.colour);
		ImGui::SliderFloat("Arch brightness", &s.brightness, 0.0f, 5.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
		ImGui::Checkbox("Next checkpoint in its own colour", &s.green_next);
		ImGui::ColorEdit3("Next colour", s.next_colour);
		ImGui::SliderFloat("Next brightness", &s.next_brightness, 0.0f, 5.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Light height", &s.height, 0.0f, 1.0f, "%.2f of the arch");
		ImGui::SliderFloat("Emitter radius##checkpoint", &s.radius, 0.005f, 0.2f, "%.3f units", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Cone angle##checkpoint", &s.cone_angle, 10.0f, 90.0f, "%.1f deg");
		ImGui::SliderFloat("Cone softness##checkpoint", &s.cone_softness, 0.0f, 1.0f, "%.2f");

		const bool dirty = !(m_settings == m_saved);
		if (ImGui::Button("Save checkpoint lights to ini")) {
			save();
		}
		ImGui::SameLine();
		if (ImGui::Button("Reload##checkpoint")) {
			load();
		}
		ImGui::SameLine();
		if (ImGui::Button("Defaults##checkpoint")) {
			m_settings = settings{};
		}
		ImGui::SameLine();
		ImGui::TextDisabled(dirty ? "unsaved changes" : INI_NAME);
	}

	// ------

	checkpoint_lights::checkpoint_lights()
	{
		p_this = this;
		load();
		shared::common::log("Checkpoints", "Module initialized.", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}
}
