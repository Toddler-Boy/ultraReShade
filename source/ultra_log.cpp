/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>

#include "ultra_log.hpp"

extern "C" IMAGE_DOS_HEADER __ImageBase;

//-----------------------------------------------------------------------------

namespace
{
	constexpr size_t	pendingMax = 1 << 20;
	constexpr size_t	wakeAt = pendingMax / 4;		// a burst goes out before the buffer fills
	constexpr DWORD		drainEveryMs = 50;				// at most this much is lost when the process is killed
	constexpr size_t	lineMax = 32 << 10;
	constexpr DWORD		scanChunk = 64 << 10;
	constexpr DWORD		moveChunk = 256 << 10;
	constexpr auto		lockTimeout = std::chrono::milliseconds ( 500 );
	constexpr ULONGLONG	faultDrainEveryMs = 250;		// games that handle access violations themselves raise thousands

	// A header is always at the start of a line; a line's own continuation never starts like one (see appendWithCrLf)
	constexpr std::string_view	runBoundary = "\n-- run start ";
	constexpr std::string_view	runPrefix = "-- run ";

	static_assert ( runBoundary.substr ( 1 ) == ultralog::runStartMarker );

	struct Log
	{
		ultralog::Id				id = ultralog::none;
		std::string					name;
		uint64_t					maxFileBytes = 0;

		// Timed: a thread that ExitProcess ended may still hold one. Taken in this order when both are
		std::timed_mutex			fileLock;			// file, writeOffset, startOffset, draining, hasWriteFailed
		std::timed_mutex			pendingLock;		// pending, dropped
		HANDLE						file = INVALID_HANDLE_VALUE;
		uint64_t					writeOffset = 0;	// where the next write goes: the end of the file
		uint64_t					startOffset = 0;	// the file's size when this run began
		std::string					draining;
		bool						hasWriteFailed = false;

		std::string					pending;
		size_t						dropped = 0;		// lines that found no room since the last drain

		std::atomic<bool>			hasFile = false;
		std::atomic<bool>			hasPending = false;
		std::atomic<bool>			isTrimPending = false;
		std::atomic<bool>			isEnded = false;
	};

	struct Registry
	{
		std::mutex					openLock;
		std::array<std::atomic<Log*>, ultralog::logsMax>	slots {};		// never freed: lines logged while other statics end still find them
		std::atomic<uint32_t>		count = 0;

		std::atomic<bool>			hasWriter = false;
		HANDLE						writer = nullptr;
		HANDLE						wake = nullptr;
		HANDLE						parked = nullptr;
		std::atomic<bool>			isStopping = false;
		std::atomic<bool>			shouldPark = false;

		PVOID						crashHandler = nullptr;
		std::atomic<ULONGLONG>		lastFaultDrain = 0;
		std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER>	gamesFilter = nullptr;		// called after ours
		std::atomic<bool>			hasOwnFilter = false;
		std::atomic<bool>			isCrashed = false;
	};

	[[ nodiscard ]] Registry& registry ()
	{
		static auto* const	instance = new Registry;
		return *instance;
	}

	[[ nodiscard ]] Log* find ( ultralog::Id id )
	{
		const auto&	r = registry ();
		return id >= 1 && id <= r.count.load ( std::memory_order_acquire ) ? r.slots[ id - 1 ].load ( std::memory_order_acquire ) : nullptr;
	}

	template <typename Visitor>
	void forEachLog ( Visitor visit )
	{
		const auto&	r = registry ();

		for ( auto i = uint32_t ( 0 ), n = r.count.load ( std::memory_order_acquire ); i < n; ++i )
			if ( auto* const l = r.slots[ i ].load ( std::memory_order_acquire ); l != nullptr )
				visit ( *l );
	}

	[[ nodiscard ]] std::string asciiOf ( std::string_view text )
	{
		std::string	out ( text );

		for ( auto& c : out )
			if ( c < 32 || c > 126 )
				c = '?';
			else if ( c == '"' )
				c = '\'';

		return out;
	}

