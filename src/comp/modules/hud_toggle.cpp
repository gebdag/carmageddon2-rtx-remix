#include "std_include.hpp"
#include "hud_toggle.hpp"

namespace comp
{
	namespace
	{
		// The HUD's two draw passes and the replay overlay (see the class comment).
		constexpr uint32_t ADDR_HudFlush = 0x004E5B00u;           // void __cdecl (void)
		constexpr uint32_t ADDR_HudPanelQuad = 0x0047CAD0u;       // __fastcall (unused ecx, edx, 4 stack args), ret 0x10
		constexpr uint32_t ADDR_ReplayOverlay = 0x004E6280u;      // void __fastcall (int mode), ret

		bool g_hidden = false;
		int g_pass_depth = 0;

		using hud_flush_t = void(__cdecl*)();
		using panel_quad_t = void(__fastcall*)(void*, int, int, int, int, int);
		using replay_overlay_t = void(__fastcall*)(int);

		hud_flush_t o_hud_flush = nullptr;
		panel_quad_t o_panel_quad = nullptr;
		replay_overlay_t o_replay_overlay = nullptr;

		struct hud_pass
		{
			explicit hud_pass(const bool active) : active(active) { if (active) { ++g_pass_depth; } }
			~hud_pass() { if (active) { --g_pass_depth; } }
			bool active;
		};

		void __cdecl hk_hud_flush()
		{
			hud_pass pass(true);
			o_hud_flush();
		}

		void __fastcall hk_panel_quad(void* ecx, const int edx, const int a, const int b, const int c, const int d)
		{
			hud_pass pass(true);
			o_panel_quad(ecx, edx, a, b, c, d);
		}

		// The replay's controls: icons and a progress bar written straight into the locked
		// back buffer, its text and its mouse cursor. RenderAFrame calls it every replay
		// frame, and the fast-scrub path (0x004E6900) calls it on its own before a swap.
		// It holds no replay state, so a hidden HUD skips it whole.
		void __fastcall hk_replay_overlay(const int mode)
		{
			if (!g_hidden) {
				o_replay_overlay(mode);
			}
		}

		bool install(const uint32_t addr, void* stub, void** original, const char* name)
		{
			if (shared::utils::hook::detour(game::rebase(addr), stub, original)) {
				return true;
			}
			shared::common::log("HUD", std::format("failed to hook {} @ 0x{:08X}", name, addr),
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return false;
		}
	}

	bool hud_toggle::hiding()
	{
		return g_hidden && g_pass_depth > 0;
	}

	void hud_toggle::toggle()
	{
		g_hidden = !g_hidden;
		shared::common::log("HUD", g_hidden ? "HUD hidden (F2)" : "HUD shown (F2)");
	}

	hud_toggle::hud_toggle()
	{
		p_this = this;

		bool ok = install(ADDR_HudFlush, hk_hud_flush, reinterpret_cast<void**>(&o_hud_flush), "HudFlush");
		ok &= install(ADDR_HudPanelQuad, hk_panel_quad, reinterpret_cast<void**>(&o_panel_quad), "HUD panel quad");
		ok &= install(ADDR_ReplayOverlay, hk_replay_overlay, reinterpret_cast<void**>(&o_replay_overlay), "replay overlay");

		shared::common::log("HUD", ok ? "F2 hides the HUD" : "F2 HUD toggle partly unavailable",
			ok ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_ERROR, false);
	}
}
