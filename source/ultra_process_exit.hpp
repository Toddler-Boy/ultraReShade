/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "ultra_addon_event.hpp"

//-----------------------------------------------------------------------------

// The process's own end in order (addon_event::ultra_process_exit): RtlExitUserProcess, where every ExitProcess ends, and
// NtTerminateProcess on its own process are hooked; the add-ons hear it once, on the exiting thread, with a time bound
namespace reshade::ultra
{
	// From the first add-on load on; once per process
	void followProcessExit ();

	// Once true, no add-on code is called any more: no event, overlay, file change or reload
	[[ nodiscard ]] inline bool hasProcessExitBegun ()
	{
		return ( eventGate.load ( std::memory_order_relaxed ) & gateExitBegun ) != 0;
	}

	// The end begins: false when it had begun before
	[[ nodiscard ]] inline bool beginProcessExit ()
	{
		return ( eventGate.fetch_or ( gateExitBegun ) & gateExitBegun ) == 0;
	}
}
