/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RESHADE_TEST_APPLICATION

#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

#include <dxgi1_6.h>

#include "com_ptr.hpp"
#include "dll_log.hpp"
#include "ultra_test_display.hpp"

//-----------------------------------------------------------------------------

namespace
{
	struct DisplayMode
	{
		DWORD	width = 0;
		DWORD	height = 0;
		DWORD	frequency = 0;

		bool operator== ( const DisplayMode& ) const = default;
	};

	IDXGISwapChain*		exclusiveSwapchain = nullptr;
	wchar_t				monitorName[ CCHDEVICENAME ] {};
	DisplayMode			desktopMode;
	bool				isSafetyInstalled = false;

	void leave ()
	{
		if ( const auto swapchain = std::exchange ( exclusiveSwapchain, nullptr ) )
			swapchain->SetFullscreenState ( FALSE, nullptr );
	}

	LPTOP_LEVEL_EXCEPTION_FILTER	filterBefore = nullptr;

	LONG WINAPI onCrash ( EXCEPTION_POINTERS* e )
	{
		leave ();
		return filterBefore != nullptr ? filterBefore ( e ) : EXCEPTION_CONTINUE_SEARCH;
	}

	[[ nodiscard ]] DisplayMode currentMode ( const wchar_t* device )
	{
		DEVMODEW	mode { .dmSize = sizeof ( DEVMODEW ) };

		if ( ! EnumDisplaySettingsW ( device, ENUM_CURRENT_SETTINGS, &mode ) )
			return {};

		return { mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency };
	}

	[[ nodiscard ]] const char* colourSpaceOf ( IDXGISwapChain* swapchain )
	{
		com_ptr<IDXGIOutput>	output;
		com_ptr<IDXGIOutput6>	output6;
		DXGI_OUTPUT_DESC1		desc {};

		if ( FAILED ( swapchain->GetContainingOutput ( &output ) ) || FAILED ( output->QueryInterface ( &output6 ) ) || FAILED ( output6->GetDesc1 ( &desc ) ) )
			return "unknown";

		return desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? "HDR" : "SDR";
	}

	// Logs the monitor's mode; anything but the desktop's mode from the start of the run is an error
	[[ nodiscard ]] bool checkMode ( IDXGISwapChain* swapchain, const char* when )
	{
		const auto	mode = currentMode ( monitorName );
		const auto	isDesktop = mode == desktopMode;

		reshade::log::message ( isDesktop ? reshade::log::level::info : reshade::log::level::error, "Test application: display mode %s: %lux%lu at %lu Hz, %s%s.", when, mode.width,
								mode.height, mode.frequency, colourSpaceOf ( swapchain ), isDesktop ? "" : ", not the desktop's" );
		return isDesktop;
	}

	// The output's mode that is exactly the desktop's (refresh rates in whole Hz as the desktop reports them)
	[[ nodiscard ]] std::optional<DXGI_MODE_DESC> desktopModeOf ( IDXGIOutput* output, DXGI_FORMAT format )
	{
		UINT	count = 0;

		if ( FAILED ( output->GetDisplayModeList ( format, 0, &count, nullptr ) ) || count == 0 )
			return std::nullopt;

		std::vector<DXGI_MODE_DESC>	modes ( count );

		if ( FAILED ( output->GetDisplayModeList ( format, 0, &count, modes.data () ) ) )
			return std::nullopt;

		std::optional<DXGI_MODE_DESC>	best;
		auto							bestDistance = 1.0;

		for ( const auto& mode : modes )
		{
			if ( mode.Width != desktopMode.width || mode.Height != desktopMode.height || mode.RefreshRate.Denominator == 0 )
				continue;

			const auto	distance = std::abs ( double ( mode.RefreshRate.Numerator ) / mode.RefreshRate.Denominator - desktopMode.frequency );

			if ( distance < bestDistance )
				best = mode, bestDistance = distance;
		}

		return best;
	}
}
//-----------------------------------------------------------------------------

reshade::ultra::TestDisplay::TestDisplay ( IDXGISwapChain* swapchain, HWND window ) : swapchain ( swapchain ), window ( window )
{
	MONITORINFOEXW	info {};
	info.cbSize = sizeof ( info );
	GetMonitorInfoW ( MonitorFromWindow ( window, MONITOR_DEFAULTTOPRIMARY ), &info );
	wcscpy_s ( monitorName, info.szDevice );
	desktopMode = currentMode ( monitorName );
	(void) checkMode ( swapchain, "at the start" );

	if ( ! isSafetyInstalled )
	{
		isSafetyInstalled = true;
		filterBefore = SetUnhandledExceptionFilter ( onCrash );
		std::atexit ( leave );
	}
}
//-----------------------------------------------------------------------------

// DXGI wants a swap chain windowed again before it is released
reshade::ultra::TestDisplay::~TestDisplay ()
{
	if ( exclusiveSwapchain == swapchain )
	{
		leave ();
		(void) checkMode ( swapchain, "after the run" );
	}

	if ( IsWindow ( previousForeground ) )
		SetForegroundWindow ( previousForeground );
}
//-----------------------------------------------------------------------------

