/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ultra_ui_anim.hpp"
#include "ultra_ui_draw.hpp"
#include "ultra_ui_font.hpp"
#include "ultra_ui_widgets.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	namespace
	{
		// The open list: our own box with its shadow, entries drawn over it
		[[ nodiscard ]] Edit list ( const char* popupId, int& index, std::span<const char* const> names, float width )
		{
			const auto	rowH = theme::px ( theme::rowHeight );
			const auto	pad = theme::px ( theme::menuPad );
			const auto	size = theme::px ( theme::sizes::choice );
			Edit		edit;

			if ( ! ImGui::BeginPopup ( popupId, ImGuiWindowFlags_NoMove ) )
				return edit;

			auto*		dl = ImGui::GetWindowDrawList ();
			const auto	min = ImGui::GetCursorScreenPos ();
			const ImVec2	max ( min.x + width, min.y + rowH * float ( names.size () ) + pad * 2.0f );
			const auto	rounding = theme::px ( theme::menuRounding );

			draw::shadow ( dl, min, max, rounding );
			dl->AddRectFilled ( min, max, draw::color ( theme::colors::popup ), rounding );
			dl->AddRect ( min, max, draw::color ( theme::colors::popupBorder ), rounding );

			for ( size_t i = 0; i < names.size (); ++i )
			{
				const ImVec2	iMin ( min.x + pad, min.y + pad + rowH * float ( i ) );
				const ImVec2	iMax ( max.x - pad, iMin.y + rowH );

				ImGui::SetCursorScreenPos ( iMin );
				ImGui::PushID ( int ( i ) );
				if ( ImGui::InvisibleButton ( "entry", ImVec2 ( iMax.x - iMin.x, rowH ) ) )
				{
					edit.changed = int ( i ) != index;
					edit.committed = edit.changed;
					index = int ( i );
					ImGui::CloseCurrentPopup ();
				}
				const auto	hovered = ImGui::IsItemHovered ();
				ImGui::PopID ();

				const auto	current = int ( i ) == index;
				if ( current || hovered )
					dl->AddRectFilled ( iMin, iMax, draw::color ( current ? theme::colors::accent : theme::colors::popupHover ), theme::px ( theme::menuItemRounding ) );

				dl->PushClipRect ( iMin, iMax, true );
				font::draw ( dl, ImVec2 ( iMin.x + theme::px ( theme::menuItemPad ), font::centredTop ( font::Face::semibold, size, iMin.y, rowH ) ), font::Face::semibold, size,
							 draw::color ( current ? theme::colors::white : theme::colors::text ), names[ i ] );
				dl->PopClipRect ();
			}

			ImGui::SetCursorScreenPos ( min );
			ImGui::Dummy ( ImVec2 ( max.x - min.x, max.y - min.y ) );
			ImGui::EndPopup ();
			return edit;
		}
	}

	Edit dropdown ( const char* id, int& index, std::span<const char* const> names, int def, float width )
	{
		const auto	rowH = theme::px ( theme::rowHeight );
		const auto	pos = ImGui::GetCursorScreenPos ();
		const auto	itemId = ImGui::GetID ( id );

		ImGui::PushID ( id );
		const auto	popupId = "list";
		const auto	open = ImGui::IsPopupOpen ( popupId );

		Edit		reset;
		if ( ImGui::InvisibleButton ( "box", ImVec2 ( width, rowH ) ) && ! open && ! ImGui::GetIO ().KeyAlt )
			ImGui::OpenPopup ( popupId );
		if ( resetClicked () )
		{
			reset.changed = index != def;
			reset.committed = reset.changed;
			index = def;
		}
		const auto	hovered = ImGui::IsItemHovered ();
		if ( hovered )
			ImGui::SetMouseCursor ( ImGuiMouseCursor_Hand );

		auto*		dl = ImGui::GetWindowDrawList ();
		const ImVec2	max ( pos.x + width, pos.y + rowH );
		const auto	size = theme::px ( theme::sizes::choice );

		dl->AddRectFilled ( pos, max, draw::color ( hovered || open ? theme::colors::controlHover : theme::colors::control ), theme::px ( theme::dropRounding ) );

		const auto	chevronSize = theme::px ( theme::chevronSize );
		if ( index >= 0 && size_t ( index ) < names.size () )
		{
			// Cut before the chevron in a narrow box
			dl->PushClipRect ( pos, ImVec2 ( max.x - theme::px ( theme::dropPadRight ) - chevronSize, max.y ), true );
			font::draw ( dl, ImVec2 ( pos.x + theme::px ( theme::dropPadLeft ), font::centredTop ( font::Face::semibold, size, pos.y, rowH ) ), font::Face::semibold, size,
						 draw::color ( theme::colors::bright ), names[ size_t ( index ) ] );
			dl->PopClipRect ();
		}

		draw::chevron ( dl, ImVec2 ( max.x - theme::px ( theme::dropPadRight ) - chevronSize * 0.5f, pos.y + rowH * 0.5f ), chevronSize, draw::color ( theme::colors::chevron ),
						anim::smoothstep ( anim::fade ( itemId, open, theme::swingSeconds ) ) );

		// The list opens a gap under the box, as wide as it
		const auto	pad = theme::px ( theme::menuPad );
		ImGui::SetNextWindowPos ( ImVec2 ( pos.x, max.y + theme::px ( theme::menuGap ) ) );
		ImGui::PushStyleVar ( ImGuiStyleVar_WindowPadding, ImVec2 ( 0.0f, 0.0f ) );
		ImGui::PushStyleVar ( ImGuiStyleVar_PopupBorderSize, 0.0f );
		ImGui::PushStyleColor ( ImGuiCol_PopupBg, IM_COL32 ( 0, 0, 0, 0 ) );
		ImGui::SetNextWindowSize ( ImVec2 ( width, rowH * float ( names.size () ) + pad * 2.0f ) );

		const auto	edit = list ( popupId, index, names, width );

		ImGui::PopStyleColor ();
		ImGui::PopStyleVar ( 2 );
		ImGui::PopID ();
		return { edit.changed || reset.changed, edit.committed || reset.committed };
	}
}
