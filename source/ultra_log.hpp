/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

//-----------------------------------------------------------------------------

// What reshade::log::message writes into: a line waits in memory and a background thread writes it to the file, so no
// thread that logs waits for the disk. A crash or the end of the process writes what is left on the spot
namespace reshade::ultra
{
	[[ nodiscard ]] bool openLog ( const std::filesystem::path& path, std::error_code& ec );

	// One finished line, ending in '\n'
	void logLine ( std::string&& line );

	// Writes every waiting line now, on the calling thread
	void flushLog ();

	// Empties the file and the recent lines
	void clearLog ();

	// An add-on's name for its lines when it is not registered: its file name; empty for ReShade itself
	[[ nodiscard ]] std::string logModuleName ( const void* module );

	// Lines logged since the start, an atomic read for the Log tab's every frame
	[[ nodiscard ]] uint64_t logLineCount ();

	// The recent lines after line number seen, which moves on to the newest. False when some of them are no longer
	// in memory: lines then holds every recent line
	[[ nodiscard ]] bool copyLogLines ( uint64_t& seen, std::vector<std::string>& lines );
}
