#include "std_include.hpp"
#include "controls.hpp"
#include "game.hpp"

namespace comp::game
{
	namespace
	{
		/*
		 * g_key_mapping: one key code per action slot, loaded from DATA\KEYMAP_<n>.TXT. A code
		 * is its line in KEYNAMES.TXT minus 2 (findings 48, 49).
		 */
		constexpr uint32_t ADDR_g_key_mapping = 0x0074B5E0u;
		constexpr int KEY_SLOTS = 77;

		constexpr uint32_t ADDR_LoadKeyMapping = 0x00487E10u;            // void(void), once at startup
		constexpr uint32_t ADDR_ControlsScreenEnd = 0x00472A30u;         // saves the map to its file
		constexpr uint32_t ADDR_controls_screen_end_callback = 0x00604A4Cu;

		// Slots: 47..50 steer left, steer right, accelerate, brake. The arrow keys sit on the
		// external camera's slots (31..34 in every shipped layout), which the Controls screen
		// does not list and so will not give up: driving can never be put on them in game.
		constexpr int SLOT_STEER_LEFT = 47;
		constexpr int SLOT_STEER_RIGHT = 48;
		constexpr int SLOT_ACCELERATE = 49;
		constexpr int SLOT_BRAKE = 50;

		constexpr int KEY_LEFT = 70, KEY_RIGHT = 71, KEY_UP = 72, KEY_DOWN = 73;
		constexpr int KEY_PAD_2 = 83, KEY_PAD_4 = 85, KEY_PAD_6 = 87, KEY_PAD_8 = 89;
		constexpr int FIRST_JOYSTICK_CODE = 107;                         // "Joy 1 B1"

		using LoadKeyMapping_t = void(__cdecl*)();
		using ControlsScreenEnd_t = int(__fastcall*)(void* menu);

		LoadKeyMapping_t o_load_key_mapping = nullptr;
		ControlsScreenEnd_t o_controls_screen_end = nullptr;

		int* key_mapping() {
			return reinterpret_cast<int*>(rebase(ADDR_g_key_mapping));
		}

		/*
		 * Drive on the arrow keys, and move whatever the arrows did onto the matching numpad
		 * keys. A fixed assignment rather than a swap, so applying it twice, or to a map a
		 * player already rearranged, always lands on the same layout. A driving slot bound to
		 * a joystick keeps it.
		 */
		void apply_arrow_driving()
		{
			int* map = key_mapping();

			const auto to_numpad = [](const int code)
			{
				switch (code)
				{
				case KEY_UP: return KEY_PAD_8;
				case KEY_DOWN: return KEY_PAD_2;
				case KEY_LEFT: return KEY_PAD_4;
				case KEY_RIGHT: return KEY_PAD_6;
				default: return code;
				}
			};

			for (int slot = 0; slot < KEY_SLOTS; ++slot)
			{
				if (slot < SLOT_STEER_LEFT || slot > SLOT_BRAKE) {
					map[slot] = to_numpad(map[slot]);
				}
			}

			const auto drive = [map](const int slot, const int arrow)
			{
				if (map[slot] < FIRST_JOYSTICK_CODE) {
					map[slot] = arrow;
				}
			};

			drive(SLOT_STEER_LEFT, KEY_LEFT);
			drive(SLOT_STEER_RIGHT, KEY_RIGHT);
			drive(SLOT_ACCELERATE, KEY_UP);
			drive(SLOT_BRAKE, KEY_DOWN);
		}

		void __cdecl hk_load_key_mapping()
		{
			o_load_key_mapping();
			apply_arrow_driving();
		}

		/*
		 * The Controls screen reloads the key-map files when it opens and saves the array
		 * back when it closes, so the layout is re-applied only once it has closed: the
		 * files keep the player's own bindings, and switching the option off restores them.
		 */
		int __fastcall hk_controls_screen_end(void* menu)
		{
			const int result = o_controls_screen_end(menu);
			apply_arrow_driving();
			return result;
		}
	}

	void install_arrow_key_driving()
	{
		const auto callback = reinterpret_cast<uint32_t*>(rebase(ADDR_controls_screen_end_callback));
		if (*callback != rebase(ADDR_ControlsScreenEnd))
		{
			shared::common::log("Game", std::format(
				"arrow-key driving not installed: the Controls screen's end callback is {:#010x}, expected {:#010x}",
				*callback, rebase(ADDR_ControlsScreenEnd)), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}

		if (!shared::utils::hook::detour(rebase(ADDR_LoadKeyMapping), hk_load_key_mapping,
			reinterpret_cast<void**>(&o_load_key_mapping)))
		{
			shared::common::log("Game", "arrow-key driving not installed: LoadKeyMapping could not be hooked",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}

		o_controls_screen_end = reinterpret_cast<ControlsScreenEnd_t>(rebase(ADDR_ControlsScreenEnd));
		shared::utils::hook::set<uint32_t>(static_cast<void*>(callback), reinterpret_cast<uint32_t>(&hk_controls_screen_end));

		// The key map may already be loaded if the proxy came up after InitialiseWorld;
		// before it, this writes into an array the loader is about to replace anyway.
		apply_arrow_driving();

		shared::common::log("Game", "arrow-key driving: steer and throttle on the arrows, the camera on numpad 8/2/4/6",
			shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
	}
}
