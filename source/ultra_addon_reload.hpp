/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <filesystem>

#include <Windows.h>

//-----------------------------------------------------------------------------

// Reloading add-ons while the game runs, development builds only (ULTRARESHADE_DEVELOPMENT): add-ons load from a copy
// in the add-on folder, so a build can overwrite their file. When the file changes, the old add-on sees every live object
// destroyed, unloads, the new copy loads and sees them all created again. The add-ons loaded after it reload with it (they
// may use it): out before it, in after it
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

	// The file an add-on found in the add-on folder loads and reloads from: the file of its name in [ADDON] AddonBuildPath
	// (a build's output), else found itself. The add-on folder decides which add-ons load
	[[ nodiscard ]] std::filesystem::path addonSource ( const std::filesystem::path& found );

	// Where the add-on in file loads from: a fresh copy in folder (the add-on folder) while reloading is enabled, else file itself
	[[ nodiscard ]] std::filesystem::path addonLoadPath ( const std::filesystem::path& file, const std::filesystem::path& folder );

	// An add-on that loaded from addonLoadPath ( file, folder ) and registered: watched from now on
	void watchAddon ( HMODULE module, const std::filesystem::path& file );

	// Once per presented frame, outside any add-on event: reloads the add-ons whose file changed (or all, when asked), then
	// runs the exclusive calls. Nothing while the build folder holds the file "publishing" (written first and removed last by
	// a publish, so add-ons and what they load change as one set)
	void pollAddonReload ();

	// Reloads every watched add-on at the next present, changed or not (the test application's repeated reloads)
	void requestAddonReload ();

	// From any thread: the add-on owner reloads at the next present, changed or not; false while reloading is off
	[[ nodiscard ]] bool requestAddonReload ( HMODULE owner );

	// From any thread: callback runs once at the next present at which no add-on's changed file waits to reload, after the
	// reloads, on the presenting thread, while no add-on event runs and the GPU is idle. owner is a registered add-on's
	// module or ReShade's own; its calls are dropped when it unregisters or refuses a reload
	[[ nodiscard ]] bool runExclusive ( HMODULE owner, void ( *callback ) ( void* context ), void* context );
	void forgetExclusiveCalls ( HMODULE owner );
#else
	constexpr bool	isAddonReloadEnabled = false;

	class AddonEventGuard
	{
	public:
		AddonEventGuard () {}
	};

	inline void startAddonReload ( const std::filesystem::path& ) {}
	inline void stopAddonReload () {}
	[[ nodiscard ]] inline std::filesystem::path addonSource ( const std::filesystem::path& found ) { return found; }
	[[ nodiscard ]] inline std::filesystem::path addonLoadPath ( const std::filesystem::path& file, const std::filesystem::path& ) { return file; }
	inline void watchAddon ( HMODULE, const std::filesystem::path& ) {}
	inline void pollAddonReload () {}
	inline void requestAddonReload () {}
	[[ nodiscard ]] inline bool requestAddonReload ( HMODULE ) { return false; }
	[[ nodiscard ]] inline bool runExclusive ( HMODULE, void ( * ) ( void* ), void* ) { return false; }
	inline void forgetExclusiveCalls ( HMODULE ) {}
#endif
}
