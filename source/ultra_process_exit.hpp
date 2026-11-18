/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <atomic>

//-----------------------------------------------------------------------------

// The process's own end in order (addon_event::ultra_process_exit): RtlExitUserProcess, where every ExitProcess ends, and
// NtTerminateProcess on its own process are hooked; the add-ons hear it once, on the exiting thread, with a time bound
namespace reshade::ultra
{
	extern std::atomic_bool	processExitBegun;

	// From the first add-on load on; once per process
	void followProcessExit ();

	// Once true, no add-on code is called any more: no event, overlay, file change or reload
	[[ nodiscard ]] inline bool hasProcessExitBegun ()
	{
		return processExitBegun.load ( std::memory_order_relaxed );
	}
}
