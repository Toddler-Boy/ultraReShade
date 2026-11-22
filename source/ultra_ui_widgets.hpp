/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <span>
#include <string>

#include <reshade_ultra_ui.h>

#include "ultra_ui_format.hpp"
#include "ultra_ui_hotkey.hpp"
#include "ultra_ui_texture.hpp"
#include "ultra_ui_theme.hpp"

//-----------------------------------------------------------------------------
// The panel's widgets, one source file each. Each submits one ImGui item at the cursor, rowHeight tall unless noted,
// and knows nothing of settings: UI_Layout places them and binds their values

namespace ultra::ui
{
	// changed: the value moved this frame. committed: the edit is finished (released, clicked), time to save it
	struct Edit
	{
		bool	changed = false;
		bool	committed = false;
	};

	// The item just pressed with Alt held: every control takes its default instead of the click
	[[ nodiscard ]] inline bool resetClicked ()	{ return ImGui::IsItemActivated () && ImGui::GetIO ().KeyAlt; }

	// When a setting takes effect: at once, at the next start, or after something the player does. A later one's label
	// shows in its theme colour with laterMark after it
	enum class Applies { now, nextStart, after };

	inline constexpr const char*	laterMark = "*";

	[[ nodiscard ]] inline ImU32 labelColor ( Applies a )
	{
		return a == Applies::nextStart ? theme::colors::warning : a == Applies::after ? theme::colors::appliesAfter : theme::shade ( theme::shades::body );
	}

	// UI_Label.cpp: a row's label, with the info dot and its tooltip when it has one; not an item, it only draws
	void label ( ImVec2 pos, const char* text, const char* tooltip, Applies applies );
	[[ nodiscard ]] float labelWidth ( const char* text, bool hasTooltip, Applies applies );

	// UI_Toggle.cpp: switch and label as one click target, Alt+click sets def. heading: a section's heading, which may show
	// its hotkey
	struct ToggleLook
	{
		bool		heading = false;
		Hotkey		hotkey;
		const char*	tooltip = nullptr;
		Applies		applies = Applies::now;
		bool		def = false;
	};

	[[ nodiscard ]] Edit toggle ( const char* id, bool& on, const char* text, const ToggleLook& look );

	// A heading without a switch, for a section that cannot be off
	void heading ( const char* text, const Hotkey& hotkey = {} );

	// UI_Collapse.cpp: the rest of a section's heading row, width wide, its arrow at the right end; true when clicked
	[[ nodiscard ]] bool collapseArrow ( const char* id, bool open, float width );

	// UI_Subheading.cpp: a group's heading inside a section, rowHeight tall; not an item, it only draws
	void subheading ( ImVec2 pos, const char* text );

	// UI_Slider.cpp: Alt+click resets to def; bipolar (lo < 0 < hi) fills from 0; mid marks a point with ticks, at the track's
	// centre where isMidCentred (each half linear); log spreads a range over decades (lo > 0) evenly along the track
	struct SliderRange
	{
		float	lo = 0.0f;
		float	hi = 1.0f;
		float	step = 0.01f;
		float	def = 0.0f;
		float	mid = 0.0f;
		bool	hasMid = false;
		bool	isMidCentred = false;
		bool	log = false;

		// A value's place along the track, 0 to 1, and back
		[[ nodiscard ]] float position ( float v ) const;
		[[ nodiscard ]] float valueAt ( float position ) const;
	};

	[[ nodiscard ]] Edit slider ( const char* id, float& value, const SliderRange& range, float width );

	// v on the range's step, inside it
	[[ nodiscard ]] float snap ( float v, const SliderRange& range );

	// UI_ColorPicker.cpp: a swatch with the colour's hex code, width wide; a click opens the picker under it, a square of
	// saturation and brightness with the hue strip beside it. Alt+click sets def
	[[ nodiscard ]] Edit colorPicker ( const char* id, ImU32& color, ImU32 def, float width );

	// UI_TextField.cpp: a line of text to type in, in the control column; changed on every keystroke
	[[ nodiscard ]] Edit textField ( const char* id, std::string& text, float width );

	// UI_ValueEdit.cpp: a slider's value as text; a double-click makes it editable, Enter or clicking away takes the
	// typed number, Escape keeps the old one
	[[ nodiscard ]] Edit valueEdit ( const char* id, float& value, const SliderRange& range, const Format& format, float width );

	// UI_Reset.cpp: the arrow after a slider off its default, resetWidth wide; true when clicked
	[[ nodiscard ]] bool resetButton ( const char* id );

	// UI_Choice.cpp: mutually exclusive choices in one pill, the selection gliding to the one picked
	[[ nodiscard ]] Edit choice ( const char* id, int& index, std::span<const char* const> names, int def, float width );

	// UI_Dropdown.cpp: a box showing the current entry, a list under it while open; Alt+click picks def
	[[ nodiscard ]] Edit dropdown ( const char* id, int& index, std::span<const char* const> names, int def, float width );

	// UI_Button.cpp, state per key (its spec item), the width never shrinking: done replaces the label for a moment after
	// flash (); progress 0 to 1 fills the button from the left, below 0 none
	[[ nodiscard ]] bool button ( const void* key, const char* text, bool primary, const char* done = nullptr, const Hotkey& hotkey = {}, float progress = -1.0f );
	[[ nodiscard ]] float buttonWidth ( const void* key, const char* text, const char* done, const Hotkey& hotkey = {} );

	// Shows the done label of the button with this key for a moment
	void flash ( const void* key );

	// UI_Tooltip.cpp: shows text after the hover lasted a moment; call every frame with whether the target is hovered
	void tooltip ( ImGuiID id, bool hovered, const char* text );

	// A label in a small rounded box, white: outlined, else filled
	struct Chip
	{
		std::string	text;
		bool		filled = false;
		const char*	tooltip = nullptr;
	};

	// UI_Header.cpp: cover, title, a row of chips under it when there are any, and two status lines, a third under them when
	// lines[ 2 ] is set; coverHeight tall, without a cover only as tall as its text
	struct HeaderInfo
	{
		const char*		title = nullptr;
		const Texture*	titleImage = nullptr;	// a mask (UI_Image.h) headerTitleHeight () tall; drawn instead of title
		std::span<const Chip>	chips;
		const Texture*	cover = nullptr;
		const char*		lines[ 3 ] {};
		ImU32			lineColors[ 3 ] { theme::shade ( theme::shades::body ), theme::shade ( theme::shades::muted ), theme::shade ( theme::shades::muted ) };
	};

	void header ( const HeaderInfo& info, float width );

	// The same from the C struct of reshade_ultra_ui.h
	void header ( const ReShadeUiHeader& info, float width );

	// The title's line, whole pixels
	[[ nodiscard ]] int headerTitleHeight ();
}
