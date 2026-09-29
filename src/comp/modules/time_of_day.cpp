#include "std_include.hpp"
#include "time_of_day.hpp"
#include "headlights.hpp"

#include "shared/common/config.hpp"
#include "shared/common/remix_api.hpp"

namespace comp
{
	struct time_of_day::sky_entry
	{
		const char* sky;
		time_of_day::mood tone;
		sky_weather weather;
	};

	namespace
	{
		using weather = time_of_day::sky_weather;
		using mood = time_of_day::mood;

		constexpr weather white_clouds(const float cover, const float type, const float density = 1.0f,
			const float aerosol = 1.0f)
		{
			return { cover, type, density, { 0.95f, 0.95f, 0.95f }, aerosol };
		}

		/*
		 * Every sky a shipped race loads: the time of day it paints, and the clouds and haze in
		 * it. Races without a sky (the silo, the arenas) are indoors and keep the standard sun.
		 *
		 * The weather is read off the textures' sky band, above any skyline or ridge. Cover is
		 * the share of that band under cloud; type is the cloud form, 0 for the banded, streaky
		 * stratus most of these skies paint and 1 for heaped cumulus; density darkens and thickens
		 * the storm and gloom skies; the colour is the measured mean of the cloud pixels, pulled
		 * most of the way to white, since the physical sky lights and tints clouds itself. Haze
		 * is raised where the texture washes out its distance (the valley mists, the snowfield,
		 * the fog races).
		 */
		constexpr time_of_day::sky_entry SKIES[] =
		{
			{ "skyblue_01", mood::day, white_clouds(0.45f, 0.20f) },
			{ "desky2",     mood::day, white_clouds(0.35f, 0.60f) },
			{ "carday",     mood::day, { 0.35f, 0.50f, 1.0f, { 0.93f, 0.92f, 1.00f }, 1.0f } },
			{ "cityskape",  mood::day, { 0.40f, 0.20f, 0.9f, { 0.90f, 0.98f, 0.97f }, 1.0f } },
			{ "twingreen",  mood::day, white_clouds(0.10f, 0.50f, 1.0f, 2.0f) },
			{ "skisky_01",  mood::day, white_clouds(0.75f, 0.00f, 0.5f, 2.5f) },

			{ "airglum",    mood::overcast, { 0.60f, 0.60f, 1.6f, { 0.82f, 0.78f, 0.86f }, 1.5f } },
			{ "airport3",   mood::overcast, { 0.70f, 0.30f, 1.2f, { 0.95f, 0.90f, 0.85f }, 1.5f } },
			{ "carsky1",    mood::overcast, { 0.70f, 0.30f, 1.2f, { 0.95f, 0.90f, 0.85f }, 1.5f } },
			{ "desglum",    mood::overcast, { 0.60f, 0.60f, 1.6f, { 0.88f, 0.80f, 0.72f }, 1.5f } },

			{ "sumosky",    mood::dusk, white_clouds(0.35f, 0.10f, 0.8f, 1.5f) },
			{ "desky",      mood::dusk, white_clouds(0.05f, 0.30f, 1.0f, 1.5f) },
			{ "car2sky",    mood::dusk, white_clouds(0.05f, 0.30f, 1.0f, 1.5f) },
			{ "nyhorizn",   mood::dusk, { 0.45f, 0.30f, 1.0f, { 0.97f, 0.88f, 0.82f }, 1.5f } },
			{ "nyhoriznp",  mood::dusk, { 0.45f, 0.20f, 1.0f, { 0.97f, 0.85f, 0.95f }, 1.5f } },
			{ "qdark",      mood::dusk, { 0.80f, 0.40f, 1.5f, { 0.90f, 0.84f, 0.78f }, 1.5f } },
			{ "twinpink",   mood::dusk, { 0.05f, 0.30f, 1.0f, { 0.97f, 0.90f, 0.97f }, 2.5f } },

			{ "twinnight",  mood::night, white_clouds(0.00f, 0.50f) },
			{ "cityskapen", mood::night, white_clouds(0.15f, 0.20f) },
			{ "qnight",     mood::night, white_clouds(0.85f, 0.40f, 1.5f) },

			{ "cityskapef", mood::fog, white_clouds(1.00f, 0.00f, 1.0f, 8.0f) },
			{ "nyhoriznf",  mood::fog, white_clouds(1.00f, 0.00f, 1.0f, 8.0f) },
			{ "cityskape4", mood::fog, white_clouds(1.00f, 0.00f, 1.0f, 8.0f) },
		};

		// Indoor tracks, unknown skies and the switched-off time of day: a fair-weather sky.
		constexpr weather STANDARD_WEATHER = white_clouds(0.30f, 0.50f);
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

