/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ultra_ui_anim.hpp"
#include "ultra_ui_draw.hpp"
#include "ultra_ui_widgets.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	bool collapseArrow ( const char* id, bool open, float width )
	{
		const auto	h = theme::px ( theme::heights::row );
		const auto	size = theme::px ( theme::icons::icon );
		const auto	pos = ImGui::GetCursorScreenPos ();
		const auto	clicked = ImGui::InvisibleButton ( id, ImVec2 ( width, h ) );
		const auto	hovered = ImGui::IsItemHovered ();

		if ( hovered )
			ImGui::SetMouseCursor ( ImGuiMouseCursor_Hand );

		draw::sectionChevron ( ImGui::GetWindowDrawList (), ImVec2 ( pos.x + width - size, pos.y + h * 0.5f ), size,
						draw::color ( theme::shade ( hovered ? theme::shades::body : theme::shades::muted ) ), anim::fade ( ImGui::GetItemID (), open ) );

		return clicked;
	}
}
