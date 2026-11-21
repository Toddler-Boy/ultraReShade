/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <atomic>
#include <cstdint>

//-----------------------------------------------------------------------------

// What every add-on event needs of the process's end and of the add-on reload (ultra_addon_reload.hpp), in a header of its own so
// the code that fires events does not pull in the reload's declarations
namespace reshade::ultra
{
	// The one word an event reads first: zero while the process runs and add-ons are not reloaded, so an event is only its callbacks
	inline constexpr uint32_t	gateExitBegun = 1;			// the process's end has begun (ultra_process_exit.hpp)
	inline constexpr uint32_t	gateReloadEnabled = 2;		// add-ons load for reloading: an event holds the guard

	extern std::atomic<uint32_t>	eventGate;

	// Set once the add-ons load with Developer mode on; never changes after that
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
}
