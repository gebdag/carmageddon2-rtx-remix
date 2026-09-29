#pragma once

namespace comp
{
	/*
	 * F2 hides the HUD, in a race and in action replay.
	 *
	 * The game's own Cycle Headups (F1) only lowers the detail level of the race HUD, and not
	 * in replay. Nearly the whole HUD is textured triangles drawn in two small scenes of its
	 * own -- HudFlush (the glyphs and the queued HUD actors) and the dim panel quad. While
	 * one of those passes runs with the HUD hidden, the BrZbModelRender hook forwards render
	 * style NONE (hiding()), so the passes keep their bookkeeping and draw nothing. The
	 * replay's controls -- icons and progress bar blitted into the back buffer, text, mouse
	 * cursor -- come from one overlay routine that holds no state, so it is skipped whole.
	 */
	class hud_toggle final : public shared::common::loader::component_module
	{
	public:
		hud_toggle();
		~hud_toggle() { p_this = nullptr; }

		static inline hud_toggle* p_this = nullptr;
		static hud_toggle* get() { return p_this; }

		void toggle();

		// Whether a HUD pass is drawing right now with the HUD hidden.
		static bool hiding();
	};
}
