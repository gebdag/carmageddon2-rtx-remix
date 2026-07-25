#include "std_include.hpp"
#include "shared/common/flags.hpp"

namespace comp::game
{
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
	}
}
