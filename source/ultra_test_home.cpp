/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <Windows.h>
#include <shellapi.h>

#include "ultra_home.hpp"
#include "ultra_test_home.hpp"

//-----------------------------------------------------------------------------

namespace
{
	// The home's folder name in the index: the test application's title
	constexpr auto	homeName = "ReShade Test App";

	struct Options
	{
		std::filesystem::path		work;
		std::vector<std::string>	addons;
		bool						hasAddons = false;
		bool						hasNoEffects = false;
		std::filesystem::path		addonBuild;
		std::string					curatedKey;
		unsigned					curatedVirtualKey = 0;
	};

	[[ nodiscard ]] std::string narrow ( const std::wstring& text )
	{
		const auto	bytes = WideCharToMultiByte ( CP_UTF8, 0, text.c_str (), static_cast<int> ( text.size () ), nullptr, 0, nullptr, nullptr );
		std::string	utf8 ( static_cast<size_t> ( std::max ( bytes, 0 ) ), '\0' );
		WideCharToMultiByte ( CP_UTF8, 0, text.c_str (), static_cast<int> ( text.size () ), utf8.data (), bytes, nullptr, nullptr );
		return utf8;
	}

	void tellDebugger ( const std::string& text )
	{
		OutputDebugStringA ( ( "ultraReShade test application: " + text + "\n" ).c_str () );
	}

	[[ nodiscard ]] std::string quoted ( const std::string& text )
	{
		std::string	result = "\"";

		for ( const auto c : text )
		{
			if ( c == '\\' || c == '"' )
				result += '\\';
			result += c;
		}

		return result + '"';
	}

	[[ nodiscard ]] std::string quotedPath ( const std::filesystem::path& path )
	{
		return quoted ( narrow ( path.native () ) );
	}

	[[ nodiscard ]] std::string pathList ( const std::filesystem::path& path )
	{
		return "[ " + quotedPath ( path ) + " ]";
	}

	[[ nodiscard ]] std::string stringList ( const std::vector<std::string>& items )
	{
		std::string	result = "[";

		for ( const auto& item : items )
			result += ( result.size () == 1 ? " " : ", " ) + quoted ( item );

		return result + " ]";
	}

	[[ nodiscard ]] std::string lowerExePath ( const std::filesystem::path& exe )
	{
		auto	text = exe.native ();
		std::ranges::replace ( text, L'/', L'\\' );

		std::wstring	lower ( text.size (), L'\0' );
		const auto		size = LCMapStringEx ( LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.c_str (), static_cast<int> ( text.size () ), lower.data (),
											   static_cast<int> ( lower.size () ), nullptr, nullptr, 0 );
		lower.resize ( static_cast<size_t> ( std::max ( size, 0 ) ) );
		return narrow ( lower );
	}

	[[ nodiscard ]] Options readOptions ()
	{
		Options		options;
		auto		argc = 0;
		const auto	argv = CommandLineToArgvW ( GetCommandLineW (), &argc );
		const auto	is = [] ( const wchar_t* arg, const wchar_t* name ) { return _wcsicmp ( arg, name ) == 0; };

		for ( auto i = 1; argv != nullptr && i < argc; ++i )
		{
			if ( is ( argv[ i ], L"-work-dir" ) && i + 1 < argc )
			{
				std::error_code	ec;
				options.work = std::filesystem::absolute ( argv[ ++i ], ec );
			}
			else if ( is ( argv[ i ], L"-no-effects" ) )
				options.hasNoEffects = true;
			else if ( is ( argv[ i ], L"-addon-build" ) && i + 1 < argc )
				options.addonBuild = argv[ ++i ];
			else if ( is ( argv[ i ], L"-addons" ) && i + 1 < argc )
			{
				options.hasAddons = true;
				const std::wstring	list = argv[ ++i ];

				for ( size_t begin = 0; begin <= list.size (); )
				{
					const auto	end = std::min ( list.find ( L',', begin ), list.size () );

					if ( end > begin )
						options.addons.push_back ( narrow ( list.substr ( begin, end - begin ) ) );
					begin = end + 1;
				}
			}
			else if ( is ( argv[ i ], L"-curated-key" ) && i + 2 < argc )
			{
				options.curatedKey = narrow ( argv[ i + 1 ] );
				options.curatedVirtualKey = static_cast<unsigned> ( wcstoul ( argv[ i + 2 ], nullptr, 10 ) );
				i += 2;
			}
		}

		LocalFree ( argv );
		return options;
	}

