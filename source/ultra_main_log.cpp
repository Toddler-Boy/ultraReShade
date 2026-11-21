/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <atomic>
#include <cstring>
#include <filesystem>
#include <intrin.h>
#include <string>

#include <Windows.h>

#include <reshade_ultra.hpp>

#include "dll_log.hpp"
#include "ultra_main_log.hpp"
#include "version.h"

extern HMODULE g_module_handle;
extern std::filesystem::path g_reshade_base_path;

//-----------------------------------------------------------------------------

namespace
{
	constexpr char		mainLogName[] = "reshade";
	constexpr uint64_t	mainLogMaxBytes = uint64_t ( 2 ) << 20;		// about 20 KB a run, so around 100 runs

	std::atomic<ultralog::Id>	mainLogId = ultralog::none;

	// The home's log folder is the base path; one handle tag keeps another API's numbers from passing for a log
	constexpr uint64_t	handleTag = uint64_t ( 0x4C4F4721 ) << 32;

	[[ nodiscard ]] uint64_t toHandle ( ultralog::Id id )
	{
		return handleTag | id;
	}

	[[ nodiscard ]] ultralog::Id fromHandle ( uint64_t handle )
	{
		return ( handle >> 32 ) == ( handleTag >> 32 ) && ultralog::isKnown ( ultralog::Id ( handle ) ) ? ultralog::Id ( handle ) : ultralog::none;
	}

	[[ nodiscard ]] std::string buildText ()
	{
		return std::string ( VERSION_STRING_FILE ) + ( ULTRARESHADE_DEVELOPMENT ? " development" : " release" ) + ( sizeof ( void* ) == 8 ? " 64-bit" : " 32-bit" );
	}

	// A call that is refused says so once per cause: a caller in a loop would fill the log
	void refuseOnce ( std::atomic<bool>& isReported, const void* caller, const char* what )
	{
		if ( isReported.exchange ( true ) )
			return;

		const auto	name = reshade::ultra::logModuleName ( caller );
		reshade::log::message ( reshade::log::level::error, "%s%s%s%s", name.empty () ? "" : "[", name.c_str (), name.empty () ? "" : "] ", what );
	}
}
//-----------------------------------------------------------------------------

bool reshade::ultra::openMainLog ()
{
	const auto	opened = ultralog::open ( g_reshade_base_path, mainLogName, mainLogMaxBytes, buildText () );
	mainLogId = opened.id;

	if ( opened.status == ultralog::OpenStatus::fileFailed )
		log::message ( log::level::error, "Opening '%s' failed with Windows error %d: its lines are not written.", ( g_reshade_base_path / L"reshade.log" ).u8string ().c_str (),
					   opened.error.value () );

	return opened.status == ultralog::OpenStatus::opened;
}
//-----------------------------------------------------------------------------

ultralog::Id reshade::ultra::mainLog ()
{
	return mainLogId;
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

void ReShadeFlushLog ()
{
	ultralog::flushAll ();
}
//-----------------------------------------------------------------------------

#if defined(RESHADE_API_LIBRARY_EXPORT)

uint64_t ReShadeLogOpen ( const char* name, uint64_t max_bytes, const char* build )
{
	static std::atomic<bool>	isNameReported;
	static std::atomic<bool>	isSizeReported;
	static std::atomic<bool>	isCountReported;

	if ( GetEnvironmentVariableW ( L"RESHADE_DISABLE_LOGGING", nullptr, 0 ) != 0 )
		return 0;

	const auto	caller = _ReturnAddress ();
	const auto	text = name != nullptr ? std::string_view ( name, strnlen ( name, ultralog::namesMax + 1 ) ) : std::string_view ();
	const auto	opened = ultralog::open ( g_reshade_base_path, text, max_bytes, build != nullptr ? std::string_view ( build, strnlen ( build, 256 ) ) : std::string_view () );

	switch ( opened.status )
	{
		case ultralog::OpenStatus::opened:
		case ultralog::OpenStatus::existing:
			break;

		case ultralog::OpenStatus::fileFailed:
			reshade::log::message ( reshade::log::level::error, "Log '%.64s': opening its file failed with Windows error %d, its lines are not written.", std::string ( text ).c_str (),
									opened.error.value () );
			break;

		case ultralog::OpenStatus::badName:
			refuseOnce ( isNameReported, caller, "A log was opened with a name that is not 1 to 64 letters, digits, '-' or '_'." );
			return 0;

		case ultralog::OpenStatus::badSize:
			refuseOnce ( isSizeReported, caller, "A log was opened with a maximum size outside 64 KiB to 1 GiB." );
			return 0;

		case ultralog::OpenStatus::tooMany:
			refuseOnce ( isCountReported, caller, "A log was opened while 64 logs are open already." );
			return 0;
	}

	return toHandle ( opened.id );
}
//-----------------------------------------------------------------------------

bool ReShadeLogWrite ( uint64_t log, int level, const char* message )
{
	static std::atomic<bool>	isHandleReported;
	static std::atomic<bool>	isLevelReported;
	static std::atomic<bool>	isMessageReported;

	const auto	id = fromHandle ( log );

	if ( id == ultralog::none )
	{
		refuseOnce ( isHandleReported, _ReturnAddress (), "A line was written to a log that is not open (a handle ReShadeLogOpen did not give)." );
		return false;
	}

	if ( level < int ( ultralog::Level::error ) || level > int ( ultralog::Level::debug ) )
	{
		refuseOnce ( isLevelReported, _ReturnAddress (), "A line was written to a log with a level outside 1 to 4." );
		return false;
	}

	if ( message == nullptr )
	{
		refuseOnce ( isMessageReported, _ReturnAddress (), "A line was written to a log without a message." );
		return false;
	}

	ultralog::write ( id, static_cast<ultralog::Level> ( level ), message );
	return true;
}
//-----------------------------------------------------------------------------

bool ReShadeLogFlush ( uint64_t log )
{
	static std::atomic<bool>	isHandleReported;

	const auto	id = fromHandle ( log );

	if ( id == ultralog::none )
	{
		refuseOnce ( isHandleReported, _ReturnAddress (), "A log was flushed that is not open (a handle ReShadeLogOpen did not give)." );
		return false;
	}

	ultralog::flush ( id );
	return true;
}
//-----------------------------------------------------------------------------

#endif
