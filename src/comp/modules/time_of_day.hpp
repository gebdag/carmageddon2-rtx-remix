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
	 * NVIDIA's runtime, sun elevation and haze for Remix Plus's physical sky. At night it also
	 * asks for every car's headlights and for the street lights.
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
			float aerosol;           // Remix Plus rtx.atmosphere.aerosolDensity
		};

		// Once per race-view submit, ahead of the sun and the lights.
		void on_race_frame();

		// Switched off, every race gets the standard sun and no night lighting.
		void set_enabled(bool enabled) { m_enabled = enabled; }

		mood current() const { return m_enabled ? m_mood : mood::standard; }
		bool is_night() const { return current() == mood::night; }
		sun_look look() const;
		const std::string& sky_name() const { return m_sky; }

		static const char* mood_name(mood m);

	private:
		static mood mood_for(const std::string& sky);
		void apply_atmosphere();

		bool m_enabled = true;
		mood m_mood = mood::standard;
		std::string m_sky;

		// What was last pushed to Remix Plus's sky, so it is set once per change.
		bool m_sky_mode_set = false;
		mood m_pushed = mood::standard;
		bool m_pushed_any = false;
	};
}
