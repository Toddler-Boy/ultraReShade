/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "ultra_ui_theme.hpp"

//-----------------------------------------------------------------------------
// The overlay's window in our look: shadow, rounded fill, an opaque edge, the tab bar with its line gliding to the selected tab, a close
// button, the cursor of the player's own Windows scheme, the message strip. It moves by the tab bar and resizes by its edges and corners

namespace ultra::ui::window
{
	// A tab: its label, the name it is saved under, what draws its content in the area the window gives it
	struct Tab
	{
		std::string					label;
		std::string					name;
		std::function<void ()>		draw;
	};

	// Where the window is, as fractions of the back buffer, and the selected tab by name. The caller keeps and saves it
	struct State
	{
		float		position[ 2 ] = { 0.02f, 0.04f };
		float		size[ 2 ] = { 0.4f, 0.9f };
		std::string	tab;
	};

	struct Result
	{
		bool	isMovedOrResized = false;	// the user let go of an edge or the tab bar: save the window
		bool	isTabChanged = false;		// the user picked another tab: save its name
		bool	isCloseWanted = false;
	};

	// Inside the frame, once the overlay is open
	[[ nodiscard ]] Result draw ( State& state, std::span<const Tab> tabs );

	// A tab's name gets a dot after it in that colour (0: none)
	void setTabDot ( std::string_view label, uint32_t color );

	// Whether text is being typed into a field of ours
	[[ nodiscard ]] bool isTyping ();

	// ImGui's own style in the look's colours and sizes, once per frame
	void applyStyle ();

	// Where the pointer is, in the player's own Windows cursor (its scheme, size and colour, at the display's scale), drawn over everything
	void drawCursor ( api::effect_runtime* runtime );

	// A notice in the message strip for that many seconds, in that colour (0: the message text colour)
	void notice ( std::string text, uint32_t color, double seconds );

	// Messages in a strip at the top left, over everything
	struct Message
	{
		std::string	text;
		uint32_t	color = 0;		// 0: the message text colour
	};

	void drawMessages ( std::span<const Message> messages );
}
