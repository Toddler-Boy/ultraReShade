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
// The overlay's look in one place: colours, sizes in unscaled pixels (times scale () when drawn), timings. The colours and the spacing
// that set the density are res/panel-theme.json, built into the DLL; a developer tunes them live and takes a changed file in (the
// add-on that has the checkout writes and follows it)

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
		inline ImU32	accent = 0;
		inline ImU32	danger = 0;		// a status that NR is off or failed
		inline ImU32	warning = 0;	// a game tweak that is not available
		inline ImU32	accentHover = 0;
		inline ImU32	text = 0;
		inline ImU32	bright = 0;
		inline ImU32	muted = 0;
		inline ImU32	nextStart = 0;	// the label of a setting that takes effect at the next start
		inline ImU32	appliesAfter = 0;// and of one that takes effect after something the player does
		inline ImU32	white = 0;
		inline ImU32	separator = 0;

		inline ImU32	panel = 0;		// the page's own background, whatever ImGui's style
		inline ImU32	control = 0;	// slider, choice and drop-down backgrounds
		inline ImU32	controlHover = 0;
		inline ImU32	track = 0;
		inline ImU32	tick = 0;
		inline ImU32	handle = 0;

		inline ImU32	switchOff = 0;
		inline ImU32	knobOff = 0;
		inline ImU32	knobOn = 0;
		inline ImU32	chevron = 0;

		inline ImU32	popup = 0;		// drop-down list and tooltip
		inline ImU32	popupBorder = 0;
		inline ImU32	popupHover = 0;
		inline constexpr float	tipEdge = 0.1f;			// a tooltip's outline: its background this far towards white, opaque

		inline ImU32	button = 0;
		inline ImU32	buttonHover = 0;

		inline ImU32	infoDot = 0;
		inline ImU32	infoGlyph = 0;
		inline ImU32	keycap = 0;
		inline ImU32	keycapEdge = 0;

		inline ImU32	window = 0;			// the overlay's window: its fill and its edge
		inline ImU32	windowEdge = 0;
		inline ImU32	titleBar = 0;		// the track the tabs sit in, the selected tab's label on its accent pill
		inline ImU32	tabSelectedText = 0;
		inline ImU32	tab = 0;			// a tab's label, the selected one's, the one under the mouse, the line under the selected one
		inline ImU32	tabActive = 0;
		inline ImU32	tabHover = 0;
		inline ImU32	tabLine = 0;
		inline ImU32	scrollTrack = 0;
		inline ImU32	scrollThumb = 0;
		inline ImU32	scrollThumbHover = 0;
		inline ImU32	message = 0;		// the message strip's box and its text
		inline ImU32	messageText = 0;
	}

	namespace sizes
	{
		inline constexpr float	body = 13.0f;
		inline constexpr float	heading = 15.5f;
		inline constexpr float	subheading = 12.0f;
		inline constexpr float	title = 24.0f;
		inline constexpr float	choice = 12.5f;
		inline constexpr float	value = 13.5f;
		inline constexpr float	key = 11.5f;
		inline constexpr float	tip = 12.5f;
		inline constexpr float	info = 10.0f;
	}

	// Layout
	inline constexpr float	panelInset = 10.0f;			// the whole page off the window's edges, all four
	inline constexpr float	panelWidth = 650.0f;		// the content's, whatever the window: narrower scrolls sideways, wider stays empty
	inline float			rowHeight = 0.0f;
	inline float			rowGap = 0.0f;
	inline float			columnGap = 0.0f;			// label to control, control to value
	inline constexpr float	resetWidth = 16.0f;
	inline constexpr float	resetIcon = 14.0f;
	inline float			indent = 0.0f;
	inline float			separatorAbove = 0.0f;
	inline float			headingAbove = 0.0f;
	inline float			headingBelow = 0.0f;
	inline float			subheadingAbove = 0.0f;		// a group's heading inside a section, unless it comes first
	inline float			firstRowAbove = 0.0f;		// a choice or drop-down right under its heading
	inline float			headingGap = 0.0f;			// switch to heading text
	inline constexpr float	inlineToggleHeight = 28.0f;

	// Slider
	inline float			sliderBack = 0.0f;
	inline float			sliderPad = 0.0f;
	inline constexpr float	bipolarMinFill = 4.0f;
	inline float			valueEditPad = 0.0f;		// the value's edit box reaches this far past its text

	// Toggle
	inline float			switchWidth = 0.0f;
	inline float			switchHeight = 0.0f;
	inline float			knobSize = 0.0f;

	// Choice, drop-down, popups
	inline float			choicePad = 0.0f;
	inline constexpr float	dropRounding = 6.0f;
	inline float			dropPadLeft = 0.0f;
	inline float			dropPadRight = 0.0f;
	inline constexpr float	chevronSize = 14.0f;
	inline float			disclosureSize = 0.0f;		// a collapsible section's arrow
	inline float			disclosureRadius = 0.0f;	// its corners' rounding
	inline constexpr float	menuRounding = 8.0f;
	inline constexpr float	menuItemRounding = 5.0f;
	inline float			menuPad = 0.0f;
	inline float			menuItemPad = 0.0f;
	inline float			menuGap = 0.0f;
	inline float			tipPadX = 0.0f;
	inline float			tipPadY = 0.0f;
	inline constexpr float	tipMaxWidth = 240.0f;
	inline constexpr float	tipGap = 6.0f;
	inline constexpr float	shadowSpread = 10.0f;
	inline constexpr float	shadowOffset = 4.0f;
	inline constexpr float	shadowAlpha = 0.55f;

	// Colour picker: the swatch in the control column, its popup's saturation and brightness square with the hue strip beside it
	inline constexpr float	pickerSquare = 150.0f;
	inline constexpr float	pickerHueWidth = 14.0f;
	inline constexpr float	pickerGap = 8.0f;			// between the parts, and around them
	inline constexpr float	pickerMarker = 5.0f;		// the point picked in the square, its radius
	inline constexpr float	swatchEdge = 1.0f;

	// Button
	inline float			buttonPadX = 0.0f;
	inline float			buttonGap = 0.0f;

	// Info dot and keycap
	inline constexpr float	infoSize = 13.0f;
	inline constexpr float	infoGap = 5.0f;
	inline float			keyHeight = 0.0f;
	inline float			keyPadX = 0.0f;
	inline constexpr float	keyEdge = 2.0f;
	inline constexpr float	keyRounding = 4.0f;
	inline constexpr float	keyGap = 4.0f;				// on top of a space
	inline constexpr float	keycapGap = 3.0f;			// between the keys of one hotkey

	// Header
	inline constexpr float	coverWidth = 84.0f;
	inline constexpr float	coverHeight = 126.0f;
	inline constexpr float	coverRounding = 8.0f;
	inline constexpr float	coverGap = 16.0f;
	inline constexpr float	chipGap = 5.0f;
	inline constexpr float	chipEdge = 1.0f;

	// The overlay's window
	inline constexpr float	windowRounding = 10.0f;
	inline constexpr float	windowPad = 10.0f;			// the content off the window's edges, at least the rounding
	inline constexpr float	windowMinWidth = 360.0f;
	inline constexpr float	windowMinHeight = 240.0f;
	inline constexpr float	resizeEdge = 6.0f;
	inline constexpr float	tabHeight = 44.0f;
	inline constexpr float	tabPadX = 14.0f;
	inline constexpr float	tabGap = 4.0f;
	inline constexpr float	tabLine = 2.0f;
	inline constexpr float	scrollbarWidth = 7.0f;
	inline constexpr float	scrollbarGap = 2.0f;
	inline constexpr float	scrollThumbMin = 24.0f;
	inline constexpr float	closeSize = 12.0f;
	inline constexpr float	messagePad = 8.0f;
	inline constexpr float	dotSize = 5.0f;

	// Timing
	inline constexpr float	disabledAlpha = 0.35f;
	inline constexpr double	swingSeconds = 0.16;
	inline constexpr float	swingStretch = 1.0f;		// the lead edge lands this much sooner, the knob stretches meanwhile
	inline constexpr double	fadeSeconds = 0.12;
	inline constexpr double	tipDelay = 0.45;
	inline constexpr double	doneSeconds = 1.2;			// a button's done label

	// The built-in theme, once at the DLL's load; a DLL without it or with one that does not read says so in its log
	void load ();

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
	[[ nodiscard ]] std::string text ();

	// A file's text taken in where it reads, else why not (the values stay as they were)
	[[ nodiscard ]] bool take ( std::string_view text, std::string& why );
}
