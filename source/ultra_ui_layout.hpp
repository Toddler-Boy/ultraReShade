/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <reshade_ultra_ui.h>

#include "ultra_ui_theme.hpp"

//-----------------------------------------------------------------------------
// A page of sections and rows (reshade_ultra_ui.h) as rows in one label and one value column; a section collapsed shows its heading alone,
// off or dimmed its rows dimmed. The overlay's tabs of an add-on and the Shaders tab are drawn by it

namespace ultra::ui
{
	// Once per drawn frame, before anything of ours: the scale of this window, the glyph atlas on its device
	void beginFrame ( api::effect_runtime* runtime );

	// The page as the whole of the tab being drawn: its own background, a scrolling child that keeps its place while the overlay is closed,
	// the header, the sections
	void drawPage ( api::effect_runtime* runtime, const ReShadeUiPage& page, void* frame );

	// The mouse was over the overlay at the last drawn page. Any thread
	[[ nodiscard ]] bool isMouseOverOverlay ();

	// The overlay is closed: the mouse is over nothing of ours
	void forgetMouseOver ();
}
