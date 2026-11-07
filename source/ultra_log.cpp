/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

#include <Windows.h>

#include <reshade_ultra.hpp>

#include "dll_log.hpp"
#include "ultra_log.hpp"

extern HMODULE g_module_handle;
extern "C" IMAGE_DOS_HEADER __ImageBase;

//-----------------------------------------------------------------------------

namespace
{
	constexpr size_t	pendingMax = 1 << 20;
	constexpr size_t	wakeAt = pendingMax / 4;		// a burst goes out before the buffer fills
	constexpr DWORD		drainEveryMs = 50;				// at most this much is lost when the process is killed
	constexpr size_t	recentMax = 10000;
	constexpr size_t	recentBytesMax = 4 << 20;
	constexpr auto		lockTimeout = std::chrono::milliseconds ( 500 );
	constexpr ULONGLONG	faultDrainEveryMs = 250;		// games that handle access violations themselves raise thousands

	struct Log
	{
		// Timed: a thread that ExitProcess ended may still hold one
		std::timed_mutex			pendingLock;		// pending, dropped, isTruncateWanted, recent, recentBytes
		std::timed_mutex			fileLock;			// file, draining, hasWriteFailed
		std::string					pending;
		std::string					draining;
		size_t						dropped = 0;		// lines that found no room since the last drain
		bool						isTruncateWanted = false;
		std::deque<std::string>		recent;
		size_t						recentBytes = 0;
		std::atomic<uint64_t>		count = 0;
		std::atomic<bool>			hasFile = false;
		std::atomic<bool>			hasWriter = false;
		bool						hasWriteFailed = false;
		HANDLE						file = INVALID_HANDLE_VALUE;
		HANDLE						writer = nullptr;
		HANDLE						wake = nullptr;
		HANDLE						parked = nullptr;
		std::atomic<bool>			isStopping = false;
		std::atomic<bool>			shouldPark = false;
		PVOID						crashHandler = nullptr;
		std::atomic<bool>			hasPending = false;
		std::atomic<ULONGLONG>		lastFaultDrain = 0;
		std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER>	gamesFilter = nullptr;		// called after ours
		std::atomic<bool>			hasOwnFilter = false;
	};

	// Never destroyed: lines logged while other statics end still find it
	[[ nodiscard ]] Log& logger ()
	{
		static auto* const	instance = new Log;
		return *instance;
	}

	[[ nodiscard ]] size_t sizeWithCrLf ( const std::string& line )
	{
		return line.size () + size_t ( std::ranges::count ( line, '\n' ) );
	}

	void appendWithCrLf ( std::string& out, const std::string& line )
	{
		for ( size_t begin = 0; begin < line.size (); )
		{
			const auto	end = std::min ( line.find ( '\n', begin ), line.size () );
			out.append ( line, begin, end - begin );

			if ( end < line.size () )
				out += "\r\n";

			begin = end + 1;
		}
	}

	[[ nodiscard ]] bool isProcessExiting ()
	{
		using ShutdownInProgress = BOOLEAN ( NTAPI* ) ();
		static const auto	inProgress = reinterpret_cast<ShutdownInProgress> (
			reinterpret_cast<void*> ( GetProcAddress ( GetModuleHandleW ( L"ntdll.dll" ), "RtlDllShutdownInProgress" ) ) );
		return inProgress != nullptr && inProgress () != 0;
	}

	// The test application has ReShade in its exe, whose statics end before ExitProcess and outside the loader lock
	[[ nodiscard ]] bool isInsideExe ()
	{
		return reinterpret_cast<HMODULE> ( &__ImageBase ) == GetModuleHandleW ( nullptr );
	}

	// The failure goes into the waiting lines: it reaches the Log tab, and the file once writing works again
	void reportFileError ( Log& l, const char* what )
	{
		const auto	error = GetLastError ();

		if ( ! std::exchange ( l.hasWriteFailed, true ) )
			reshade::log::message ( reshade::log::level::error, "%s the log file failed with error code %lu.", what, error );
	}

