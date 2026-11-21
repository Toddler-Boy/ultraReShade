/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <filesystem>

#include <Windows.h>

#include "ultra_addon_event.hpp"

extern std::filesystem::path get_module_path ( HMODULE module );

//-----------------------------------------------------------------------------

// Reloading add-ons while the game runs, with Developer mode on (ultra::isDevelopmentOn): add-ons load from a copy in the add-on
// folder, so a build can overwrite their file. When the file changes, the old add-on sees every live object destroyed, unloads,
// the new copy loads and sees them all created again. The add-ons loaded after it reload with it (they may use it): out before
// it, in after it. With it off every call below does nothing or returns the plain answer, and no event holds a guard
namespace reshade::ultra
{
	// Starts tracking live objects; removes copies a previous run left in the add-on folder
	void startAddonReload ( const std::filesystem::path& addonFolder );
	void stopAddonReload ();

	// The file an add-on found in the add-on folder loads and reloads from: the file of its name in [ADDON] AddonBuildPath
	// (a build's output), else found itself. The add-on folder decides which add-ons load
	[[ nodiscard ]] std::filesystem::path addonSource ( const std::filesystem::path& found );

	// The file the loaded add-on's code comes from: the file addonSource named while reloading is enabled, else the module's own.
	// The presenting thread (the add-on list is its)
	[[ nodiscard ]] std::filesystem::path addonSourceOf ( HMODULE module );

	// Where the add-on in file loads from: a fresh copy of this process's in folder (the add-on folder) while reloading is
	// enabled, else file itself; empty when the copy can't be made (logged): file is never loaded in place then
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
}
