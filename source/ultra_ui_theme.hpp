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
// The overlay's look in one place: every colour, size and timing is a value of res/panel-theme.json, built into the DLL. Sizes are
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
		inline float	body = 0.0f;
		inline float	heading = 0.0f;
		inline float	subheading = 0.0f;
		inline float	title = 0.0f;
		inline float	choice = 0.0f;
		inline float	value = 0.0f;
		inline float	key = 0.0f;
		inline float	tip = 0.0f;
		inline float	info = 0.0f;
	}

	// Layout
	inline float	panelInset = 0.0f;			// the whole page off the window's edges, all four
	inline float	panelWidth = 0.0f;			// the content's, whatever the window: narrower scrolls sideways, wider stays empty
	inline float	rowHeight = 0.0f;
	inline float	rowGap = 0.0f;
	inline float	columnGap = 0.0f;			// label to control, control to value
	inline float	resetWidth = 0.0f;
	inline float	resetIcon = 0.0f;
	inline float	indent = 0.0f;
	inline float	separatorAbove = 0.0f;
	inline float	headingAbove = 0.0f;
	inline float	headingBelow = 0.0f;
	inline float	subheadingAbove = 0.0f;		// a group's heading inside a section, unless it comes first
	inline float	firstRowAbove = 0.0f;		// a choice or drop-down right under its heading
	inline float	headingGap = 0.0f;			// switch to heading text
	inline float	inlineToggleHeight = 0.0f;
	inline float	separatorLine = 0.0f;		// the separator's height
	inline float	dragThreshold = 0.0f;		// a heading moves its section after the mouse went this far

	// Slider
	inline float	sliderBack = 0.0f;
	inline float	sliderPad = 0.0f;
	inline float	sliderHandleGrow = 0.0f;	// the handle past the fill, on each side, while hovered
	inline float	bipolarMinFill = 0.0f;
	inline float	valueEditPad = 0.0f;		// the value's edit box reaches this far past its text

	// Toggle
	inline float	switchWidth = 0.0f;
	inline float	switchHeight = 0.0f;
	inline float	knobSize = 0.0f;

	// Choice, drop-down, popups
	inline float	choicePad = 0.0f;
	inline float	dropRounding = 0.0f;
	inline float	dropPadLeft = 0.0f;
	inline float	dropPadRight = 0.0f;
	inline float	chevronSize = 0.0f;
	inline float	disclosureSize = 0.0f;		// a collapsible section's arrow
	inline float	disclosureRadius = 0.0f;	// its corners' rounding
	inline float	menuRounding = 0.0f;
	inline float	menuItemRounding = 0.0f;
	inline float	menuPad = 0.0f;
	inline float	menuItemPad = 0.0f;
	inline float	menuGap = 0.0f;
	inline float	tipPadX = 0.0f;
	inline float	tipPadY = 0.0f;
	inline float	tipMaxWidth = 0.0f;
	inline float	tipGap = 0.0f;
	inline float	tipEdge = 0.0f;				// a tooltip's outline: its background this far towards white, opaque
	inline float	shadowSpread = 0.0f;
	inline float	shadowOffset = 0.0f;
	inline float	shadowAlpha = 0.0f;

	// Colour picker: the swatch in the control column, its popup's saturation and brightness square with the hue strip beside it
	inline float	pickerSquare = 0.0f;
	inline float	pickerHueWidth = 0.0f;
	inline float	pickerGap = 0.0f;			// between the parts, and around them
	inline float	pickerMarker = 0.0f;		// the point picked in the square, its radius
	inline float	swatchEdge = 0.0f;
	inline float	pickerLine = 0.0f;			// the picked point's ring and the hue bar

	// Button
	inline float	buttonPadX = 0.0f;
	inline float	buttonGap = 0.0f;

	// Info dot and keycap
	inline float	infoSize = 0.0f;
	inline float	infoGap = 0.0f;
	inline float	keyHeight = 0.0f;
	inline float	keyPadX = 0.0f;
	inline float	keyEdge = 0.0f;
	inline float	keyRounding = 0.0f;
	inline float	keyGap = 0.0f;				// on top of a space
	inline float	keycapGap = 0.0f;			// between the keys of one hotkey

	// Header
	inline float	coverWidth = 0.0f;
	inline float	coverHeight = 0.0f;
	inline float	coverRounding = 0.0f;
	inline float	coverGap = 0.0f;
	inline float	chipGap = 0.0f;
	inline float	chipEdge = 0.0f;

	// The overlay's window
	inline float	windowRounding = 0.0f;
	inline float	windowPad = 0.0f;			// the content off the window's edges, at least the rounding
	inline float	windowMinWidth = 0.0f;
	inline float	windowMinHeight = 0.0f;
	inline float	resizeEdge = 0.0f;
	inline float	tabHeight = 0.0f;				// the tab track's height; the bar adds the insets above and below
	inline float	tabPadX = 0.0f;
	inline float	tabGap = 0.0f;
	inline float	tabLine = 0.0f;
	inline float	scrollbarWidth = 0.0f;
	inline float	scrollbarGap = 0.0f;
	inline float	scrollThumbMin = 0.0f;
	inline float	closeSize = 0.0f;
	inline float	messagePad = 0.0f;
	inline float	dotSize = 0.0f;
	inline float	tabInsetTop = 0.0f;			// the space above the tab track and below it, in the bar
	inline float	tabInsetBottom = 0.0f;
	inline float	choiceRowHeight = 0.0f;			// a row-style choice's height, its row's too; every other row is rowHeight
	inline float	tabPillInset = 0.0f;		// the selected pill's in the track
	inline float	dotGap = 0.0f;				// a tab's dot to its label
	inline float	dotRing = 0.0f;				// the dot's dark ring
	inline float	closeLine = 0.0f;			// the close cross's line

	// Timing, in seconds
	inline float	disabledAlpha = 0.0f;
	inline float	swingSeconds = 0.0f;
	inline float	swingStretch = 0.0f;		// the lead edge lands this much sooner, the knob stretches meanwhile
	inline float	fadeSeconds = 0.0f;
	inline float	tipDelay = 0.0f;
	inline float	doneSeconds = 0.0f;			// a button's done label

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
	[[ nodiscard ]] std::string text ();

	// A file's text taken in: every value it holds right is applied, one it lacks or holds wrong stays as this build shipped it. False
	// with the first such value named in why
	[[ nodiscard ]] bool take ( std::string_view text, std::string& why );
}
