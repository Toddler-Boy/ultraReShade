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
	float labelWidth ( const char* text, bool hasTooltip, Applies applies )
	{
		const auto	size = theme::px ( theme::text::body );
		auto		w = font::width ( font::Face::regular, size, text );
		if ( applies != Applies::now )
			w += font::width ( font::Face::regular, size, laterMark );

		return hasTooltip ? w + theme::px ( theme::spacing::xs + theme::icons::icon ) : w;
	}

	void label ( ImVec2 pos, const char* text, const char* tooltip, Applies applies )
	{
		auto*		dl = ImGui::GetWindowDrawList ();
		const auto	size = theme::px ( theme::text::body );
		const auto	rowH = theme::px ( theme::heights::row );
		const auto	top = font::centredTop ( font::Face::regular, size, pos.y, rowH );
		const auto	color = draw::color ( labelColor ( applies ) );
		auto		w = font::draw ( dl, ImVec2 ( pos.x, top ), font::Face::regular, size, color, text );
		if ( applies != Applies::now )
			w += font::draw ( dl, ImVec2 ( pos.x + w, top ), font::Face::regular, size, color, laterMark );

		if ( tooltip == nullptr )
			return;

		const auto	dot = theme::px ( theme::icons::icon );
		draw::infoDot ( dl, ImVec2 ( pos.x + w + theme::px ( theme::spacing::xs ) + dot * 0.5f, pos.y + rowH * 0.5f ) );

		const ImVec2	max ( pos.x + w + theme::px ( theme::spacing::xs ) + dot, pos.y + rowH );
		ui::tooltip ( ImGui::GetID ( text ), ImGui::IsMouseHoveringRect ( pos, max ), tooltip );
	}
}