	// Timed locks: at exit the writer may have been ended holding one
	void drain ()
	{
		auto&				l = logger ();
		std::unique_lock	file ( l.fileLock, lockTimeout );

		if ( ! file.owns_lock () )
			return;

		size_t	lost = 0;
		auto	isTruncating = false;
		{
			std::unique_lock	lock ( l.pendingLock, lockTimeout );

			if ( ! lock.owns_lock () )
				return;

			l.pending.swap ( l.draining );
			l.hasPending = false;
			lost = std::exchange ( l.dropped, size_t ( 0 ) );
			isTruncating = std::exchange ( l.isTruncateWanted, false );
		}

		if ( l.file != INVALID_HANDLE_VALUE )
		{
			if ( isTruncating && ( ! SetFilePointerEx ( l.file, LARGE_INTEGER {}, nullptr, FILE_BEGIN ) || ! SetEndOfFile ( l.file ) ) )
				reportFileError ( l, "Emptying" );

			if ( lost != 0 )
			{
				char		note[ 80 ];
				const auto	length = std::snprintf ( note, sizeof ( note ), "(%zu lines dropped, the log's buffer was full)\r\n", lost );
				l.draining.append ( note, size_t ( std::clamp ( length, 0, int ( sizeof ( note ) ) - 1 ) ) );
			}

			auto	written = DWORD ( 0 );

			if ( ! l.draining.empty () && ! WriteFile ( l.file, l.draining.data (), DWORD ( l.draining.size () ), &written, nullptr ) )
				reportFileError ( l, "Writing to" );
		}

		l.draining.clear ();
	}

	[[ nodiscard ]] bool isCrash ( DWORD code )
	{
		return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO
			   || code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
	}

	// First chance, last of the vectored handlers, for a game that replaces our filter without calling it or ends the
	// process on its own. Handled faults in a loop cost a drain at most every 250 ms, and only with lines waiting
	LONG CALLBACK onException ( EXCEPTION_POINTERS* e )
	{
		auto&	l = logger ();

		if ( ! isCrash ( e->ExceptionRecord->ExceptionCode ) || ! l.hasPending )
			return EXCEPTION_CONTINUE_SEARCH;

		const auto	now = GetTickCount64 ();
		auto		last = l.lastFaultDrain.load ();

		if ( now - last >= faultDrainEveryMs && l.lastFaultDrain.compare_exchange_strong ( last, now ) )
			drain ();

		return EXCEPTION_CONTINUE_SEARCH;
	}

	// The process dies of this one: everything waiting goes out, then the game's own filter. A game's filter that
	// calls the one it replaced (ours) comes back here
	LONG WINAPI onUnhandled ( EXCEPTION_POINTERS* e )
	{
		static thread_local auto	isInside = false;

		if ( isInside )
			return EXCEPTION_CONTINUE_SEARCH;

		isInside = true;
		drain ();
		const auto	games = logger ().gamesFilter.load ();
		const auto	result = games != nullptr ? games ( e ) : EXCEPTION_CONTINUE_SEARCH;
		isInside = false;
		return result;
	}

	// Once, before the add-ons load, so their filters chain to ours. Never set again over theirs: an add-on that finds a
	// filter over its own can't unload
	void installFilter ( Log& l )
	{
		l.gamesFilter = SetUnhandledExceptionFilter ( &onUnhandled );
		l.hasOwnFilter = true;
	}

	// Ours goes only while it is the process's: a filter set over it may call it after the unload
	void takeOutFilter ( Log& l )
	{
		if ( ! l.hasOwnFilter )
			return;

		l.hasOwnFilter = false;

		if ( const auto now = SetUnhandledExceptionFilter ( l.gamesFilter ); now != &onUnhandled )
		{
			SetUnhandledExceptionFilter ( now );
			reshade::log::message ( reshade::log::level::warning, "An unhandled exception filter (%p) was set over the log's own, which it may still call.",
									reinterpret_cast<void*> ( now ) );
		}
	}

