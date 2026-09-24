#include "std_include.hpp"
#include <psapi.h>

#include "comp.hpp"
#include "d3d9_proxy.hpp"
#include "shared/common/flags.hpp"
#include "shared/common/config.hpp"

namespace comp
{
	std::unordered_set<HWND> wnd_class_list;

	// Registered by WinMain at 0x0051AAA0 (PTR_s_Carma2MainWndClass_006621D0).
	#define WINDOW_CLASS_NAME "Carma2MainWndClass"

	BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lParam)
	{
		DWORD window_pid, target_pid = static_cast<DWORD>(lParam);
		GetWindowThreadProcessId(hwnd, &window_pid);

		if (window_pid == target_pid && IsWindowVisible(hwnd))
		{
			char class_name[256];
			GetClassNameA(hwnd, class_name, sizeof(class_name));

			if (!wnd_class_list.contains(hwnd))
			{
				char debug_msg[256];
				wsprintfA(debug_msg, "> HWND: %p, PID: %u, Class: %s, Visible: %d \n", hwnd, window_pid, class_name, IsWindowVisible(hwnd));
				shared::common::log("Main", debug_msg, shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
				wnd_class_list.insert(hwnd);
			}

			if (std::string_view(class_name).contains(WINDOW_CLASS_NAME))
			{
				shared::globals::main_window = hwnd;
				return FALSE;
			}
		}

		return TRUE;
	}

	DWORD WINAPI find_game_window([[maybe_unused]] LPVOID lpParam)
	{
		std::uint32_t T = 0;

		shared::common::log("Main", "Waiting for window with classname containing '" WINDOW_CLASS_NAME "' ...", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		{
			while (!shared::globals::main_window)
			{
				EnumWindows(enum_windows_proc, static_cast<LPARAM>(GetCurrentProcessId()));
				if (!shared::globals::main_window) {
					Sleep(1u); T += 1u;
				}

				if (T >= 30000)
				{
					Beep(300, 100); Sleep(100); Beep(200, 100);
					shared::common::log("Main", "Could not find '" WINDOW_CLASS_NAME "' Window. Not loading RTX Compatibility Mod.", shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
					return TRUE;
				}
			}
		}

		if (!shared::common::flags::has_flag("nobeep")) {
			Beep(523, 100);
		}

		// Post-load DLLs (after window is found, game is running)
		d3d9_proxy::load_postload_dlls();

		comp::main();
		return 0;
	}
}

BOOL APIENTRY DllMain(HMODULE hmodule, const DWORD ul_reason_for_call, LPVOID)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
	{
		// Before the game creates its window. A process Windows considers DPI-unaware has
		// its window and back buffer scaled by the display's scale factor, so on a 250%
		// display nGlide's 3840x2160 becomes a 1536x864 swap chain -- and Remix renders at
		// that. The compatibility override users set for this is keyed on the EXE's path,
		// which the Remix launcher changes when it renames CARMA2_HW.EXE.
		SetProcessDPIAware();

		shared::common::console();
		shared::globals::setup_dll_module(hmodule);
		shared::globals::setup_exe_module();
		shared::globals::setup_homepath();

		shared::common::set_console_color_blue(true);
		std::cout << "Launching RTX Remix Comp [" << COMP_MOD_VERSION_MAJOR << "." << COMP_MOD_VERSION_MINOR << "." << COMP_MOD_VERSION_PATCH << "]\n";
		std::cout << "> Compiled On : " + std::string(__DATE__) + " " + std::string(__TIME__) + "\n";
		std::cout << "> Based on xoxor4d/remix-comp-base\n";
		std::cout << "> Adapted by kim2091 for Vibe Reverse Engineering\n";
		std::cout << "> Running as d3d9.dll proxy\n\n";
		shared::common::set_console_color_default();

		// Load config from INI file next to the DLL
		shared::common::config::get().load(shared::globals::root_path + "\\remix-comp-proxy.ini");

		// Pre-load DLLs (before the d3d9 chain is established)
		d3d9_proxy::load_preload_dlls();

		// Load the real d3d9 chain (Remix bridge or system d3d9.dll)
		if (!d3d9_proxy::init())
			return TRUE;

		if (const auto MH_INIT_STATUS = MH_Initialize(); MH_INIT_STATUS != MH_STATUS::MH_OK)
		{
			shared::common::log("Main", std::format("MinHook failed to initialize with code: {:d}", static_cast<int>(MH_INIT_STATUS)), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return TRUE;
		}

		// Setup memory addresses (eg. patterns)
		comp::game::init_game_addresses();

		// Find game window thread (registers modules once window is found)
		if (const auto t = CreateThread(nullptr, 0, comp::find_game_window, nullptr, 0, nullptr); t) {
			CloseHandle(t);
		}
	}

	return TRUE;
}
