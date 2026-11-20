/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <filesystem>

//-----------------------------------------------------------------------------

// The test application as an installed game: its own index, home, settings and cache in the run's -work-dir, as ultraGame's app
// writes them; ULTRAGAME_INDEX then names the index. The options it reads are in the usage text (ultra_test_run.cpp)
namespace reshade::ultra
{
	// The work folder; empty with the reason on the debugger's output when there is no -work-dir or a file could not be written
	[[ nodiscard ]] std::filesystem::path seedTestHome ( const std::filesystem::path& exe );
}
