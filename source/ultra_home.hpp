/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <filesystem>
#include <string>

//-----------------------------------------------------------------------------

// A game's home, %APPDATA%\ultraGame\user\games\<folder>: ultraGame's index (user\installed.json) names it for each
// installed exe, and ReShade runs only in a listed exe. The home is ReShade's base path: reshade.json (ultraGame's, never
// written here), reshade-user.json (the user's changes, the only file ReShade writes) and reshade.log
namespace reshade::ultra
{
	// Names another index file (its folder takes the place of %APPDATA%\ultraGame\user); the test application sets it
	inline constexpr wchar_t	indexVariable[] = L"ULTRAGAME_INDEX";

	// The exe's home from the index, empty when the exe is not listed. A plain file read, fit for DllMain; nothing is logged
	// yet, the reason for an empty result goes to the debugger's output
	[[ nodiscard ]] std::filesystem::path findHome ( const std::filesystem::path& exe );

	// Below need the home as the base path
	[[ nodiscard ]] std::filesystem::path curatedSettingsPath ();
	[[ nodiscard ]] std::filesystem::path userSettingsPath ();
	[[ nodiscard ]] std::filesystem::path logPath ();

	// ultraGame's own settings (settings.json beside the user folder): its section hotkeys holds ReShade's [INPUT] keys,
	// screenshots the screenshot folder
	[[ nodiscard ]] std::filesystem::path appSettingsPath ();

	// Developer mode: development/enabled in ultraGame's settings.json, read once at the first call (after the home is found) and
	// never again, so one value holds for the whole run. A file that is missing or does not read is off
	[[ nodiscard ]] bool isDevelopmentOn ();

	// The game's shown title from its index entry (UTF-8), empty when the entry has none
	[[ nodiscard ]] const std::string& gameTitle ();

	// Once the log is open: where the home and its files are
	void logHome ();
}
