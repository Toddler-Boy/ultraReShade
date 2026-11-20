/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if RESHADE_GUI

#include <algorithm>
#include <cmath>

#include <Windows.h>
#include <dwmapi.h>
#include <ShellScalingApi.h>

#include "ultra_overlay.hpp"

//-----------------------------------------------------------------------------

reshade::ultra::DisplayScale reshade::ultra::measureDisplayScale ( void* window, float backBufferHeight )
{
	const auto	hwnd = static_cast<HWND> ( window );

	if ( hwnd == nullptr )
	{
		const auto	scale = backBufferHeight >= 2160.0f ? 2.0f : backBufferHeight >= 1440.0f ? 1.5f : 1.0f;
		return { 1.0f, scale, scale };
	}

	// Measured as a per-monitor aware caller: the monitor's real scale and the window's real pixels, also for a game
	// Windows stretches (a DPI unaware one, or a small back buffer in a large window)
	const auto	previous = SetThreadDpiAwarenessContext ( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
	UINT		dpiX = 96;
	UINT		dpiY = 96;
	RECT		frame {};
	const auto	hasDpi = SUCCEEDED ( GetDpiForMonitor ( MonitorFromWindow ( hwnd, MONITOR_DEFAULTTONEAREST ), MDT_EFFECTIVE_DPI, &dpiX, &dpiY ) );
	const auto	hasFrame = SUCCEEDED ( DwmGetWindowAttribute ( hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof ( frame ) ) ) && frame.bottom > frame.top;
	SetThreadDpiAwarenessContext ( previous );

	DisplayScale	measured;
	measured.monitorScale = hasDpi ? float ( dpiY ) / 96.0f : 1.0f;
	measured.pixelSize = hasFrame && backBufferHeight > 0.0f ? backBufferHeight / float ( frame.bottom - frame.top ) : 1.0f;
	measured.scale = std::clamp ( std::round ( measured.monitorScale * measured.pixelSize * 4.0f ) / 4.0f, 0.5f, 4.0f );
	return measured;
}
//-----------------------------------------------------------------------------

#endif
