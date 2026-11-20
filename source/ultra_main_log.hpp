/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <string>

#include "ultra_log.hpp"

//-----------------------------------------------------------------------------

// ReShade's use of the log library (ultra_log): its own log, reshade.log in the home, and the exports that give add-ons
// and modules logs of their own in the same home
namespace reshade::ultra
{
	// Opens reshade.log in the home; false when its file cannot be written (lines are kept in memory, the reason is the
	// log's first error). Without it every message is dropped
	[[ nodiscard ]] bool openMainLog ();

	[[ nodiscard ]] ultralog::Id mainLog ();

	// An add-on's name for its lines when it is not registered: its file name; empty for ReShade itself
	[[ nodiscard ]] std::string logModuleName ( const void* module );
}