	const time_of_day::sky_entry* time_of_day::entry_for(const std::string& sky)
	{
		for (const auto& entry : SKIES)
		{
			if (_stricmp(entry.sky, sky.c_str()) == 0) {
				return &entry;
			}
		}
		return nullptr;
	}

	time_of_day::mood time_of_day::current() const
	{
		return m_enabled && m_entry ? m_entry->tone : mood::standard;
	}

	time_of_day::sky_weather time_of_day::weather() const
	{
		return m_enabled && m_entry ? m_entry->weather : STANDARD_WEATHER;
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
		case mood::overcast: return { 60.0f, 30.0f, { 0.92f, 0.95f, 1.0f }, 0.45f, 4.0f };
		case mood::dusk:     return { 10.0f, 30.0f, { 1.0f, 0.62f, 0.38f }, 0.9f, 0.0f };
		case mood::night:    return { -3.5f, 30.0f, { 0.6f, 0.7f, 1.0f }, 1.0f, 0.0f };
		case mood::fog:      return { 50.0f, 30.0f, { 0.95f, 0.95f, 0.95f }, 0.35f, 8.0f };
		default:             return { 60.0f, 30.0f, { 1.0f, 1.0f, 1.0f }, 1.0f, 0.0f };
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
			m_entry = entry_for(sky);
			shared::common::log("TimeOfDay", std::format("sky '{}': {}", sky.empty() ? "none" : sky,
				mood_name(m_entry ? m_entry->tone : mood::standard)), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, true);
		}

		apply_atmosphere();

		if (const auto lights = headlights::get(); lights) {
			lights->set_night(is_night());
		}
	}

	/*
	 * On Remix Plus the sky is the runtime's own physical sky, rtx.skyMode 1 (Numos), in
	 * place of the rasterized sky the proxy draws on NVIDIA's runtime. Its clouds and haze
	 * follow the race's sky texture; the sun in it is placed by the sun module, which owns
	 * the sun's angle on either runtime. The weather presets are left alone: they also drive
	 * the volumetric fog, which the proxy sets from the track's depth cue. Pushed once per
	 * change: these are global options crossing the bridge.
	 *
	 * [Sky] PhysicalSky off puts Remix Plus back on rtx.skyMode 0, the rasterized sky the
	 * proxy draws on stock Remix.
	 */
	bool time_of_day::physical_sky()
	{
		return shared::common::remix_api::has_atmosphere() && shared::common::config::get().sky.physical;
	}

	void time_of_day::apply_atmosphere()
	{
		using shared::common::remix_api;
		if (!remix_api::has_atmosphere()) {
			return;
		}

		const int mode = physical_sky() ? 1 : 0;
		if (m_sky_mode != mode)
		{
			const bool set = remix_api::set_config("rtx.skyMode", mode ? "1" : "0");
			if (set) {
				m_sky_mode = mode;
			}
			shared::common::log("TimeOfDay", std::format("rtx.skyMode = {} ({}) {}", mode,
				mode ? "physical sky" : "the game's sky", set ? "set" : "refused"),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, true);
		}
		if (!mode)
		{
			m_pushed.reset();   // the clouds follow the race again when the physical sky returns
			return;
		}

		if (!m_clouds_enabled) {
			m_clouds_enabled = remix_api::set_config("rtx.atmosphere.cloudEnabled", "True");
		}

		const sky_weather w = weather();
		if (m_pushed == w) {
			return;
		}

		const auto number = [](const float value) { return std::format("{:.3f}", value); };
		const bool pushed = remix_api::set_config("rtx.atmosphere.cloudCoverageMean", number(w.cloud_cover))
			&& remix_api::set_config("rtx.atmosphere.cloudTypeMean", number(w.cloud_type))
			&& remix_api::set_config("rtx.atmosphere.cloudDensity", number(w.cloud_density))
			&& remix_api::set_config("rtx.atmosphere.cloudColor", std::format("{:.3f}, {:.3f}, {:.3f}",
				w.cloud_colour[0], w.cloud_colour[1], w.cloud_colour[2]))
			&& remix_api::set_config("rtx.atmosphere.aerosolDensity", number(w.aerosol));

		if (pushed)
		{
			m_pushed = w;
			shared::common::log("TimeOfDay", std::format("physical sky: {} -- cloud cover {:.2f}, type {:.2f},"
				" density {:.2f}, haze {:.1f}", mood_name(current()), w.cloud_cover, w.cloud_type,
				w.cloud_density, w.aerosol));
		}
	}

	time_of_day::time_of_day()
	{
		p_this = this;
	}
}
