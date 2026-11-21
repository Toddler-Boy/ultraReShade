/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ultra_ui_draw.hpp"
#include "ultra_ui_font.hpp"
#include "ultra_ui_widgets.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	void subheading ( ImVec2 pos, const char* text )
	{
		const auto	size = theme::px ( theme::sizes::subheading );
		font::draw ( ImGui::GetWindowDrawList (), ImVec2 ( pos.x, font::centredTop ( font::Face::semibold, size, pos.y, theme::px ( theme::rowHeight ) ) ),
					 font::Face::semibold, size, draw::color ( theme::colors::muted ), text );
	}
}
