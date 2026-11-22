/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ultra_ui_draw.hpp"
#include "ultra_ui_widgets.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	bool resetButton ( const char* id )
	{
		const auto	w = theme::px ( theme::spacing::xl );
		const auto	h = theme::px ( theme::heights::row );
		const auto	pos = ImGui::GetCursorScreenPos ();
		const auto	clicked = ImGui::InvisibleButton ( id, ImVec2 ( w, h ) );

		if ( ImGui::IsItemHovered () )
			ImGui::SetMouseCursor ( ImGuiMouseCursor_Hand );

		const auto	col = ImGui::IsItemHovered () ? theme::accentHover () : theme::colors::accent;
		draw::resetIcon ( ImGui::GetWindowDrawList (), ImVec2 ( pos.x + w * 0.5f, pos.y + h * 0.5f ), theme::px ( theme::icons::icon ), draw::color ( col ) );

		return clicked;
	}
}