	[[ nodiscard ]] std::string formatted ( const char* format, ... )
	{
		char	text[ 512 ];
		va_list	args;
		va_start ( args, format );
		const auto	length = std::vsnprintf ( text, sizeof ( text ), format, args );
		va_end ( args );
		return std::string ( text, size_t ( std::clamp ( length, 0, int ( sizeof ( text ) ) - 1 ) ) );
	}

	[[ nodiscard ]] std::string timeText ()
	{
		SYSTEMTIME	t;
		GetLocalTime ( &t );
		return formatted ( "%04d-%02d-%02d %02d:%02d:%02d.%03d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds );
	}

	[[ nodiscard ]] std::string exeName ()
	{
		wchar_t		path[ MAX_PATH ];
		const auto	length = GetModuleFileNameW ( nullptr, path, MAX_PATH );
		const auto	name = std::wstring_view ( path, length );
		const auto	slash = name.find_last_of ( L"\\/" );
		const auto	file = slash == std::wstring_view::npos ? name : name.substr ( slash + 1 );
		std::string	ascii;

		for ( const auto c : file )
			ascii += c >= 32 && c <= 126 && c != L'"' ? char ( c ) : '?';

		return ascii;
	}

	[[ nodiscard ]] std::string runHeader ( std::string_view build )
	{
		constexpr size_t	buildMax = 200;
		std::string			line ( ultralog::runStartMarker );
		line += timeText ();
		line += formatted ( " pid %lu exe \"", GetCurrentProcessId () );
		line += exeName ();
		line += "\" build \"";
		line += asciiOf ( build.substr ( 0, buildMax ) );
		line += "\"\n";
		return line;
	}

	[[ nodiscard ]] const char* levelName ( ultralog::Level level )
	{
		switch ( level )
		{
			case ultralog::Level::warning:	return "WARN ";
			case ultralog::Level::info:		return "INFO ";
			case ultralog::Level::debug:	return "DEBUG";
			case ultralog::Level::error:	return "ERROR";
		}

		return "ERROR";
	}

	[[ nodiscard ]] std::string makeLine ( ultralog::Level level, std::string_view message )
	{
		while ( ! message.empty () && ( message.back () == '\n' || message.back () == '\r' ) )
			message.remove_suffix ( 1 );

		const auto	isCut = message.size () > lineMax;

		if ( isCut )
			message = message.substr ( 0, lineMax );

		SYSTEMTIME	t;
		GetLocalTime ( &t );
		auto		line = formatted ( "%02d:%02d:%02d.%03d [%5lu] | %s | ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId (), levelName ( level ) );
		line.append ( message );

		if ( isCut )
			line += " ... (line cut)";

		line += '\n';
		return line;
	}

	[[ nodiscard ]] size_t sizeWithCrLf ( const std::string& line )
	{
		return line.size () + size_t ( std::ranges::count ( line, '\n' ) );
	}

	// A continuation line that would read as a header gets a space in front
	void appendWithCrLf ( std::string& out, const std::string& line )
	{
		for ( size_t begin = 0; begin < line.size (); )
		{
			const auto	end = std::min ( line.find ( '\n', begin ), line.size () );

			if ( begin != 0 && std::string_view ( line ).substr ( begin ).starts_with ( runPrefix ) )
				out += ' ';

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

	// An exe that holds this code ends its statics before ExitProcess and outside the loader lock
	[[ nodiscard ]] bool isInsideExe ()
	{
		return reinterpret_cast<HMODULE> ( &__ImageBase ) == GetModuleHandleW ( nullptr );
	}

	// Puts a line among the waiting ones; true when the writer should wake for a burst. forced: it is never dropped for
	// the buffer being full (header, end line, the log's own errors)
	[[ nodiscard ]] bool queue ( Log& l, const std::string& line, bool isForced, std::string_view filePrefix = {} )
	{
		std::unique_lock	lock ( l.pendingLock, lockTimeout );

		if ( ! lock.owns_lock () || ! l.hasFile )
			return false;

		const auto	size = sizeWithCrLf ( line ) + filePrefix.size ();
		auto		shouldWake = false;

		if ( ! isForced && l.pending.size () + size > pendingMax )
			++l.dropped;
		else
		{
			shouldWake = l.pending.size () < wakeAt && l.pending.size () + size >= wakeAt;
			l.pending.append ( filePrefix );
			appendWithCrLf ( l.pending, line );
		}

		l.hasPending = true;
		return shouldWake;
	}

	[[ nodiscard ]] bool readAt ( HANDLE file, uint64_t offset, void* data, DWORD size )
	{
		OVERLAPPED	where {};
		where.Offset = DWORD ( offset );
		where.OffsetHigh = DWORD ( offset >> 32 );
		auto		got = DWORD ( 0 );
		return ReadFile ( file, data, size, &got, &where ) && got == size;
	}

	[[ nodiscard ]] bool writeAt ( HANDLE file, uint64_t offset, const char* data, size_t size )
	{
		for ( size_t done = 0; done < size; )
		{
			OVERLAPPED	where {};
			where.Offset = DWORD ( offset + done );
			where.OffsetHigh = DWORD ( ( offset + done ) >> 32 );
			auto		written = DWORD ( 0 );

			if ( ! WriteFile ( file, data + done, DWORD ( std::min<size_t> ( size - done, pendingMax ) ), &written, &where ) || written == 0 )
				return false;

			done += written;
		}

		return true;
	}

	// The failure goes into the waiting lines: it reaches the viewers, and the file once writing works again
	void reportFileError ( Log& l, const char* what )
	{
		const auto	error = GetLastError ();

		if ( ! std::exchange ( l.hasWriteFailed, true ) )
			(void) queue ( l, makeLine ( ultralog::Level::error, formatted ( "%s the log file failed with Windows error %lu.", what, error ) ), true );
	}

	// Where each run of the file's first size bytes begins. The first begins at 0 whatever the file starts with
	[[ nodiscard ]] bool findRuns ( HANDLE file, uint64_t size, std::vector<uint64_t>& runs )
	{
		std::vector<char>	buffer ( scanChunk + runBoundary.size () );
		auto				carry = size_t ( 0 );
		runs.push_back ( 0 );

		for ( auto at = uint64_t ( 0 ); at < size; )
		{
			const auto	want = DWORD ( std::min<uint64_t> ( scanChunk, size - at ) );

			if ( ! readAt ( file, at, buffer.data () + carry, want ) )
				return false;

			const std::string_view	view ( buffer.data (), carry + want );

			for ( auto from = view.find ( runBoundary ); from != std::string_view::npos; from = view.find ( runBoundary, from + 1 ) )
				runs.push_back ( at - carry + from + 1 );

			carry = std::min ( view.size (), runBoundary.size () - 1 );
			std::memmove ( buffer.data (), view.data () + view.size () - carry, carry );
			at += want;
		}

		return true;
	}

	// The oldest whole runs go until what is left fits; the last run stays whatever its size. File lock held, writer thread
	void trim ( Log& l )
	{
		l.isTrimPending = false;

		std::vector<uint64_t>	runs;

		if ( ! findRuns ( l.file, l.startOffset, runs ) )
			return reportFileError ( l, "Reading" );

		const auto	fits = std::ranges::find_if ( runs, [&l] ( const uint64_t at ) { return l.startOffset - at <= l.maxFileBytes; } );
		const auto	cut = fits != runs.end () ? *fits : runs.back ();

		if ( cut == 0 )
			return;

		std::vector<char>	buffer ( moveChunk );
		const auto			end = l.writeOffset;
		auto				to = uint64_t ( 0 );

		for ( auto from = cut; from < end; )
		{
			const auto	size = DWORD ( std::min<uint64_t> ( moveChunk, end - from ) );

			if ( ! readAt ( l.file, from, buffer.data (), size ) || ! writeAt ( l.file, to, buffer.data (), size ) )
				return reportFileError ( l, "Trimming" );

			from += size;
			to += size;
		}

		LARGE_INTEGER	length {};
		length.QuadPart = LONGLONG ( to );

		if ( ! SetFilePointerEx ( l.file, length, nullptr, FILE_BEGIN ) || ! SetEndOfFile ( l.file ) )
			return reportFileError ( l, "Shortening" );

		l.writeOffset = to;
		const auto	kept = size_t ( std::ranges::count_if ( runs, [cut] ( const uint64_t at ) { return at >= cut; } ) );
		(void) queue ( l, makeLine ( ultralog::Level::info, formatted ( "The log's oldest runs went: %llu bytes off its start, %zu of %zu runs are left (limit %llu bytes).",
																		  (unsigned long long) cut, kept, runs.size (), (unsigned long long) l.maxFileBytes ) ), true );
	}

	// mayTrim: the writer thread's, or the caller's when there is no writer thread
	void drain ( Log& l, bool mayTrim )
	{
		const auto	isTrimming = mayTrim && l.isTrimPending.load ();

		if ( ! l.hasPending && ! isTrimming )
			return;

		std::unique_lock	file ( l.fileLock, lockTimeout );

		if ( ! file.owns_lock () )
			return;

		if ( isTrimming && l.file != INVALID_HANDLE_VALUE )
			trim ( l );

		size_t	lost = 0;
		{
			std::unique_lock	lock ( l.pendingLock, lockTimeout );

			if ( ! lock.owns_lock () )
				return;

			l.pending.swap ( l.draining );
			l.hasPending = false;
			lost = std::exchange ( l.dropped, size_t ( 0 ) );
		}

		if ( l.file != INVALID_HANDLE_VALUE )
		{
			if ( lost != 0 )
				l.draining += formatted ( "(%zu lines dropped, the log's buffer was full)\r\n", lost );

			if ( ! l.draining.empty () )
			{
				if ( writeAt ( l.file, l.writeOffset, l.draining.data (), l.draining.size () ) )
					l.writeOffset += l.draining.size ();
				else
					reportFileError ( l, "Writing to" );
			}
		}

		l.draining.clear ();
	}

	void serviceAll ()
	{
		forEachLog ( [] ( Log& l ) { drain ( l, true ); } );
	}

	void append ( Log& l, const std::string& line, bool isForced, std::string_view filePrefix = {} )
	{
		const auto	shouldWake = queue ( l, line, isForced, filePrefix );
		auto&		r = registry ();

		if ( ! r.hasWriter )
			drain ( l, true );
		else if ( shouldWake )
			SetEvent ( r.wake );
	}

	[[ nodiscard ]] bool isCrash ( DWORD code )
	{
		return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO
			   || code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
	}

	[[ nodiscard ]] bool hasPendingLines ()
	{
		auto	has = false;
		forEachLog ( [&has] ( const Log& l ) { has = has || l.hasPending; } );
		return has;
	}

	// First chance, last of the vectored handlers, for a game that replaces our filter without calling it or ends the
	// process on its own. Handled faults in a loop cost a drain at most every 250 ms, and only with lines waiting
	LONG CALLBACK onException ( EXCEPTION_POINTERS* e )
	{
		auto&	r = registry ();

		if ( ! isCrash ( e->ExceptionRecord->ExceptionCode ) || ! hasPendingLines () )
			return EXCEPTION_CONTINUE_SEARCH;

		const auto	now = GetTickCount64 ();
		auto		last = r.lastFaultDrain.load ();

		if ( now - last >= faultDrainEveryMs && r.lastFaultDrain.compare_exchange_strong ( last, now ) )
			ultralog::flushAll ();

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
		auto&	r = registry ();
		r.isCrashed = true;
		ultralog::flushAll ();
		const auto	games = r.gamesFilter.load ();
		const auto	result = games != nullptr ? games ( e ) : EXCEPTION_CONTINUE_SEARCH;
		isInside = false;
		return result;
	}

	// Once, before the add-ons load, so their filters chain to ours. Never set again over theirs: an add-on that finds a
	// filter over its own can't unload
	void installCrashHandlers ( Registry& r )
	{
		if ( r.crashHandler == nullptr )
			r.crashHandler = AddVectoredExceptionHandler ( 0, &onException );

		if ( ! r.hasOwnFilter )
		{
			r.gamesFilter = SetUnhandledExceptionFilter ( &onUnhandled );
			r.hasOwnFilter = true;
		}
	}

	// Ours goes only while it is the process's: a filter set over it may call it after the unload
	void takeOutCrashHandlers ( Registry& r )
	{
		if ( r.crashHandler != nullptr )
			RemoveVectoredExceptionHandler ( r.crashHandler );
		r.crashHandler = nullptr;

		if ( ! r.hasOwnFilter )
			return;

		r.hasOwnFilter = false;

		if ( const auto now = SetUnhandledExceptionFilter ( r.gamesFilter ); now != &onUnhandled )
		{
			SetUnhandledExceptionFilter ( now );

			if ( auto* const first = find ( 1 ); first != nullptr )
				(void) queue ( *first, makeLine ( ultralog::Level::warning, formatted ( "An unhandled exception filter (%p) was set over the log's own, which it may still call.",
																					   reinterpret_cast<void*> ( now ) ) ), true );
		}
	}

	// Idle priority and background mode (low I/O and memory priority too): the writer never gets ahead of a game thread
	DWORD WINAPI writeLogs ( void* )
	{
		auto&	r = registry ();
		SetThreadPriority ( GetCurrentThread (), THREAD_MODE_BACKGROUND_BEGIN );

		while ( ! r.isStopping )
		{
			WaitForSingleObject ( r.wake, drainEveryMs );
			serviceAll ();
		}

		serviceAll ();

		// Ending needs the loader lock, which the DllMain that unloads ReShade holds: it ends the thread here instead
		if ( r.shouldPark )
		{
			SetEvent ( r.parked );
			Sleep ( INFINITE );
		}

		return 0;
	}

	[[ nodiscard ]] bool startWriter ( Registry& r )
	{
		if ( r.writer != nullptr )
			return true;

		r.wake = CreateEventW ( nullptr, FALSE, FALSE, nullptr );
		r.parked = CreateEventW ( nullptr, TRUE, FALSE, nullptr );
		r.writer = r.wake != nullptr && r.parked != nullptr ? CreateThread ( nullptr, 0, &writeLogs, nullptr, CREATE_SUSPENDED, nullptr ) : nullptr;

		if ( r.writer == nullptr )
			return false;

		SetThreadPriority ( r.writer, THREAD_PRIORITY_IDLE );
		ResumeThread ( r.writer );
		r.hasWriter = true;
		return true;
	}

	void stopWriter ( Registry& r )
	{
		if ( r.writer == nullptr )
			return;

		r.hasWriter = false;

		// ExitProcess has ended every other thread already
		if ( ! isProcessExiting () )
		{
			r.shouldPark = ! isInsideExe ();
			r.isStopping = true;
			SetThreadPriority ( r.writer, THREAD_PRIORITY_NORMAL );
			SetEvent ( r.wake );

			if ( ! r.shouldPark )
			{
				if ( WaitForSingleObject ( r.writer, 5000 ) != WAIT_OBJECT_0 )
					OutputDebugStringA ( "ultralog: the writer thread did not end in 5 s.\n" );
			}
			else
			{
				if ( WaitForSingleObject ( r.parked, 5000 ) != WAIT_OBJECT_0 )
					OutputDebugStringA ( "ultralog: the writer thread did not finish in 5 s, it is ended where it is.\n" );

				// Safe here: parked in Sleep, it holds no lock, no heap and runs none of our code again
				TerminateThread ( r.writer, 0 );
				WaitForSingleObject ( r.writer, 1000 );
			}
		}

		CloseHandle ( r.writer );
		CloseHandle ( r.wake );
		CloseHandle ( r.parked );
		r.writer = r.wake = r.parked = nullptr;
	}

	void closeFile ( Log& l )
	{
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
		}
	}

	void shutdown ()
	{
		auto&	r = registry ();
		stopWriter ( r );
		takeOutCrashHandlers ( r );
		ultralog::endRuns ();
		forEachLog ( closeFile );
	}

	// Ends with this module's statics: after DllMain's last lines (DLL), after WinMain returned (exe)
	struct LogEnd
	{
		~LogEnd ()
		{
			shutdown ();
		}
	};

	LogEnd	logEnd;
}
//-----------------------------------------------------------------------------

bool ultralog::isValidName ( std::string_view name )
{
	return ! name.empty () && name.size () <= namesMax && std::ranges::all_of ( name, [] ( const char c )
	{
		return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '-' || c == '_';
	} );
}
//-----------------------------------------------------------------------------

ultralog::Opened ultralog::open ( const std::filesystem::path& folder, std::string_view name, uint64_t maxBytesOfFile, std::string_view build )
{
	if ( ! isValidName ( name ) )
		return { none, OpenStatus::badName, {} };

	if ( maxBytesOfFile < minBytes || maxBytesOfFile > maxBytes )
		return { none, OpenStatus::badSize, {} };

	auto&					r = registry ();
	std::scoped_lock		guard ( r.openLock );
	const auto				used = r.count.load ();

	for ( auto i = uint32_t ( 0 ); i < used; ++i )
		if ( const auto* const other = r.slots[ i ].load (); other != nullptr && _stricmp ( other->name.c_str (), std::string ( name ).c_str () ) == 0 )
			return { other->id, OpenStatus::existing, {} };

	if ( used >= logsMax )
		return { none, OpenStatus::tooMany, {} };

	auto*			l = new Log;
	std::error_code	error;
	l->id = used + 1;
	l->name = name;
	l->maxFileBytes = maxBytesOfFile;

	std::error_code	ignored;
	std::filesystem::create_directories ( folder, ignored );

	const auto	path = folder / std::filesystem::path ( std::string ( name ) + ".log" );
	l->file = CreateFileW ( path.c_str (), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );

	if ( l->file == INVALID_HANDLE_VALUE )
		error.assign ( int ( GetLastError () ), std::system_category () );
	else if ( LARGE_INTEGER size {}; GetFileSizeEx ( l->file, &size ) )
	{
		l->startOffset = l->writeOffset = uint64_t ( size.QuadPart );
		l->isTrimPending = l->startOffset > maxBytesOfFile;
		l->hasFile = true;
	}
	else
	{
		error.assign ( int ( GetLastError () ), std::system_category () );
		CloseHandle ( std::exchange ( l->file, INVALID_HANDLE_VALUE ) );
	}

	(void) queue ( *l, runHeader ( build ), true, l->startOffset != 0 ? "\r\n" : "" );

	r.slots[ l->id - 1 ].store ( l, std::memory_order_release );
	r.count.store ( l->id, std::memory_order_release );

	installCrashHandlers ( r );

	if ( ! startWriter ( r ) )
		(void) queue ( *l, makeLine ( Level::error, formatted ( "Starting the log's writer thread failed with Windows error %lu: each line is written by the thread that logs it.", GetLastError () ) ), true );

	return { l->id, error ? OpenStatus::fileFailed : OpenStatus::opened, error };
}
//-----------------------------------------------------------------------------

bool ultralog::isKnown ( Id log )
{
	return find ( log ) != nullptr;
}
//-----------------------------------------------------------------------------

void ultralog::write ( Id log, Level level, std::string_view message )
{
	if ( auto* const l = find ( log ); l != nullptr )
		append ( *l, makeLine ( level, message ), false );
}
//-----------------------------------------------------------------------------

void ultralog::flush ( Id log )
{
	if ( auto* const l = find ( log ); l != nullptr )
		drain ( *l, false );
}
//-----------------------------------------------------------------------------

void ultralog::flushAll ()
{
	forEachLog ( [] ( Log& l ) { drain ( l, false ); } );
}
//-----------------------------------------------------------------------------

void ultralog::endRuns ()
{
	if ( ! registry ().isCrashed )
		forEachLog ( [] ( Log& l )
		{
			if ( l.hasFile && ! l.isEnded.exchange ( true ) )
				(void) queue ( l, formatted ( "%s%s\n", runEndMarker.data (), timeText ().c_str () ), true );
		} );

	flushAll ();
}
//-----------------------------------------------------------------------------

void ultralog::noteCrash ()
{
	registry ().isCrashed = true;
	flushAll ();
}
//-----------------------------------------------------------------------------

std::string_view ultralog::lastRun ( std::string_view fileText )
{
	const auto	at = fileText.rfind ( runBoundary );
	return at != std::string_view::npos ? fileText.substr ( at + 1 ) : fileText;
}