	// The add-on files of a folder, by name
	[[ nodiscard ]] std::vector<std::string> addonFiles ( const std::filesystem::path& folder )
	{
#ifdef _WIN64
		constexpr auto	extension = L".addon64";
#else
		constexpr auto	extension = L".addon32";
#endif
		std::vector<std::string>	names;
		std::error_code				ec;

		for ( std::filesystem::directory_iterator it ( folder, ec ), end; ! ec && it != end; it.increment ( ec ) )
			if ( it->path ().extension () == extension )
				names.push_back ( narrow ( it->path ().filename ().native () ) );

		std::ranges::sort ( names );
		return names;
	}

	[[ nodiscard ]] std::string effectEntries ( const std::filesystem::path& shaders )
	{
		struct Technique
		{
			const char*	name;
			const char*	file;
			bool		isDebug;
		};

		static constexpr Technique	techniques[] = {
			{ "UltraGame_Frame", "ultraGame.fx", false }, { "UltraGame", "ultraGame.fx", false },
			{ "UltraGame_Stats", "ultraGame_debug.fx", true }, { "UltraGame_Buffers", "ultraGame_debug.fx", true }, { "UltraGame_Debug", "ultraGame_debug.fx", true } };

		std::string	entries;

		for ( const auto& technique : techniques )
		{
			std::error_code	ec;

			if ( ! std::filesystem::exists ( shaders / technique.file, ec ) )
				continue;

			entries += ( entries.empty () ? "\t\t" : ",\n\t\t" );
			entries += std::string ( "{ \"technique\": " ) + quoted ( technique.name ) + ", \"file\": " + quoted ( technique.file ) + ( technique.isDebug ? ", \"enabled\": false }" : " }" );
		}

		return entries;
	}

	[[ nodiscard ]] bool writeFile ( const std::filesystem::path& file, const std::string& text )
	{
		std::error_code	ec;
		std::filesystem::create_directories ( file.parent_path (), ec );

		std::ofstream	out ( file, std::ios::binary | std::ios::trunc );
		out << text;
		out.flush ();

		if ( ! out )
		{
			tellDebugger ( "'" + narrow ( file.native () ) + "' could not be written." );
			return false;
		}

		return true;
	}

	[[ nodiscard ]] std::string hotkeyEntry ( const char* name, unsigned virtualKey )
	{
		return std::string ( "\t\t" ) + quoted ( name ) + ": [ " + std::to_string ( virtualKey ) + ", 0, 0, 0 ]";
	}
}
//-----------------------------------------------------------------------------

