/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <imgui.h>
#include <reshade_api.hpp>

namespace ultra::ui
{
	namespace api = reshade::api;
}

//-----------------------------------------------------------------------------
// The overlay's look in one place: every colour, shade, size and timing is a value of res/panel-theme.json, built into the DLL. Sizes are
// unscaled pixels (times scale () when drawn). With a source folder the checkout's file is read instead and followed while the game runs

namespace ultra::ui::theme
{
	[[ nodiscard ]] constexpr ImU32 rgb ( uint32_t rrggbb )
	{
		return IM_COL32 ( ( rrggbb >> 16 ) & 0xFF, ( rrggbb >> 8 ) & 0xFF, rrggbb & 0xFF, 0xFF );
	}

	// The screen's own scale (Windows display scaling times how large the game's pixels are shown), independent of ImGui's font
	// settings; measured by measureScale once per drawn frame
	[[ nodiscard ]] float scale ();
	void measureScale ( void* window, float backBufferHeight );

	// The same scale for a window whose back buffer is that tall, measured now; 0 when the window does not tell
	[[ nodiscard ]] float screenScale ( void* window, float backBufferHeight );
	[[ nodiscard ]] inline float px ( float unscaled )	{ return unscaled * scale (); }

	namespace colors
	{
		inline ImU32	background = 0;		// the page's and the overlay window's fill
		inline ImU32	text = 0;
		inline ImU32	accent = 0;
		inline ImU32	danger = 0;			// a status that NR is off or failed
		inline ImU32	warning = 0;		// a game tweak that is not available
		inline ImU32	appliesAfter = 0;	// and of one that takes effect after something the player does
		inline ImU32	logoSweep = 0;		// the band that sweeps across the logo in the tab bar
	}

	// Where a grey lies between background (0) and text (1)
	namespace shades
	{
		inline float	surface = 0.0f;
		inline float	raised = 0.0f;
		inline float	edge = 0.0f;
		inline float	hover = 0.0f;
		inline float	strong = 0.0f;
		inline float	muted = 0.0f;
		inline float	body = 0.0f;
	}

	[[ nodiscard ]] ImU32 shade ( float t );

	// The accent a fifth of the way to white, for what the mouse is over
	[[ nodiscard ]] ImU32 accentHover ();

	// The steps every widget shares, unscaled pixels
	namespace spacing
	{
		inline float	xs = 0.0f;
		inline float	m = 0.0f;
		inline float	l = 0.0f;
		inline float	xl = 0.0f;
		inline float	indent = 0.0f;		// a nested section's rows, one step in for each level
	}

	namespace radius
	{
		inline float	little = 0.0f;
		inline float	large = 0.0f;
	}

	namespace lines
	{
		inline float	line = 0.0f;
	}

	namespace heights
	{
		inline float	row = 0.0f;
		inline float	large = 0.0f;
		inline float	bar = 0.0f;
	}

	namespace text
	{
		inline float	little = 0.0f;
		inline float	body = 0.0f;
		inline float	large = 0.0f;
		inline float	title = 0.0f;
	}

	namespace icons
	{
		inline float	icon = 0.0f;
	}

	// The size of one specific thing
	namespace parts
	{
		inline float	panelWidth = 0.0f;			// the content's, whatever the window: narrower scrolls sideways, wider stays empty
		inline float	tipMaxWidth = 0.0f;
		inline float	pickerSquare = 0.0f;
		inline float	pickerMarker = 0.0f;		// the point picked in the square, its radius
		inline float	coverWidth = 0.0f;
		inline float	coverHeight = 0.0f;
		inline float	windowMinWidth = 0.0f;
		inline float	windowMinHeight = 0.0f;
		inline float	resizeEdge = 0.0f;
		inline float	switchWidth = 0.0f;
		inline float	scrollbarWidth = 0.0f;
		inline float	scrollThumbMin = 0.0f;
		inline float	bipolarMinFill = 0.0f;
		inline float	barShadowHeight = 0.0f;		// the tab bar's shadow over the content, black at effects::shadowAlpha fading to clear
		inline float	dragThreshold = 0.0f;		// a heading moves its section after the mouse went this far
	}

	// Timing, in seconds
	namespace motion
	{
		inline float	transition = 0.0f;
		inline float	tipDelay = 0.0f;
		inline float	doneSeconds = 0.0f;			// a button's done label
	}

	namespace effects
	{
		inline float	shadowSpread = 0.0f;
		inline float	shadowOffset = 0.0f;
		inline float	shadowAlpha = 0.0f;
		inline float	disabledAlpha = 0.0f;
		inline float	tipEdge = 0.0f;				// a tooltip's outline: its background this far towards the text colour, opaque
		inline float	sliderHandleGrow = 0.0f;	// the handle past the fill, on each side, while hovered
	}

	namespace logo
	{
		inline float	sweepAngle = 0.0f;			// the band's lean from vertical, in degrees
		inline float	sweepWidth = 0.0f;
		inline float	sweepSeconds = 0.0f;		// one pass across the logo, one pass every sweepPeriod seconds
		inline float	sweepPeriod = 0.0f;
	}

	// The built-in theme, once at the DLL's load; with a source folder the checkout's file takes its place and is followed from then on
	void load ();

	// The DLL's add-ons unload: the checkout's file is no longer followed
	void unload ();

	// A value under its name, as this build shipped it
	struct Tuned
	{
		const char*	name;
		float*		value;
		float		shipped;
	};

	struct TunedColor
	{
		const char*	name;
		ImU32*		value;
		ImU32		shipped;
	};

	[[ nodiscard ]] std::span<const Tuned> tuned ();
	[[ nodiscard ]] std::span<const TunedColor> tunedColors ();

	// A value by its role (include/reshade_ultra_ui.h): colours as ImGui packs them, sizes unscaled
	[[ nodiscard ]] ImU32 colorOf ( uint32_t role );
	[[ nodiscard ]] float sizeOf ( uint32_t role );

	// The values now as the file's text, as a person writes it
	[[ nodiscard ]] std::string fileText ();

	// A file's text taken in: every value it holds right is applied, one it lacks or holds wrong stays as this build shipped it. False
	// with the first such value named in why
	[[ nodiscard ]] bool take ( std::string_view text, std::string& why );
}
