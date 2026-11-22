/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cmath>

#include "ultra_ui_anim.hpp"
#include "ultra_ui_draw.hpp"
#include "ultra_ui_font.hpp"
#include "ultra_ui_widgets.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	Edit choice ( const char* id, int& index, std::span<const char* const> names, int def, float width )
	{
		const auto	count = int ( names.size () );
		const auto	rowH = theme::px ( theme::heights::large );
		const auto	pad = theme::px ( theme::spacing::xs );
		const auto	pos = ImGui::GetCursorScreenPos ();
		const auto	itemId = ImGui::GetID ( id );
		const auto	cell = width / float ( std::max ( count, 1 ) );
		Edit		edit;

		ImGui::InvisibleButton ( id, ImVec2 ( width, rowH ) );
		if ( ImGui::IsItemHovered () || ImGui::IsItemActive () )
			ImGui::SetMouseCursor ( ImGuiMouseCursor_Hand );

		// A click or a drag picks the choice under the pointer; Alt+click picks the default
		const auto	pick = [ & ] ( int i )
		{
			edit.changed = i != index;
			edit.committed = edit.changed;
			index = i;
		};

		if ( resetClicked () )
			pick ( def );
		else if ( ImGui::IsItemActive () && ! ImGui::GetIO ().KeyAlt && count > 0 )
			pick ( std::clamp ( int ( std::floor ( ( ImGui::GetMousePos ().x - pos.x ) / cell ) ), 0, count - 1 ) );

		auto*		dl = ImGui::GetWindowDrawList ();
		const ImVec2	max ( pos.x + width, pos.y + rowH );

		dl->AddRectFilled ( pos, max, draw::color ( theme::shade ( theme::shades::surface ) ), rowH * 0.5f );

		// Under the selection's pill, which covers it once it has glided there
		if ( ImGui::IsItemHovered () && count > 0 )
		{
			const auto	hovered = std::clamp ( int ( std::floor ( ( ImGui::GetMousePos ().x - pos.x ) / cell ) ), 0, count - 1 );
			dl->AddRectFilled ( ImVec2 ( pos.x + float ( hovered ) * cell + pad, pos.y + pad ), ImVec2 ( pos.x + float ( hovered + 1 ) * cell - pad, max.y - pad ),
								draw::color ( theme::shade ( theme::shades::hover ) ), ( rowH - pad * 2.0f ) * 0.5f );
		}

		const auto	edges = anim::swing ( itemId, float ( index ) );
		dl->AddRectFilled ( ImVec2 ( pos.x + edges.lo * cell + pad, pos.y + pad ), ImVec2 ( pos.x + ( edges.hi + 1.0f ) * cell - pad, max.y - pad ),
							draw::color ( theme::colors::accent ), ( rowH - pad * 2.0f ) * 0.5f );

		const auto	size = theme::px ( theme::text::body );
		const auto	top = font::centredTop ( font::Face::semibold, size, pos.y, rowH );
		for ( int i = 0; i < count; ++i )
		{
			const auto	w = font::width ( font::Face::semibold, size, names[ size_t ( i ) ] );
			const auto	x = pos.x + cell * float ( i );
			dl->PushClipRect ( ImVec2 ( x + pad, pos.y ), ImVec2 ( x + cell - pad, max.y ), true );
			font::draw ( dl, ImVec2 ( x + cell * 0.5f - w * 0.5f, top ), font::Face::semibold, size, draw::color ( theme::colors::text ), names[ size_t ( i ) ] );
			dl->PopClipRect ();
		}

		return edit;
	}
}
