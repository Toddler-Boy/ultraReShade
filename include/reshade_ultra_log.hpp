/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdint>

#include "reshade.hpp"

// ultraReShade's logs for add-ons and the modules they load: named logs in the game's home (the folder of reshade.log), all
// written by one background thread of the process, so no caller waits for the disk. Every log behaves the same:
//
// A log is the file <name>.log, appended to and never put aside. Each run (one process, from its first open of the log)
// starts with a header line and a clean exit of the process ends it with an end line, both at the start of a line:
//   -- run start 2026-11-20 14:03:22.123 pid 1234 exe "game.exe" build "6.8.0.0 development off 64-bit"
//   -- run end 2026-11-20 14:09:00.456
// A run, from its header to the next header or the file's end, without an end line did not end cleanly: the process
// crashed or was killed. The lines between are "HH:MM:SS.mmm [thread] | LEVEL | text", LEVEL being ERROR, WARN, INFO or
// DEBUG, and the file's lines end in CRLF.
//
// open names the maximum size of the file: when a process starts a run in a file over it, the file's oldest whole runs go
// (on the writer thread, soon after open) until it fits. The run before stays whole whatever its size, the new run is
// never touched. Whoever logs more names a larger size. A name is 1 to 64 letters, digits, '-' or '_', matched without
// regard to case; a size is 64 KiB to 1 GiB; a process holds up to 64 logs. "reshade" is ultraReShade's own log.
//
// A log is opened again freely (an add-on that reloads, a module and its add-on): the same name gives the same handle, its
// first open's size and build text stay, and no second run starts. Logs stay open until the process ends.
//
// Handles are opaque numbers; 0 is none. A call with a handle that open did not give, a level outside 1 to 4 or a null
// text is refused (false, 0) and says so once in reshade.log; a failed open still gives a handle when only the file could
// not be opened (the failure is in reshade.log, the lines are dropped), and 0 when it was refused. On a ReShade without
// these functions every call is refused quietly, and nothing is written.
//
// Threads: every function may be called from any thread. Nothing blocks on file I/O: a line waits in memory for the writer
// thread (up to 1 MiB per log; beyond it lines are dropped and the file says how many). A line is cut at 32 KiB, its
// trailing newlines are dropped, and it may hold line breaks inside (shown as several lines).

#if defined(RESHADE_API_LIBRARY)

RESHADE_API_LIBRARY_DECL uint64_t ReShadeLogOpen ( const char* name, uint64_t max_bytes, const char* build );
RESHADE_API_LIBRARY_DECL bool ReShadeLogWrite ( uint64_t log, int level, const char* message );
RESHADE_API_LIBRARY_DECL bool ReShadeLogFlush ( uint64_t log );

#endif

namespace reshade::logs
{
	enum class level : int
	{
		error = 1,
		warning = 2,
		info = 3,
		debug = 4
	};

	namespace internal
	{
		struct functions
		{
			uint64_t ( *open ) ( const char*, uint64_t, const char* ) = nullptr;
			bool ( *write ) ( uint64_t, int, const char* ) = nullptr;
			bool ( *flush ) ( uint64_t ) = nullptr;
		};

#if defined(RESHADE_API_LIBRARY)
		inline const functions& get ()
		{
			static const functions	table { &ReShadeLogOpen, &ReShadeLogWrite, &ReShadeLogFlush };
			return table;
		}
#else
		// Looked up once; a function this ReShade lacks stays null
		inline const functions& get ()
		{
			static const functions	table = []
			{
				const auto	module = reshade::internal::get_reshade_module_handle ();
				functions	f;

				if ( module != nullptr )
				{
					f.open = reinterpret_cast<decltype ( f.open )> ( GetProcAddress ( module, "ReShadeLogOpen" ) );
					f.write = reinterpret_cast<decltype ( f.write )> ( GetProcAddress ( module, "ReShadeLogWrite" ) );
					f.flush = reinterpret_cast<decltype ( f.flush )> ( GetProcAddress ( module, "ReShadeLogFlush" ) );
				}

				return f;
			} ();
			return table;
		}
#endif
	}

	// The log <name>.log with its maximum size and a text naming the caller's version for the run header (null: none);
	// 0 when refused. Opens the file and starts the run on the first call for a name, so call it once the home is known
	[[ nodiscard ]] inline uint64_t open ( const char* name, uint64_t max_bytes, const char* build = nullptr )
	{
		const auto	f = internal::get ().open;
		return f != nullptr ? f ( name, max_bytes, build ) : 0;
	}

	// One line with the time, the calling thread and the level; false when refused
	inline bool write ( uint64_t log, level severity, const char* message )
	{
		const auto	f = internal::get ().write;
		return f != nullptr && f ( log, static_cast<int> ( severity ), message );
	}

	// Writes what waits for the file now, on the calling thread (a crash handler's last step); false when refused
	inline bool flush ( uint64_t log )
	{
		const auto	f = internal::get ().flush;
		return f != nullptr && f ( log );
	}
}
