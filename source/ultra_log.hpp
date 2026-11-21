/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

//-----------------------------------------------------------------------------

// Named logs in one folder, each <name>.log, written the same way by every part of ultraGame. No dependency on ReShade
// (nothing but the standard library and Windows): ultraReShade compiles it in, and so does any process without ReShade.
//
// A log is appended to, never put aside. Each run starts with a header line and a clean exit ends with an end line, both
// at the start of a line:
//   -- run start 2026-11-20 14:03:22.123 pid 1234 exe "game.exe" build "6.8.0.0 development 64-bit"
//   -- run end 2026-11-20 14:09:00.456
// A run (from its header to the next header) without an end line did not end cleanly: the process crashed or was killed.
// The lines between are "HH:MM:SS.mmm [thread] | LEVEL | text" (ERROR, WARN, INFO, DEBUG), CRLF in the file.
//
// Whoever opens a log names its maximum size. At the start, on the writer thread, a file over its size loses its oldest
// whole runs until it fits; the run before this one stays whole whatever its size, and nothing of the new run is touched.
//
// One background thread per process writes every log's lines, so no thread that logs waits for the disk. One process
// writes a file (others may read it). Every function may be called from any thread
namespace ultralog
{
	using Id = uint32_t;

	inline constexpr Id	none = 0;

	enum class Level : uint32_t
	{
		error = 1,
		warning = 2,
		info = 3,
		debug = 4
	};

	inline constexpr uint64_t			minBytes = uint64_t ( 64 ) << 10;
	inline constexpr uint64_t			maxBytes = uint64_t ( 1 ) << 30;
	inline constexpr size_t				namesMax = 64;
	inline constexpr size_t				logsMax = 64;
	inline constexpr std::string_view	runStartMarker = "-- run start ";
	inline constexpr std::string_view	runEndMarker = "-- run end ";

	enum class OpenStatus
	{
		opened,
		existing,		// the name was open already: its id, size and folder stay
		badName,		// not 1 to namesMax of letters, digits, '-' and '_'
		badSize,		// outside minBytes to maxBytes
		tooMany,		// logsMax logs are open
		fileFailed		// the lines are not written (error says why)
	};

	struct Opened
	{
		Id					id = none;
		OpenStatus			status = OpenStatus::badName;
		std::error_code		error;
	};

	[[ nodiscard ]] bool isValidName ( std::string_view name );

	// Opens folder/name.log (the folder is made), appends the run's header and starts the writer thread with the first log.
	// build is any text naming this program's version, shown in the header
	[[ nodiscard ]] Opened open ( const std::filesystem::path& folder, std::string_view name, uint64_t maxBytesOfFile, std::string_view build );

	[[ nodiscard ]] bool isKnown ( Id log );

	// One line, with time, thread and level; message's trailing newlines are dropped and a line beyond 32 KiB is cut.
	// Nothing for an unknown log
	void write ( Id log, Level level, std::string_view message );

	// Writes the lines waiting to the file now, on the calling thread (a crash handler's last step)
	void flush ( Id log );
	void flushAll ();

	// Ends this run in every log that has not ended it: the end line, then everything waiting is written. Not after the
	// process crashed (an unhandled exception passed through here, or noteCrash): the missing end line is how it shows
	void endRuns ();

	// The process ends in a crash that no unhandled exception filter saw (its exit status says so): no log gets an end line
	// any more, and what waits is written
	void noteCrash ();

	// The last run of a log file's text: from its last header line to the end, the whole text without one
	[[ nodiscard ]] std::string_view lastRun ( std::string_view fileText );
}