	// Idle priority and background mode (low I/O and memory priority too): the writer never gets ahead of a game thread
	DWORD WINAPI writeLog ( void* )
	{
		auto&	l = logger ();
		SetThreadPriority ( GetCurrentThread (), THREAD_MODE_BACKGROUND_BEGIN );

		while ( ! l.isStopping )
		{
			WaitForSingleObject ( l.wake, drainEveryMs );
			drain ();
		}

		drain ();

		// Ending needs the loader lock, which the DllMain that unloads ReShade holds: it ends the thread here instead
		if ( l.shouldPark )
		{
			SetEvent ( l.parked );
			Sleep ( INFINITE );
		}

		return 0;
	}

	void startWriter ( Log& l )
	{
		if ( l.writer != nullptr )
			return;

		l.wake = CreateEventW ( nullptr, FALSE, FALSE, nullptr );
		l.parked = CreateEventW ( nullptr, TRUE, FALSE, nullptr );
		l.writer = l.wake != nullptr && l.parked != nullptr ? CreateThread ( nullptr, 0, &writeLog, nullptr, CREATE_SUSPENDED, nullptr ) : nullptr;

		if ( l.writer == nullptr )
		{
			reshade::log::message ( reshade::log::level::error, "Starting the log's writer thread failed with error code %lu: each line is written by the thread that logs it.",
									GetLastError () );
			return;
		}

		SetThreadPriority ( l.writer, THREAD_PRIORITY_IDLE );
		ResumeThread ( l.writer );
		l.hasWriter = true;
	}

	void stopWriter ( Log& l )
	{
		if ( l.writer == nullptr )
			return;

		l.hasWriter = false;

		// ExitProcess has ended every other thread already
		if ( ! isProcessExiting () )
		{
			l.shouldPark = ! isInsideExe ();
			l.isStopping = true;
			SetThreadPriority ( l.writer, THREAD_PRIORITY_NORMAL );
			SetEvent ( l.wake );

			if ( ! l.shouldPark )
			{
				if ( WaitForSingleObject ( l.writer, 5000 ) != WAIT_OBJECT_0 )
					OutputDebugStringA ( "ReShade: the log's writer thread did not end in 5 s.\n" );
			}
			else
			{
				if ( WaitForSingleObject ( l.parked, 5000 ) != WAIT_OBJECT_0 )
					OutputDebugStringA ( "ReShade: the log's writer thread did not finish in 5 s, it is ended where it is.\n" );

				// Safe here: parked in Sleep, it holds no lock, no heap and runs none of our code again
				TerminateThread ( l.writer, 0 );
				WaitForSingleObject ( l.writer, 1000 );
			}
		}

		CloseHandle ( l.writer );
		CloseHandle ( l.wake );
		CloseHandle ( l.parked );
		l.writer = l.wake = l.parked = nullptr;
	}

	void closeLog ()
	{
		auto&	l = logger ();

		if ( l.crashHandler != nullptr )
			RemoveVectoredExceptionHandler ( l.crashHandler );
		l.crashHandler = nullptr;

		stopWriter ( l );
		takeOutFilter ( l );
		drain ();
		l.hasFile = false;

		if ( std::unique_lock file ( l.fileLock, lockTimeout ); file.owns_lock () )
		{
			if ( l.file != INVALID_HANDLE_VALUE )
				CloseHandle ( l.file );
			l.file = INVALID_HANDLE_VALUE;
			std::string ().swap ( l.draining );
		}

		if ( std::unique_lock lock ( l.pendingLock, lockTimeout ); lock.owns_lock () )
		{
			std::string ().swap ( l.pending );
			std::deque<std::string> ().swap ( l.recent );
			l.recentBytes = 0;
		}
	}

	// Ends with this module's statics: after DllMain's last lines (DLL), after WinMain returned (test application)
	struct LogEnd
	{
		~LogEnd ()
		{
			closeLog ();
		}
	};

	LogEnd	logEnd;
}
//-----------------------------------------------------------------------------