std::filesystem::path reshade::ultra::seedTestHome ( const std::filesystem::path& exe )
{
	const auto	options = readOptions ();

	if ( options.work.empty () )
	{
		tellDebugger ( "-work-dir is needed: the run's index, home and cache are in it." );
		return {};
	}

	const auto		user = options.work / L"user";
	const auto		home = user / L"games" / homeName;
	const auto		folder = exe.parent_path ();
	const auto		shaders = folder / L"reshade-shaders" / L"Shaders";
	std::error_code	ec;
	const auto		addonFolder = std::filesystem::is_directory ( folder / L"ultragame", ec ) ? folder / L"ultragame" : folder;

	auto	addons = options.addons;
	if ( ! options.hasAddons )
	{
		addons = addonFiles ( addonFolder );
		addons.insert ( addons.begin (), "Effect Runtime Sync" );
	}

	std::string	input;
	if ( ! options.curatedKey.empty () )
		input = ",\n\t\t\"INPUT\": {\n\t\t\t" + quoted ( options.curatedKey ) + ": [ " + std::to_string ( options.curatedVirtualKey ) + ", 0, 0, 0 ]\n\t\t}";

	std::string	addonSection = "\t\t\"ADDON\": {\n";
	if ( ! options.addonBuild.empty () )
		addonSection += "\t\t\t\"AddonBuildPath\": " + quotedPath ( options.addonBuild ) + ",\n";
	addonSection += "\t\t\t\"AddonPath\": " + quotedPath ( addonFolder ) + "\n\t\t}";

	const auto	reshade =
		"{\n\t\"schema\": 1,\n\t\"config\": {\n" + addonSection + ",\n"
		"\t\t\"GENERAL\": {\n"
		"\t\t\t\"EffectSearchPaths\": " + pathList ( shaders / L"**" ) + ",\n"
		"\t\t\t\"TextureSearchPaths\": " + pathList ( shaders / L"**" ) + ",\n"
		"\t\t\t\"IntermediateCachePath\": " + quotedPath ( options.work / L"cache" ) + "\n\t\t},\n"
		"\t\t\"OVERLAY\": {\n\t\t\t\"TutorialProgress\": 4\n\t\t}" + input + "\n\t},\n"
		"\t\"addons\": " + stringList ( addons ) + ",\n"
		"\t\"definitions\": {\n\t\t\"RESHADE_DEPTH_INPUT_IS_REVERSED\": \"0\"\n\t},\n"
		"\t\"effects\": [\n" + ( options.hasNoEffects ? std::string () : effectEntries ( shaders ) ) + "\n\t],\n"
		"\t\"effectFiles\": {\n\t\t\"ultraGame_debug.fx\": { \"values\": { \"DepthView\": 1, \"TargetsShow\": 1 }, \"definitions\": {} }\n\t}\n}\n";

	const auto	index =
		"{\n\t\"schema\": 1,\n\t\"exes\": {\n\t\t" + quoted ( lowerExePath ( exe ) ) + ": " + quoted ( homeName ) + "\n\t},\n"
		"\t\"games\": {\n\t\t" + quoted ( homeName ) + ": { \"title\": \"ReShade Test App\", \"folder\": " + quotedPath ( folder ) + ", \"name\": \"reshadeTestApp\" }\n\t}\n}\n";

	const auto	settings =
		"{\n\t\"screenshots\": {\n\t\t\"folder\": " + quotedPath ( options.work / L"screenshots" ) + ",\n\t\t\"perGameFolderEnabled\": true\n\t},\n"
		"\t\"hotkeys\": {\n" + hotkeyEntry ( "KeyOverlay", VK_HOME ) + ",\n" + hotkeyEntry ( "KeyScreenshot", VK_SNAPSHOT ) + ",\n"
		+ hotkeyEntry ( "KeyEffects", 0 ) + ",\n" + hotkeyEntry ( "KeyReload", 0 ) + ",\n" + hotkeyEntry ( "KeyFPS", 0 ) + ",\n" + hotkeyEntry ( "KeyFrametime", 0 ) + "\n\t}\n}\n";

	// A bench's own settings.json, with every section, stays as it is
	const auto	settingsFile = options.work / L"settings.json";
	const auto	isSeedWanted = ! std::filesystem::exists ( settingsFile, ec );

	std::filesystem::create_directories ( options.work / L"cache", ec );

	if ( ! writeFile ( home / L"reshade.json", reshade ) || ! writeFile ( user / L"installed.json", index ) || ( isSeedWanted && ! writeFile ( settingsFile, settings ) ) )
		return {};

	SetEnvironmentVariableW ( indexVariable, ( user / L"installed.json" ).c_str () );
	return options.work;
}
