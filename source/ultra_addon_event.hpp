/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

//-----------------------------------------------------------------------------

// What every add-on event needs of the reload (ultra_addon_reload.hpp), in a header of its own so the code that fires
// events does not pull in the reload's declarations
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
#else
	constexpr bool	isAddonReloadEnabled = false;

	class AddonEventGuard
	{
	public:
		AddonEventGuard () {}
	};
#endif
}