std::optional<SIZE> reshade::ultra::TestDisplay::apply ( const std::optional<TestSwitch>& change )
{
	if ( ! change.has_value () )
		return std::nullopt;

	switch ( change->kind )
	{
	case TestSwitchKind::fullscreen:
		return enterFullscreen ();
	case TestSwitchKind::windowed:
		return leaveFullscreen ();
	case TestSwitchKind::bufferSize:
		reshade::log::message ( reshade::log::level::info, "Test application: buffers resized to %ldx%ld.", change->width, change->height );
		return SIZE { change->width, change->height };
	}

	return std::nullopt;
}
//-----------------------------------------------------------------------------

bool reshade::ultra::TestDisplay::isFullscreen () const
{
	BOOL	isExclusive = FALSE;
	return SUCCEEDED ( swapchain->GetFullscreenState ( &isExclusive, nullptr ) ) && isExclusive;
}
//-----------------------------------------------------------------------------

// The target is set to the desktop's own mode first: going fullscreen without it, DXGI would pick a mode that fits the
// buffers
std::optional<SIZE> reshade::ultra::TestDisplay::enterFullscreen ()
{
	if ( isFullscreen () )
		return std::nullopt;

	com_ptr<IDXGIOutput>	output;
	DXGI_SWAP_CHAIN_DESC	desc {};

	if ( FAILED ( swapchain->GetContainingOutput ( &output ) ) || FAILED ( swapchain->GetDesc ( &desc ) ) )
	{
		reshade::log::message ( reshade::log::level::error, "Test application: no output for fullscreen." );
		return std::nullopt;
	}

	DXGI_OUTPUT_DESC	outputDesc {};
	output->GetDesc ( &outputDesc );

	if ( wcscmp ( outputDesc.DeviceName, monitorName ) != 0 || currentMode ( monitorName ) != desktopMode )
	{
		reshade::log::message ( reshade::log::level::error, "Test application: the window moved to another monitor or its mode changed; no fullscreen." );
		return std::nullopt;
	}

	const auto	mode = desktopModeOf ( output.get (), desc.BufferDesc.Format );

	if ( ! mode.has_value () )
	{
		reshade::log::message ( reshade::log::level::error, "Test application: the output lists no mode equal to the desktop's %lux%lu at %lu Hz; no fullscreen.", desktopMode.width,
								desktopMode.height, desktopMode.frequency );
		return std::nullopt;
	}

	(void) checkMode ( swapchain, "before fullscreen" );
	GetWindowRect ( window, &windowedRect );

	// Exclusive fullscreen needs the foreground, which Windows only hands over from the thread that has it
	previousForeground = GetForegroundWindow ();
	const auto	foregroundThread = GetWindowThreadProcessId ( previousForeground, nullptr );
	AttachThreadInput ( GetCurrentThreadId (), foregroundThread, TRUE );
	SetForegroundWindow ( window );
	AttachThreadInput ( GetCurrentThreadId (), foregroundThread, FALSE );

	if ( FAILED ( swapchain->ResizeTarget ( &*mode ) ) )
	{
		reshade::log::message ( reshade::log::level::error, "Test application: ResizeTarget to the desktop's mode failed; no fullscreen." );
		return std::nullopt;
	}

	exclusiveSwapchain = swapchain;
	const auto	result = swapchain->SetFullscreenState ( TRUE, output.get () );

	if ( FAILED ( result ) )
	{
		exclusiveSwapchain = nullptr;
		reshade::log::message ( reshade::log::level::error, "Test application: SetFullscreenState failed with 0x%08lx.", static_cast<unsigned long> ( result ) );
		return std::nullopt;
	}

	if ( ! checkMode ( swapchain, "in exclusive fullscreen" ) )
	{
		leave ();
		(void) checkMode ( swapchain, "after leaving fullscreen early" );
	}

	return SIZE { LONG ( mode->Width ), LONG ( mode->Height ) };
}
//-----------------------------------------------------------------------------

SIZE reshade::ultra::TestDisplay::leaveFullscreen ()
{
	exclusiveSwapchain = nullptr;
	swapchain->SetFullscreenState ( FALSE, nullptr );
	(void) checkMode ( swapchain, "after leaving fullscreen" );

	if ( windowedRect.right > windowedRect.left )
		SetWindowPos ( window, nullptr, windowedRect.left, windowedRect.top, windowedRect.right - windowedRect.left, windowedRect.bottom - windowedRect.top,
					   SWP_NOZORDER | SWP_NOACTIVATE );

	if ( IsWindow ( previousForeground ) )
		SetForegroundWindow ( std::exchange ( previousForeground, nullptr ) );

	RECT	client {};
	GetClientRect ( window, &client );
	return SIZE { client.right, client.bottom };
}
//-----------------------------------------------------------------------------

void reshade::ultra::leaveTestFullscreen ()
{
	leave ();
}
//-----------------------------------------------------------------------------

#endif
