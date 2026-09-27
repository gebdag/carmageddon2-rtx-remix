#include "std_include.hpp"
#include "time_of_day.hpp"
#include "headlights.hpp"

#include "shared/common/remix_api.hpp"

namespace comp
{
	namespace
	{
		struct sky_mood
		{
			const char* sky;
			time_of_day::mood mood;
		};

		// Every sky a shipped race loads, by the time of day it paints. Races without a sky
		// (the silo, the arenas) are indoors and keep the standard sun.
		constexpr sky_mood SKY_MOODS[] =
		{
			{ "skyblue_01", time_of_day::mood::day },
			{ "desky2", time_of_day::mood::day },
			{ "carday", time_of_day::mood::day },
			{ "cityskape", time_of_day::mood::day },
			{ "twingreen", time_of_day::mood::day },
			{ "skisky_01", time_of_day::mood::day },
			{ "sumosky", time_of_day::mood::day },

			{ "airglum", time_of_day::mood::overcast },
			{ "airport3", time_of_day::mood::overcast },
			{ "carsky1", time_of_day::mood::overcast },
			{ "desglum", time_of_day::mood::overcast },

			{ "desky", time_of_day::mood::dusk },
			{ "nyhorizn", time_of_day::mood::dusk },
			{ "nyhoriznp", time_of_day::mood::dusk },
			{ "qdark", time_of_day::mood::dusk },
			{ "twinpink", time_of_day::mood::dusk },
			{ "car2sky", time_of_day::mood::dusk },

			{ "twinnight", time_of_day::mood::night },
			{ "cityskapen", time_of_day::mood::night },
			{ "qnight", time_of_day::mood::night },

			{ "cityskapef", time_of_day::mood::fog },
			{ "nyhoriznf", time_of_day::mood::fog },
			{ "cityskape4", time_of_day::mood::fog },
		};
	}

	const char* time_of_day::mood_name(const mood m)
	{
		switch (m)
		{
		case mood::day: return "day";
		case mood::overcast: return "overcast";
		case mood::dusk: return "dusk";
		case mood::night: return "night";
		case mood::fog: return "fog";
		default: return "standard";
		}
	}

	time_of_day::mood time_of_day::mood_for(const std::string& sky)
	{
		for (const auto& entry : SKY_MOODS)
		{
			if (_stricmp(entry.sky, sky.c_str()) == 0) {
				return entry.mood;
			}
		}
		return mood::standard;
	}

	/*
	 * The sun each mood asks for, relative to the player's standard sun. Night puts the sun
	 * 3.5 degrees below the horizon: Remix Plus's sky keeps a twilight glow there, which is
	 * why its strength stays at 1 -- the sun still has to light the sky from below the
	 * horizon. The proxy's own sun, with no sky to glow, is off below the horizon. An angular
	 * diameter of 0 keeps the player's own.
	 */
	time_of_day::sun_look time_of_day::look() const
	{
		switch (current())
		{
		case mood::overcast: return { 60.0f, 30.0f, { 0.92f, 0.95f, 1.0f }, 0.45f, 4.0f, 3.0f };
		case mood::dusk:     return { 10.0f, 30.0f, { 1.0f, 0.62f, 0.38f }, 0.9f, 0.0f, 1.5f };
		case mood::night:    return { -3.5f, 30.0f, { 0.6f, 0.7f, 1.0f }, 1.0f, 0.0f, 1.0f };
		case mood::fog:      return { 50.0f, 30.0f, { 0.95f, 0.95f, 0.95f }, 0.35f, 8.0f, 8.0f };
		default:             return { 60.0f, 30.0f, { 1.0f, 1.0f, 1.0f }, 1.0f, 0.0f, 1.0f };
		}
	}

	void time_of_day::on_race_frame()
	{
		const game::horizon_settings horizon = game::read_horizon();
		const game::br_pixelmap* pm = horizon.texture;
		const std::string sky = pm && game::can_read(pm, sizeof(*pm)) && pm->identifier
			&& game::can_read(pm->identifier, 1) ? pm->identifier : "";

		if (sky != m_sky)
		{
			m_sky = sky;
			m_mood = mood_for(sky);
			shared::common::log("TimeOfDay", std::format("sky '{}': {}", sky.empty() ? "none" : sky,
				mood_name(m_mood)), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, true);
		}

		apply_atmosphere();

		if (const auto lights = headlights::get(); lights) {
			lights->set_night(is_night());
		}
	}

	/*
	 * On Remix Plus the sky is the runtime's own physical sky, rtx.skyMode 1, and each mood
	 * sets its haze. The sun in it is placed by the sun module, which owns the sun's angle on
	 * either runtime. Pushed once per change: these are global options crossing the bridge.
	 */
	void time_of_day::apply_atmosphere()
	{
		using shared::common::remix_api;
		if (!remix_api::has_atmosphere()) {
			return;
		}

		if (!m_sky_mode_set)
		{
			m_sky_mode_set = remix_api::set_config("rtx.skyMode", "1");
			shared::common::log("TimeOfDay", std::format("rtx.skyMode = 1 (physical sky) {}",
				m_sky_mode_set ? "set" : "refused"), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, true);
		}

		if (m_pushed_any && m_pushed == current()) {
			return;
		}

		const sun_look l = look();
		if (remix_api::set_config("rtx.atmosphere.aerosolDensity", std::format("{:.2f}", l.aerosol)))
		{
			m_pushed = current();
			m_pushed_any = true;
			shared::common::log("TimeOfDay", std::format("physical sky: {} (aerosol {:.1f})",
				mood_name(current()), l.aerosol));
		}
	}

	time_of_day::time_of_day()
	{
		p_this = this;
	}
}
