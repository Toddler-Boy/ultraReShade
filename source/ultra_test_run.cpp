/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RESHADE_TEST_APPLICATION

#include <cwchar>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#include "dll_log.hpp"
#include "ultra_addon_reload.hpp"
#include "ultra_test_check.hpp"
#include "ultra_test_run.hpp"

//-----------------------------------------------------------------------------

namespace
{
	constexpr float					fixedStep = 1.0f / 60.0f;
	constexpr std::chrono::seconds	loadTimeout { 120 };

	[[ nodiscard ]] std::string narrow ( const wchar_t* text )
	{
		std::string	out;

		for ( ; *text != L'\0'; ++text )
			out += *text < 128 ? static_cast<char> ( *text ) : '?';

		return out;
	}

	// A whole decimal number, nothing after it
	[[ nodiscard ]] std::optional<size_t> parseCount ( const wchar_t* text )
	{
		wchar_t*	end = nullptr;
		const auto	value = std::wcstoull ( text, &end, 10 );

		if ( end == text || *end != L'\0' || *text == L'-' )
			return std::nullopt;

		return static_cast<size_t> ( value );
	}

	// A decimal number, nothing after it
	[[ nodiscard ]] std::optional<float> parseNumber ( const wchar_t* text )
	{
		wchar_t*	end = nullptr;
		const auto	value = std::wcstof ( text, &end );

		if ( end == text || *end != L'\0' )
			return std::nullopt;

		return value;
	}

	void reportBadValue ( const wchar_t* option, const wchar_t* value )
	{
		reshade::log::message ( reshade::log::level::error, "Test application: '%s' is not a valid value for %s.", narrow ( value ).c_str (), narrow ( option ).c_str () );
	}

	void reportMissingValue ( const wchar_t* option )
	{
		reshade::log::message ( reshade::log::level::error, "Test application: %s needs a value.", narrow ( option ).c_str () );
	}

	[[ nodiscard ]] bool isOption ( const wchar_t* arg, const wchar_t* name )
	{
		return _wcsicmp ( arg, name ) == 0;
	}
}
//-----------------------------------------------------------------------------

reshade::ultra::TestOptions reshade::ultra::parseTestOptions ()
{
	TestOptions	options;
	auto		argc = 0;
	const auto	argv = CommandLineToArgvW ( GetCommandLineW (), &argc );

	if ( argv == nullptr )
		return options;

	for ( auto i = 1; i < argc; ++i )
	{
		const auto	arg = argv[ i ];
		const auto	isKnown = isOption ( arg, L"-api" ) || isOption ( arg, L"-scene" ) || isOption ( arg, L"-jitter" ) || isOption ( arg, L"-sss" ) || isOption ( arg, L"-env" ) || isOption ( arg, L"-fov" ) || isOption ( arg, L"-hdr" )
							  || isOption ( arg, L"-paper-white" ) || isOption ( arg, L"-peak" ) || isOption ( arg, L"-reload-every" )
							  || isOption ( arg, L"-constants" ) || isOption ( arg, L"-owner" )
							  || isOption ( arg, L"-size" ) || isOption ( arg, L"-frames" ) || isOption ( arg, L"-resize-at" );

		if ( ! isKnown )
			continue;

		if ( i + 1 >= argc )
		{
			reportMissingValue ( arg );
			options.isValid = false;
			break;
		}

		const auto	value = argv[ ++i ];

		if ( isOption ( arg, L"-api" ) )
		{
			if ( isOption ( value, L"dx11" ) )
				options.api = api::device_api::d3d11;
			else if ( isOption ( value, L"dx12" ) )
				options.api = api::device_api::d3d12;
			else
				options.isValid = false;
		}
		else if ( isOption ( arg, L"-scene" ) )
		{
			if ( isOption ( value, L"cube" ) )
				options.scene.kind = TestSceneKind::cube;
			else if ( isOption ( value, L"head" ) )
				options.scene.kind = TestSceneKind::head;
			else
				options.isValid = false;
		}
		else if ( isOption ( arg, L"-jitter" ) || isOption ( arg, L"-sss" ) )
		{
			auto&	setting = isOption ( arg, L"-jitter" ) ? options.scene.isJittered : options.scene.isScattering;

			if ( isOption ( value, L"on" ) )
				setting = true;
			else if ( isOption ( value, L"off" ) )
				setting = false;
			else
				options.isValid = false;
		}
		else if ( isOption ( arg, L"-env" ) )
			options.scene.environment = value;
		else if ( isOption ( arg, L"-owner" ) )
			options.owner = value;
		else if ( isOption ( arg, L"-fov" ) )
		{
			const auto	degrees = parseNumber ( value );

			if ( degrees.has_value () && *degrees > 1.0f && *degrees < 170.0f )
				options.scene.fieldOfView = *degrees;
			else
				options.isValid = false;
		}
		else if ( isOption ( arg, L"-constants" ) )
		{
			if ( isOption ( value, L"root" ) )
				options.scene.constantsPath = TestConstantsPath::root;
			else if ( isOption ( value, L"cbv" ) )
				options.scene.constantsPath = TestConstantsPath::cbv;
			else if ( isOption ( value, L"table" ) )
				options.scene.constantsPath = TestConstantsPath::table;
			else
				options.isValid = false;
		}
		else if ( isOption ( arg, L"-hdr" ) )
		{
			if ( isOption ( value, L"off" ) )
				options.scene.output = TestOutput::sdr;
			else if ( isOption ( value, L"scrgb" ) )
				options.scene.output = TestOutput::scRgb;
			else if ( isOption ( value, L"pq" ) )
				options.scene.output = TestOutput::pq;
			else
				options.isValid = false;
		}
		else if ( isOption ( arg, L"-paper-white" ) || isOption ( arg, L"-peak" ) )
		{
			const auto	nits = parseNumber ( value );

			if ( nits.has_value () && *nits >= 1.0f && *nits <= 10000.0f )
				( isOption ( arg, L"-peak" ) ? options.scene.peak : options.scene.paperWhite ) = *nits;
			else
				options.isValid = false;
		}
		else if ( isOption ( arg, L"-size" ) )
		{
			auto	width = 0L;
			auto	height = 0L;
			wchar_t	rest = L'\0';

			if ( std::swscanf ( value, L"%ldx%ld%lc", &width, &height, &rest ) == 2 && width > 0 && height > 0 )
			{
				options.width = width;
				options.height = height;
			}
			else
				options.isValid = false;
		}
		else if ( const auto count = parseCount ( value ); count.has_value () )
		{
			if ( isOption ( arg, L"-frames" ) )
				options.frames = *count;
			else if ( isOption ( arg, L"-reload-every" ) )
				options.reloadEvery = *count;
			else
				options.resizeAt = *count;
		}
		else
			options.isValid = false;

		if ( ! options.isValid )
		{
			reportBadValue ( arg, value );
			break;
		}
	}

	LocalFree ( argv );

	// A model from -model turns like the built-in head, framed by everything it draws
	if ( testModelFolder ().has_value () )
		options.scene.kind = TestSceneKind::model;

	return options;
}
//-----------------------------------------------------------------------------

