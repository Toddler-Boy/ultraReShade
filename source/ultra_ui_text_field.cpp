/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

#include "ultra_ui_draw.hpp"
#include "ultra_ui_font.hpp"
#include "ultra_ui_widgets.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	namespace
	{
		constexpr double	blinkPeriod = 1.0;
		constexpr double	blinkOn = 0.6;
		constexpr float		selectionAlpha = 0.45f;
		constexpr size_t	maxChars = 63;

		// ImGui's own text field does the editing, drawn invisibly; we draw its text, selection and caret in our font, as
		// UI_ValueEdit does. One field is edited at a time
		struct Typing
		{
			ImGuiID		id = 0;
			int			cursor = 0;
			int			selStart = 0;
			int			selEnd = 0;
			double		cursorMoved = 0.0;
		} typing;

		int onEdit ( ImGuiInputTextCallbackData* data )
		{
			if ( data->CursorPos != typing.cursor )
				typing.cursorMoved = ImGui::GetTime ();
			typing.cursor = data->CursorPos;
			typing.selStart = std::min ( data->SelectionStart, data->SelectionEnd );
			typing.selEnd = std::max ( data->SelectionStart, data->SelectionEnd );
			return 0;
		}
	}

	Edit textField ( const char* id, std::string& text, float width )
	{
		const auto	rowH = theme::px ( theme::rowHeight );
		const auto	pad = theme::px ( theme::valueEditPad );
		const auto	size = theme::px ( theme::sizes::body );
		const auto	pos = ImGui::GetCursorScreenPos ();
		const auto	rounding = theme::px ( theme::dropRounding );
		auto*		dl = ImGui::GetWindowDrawList ();

		char	buffer[ maxChars + 1 ] {};
		text.copy ( buffer, std::min ( text.size (), maxChars ) );

		ImGui::PushStyleColor ( ImGuiCol_FrameBg, IM_COL32 ( 0, 0, 0, 0 ) );
		ImGui::PushStyleColor ( ImGuiCol_Text, IM_COL32 ( 0, 0, 0, 0 ) );
		ImGui::PushStyleColor ( ImGuiCol_TextSelectedBg, IM_COL32 ( 0, 0, 0, 0 ) );
		ImGui::PushStyleColor ( ImGuiCol_InputTextCursor, IM_COL32 ( 0, 0, 0, 0 ) );
		ImGui::PushStyleVar ( ImGuiStyleVar_FramePadding, ImVec2 ( pad, ( rowH - ImGui::GetFontSize () ) * 0.5f ) );
		ImGui::PushStyleVar ( ImGuiStyleVar_FrameBorderSize, 0.0f );
		ImGui::SetNextItemWidth ( width );
		Edit		edit;
		edit.changed = ImGui::InputText ( id, buffer, sizeof buffer, ImGuiInputTextFlags_CallbackAlways, onEdit, nullptr );
		const auto	active = ImGui::IsItemActive ();
		const auto	hovered = ImGui::IsItemHovered ();
		edit.committed = ImGui::IsItemDeactivated ();
		const auto	itemId = ImGui::GetItemID ();
		ImGui::PopStyleVar ( 2 );
		ImGui::PopStyleColor ( 4 );

		if ( edit.changed )
			text = buffer;
		if ( active )
			typing.id = itemId;
		else if ( typing.id == itemId )
			typing.id = 0;

		const ImVec2	boxMin ( pos.x, pos.y );
		const ImVec2	boxMax ( pos.x + width, pos.y + rowH );
		dl->AddRectFilled ( boxMin, boxMax, draw::color ( theme::colors::control ), rounding );
		if ( active || hovered )
			dl->AddRect ( boxMin, boxMax, draw::color ( active ? theme::colors::accent : theme::colors::track ), rounding );
		if ( hovered )
			ImGui::SetMouseCursor ( ImGuiMouseCursor_TextInput );

		// Selection, text, then the caret, blinking while it rests
		const auto	x = pos.x + pad;
		const auto	top = font::centredTop ( font::Face::regular, size, pos.y, rowH );
		const auto	lineH = font::lineHeight ( font::Face::regular, size );
		const auto	offsetX = [ & ] ( int at ) { return font::width ( font::Face::regular, size, std::string_view ( text ).substr ( 0, size_t ( std::clamp ( at, 0, int ( text.size () ) ) ) ) ); };

		if ( active && typing.selEnd > typing.selStart )
			dl->AddRectFilled ( ImVec2 ( x + offsetX ( typing.selStart ), top ), ImVec2 ( x + offsetX ( typing.selEnd ), top + lineH ),
								draw::color ( theme::colors::accent, selectionAlpha ) );
		font::draw ( dl, ImVec2 ( x, top ), font::Face::regular, size, draw::color ( theme::colors::bright ), text );
		if ( active && std::fmod ( ImGui::GetTime () - typing.cursorMoved, blinkPeriod ) < blinkOn )
		{
			const auto	caret = std::floor ( x + offsetX ( typing.cursor ) ) + 0.5f;
			dl->AddLine ( ImVec2 ( caret, top ), ImVec2 ( caret, top + lineH ), draw::color ( theme::colors::bright ) );
		}
		return edit;
	}
}
