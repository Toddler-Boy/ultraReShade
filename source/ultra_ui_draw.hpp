/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "ultra_ui_hotkey.hpp"
#include "ultra_ui_theme.hpp"

//-----------------------------------------------------------------------------
// Drawing shared by the widgets. Every colour goes through color (), so a disabled group's alpha reaches it

namespace ultra::ui::draw
{
	[[ nodiscard ]] ImU32 color ( ImU32 c, float alpha = 1.0f );
	[[ nodiscard ]] ImU32 mix ( ImU32 a, ImU32 b, float t );

	// Where a box of that size wanted at pos goes so it stays on the screen: moved up and left only as far as needed
	[[ nodiscard ]] ImVec2 onScreen ( ImVec2 pos, ImVec2 size );

	// A soft drop shadow under a rounded box, drawn before it; reaches past the window's clip rect
	void shadow ( ImDrawList* drawList, ImVec2 min, ImVec2 max, float rounding, float alpha = 1.0f );

	// A hotkey as keycaps, one per key, at pos (top left); their width
	float keycaps ( ImDrawList* drawList, ImVec2 pos, const Hotkey& hotkey );
	[[ nodiscard ]] float keycapsWidth ( const Hotkey& hotkey );

	// The filled circle with an "i" after a label that has a tooltip, centred on centre
	void infoDot ( ImDrawList* drawList, ImVec2 centre );

	// The reset arrow after a slider off its default
	void resetIcon ( ImDrawList* drawList, ImVec2 centre, float size, ImU32 col );

	// The drop-down's chevron; open turns it upside down
	void chevron ( ImDrawList* drawList, ImVec2 centre, float size, ImU32 col, float open );

	// A collapsible section's arrow, a filled triangle with corners rounded by radius: pointing right while closed, turned
	// down as open goes to 1
	void disclosure ( ImDrawList* drawList, ImVec2 centre, float size, float radius, ImU32 col, float open );
}
