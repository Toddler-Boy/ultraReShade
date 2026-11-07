/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RESHADE_TEST_APPLICATION

#include <atomic>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

#include <Windows.h>
#include <shellapi.h>

#include <stb_image.h>
#include <stb_image_write.h>
#include <stb_image_write_hdr_png.h>

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

	void writeToStream ( void* file, void* data, int size )
	{
		static_cast<std::ofstream*> ( file )->write ( static_cast<const char*> ( data ), size );
	}

	template <typename T>
	void put ( std::vector<char>& out, T value )
	{
		const auto	bytes = reinterpret_cast<const char*> ( &value );
		out.insert ( out.end (), bytes, bytes + sizeof ( T ) );
	}

	void putAttribute ( std::vector<char>& out, const char* name, const char* type, const std::vector<char>& value )
	{
		out.insert ( out.end (), name, name + std::strlen ( name ) + 1 );
		out.insert ( out.end (), type, type + std::strlen ( type ) + 1 );
		put ( out, static_cast<int32_t> ( value.size () ) );
		out.insert ( out.end (), value.begin (), value.end () );
	}

	// OpenEXR, uncompressed scanlines of half floats (B, G, R in the file's alphabetical order): scRGB as is,
	// 1.0 = 80 nits, values above 1.0 kept
	void writeExr ( std::ofstream& file, uint32_t width, uint32_t height, const uint16_t* rgba )
	{
		std::vector<char>	header { 0x76, 0x2f, 0x31, 0x01, 2, 0, 0, 0 };
		std::vector<char>	channels;

		for ( const auto name : { "B", "G", "R" } )
		{
			channels.insert ( channels.end (), { name[ 0 ], 0 } );
			put ( channels, int32_t ( 1 ) );		// half
			put ( channels, int32_t ( 0 ) );		// linear flag and reserved
			put ( channels, int32_t ( 1 ) );
			put ( channels, int32_t ( 1 ) );
		}
		channels.push_back ( 0 );

		std::vector<char>	window;
		for ( const auto v : { 0, 0, int32_t ( width ) - 1, int32_t ( height ) - 1 } )
			put ( window, int32_t ( v ) );

		std::vector<char>	one;
		put ( one, 1.0f );
		std::vector<char>	centre;
		put ( centre, 0.0f );
		put ( centre, 0.0f );

		putAttribute ( header, "channels", "chlist", channels );
		putAttribute ( header, "compression", "compression", { 0 } );
		putAttribute ( header, "dataWindow", "box2i", window );
		putAttribute ( header, "displayWindow", "box2i", window );
		putAttribute ( header, "lineOrder", "lineOrder", { 0 } );
		putAttribute ( header, "pixelAspectRatio", "float", one );
		putAttribute ( header, "screenWindowCenter", "v2f", centre );
		putAttribute ( header, "screenWindowWidth", "float", one );
		header.push_back ( 0 );

		const auto	lineBytes = size_t ( width ) * 3 * sizeof ( uint16_t );
		const auto	firstLine = header.size () + size_t ( height ) * sizeof ( uint64_t );

		for ( uint32_t y = 0; y < height; ++y )
			put ( header, uint64_t ( firstLine + y * ( 8 + lineBytes ) ) );

		file.write ( header.data (), std::streamsize ( header.size () ) );

		std::vector<uint16_t>	line ( size_t ( width ) * 3 );

		for ( uint32_t y = 0; y < height; ++y )
		{
			for ( uint32_t x = 0; x < width; ++x )
				for ( auto c = 0; c < 3; ++c )
					line[ size_t ( c ) * width + x ] = rgba[ ( size_t ( y ) * width + x ) * 4 + 2 - c ];

			const int32_t	header2[ 2 ] { int32_t ( y ), int32_t ( lineBytes ) };
			file.write ( reinterpret_cast<const char*> ( header2 ), sizeof ( header2 ) );
			file.write ( reinterpret_cast<const char*> ( line.data () ), std::streamsize ( lineBytes ) );
		}
	}

	// SDR: 8-bit PNG. scRGB (half float): OpenEXR, the file's extension becomes .exr. HDR10 (10-bit, PQ): 16-bit PNG
	// tagged Rec.2020 PQ, the code values as presented
	[[ nodiscard ]] bool writeCapture ( reshade::runtime* runtime )
	{
		uint32_t	width = 0;
		uint32_t	height = 0;
		runtime->get_screenshot_width_and_height ( &width, &height );

		const auto	device = runtime->get_device ();
		const auto	format = reshade::api::format_to_default_typed ( device->get_resource_desc ( runtime->get_swapchain ()->get_current_back_buffer () ).texture.format, 0 );
		const auto	isScRgb = format == reshade::api::format::r16g16b16a16_float;
		const auto	isHdr10 = format == reshade::api::format::r10g10b10a2_unorm || format == reshade::api::format::b10g10r10a2_unorm;
		std::vector<uint8_t>	pixels ( size_t ( width ) * size_t ( height ) * ( isScRgb ? 8 : 4 ) );

		if ( pixels.empty () || ! runtime->capture_screenshot ( pixels.data () ) )
			return false;

		if ( isScRgb )
			state.path.replace_extension ( L".exr" );

		std::error_code	ec;
		std::filesystem::create_directories ( state.path.parent_path (), ec );

		auto	file = std::ofstream ( state.path, std::ios::binary );

		if ( isScRgb )
			writeExr ( file, width, height, reinterpret_cast<const uint16_t*> ( pixels.data () ) );
		else if ( isHdr10 )
		{
			std::vector<uint16_t>	codes ( size_t ( width ) * size_t ( height ) * 3 );

			for ( size_t i = 0; i < size_t ( width ) * size_t ( height ); ++i )
			{
				uint32_t	packed;
				std::memcpy ( &packed, pixels.data () + i * 4, 4 );
				const uint32_t	r = packed & 1023;
				const uint32_t	g = ( packed >> 10 ) & 1023;
				const uint32_t	b = ( packed >> 20 ) & 1023;
				const bool		isBgr = format == reshade::api::format::b10g10r10a2_unorm;

				codes[ i * 3 ] = static_cast<uint16_t> ( ( isBgr ? b : r ) << 6 );
				codes[ i * 3 + 1 ] = static_cast<uint16_t> ( g << 6 );
				codes[ i * 3 + 2 ] = static_cast<uint16_t> ( ( isBgr ? r : b ) << 6 );
			}

			constexpr unsigned char	primariesBt2020 = 9;
			constexpr unsigned char	transferPq = 16;
			stbi_write_hdr_png_to_func ( writeToStream, &file, static_cast<int> ( width ), static_cast<int> ( height ), 3, codes.data (), 0, primariesBt2020, transferPq );
		}
		else
			stbi_write_png_to_func ( writeToStream, &file, static_cast<int> ( width ), static_cast<int> ( height ), 4, pixels.data (), static_cast<int> ( width ) * 4 );

		return file.good () && file.tellp () > 0;
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
