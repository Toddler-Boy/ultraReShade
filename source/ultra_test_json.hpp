/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

//-----------------------------------------------------------------------------

// -json-tests: the json functions of the add-on API checked through their exports, in a "json-test" folder of the run's
// base path. Text, files, failures and layers at the first present; change notifications over the following seconds,
// then the window closes. Each failed check is an error line (exit code 4)
namespace reshade::ultra
{
	[[ nodiscard ]] bool isJsonTestRequested ();

	// Once per present, on the presenting thread; window is closed when the checks are done
	void stepJsonTests ( void* window );

	// At the end of the run: unfinished checks are an error
	void finishJsonTests ();
}