bool reshade::ultra::openLog ( const std::filesystem::path& path, std::error_code& ec )
{
	auto&	l = logger ();
	{
		std::unique_lock	file ( l.fileLock );

		if ( l.file != INVALID_HANDLE_VALUE )
			CloseHandle ( l.file );

		l.file = CreateFileW ( path.c_str (), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
		l.hasFile = l.file != INVALID_HANDLE_VALUE;

		if ( ! l.hasFile )
		{
			ec.assign ( static_cast<int> ( GetLastError () ), std::system_category () );
			return false;
		}

		l.hasWriteFailed = false;
		l.draining.reserve ( pendingMax );
	}

	{
		std::unique_lock	lock ( l.pendingLock );
		l.pending.reserve ( pendingMax );
	}

	ec.clear ();
	startWriter ( l );

	if ( l.crashHandler == nullptr )
		l.crashHandler = AddVectoredExceptionHandler ( 0, &onException );

	if ( ! l.hasOwnFilter )
		installFilter ( l );

	return true;
}
//-----------------------------------------------------------------------------

void reshade::ultra::logLine ( std::string&& line )
{
	auto&	l = logger ();
	auto	shouldWake = false;
	{
		std::unique_lock	lock ( l.pendingLock, lockTimeout );

		if ( ! lock.owns_lock () )
			return;

		if ( l.hasFile )
		{
			const auto	size = sizeWithCrLf ( line );

			if ( l.pending.size () + size > pendingMax )
				++l.dropped;
			else
			{
				shouldWake = l.pending.size () < wakeAt && l.pending.size () + size >= wakeAt;
				appendWithCrLf ( l.pending, line );
				l.hasPending = true;
			}
		}

		l.recentBytes += line.size ();
		l.recent.push_back ( std::move ( line ) );

		while ( l.recent.size () > recentMax || ( l.recentBytes > recentBytesMax && l.recent.size () > 1 ) )
		{
			l.recentBytes -= l.recent.front ().size ();
			l.recent.pop_front ();
		}

		l.count.fetch_add ( 1, std::memory_order_release );
	}

	if ( ! l.hasWriter )
	{
		if ( l.hasFile )
			drain ();
	}
	else if ( shouldWake )
		SetEvent ( l.wake );
}
//-----------------------------------------------------------------------------

void reshade::ultra::flushLog ()
{
	drain ();
}
//-----------------------------------------------------------------------------

void reshade::ultra::clearLog ()
{
	auto&	l = logger ();

	if ( std::unique_lock lock ( l.pendingLock, lockTimeout ); lock.owns_lock () )
	{
		l.pending.clear ();
		l.dropped = 0;
		l.isTruncateWanted = true;
		l.recent.clear ();
		l.recentBytes = 0;
	}

	if ( l.hasWriter )
		SetEvent ( l.wake );
	else
		drain ();
}
//-----------------------------------------------------------------------------

std::string reshade::ultra::logModuleName ( const void* module )
{
	HMODULE	handle = nullptr;

	if ( module == nullptr || ! GetModuleHandleExW ( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR> ( module ), &handle )
		 || handle == g_module_handle )
		return {};

	wchar_t		path[ MAX_PATH ];
	const auto	length = GetModuleFileNameW ( handle, path, MAX_PATH );

	return length != 0 ? std::filesystem::path ( path, path + length ).filename ().u8string () : std::string ();
}
//-----------------------------------------------------------------------------

uint64_t reshade::ultra::logLineCount ()
{
	return logger ().count.load ( std::memory_order_acquire );
}
//-----------------------------------------------------------------------------

bool reshade::ultra::copyLogLines ( uint64_t& seen, std::vector<std::string>& lines )
{
	auto&				l = logger ();
	std::unique_lock	lock ( l.pendingLock, lockTimeout );

	if ( ! lock.owns_lock () )
		return true;

	const auto	count = l.count.load ( std::memory_order_relaxed );
	const auto	first = count - l.recent.size ();
	const auto	isComplete = seen >= first;

	for ( auto i = isComplete ? seen : first; i < count; ++i )
		lines.push_back ( l.recent[ size_t ( i - first ) ] );

	seen = count;
	return isComplete;
}
//-----------------------------------------------------------------------------

void ReShadeFlushLog ()
{
	reshade::ultra::flushLog ();
}
//-----------------------------------------------------------------------------