reshade::ultra::TestFrames::TestFrames ( const TestOptions& options, HWND window )
	: frames ( options.frames ), resizeAt ( options.resizeAt ), reloadEvery ( options.reloadEvery ), window ( window )
{
}
//-----------------------------------------------------------------------------

bool reshade::ultra::TestFrames::isFixedStep () const
{
	return frames != 0;
}
//-----------------------------------------------------------------------------

// Every run shows time 0 until ReShade has loaded its effects, so the motion starts clean and, with a fixed step, frame
// n is the same picture however long compiling takes
bool reshade::ultra::TestFrames::next ()
{
	if ( ! hasStarted )
	{
		const auto	isLoaded = isReShadeLoaded ();

		if ( ! isLoaded && std::chrono::steady_clock::now () - start < loadTimeout )
			return true;

		if ( ! isLoaded )
			reshade::log::message ( reshade::log::level::error, "Test application: ReShade had not loaded its effects after %d s, the frames start anyway.",
									static_cast<int> ( loadTimeout.count () ) );

		hasStarted = true;
		start = std::chrono::steady_clock::now ();
	}

	if ( isFixedStep () && upcoming >= frames )
		return false;

	current = upcoming++;

	if ( resizeAt == current )
		shrinkWindow ();

	if ( reloadEvery != 0 && current != 0 && current % reloadEvery == 0 )
		requestAddonReload ();

	return true;
}
//-----------------------------------------------------------------------------

size_t reshade::ultra::TestFrames::index () const
{
	return current;
}
//-----------------------------------------------------------------------------

float reshade::ultra::TestFrames::seconds () const
{
	if ( isFixedStep () )
		return static_cast<float> ( current ) * fixedStep;

	if ( ! hasStarted )
		return 0.0f;

	return std::chrono::duration<float> ( std::chrono::steady_clock::now () - start ).count ();
}
//-----------------------------------------------------------------------------

// The size message arrives before this returns, so the frame it starts already renders at the new size
void reshade::ultra::TestFrames::shrinkWindow ()
{
	RECT	client {};
	GetClientRect ( window, &client );

	RECT	frame { 0, 0, client.right * 3 / 4, client.bottom * 3 / 4 };
	AdjustWindowRectEx ( &frame, static_cast<DWORD> ( GetWindowLongW ( window, GWL_STYLE ) ), FALSE, static_cast<DWORD> ( GetWindowLongW ( window, GWL_EXSTYLE ) ) );
	SetWindowPos ( window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE );
}
//-----------------------------------------------------------------------------

#endif
