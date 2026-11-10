/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <filesystem>

#include <Windows.h>

//-----------------------------------------------------------------------------

// Reloading add-ons while the game runs, development builds only (ULTRARESHADE_DEVELOPMENT): add-ons load from a copy
// next to their file, so a build can overwrite it. When the file changes, the old add-on sees every live object
// destroyed, unloads, the new copy loads and sees them all created again
namespace reshade::ultra
{
#if ULTRARESHADE_DEVELOPMENT
	// Set once the add-ons load; never changes after that
	extern bool isAddonReloadEnabled;

	// Held by every add-on event while reloading is enabled, so no event runs while an add-on is swapped
	class AddonEventGuard
	{
	public:
		AddonEventGuard ();
		~AddonEventGuard ();

		AddonEventGuard ( const AddonEventGuard& ) = delete;
		AddonEventGuard& operator= ( const AddonEventGuard& ) = delete;

	private:
		bool	isCounted;
	};

	// The calling thread runs an add-on event (counted only while reloading is enabled)
	[[ nodiscard ]] bool isInAddonEvent ();

	// Starts tracking live objects; removes copies a previous run left in the add-on folder
	void startAddonReload ( const std::filesystem::path& addonFolder );
	void stopAddonReload ();

	// Where the add-on in file loads from: a fresh copy next to it while reloading is enabled, else file itself
	[[ nodiscard ]] std::filesystem::path addonLoadPath ( const std::filesystem::path& file );

	// An add-on that loaded from addonLoadPath ( file ) and registered: watched from now on
	void watchAddon ( HMODULE module, const std::filesystem::path& file );

	// Once per presented frame, outside any add-on event: reloads the add-ons whose file changed (or all, when asked)
	void pollAddonReload ();

	// Reloads every watched add-on at the next present, changed or not (the test application's repeated reloads)
	void requestAddonReload ();
#else
	constexpr bool	isAddonReloadEnabled = false;

	class AddonEventGuard
	{
	public:
		AddonEventGuard () {}
	};

	inline void startAddonReload ( const std::filesystem::path& ) {}
	inline void stopAddonReload () {}
	[[ nodiscard ]] inline std::filesystem::path addonLoadPath ( const std::filesystem::path& file ) { return file; }
	inline void watchAddon ( HMODULE, const std::filesystem::path& ) {}
	inline void pollAddonReload () {}
	inline void requestAddonReload () {}
#endif
}
