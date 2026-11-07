/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cwchar>
#include <cwctype>

#include <Windows.h>

#include "dll_log.hpp"
#include "ini_file.hpp"
#include "ultra_config.hpp"

//-----------------------------------------------------------------------------

namespace
{
	constexpr wchar_t	curatedName[] = L"ultragame";
	constexpr wchar_t	userSuffix[] = L"-user";

	[[ nodiscard ]] bool isSameText ( const std::wstring& a, const std::wstring& b )
	{
		return _wcsicmp ( a.c_str (), b.c_str () ) == 0;
	}
}
//-----------------------------------------------------------------------------

bool reshade::ultra::isConfigLayered ()
{
	static const auto	isLayered = isCuratedConfigFile ( g_reshade_base_path / L"ultragame.ini" );
	return isLayered;
}
//-----------------------------------------------------------------------------

// Through the profile API, which shares the file: the add-on writes it the same way
bool reshade::ultra::isCuratedConfigFile ( const std::filesystem::path& path )
{
	return GetPrivateProfileIntW ( L"INSTALL", L"UltraGameConfig", 0, path.c_str () ) == 1;
}
//-----------------------------------------------------------------------------

std::string reshade::ultra::configBaseName ()
{
	return isConfigLayered () ? "ultragame" : "ReShade";
}
//-----------------------------------------------------------------------------

// The names create_effect_runtime gives: the base name, then VR, then a number from the second runtime on
bool reshade::ultra::isCuratedConfig ( const std::filesystem::path& path )
{
	if ( ! isConfigLayered () || ! isSameText ( path.extension (), L".ini" ) || ! isSameText ( path.parent_path (), g_reshade_base_path ) )
		return false;

	const auto	stem = path.stem ().wstring ();
	const auto	nameLength = std::wcslen ( curatedName );

	if ( stem.size () < nameLength || ! isSameText ( stem.substr ( 0, nameLength ), curatedName ) )
		return false;

	auto	rest = stem.substr ( nameLength );

	if ( rest.size () >= 2 && isSameText ( rest.substr ( 0, 2 ), L"VR" ) )
		rest.erase ( 0, 2 );

	return std::ranges::all_of ( rest, [] ( wchar_t c ) { return std::iswdigit ( c ) != 0; } );
}
//-----------------------------------------------------------------------------

std::filesystem::path reshade::ultra::userConfigPath ( const std::filesystem::path& curated )
{
	return curated.parent_path () / ( curated.stem ().wstring () + userSuffix + curated.extension ().wstring () );
}
//-----------------------------------------------------------------------------

std::filesystem::path reshade::ultra::basePathConfig ( const std::filesystem::path& folder )
{
	const auto	curated = folder / L"ultragame.ini";

	return isCuratedConfigFile ( curated ) ? curated : folder / L"ReShade.ini";
}
//-----------------------------------------------------------------------------

std::filesystem::path reshade::ultra::logPath ()
{
	return g_reshade_base_path / L"ReShade.log";
}
//-----------------------------------------------------------------------------

void reshade::ultra::logConfigLayers ()
{
	if ( ! isConfigLayered () )
		return;

	const auto	curated = g_reshade_base_path / L"ultragame.ini";
	reshade::log::message ( reshade::log::level::info, "Settings come from '%s' with '%s' on top; changes go into the latter.", curated.u8string ().c_str (),
							userConfigPath ( curated ).u8string ().c_str () );

	std::error_code	ec;
	if ( std::filesystem::exists ( g_reshade_base_path / L"ReShade.ini", ec ) )
		reshade::log::message ( reshade::log::level::warning, "'%s' is ignored while ultragame.ini holds the settings.", ( g_reshade_base_path / L"ReShade.ini" ).u8string ().c_str () );
}
