/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <string>

#include <Windows.h>

#include <reshade_ultra_json.hpp>

#include "dll_log.hpp"
#include "ultra_home.hpp"
#include "ultra_json.hpp"

extern std::filesystem::path g_reshade_base_path;

//-----------------------------------------------------------------------------

namespace
{
	// The index's folder (%APPDATA%\ultraGame\user): homes are in its games folder, ultraGame's settings beside it
	std::filesystem::path	indexFolder;
	std::string				title;

	[[ nodiscard ]] std::wstring environmentValue ( const wchar_t* name )
	{
		const auto		size = GetEnvironmentVariableW ( name, nullptr, 0 );
		std::wstring	value ( size, L'\0' );
		value.resize ( size != 0 ? GetEnvironmentVariableW ( name, value.data (), size ) : 0 );
		return value;
	}

	[[ nodiscard ]] std::filesystem::path indexPath ()
	{
		if ( auto other = environmentValue ( reshade::ultra::indexVariable ); ! other.empty () )
			return std::filesystem::path ( std::move ( other ) );

		const auto	appData = environmentValue ( L"APPDATA" );
		return appData.empty () ? std::filesystem::path () : std::filesystem::path ( appData ) / L"ultraGame" / L"user" / L"installed.json";
	}

	// As the index writes them: lower case, backslashes
	[[ nodiscard ]] std::string indexKey ( const std::filesystem::path& exe )
	{
		auto	text = exe.native ();
		std::ranges::replace ( text, L'/', L'\\' );

		std::wstring	lower ( text.size (), L'\0' );
		const auto		size = LCMapStringEx ( LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.c_str (), static_cast<int> ( text.size () ), lower.data (),
											   static_cast<int> ( lower.size () ), nullptr, nullptr, 0 );
		lower.resize ( static_cast<size_t> ( std::max ( size, 0 ) ) );

		const auto	bytes = WideCharToMultiByte ( CP_UTF8, 0, lower.c_str (), static_cast<int> ( lower.size () ), nullptr, 0, nullptr, nullptr );
		std::string	key ( static_cast<size_t> ( std::max ( bytes, 0 ) ), '\0' );
		WideCharToMultiByte ( CP_UTF8, 0, lower.c_str (), static_cast<int> ( lower.size () ), key.data (), bytes, nullptr, nullptr );
		return key;
	}

	void tellDebugger ( const std::wstring& text )
	{
		OutputDebugStringW ( ( L"ultraReShade: " + text + L"\n" ).c_str () );
	}
}
//-----------------------------------------------------------------------------

std::filesystem::path reshade::ultra::findHome ( const std::filesystem::path& exe )
{
	const auto	index = indexPath ();

	if ( index.empty () )
	{
		tellDebugger ( L"no APPDATA folder, so no index of installed games: inactive." );
		return {};
	}

	json::error	error;
	const auto	root = readJsonFile ( index, error );

	if ( root == nullptr )
	{
		tellDebugger ( std::wstring ( L"the index of installed games can't be used, inactive: " ) + std::filesystem::u8path ( error.message ).wstring () );
		return {};
	}

	const char*	folder = nullptr;
	size_t		length = 0;

	if ( ! ReShadeJsonGetString ( ReShadeJsonFind ( ReShadeJsonFind ( root.get (), "exes" ), indexKey ( exe ).c_str () ), &folder, &length ) || length == 0 )
	{
		tellDebugger ( L"'" + exe.native () + L"' is not in '" + index.native () + L"': inactive." );
		return {};
	}

	const std::string	homeName ( folder, length );
	const char*			text = nullptr;

	if ( ReShadeJsonGetString ( ReShadeJsonFind ( ReShadeJsonFind ( ReShadeJsonFind ( root.get (), "games" ), homeName.c_str () ), "title" ), &text, &length ) )
		title.assign ( text, length );

	indexFolder = index.parent_path ();
	return indexFolder / L"games" / std::filesystem::u8path ( homeName );
}
//-----------------------------------------------------------------------------

std::filesystem::path reshade::ultra::curatedSettingsPath ()
{
	return g_reshade_base_path / L"reshade.json";
}
//-----------------------------------------------------------------------------

std::filesystem::path reshade::ultra::userSettingsPath ()
{
	return g_reshade_base_path / L"reshade-user.json";
}
//-----------------------------------------------------------------------------

std::filesystem::path reshade::ultra::logPath ()
{
	return g_reshade_base_path / L"reshade.log";
}
//-----------------------------------------------------------------------------

std::filesystem::path reshade::ultra::appSettingsPath ()
{
	return indexFolder.parent_path () / L"settings.json";
}
//-----------------------------------------------------------------------------

const std::string& reshade::ultra::gameTitle ()
{
	return title;
}
//-----------------------------------------------------------------------------

void reshade::ultra::logHome ()
{
	reshade::log::message ( reshade::log::level::info, "Home '%s': '%s' with '%s' on top (changes go into the latter), hotkeys from '%s'.",
							g_reshade_base_path.u8string ().c_str (), curatedSettingsPath ().filename ().u8string ().c_str (),
							userSettingsPath ().filename ().u8string ().c_str (), appSettingsPath ().u8string ().c_str () );
}
