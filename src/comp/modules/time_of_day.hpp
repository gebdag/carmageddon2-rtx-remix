#pragma once

namespace comp
{
	/*
	 * Each race's time of day, read from the sky it loads.
	 *
	 * The game has no clock: every race is lit by the same white sun (see sun.hpp). What a
	 * race sets is its sky texture and depth cue, and the designers used those for the time
	 * of day -- TWINNIGHT and CITYSKAPEN on the night races, DESKY's red sunset, AIRGLUM's
	 * overcast. The sky's pixelmap name is therefore the mood, and this module turns it into
	 * the sun a path tracer needs: angle, colour and strength for the proxy's own sun on
	 * NVIDIA's runtime, and on Remix Plus the whole physical sky -- its sun, and clouds and haze
	 * matched to what each sky texture paints. At night it also asks for every car's
	 * headlights and for the street lights.
	 */
	class time_of_day final : public shared::common::loader::component_module
	{
	public:
		time_of_day();
		~time_of_day() { p_this = nullptr; }

		static inline time_of_day* p_this = nullptr;
		static time_of_day* get() { return p_this; }

		enum class mood : uint8_t
		{
			standard,   // the game's own sun: every race when switched off, and indoor tracks
			day,
			overcast,
			dusk,
			night,
			fog,
		};

		// How the current mood changes the player's standard sun.
		struct sun_look
		{
			float elevation;         // degrees above the horizon; below 0 there is no direct sun
			float azimuth;           // degrees from +Z towards +X
			float tint[3];           // multiplies the sun colour
			float brightness;        // multiplies the sun brightness
			float angular_diameter;  // degrees; a hazy sky spreads the sun into a wider disc
		};

		// The clouds and haze a sky texture paints, for Remix Plus's physical sky.
		struct sky_weather
		{
			float cloud_cover;       // rtx.atmosphere.cloudCoverageMean: 0 clear .. 1 overcast
			float cloud_type;        // rtx.atmosphere.cloudTypeMean: 0 stratus .. 1 cumulus
			float cloud_density;     // rtx.atmosphere.cloudDensity: higher is thicker, darker cloud
			float cloud_colour[3];   // rtx.atmosphere.cloudColor, the cloud albedo
			float aerosol;           // rtx.atmosphere.aerosolDensity: haze

			bool operator==(const sky_weather&) const = default;
		};

		// Once per race-view submit, ahead of the sun and the lights.
		void on_race_frame();

		// Switched off, every race gets the standard sun and no night lighting.
		void set_enabled(bool enabled) { m_enabled = enabled; }

		mood current() const;
		bool is_night() const { return current() == mood::night; }
		sun_look look() const;
		sky_weather weather() const;
		const std::string& sky_name() const { return m_sky; }

		static const char* mood_name(mood m);

		// Whether the runtime's physical sky is the sky ([Sky] PhysicalSky on Remix Plus).
		// False on stock Remix, and on Remix Plus set to the game's own sky: the proxy then
		// draws the rasterized sky and places its own sun light.
		static bool physical_sky();

		// One shipped sky: its mood and weather (time_of_day.cpp).
		struct sky_entry;

	private:
		static const sky_entry* entry_for(const std::string& sky);
		void apply_atmosphere();

		bool m_enabled = true;
		const sky_entry* m_entry = nullptr;   // the race's sky in the table; null when unknown
		std::string m_sky;

		// What was last pushed to Remix Plus's sky, so it is set once per change.
		int m_sky_mode = -1;   // rtx.skyMode last set; -1 before the first push
		bool m_clouds_enabled = false;
		std::optional<sky_weather> m_pushed;
	};
}
