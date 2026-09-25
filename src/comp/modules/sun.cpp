#include "std_include.hpp"
#include "sun.hpp"

#include "shared/common/remix_api.hpp"

namespace comp
{
	namespace
	{
		constexpr const char* INI_NAME = "carma2-sun.ini";
		constexpr const char* INI_SECTION = "Sun";

		constexpr float DEG_TO_RAD = 3.14159265f / 180.0f;

		bool remix_lights_available()
		{
			using shared::common::remix_api;

			if (!remix_api::is_initialized()) {
				return false;
			}

			const auto& bridge = remix_api::get().m_bridge;
			return bridge.CreateLight && bridge.DestroyLight && bridge.DrawLightInstance;
		}
	}

	std::string sun::ini_path()
	{
		return shared::globals::root_path + "\\" + INI_NAME;
	}

	// ------
	// settings

	void sun::load()
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
		read_float("Elevation", s.elevation, -10.0f, 90.0f);
		read_float("Azimuth", s.azimuth, -180.0f, 180.0f);
		read_float("ColourR", s.colour[0], 0.0f, 1.0f);
		read_float("ColourG", s.colour[1], 0.0f, 1.0f);
		read_float("ColourB", s.colour[2], 0.0f, 1.0f);
		read_float("Brightness", s.brightness, 0.0f, 100.0f);
		read_float("AngularDiameter", s.angular_diameter, 0.01f, 30.0f);
		read_float("VolumetricScale", s.volumetric_scale, 0.0f, 10.0f);

		m_settings = s;
		m_saved = s;
	}

	void sun::save()
	{
		const std::string path = ini_path();
		const settings& s = m_settings;

		const auto write = [&path](const char* key, const std::string& value) {
			return WritePrivateProfileStringA(INI_SECTION, key, value.c_str(), path.c_str()) != FALSE;
		};
		const auto number = [](const float value) { return std::format("{:.4f}", value); };

		bool ok = write("Enabled", s.enabled ? "1" : "0");
		ok &= write("Elevation", number(s.elevation));
		ok &= write("Azimuth", number(s.azimuth));
		ok &= write("ColourR", number(s.colour[0]));
		ok &= write("ColourG", number(s.colour[1]));
		ok &= write("ColourB", number(s.colour[2]));
		ok &= write("Brightness", number(s.brightness));
		ok &= write("AngularDiameter", number(s.angular_diameter));
		ok &= write("VolumetricScale", number(s.volumetric_scale));

		if (ok)
		{
			m_saved = s;
			shared::common::log("Sun", std::format("saved {}", path));
		}
		else
		{
			shared::common::log("Sun", std::format("could not write {}", path),
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
	}

	// ------
	// remix

	void sun::destroy()
	{
		if (m_handle && shared::common::remix_api::is_initialized()) {
			shared::common::remix_api::get().m_bridge.DestroyLight(m_handle);
		}
		m_handle = nullptr;
	}

	/*
	 * Remix drops an API light from any frame it is not drawn in, so the handle is drawn on
	 * every race frame. It is only described again when a setting changes: Remix derives
	 * the handle from the hash, so creating under the same hash replaces the light in place.
	 *
	 * The light is destroyed only when switched off, never when the race view is left.
	 * Remix Plus applies a destroy at the start of its next scene frame but a create at once,
	 * and the menus may render no scene frame at all; a destroy on the way out would then
	 * land after the create on the way back in and erase the sun under a live handle.
	 */
	void sun::on_race_frame()
	{
		const settings& s = m_settings;

		if (!s.enabled || !(s.brightness > 0.0f))
		{
			destroy();
			return;
		}

		if (!remix_lights_available()) {
			return;
		}

		const auto& bridge = shared::common::remix_api::get().m_bridge;

		if (!m_handle || !(m_described == s))
		{
			// Remix wants the direction the light travels: from the sun, down to the track.
			const float elevation = s.elevation * DEG_TO_RAD;
			const float azimuth = s.azimuth * DEG_TO_RAD;

			remixapi_LightInfoDistantEXT distant{};
			distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
			distant.direction = {
				-std::cos(elevation) * std::sin(azimuth),
				-std::sin(elevation),
				-std::cos(elevation) * std::cos(azimuth),
			};
			distant.angularDiameterDegrees = s.angular_diameter;
			distant.volumetricRadianceScale = s.volumetric_scale;

			remixapi_LightInfo info{};
			info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			info.pNext = &distant;
			info.hash = shared::utils::string_hash64("carma2-sun");
			info.radiance = { s.colour[0] * s.brightness, s.colour[1] * s.brightness, s.colour[2] * s.brightness };
			// Remix Plus stops applying updates to a static light after 50 of them, which one
			// slider drag in the menu exceeds. The sun is only described on a change anyway.
			info.isDynamic = TRUE;

			remixapi_LightHandle handle = m_handle;
			if (bridge.CreateLight(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS)
			{
				if (!m_create_failed)
				{
					m_create_failed = true;
					shared::common::log("Sun", "Remix refused the sun (CreateLight failed)",
						shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				}
				return;
			}

			m_handle = handle;
			m_described = s;
		}

		bridge.DrawLightInstance(m_handle);
	}

	// ------
	// menu

	void sun::draw_menu()
	{
		settings& s = m_settings;

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
		else {
			ImGui::TextDisabled("%s", shared::common::remix_api::runtime());
		}

		ImGui::Checkbox("Sun", &s.enabled);
		ImGui::TextDisabled("The game's own sun: 60 deg up, 30 deg from +Z towards +X, white, on every track.");

		ImGui::Spacing();
		ImGui::SliderFloat("Elevation", &s.elevation, -5.0f, 90.0f, "%.1f deg");
		ImGui::SliderFloat("Azimuth", &s.azimuth, -180.0f, 180.0f, "%.1f deg");
		ImGui::ColorEdit3("Colour", s.colour);
		ImGui::SliderFloat("Brightness", &s.brightness, 0.01f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Angular diameter", &s.angular_diameter, 0.05f, 10.0f, "%.2f deg", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Volumetric scale", &s.volumetric_scale, 0.0f, 5.0f, "%.2f");
		ImGui::TextDisabled("1.0 lights a white surface facing the sun to full brightness. "
			"A wider disc gives softer shadows.");

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

	sun::sun()
	{
		p_this = this;
		load();

		shared::common::log("Sun", std::format("Module initialized, sun {}.", m_settings.enabled ? "on" : "off"),
			shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}
}
