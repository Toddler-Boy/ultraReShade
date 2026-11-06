/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RESHADE_TEST_APPLICATION

#include <atomic>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

#include <Windows.h>
#include <shellapi.h>

#include <stb_image_write.h>

#include "dll_log.hpp"
#include "runtime.hpp"
#include "ultra_test_check.hpp"
#include "ultra_test_run.hpp"

extern std::filesystem::path g_reshade_base_path;

//-----------------------------------------------------------------------------

namespace
{
	struct CaptureState
	{
		std::optional<size_t>	frame;
		std::filesystem::path	path;
		const reshade::ultra::TestFrames*	frames = nullptr;
		size_t								presents = 0;
		bool								isWritten = false;
		std::atomic<bool>					isLoaded = false;
	};

	CaptureState	state;

	[[ nodiscard ]] bool writeCapture ( reshade::runtime* runtime )
	{
		uint32_t	width = 0;
		uint32_t	height = 0;
		runtime->get_screenshot_width_and_height ( &width, &height );

		std::vector<uint8_t>	pixels ( size_t ( width ) * size_t ( height ) * 4 );

		if ( pixels.empty () || ! runtime->capture_screenshot ( pixels.data () ) )
			return false;

		std::error_code	ec;
		std::filesystem::create_directories ( state.path.parent_path (), ec );

		auto	context = std::ofstream ( state.path, std::ios::binary );
		stbi_write_png_to_func ( [] ( void* file, void* data, int size ) { static_cast<std::ofstream*> ( file )->write ( static_cast<const char*> ( data ), size ); },
								 &context, static_cast<int> ( width ), static_cast<int> ( height ), 4, pixels.data (), static_cast<int> ( width ) * 4 );
		return context.good () && context.tellp () > 0;
	}

	// The picture as presented, without the overlay: after the add-ons' and ReShade's effects (NR on the Present route)
	void afterEffects ( reshade::runtime* runtime )
	{
		++state.presents;
		state.isLoaded = state.isLoaded || ! runtime->is_loading ();

		if ( state.frames != nullptr && state.frame == state.frames->index () && ! state.isWritten )
		{
			state.isWritten = writeCapture ( runtime );
			reshade::log::message ( state.isWritten ? reshade::log::level::info : reshade::log::level::error, "Test application: capture of frame %zu %s '%s'.", *state.frame,
									state.isWritten ? "written to" : "failed for", state.path.u8string ().c_str () );
		}
	}

	[[ nodiscard ]] bool logHasErrors ()
	{
		std::ifstream	log ( g_reshade_base_path / L"ReShade.log" );
		std::string		line;

		while ( std::getline ( log, line ) )
			if ( line.find ( "| ERROR |" ) != std::string::npos )
				return true;

		return false;
	}
}
//-----------------------------------------------------------------------------

reshade::ultra::TestCheck::TestCheck ()
{
	auto		argc = 0;
	const auto	argv = CommandLineToArgvW ( GetCommandLineW (), &argc );

	for ( auto i = 1; argv != nullptr && i < argc; ++i )
	{
		if ( _wcsicmp ( argv[ i ], L"-capture" ) != 0 )
			continue;

		wchar_t*	end = nullptr;
		const auto	frame = i + 2 < argc ? std::wcstoull ( argv[ i + 1 ], &end, 10 ) : 0;

		if ( i + 2 >= argc || end == argv[ i + 1 ] || *end != L'\0' )
		{
			reshade::log::message ( reshade::log::level::error, "Test application: -capture needs a frame number and a file." );
			isCommandLineValid = false;
			break;
		}

		captureFrame = static_cast<size_t> ( frame );
		capturePath = std::filesystem::absolute ( argv[ i + 2 ] );
		i += 2;
	}

	LocalFree ( argv );

	state.frame = captureFrame;
	state.path = capturePath;
	reshade::runtime::ultra_after_effects = afterEffects;
}
//-----------------------------------------------------------------------------

reshade::ultra::TestCheck::~TestCheck ()
{
	reshade::runtime::ultra_after_effects = nullptr;
}
//-----------------------------------------------------------------------------

void reshade::ultra::TestCheck::watch ( const TestFrames& frames )
{
	state.frames = &frames;
}
//-----------------------------------------------------------------------------

bool reshade::ultra::isReShadeLoaded ()
{
	return state.isLoaded;
}
//-----------------------------------------------------------------------------

bool reshade::ultra::TestCheck::isValid () const
{
	return isCommandLineValid;
}
//-----------------------------------------------------------------------------

int reshade::ultra::TestCheck::exitCode ( int loopResult ) const
{
	if ( loopResult != EXIT_SUCCESS )
		return loopResult;

	if ( state.presents == 0 )
		return 2;

	if ( captureFrame.has_value () && ! state.isWritten )
		return 3;

	return logHasErrors () ? 4 : EXIT_SUCCESS;
}
//-----------------------------------------------------------------------------

#endif
