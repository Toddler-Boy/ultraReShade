/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <filesystem>

#include <Windows.h>

//-----------------------------------------------------------------------------

// The json functions of the add-on API (include/reshade_ultra_json.hpp): ultra_json.cpp holds values, text, files and
// layers, ultra_json_watch.cpp the change notifications on top of the file watch
namespace reshade::ultra
{
	// UTF-8, absolute or relative to ReShade's base path; empty for a null or empty path
	[[ nodiscard ]] std::filesystem::path resolveJsonPath ( const char* path );

	// owner wrote file through ReShadeJsonWriteFile: its own watches are not told of this change
	void noteJsonWrite ( void* owner, const std::filesystem::path& file );

	// An add-on that unregisters loses its json watches (also those whose callback lies in its module)
	void forgetJsonWatches ( HMODULE module );

	// The add-ons unload: every json watch ends
	void stopJsonWatch ();

	enum class JsonWriteFault
	{
		none,
		partialWrite,	// the temporary file fails half written, as on a full disk
		beforeReplace	// renaming it over the old file fails
	};

#if defined(RESHADE_TEST_APPLICATION)
	// The test application's json file writes fail this way, until set back to none
	void setJsonWriteFault ( JsonWriteFault fault );
#endif
}
