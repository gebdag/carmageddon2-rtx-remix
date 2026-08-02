#include "std_include.hpp"
#include "shared/common/flags.hpp"

namespace comp::game
{
	namespace
	{
		/*
		 * Rewrites a function prologue in place, but only if the bytes on disk are exactly
		 * the ones the patch was authored against -- a mismatch means a different build,
		 * and writing anyway would corrupt code.
		 */
		bool patch_code(const uint32_t addr, const uint8_t* expected, const uint8_t* replacement,
			const size_t size, const char* name)
		{
			const auto target = reinterpret_cast<uint8_t*>(rebase(addr));

			if (std::memcmp(target, expected, size) != 0)
			{
			shared::common::log("Game", std::format("{} @ 0x{:08X} does not match - patch skipped",
					name, addr), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				return false;
			}

			DWORD old_protect = 0;
			if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &old_protect)) {
				return false;
			}

			std::memcpy(target, replacement, size);
			VirtualProtect(target, size, old_protect, &old_protect);
			FlushInstructionCache(GetCurrentProcess(), target, size);
			return true;
		}

		/*
		 * The tint-poly overlay pool (10 slots, stride 0x6450, base 0x00705C80) is indexed
		 * by TintPolyShow (0x004D8220) and TintPolyHide (0x004D8250) without any range
		 * check. The frontend's enter-from-race path hides three slots by handle, and the
		 * third handle (0x00655E50) is never written anywhere in the binary -- it is
		 * permanently -1. slot[-1] lands in the track bounds-tree pool, which on the
		 * biggest maps holds nonzero bytes, so the in_use guard passes and the game writes
		 * render_style through float garbage: the ESC-to-menu crash on the city map. A
		 * stock bug; small maps leave that memory zeroed and never trip it.
		 *
		 * The fix folds a bounds check into the index-scaling prologue: eax = ecx * 0x6450
		 * as one imul instead of the compiler's lea/shl chain, which frees exactly the
		 * bytes for `cmp ecx, 10 / jae <existing ret>`. The tails are untouched.
		 */
		void patch_tint_poly_bounds()
		{
			// lea eax,[ecx+ecx*4] / shl eax,6 / add eax,ecx / lea eax,[eax+eax*4] / shl eax,4
			static const uint8_t show_expected[14] = {
				0x8D, 0x04, 0x89, 0xC1, 0xE0, 0x06, 0x03, 0xC1,
				0x8D, 0x04, 0x80, 0xC1, 0xE0, 0x04,
			};
			// cmp ecx,10 / jae 0x004D824C (ret) / imul eax,ecx,0x6450 / nop
			static const uint8_t show_patched[14] = {
				0x83, 0xF9, 0x0A, 0x73, 0x27,
				0x69, 0xC1, 0x50, 0x64, 0x00, 0x00, 0x90, 0x90, 0x90,
			};

			// lea eax,[ecx+ecx*4] / shl eax,6 / add eax,ecx / xor ecx,ecx
			// lea eax,[eax+eax*4] / shl eax,4
			static const uint8_t hide_expected[16] = {
				0x8D, 0x04, 0x89, 0xC1, 0xE0, 0x06, 0x03, 0xC1, 0x33, 0xC9,
				0x8D, 0x04, 0x80, 0xC1, 0xE0, 0x04,
			};
			// cmp ecx,10 / jae 0x004D828A (ret) / imul eax,ecx,0x6450 / xor ecx,ecx / nops
			static const uint8_t hide_patched[16] = {
				0x83, 0xF9, 0x0A, 0x73, 0x35,
				0x69, 0xC1, 0x50, 0x64, 0x00, 0x00, 0x33, 0xC9, 0x90, 0x90, 0x90,
			};

			const bool show_ok = patch_code(0x004D8220u, show_expected, show_patched,
				sizeof(show_expected), "TintPolyShow");
			const bool hide_ok = patch_code(0x004D8250u, hide_expected, hide_patched,
				sizeof(hide_expected), "TintPolyHide");

			if (show_ok && hide_ok)
			{
				shared::common::log("Game", "tint-poly slot bounds patched - ESC menu crash fixed",
					shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
			}
		}
	}

	scene_fog read_scene_fog()
	{
		scene_fog fog{};

		const auto read_int = [](const uint32_t addr) {
			return *reinterpret_cast<const int*>(rebase(addr));
		};

		const int type = read_int(ADDR_g_fogType);
		if (type < 0 || type > 2) {
			return fog;
		}

		// ApplyDepthCueToMaterial (0x004451EF..0x00445245): the two TXT exponents place
		// the fog band on a log scale around the Yon draw distance.
		const float yon = *reinterpret_cast<const float*>(rebase(ADDR_g_yon));
		fog.min_distance = yon * std::pow(10.0f, static_cast<float>(-read_int(ADDR_g_fogP1)) * 0.1f);
		fog.max_distance = yon * std::pow(10.0f, static_cast<float>(read_int(ADDR_g_fogP2)) * 0.1f);
		fog.enabled = fog.max_distance > fog.min_distance;

		// Per-mode colour, from the jump table at 0x00445328: "dark" fades to black,
		// "fog" to near-white, "colour" to the level's authored RGB.
		switch (type)
		{
		case 0:
			fog.colour = 0x000000u;
			break;
		case 1:
			fog.colour = 0xF8F8F8u;
			break;
		default:
		{
			const auto channel = [&](const uint32_t addr) {
				return static_cast<uint32_t>(std::clamp(read_int(addr), 0, 255));
			};
			fog.colour = (channel(ADDR_g_fogR) << 16) | (channel(ADDR_g_fogG) << 8)
				| channel(ADDR_g_fogB);
			break;
		}
		}

		return fog;
	}

	// Every address this port needs is a fixed RVA in a non-relocatable executable, so there
	// is nothing to pattern-scan for. They live as constants in game.hpp; this hook only
	// reports the module base so a mismatch is obvious in the log.
	void init_game_addresses()
	{
		shared::common::log("Game", std::format("CARMA2_HW.EXE base 0x{:08X} (expected 0x{:08X})",
			shared::globals::exe_module_addr, PREFERRED_BASE),
			shared::globals::exe_module_addr == PREFERRED_BASE
				? shared::common::LOG_TYPE::LOG_TYPE_GREEN
				: shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);

		patch_tint_poly_bounds();
	}
}
