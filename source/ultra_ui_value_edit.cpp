/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

		// ImGui's own text field does the editing, drawn invisibly; we draw its text, selection and caret in our font.
		// Where a click lands is ours too: ImGui would place it by its font's glyphs, not ours
		struct Field
		{
			ImGuiID		id = 0;
			bool		focusPending = false;
			bool		wasActive = false;
			char		buffer[ 32 ] {};
			std::string	text;
			int			cursor = 0;
			int			selStart = 0;
			int			selEnd = 0;
			int			anchor = -1;			// where a mouse selection started, while the button is down
			double		cursorMoved = 0.0;
			float		x = 0.0f;
			float		size = 0.0f;
			ImVec2		boxMin;
			ImVec2		boxMax;
		} field;

		// x of byte offset at in the field's text
		[[ nodiscard ]] float offsetX ( int at )
		{
			return font::width ( font::Face::bold, field.size, std::string_view ( field.text ).substr ( 0, size_t ( at ) ), true );
		}

		// The byte offset nearest to x, on a character boundary
		[[ nodiscard ]] int hit ( float x )
		{
			auto	best = 0;
			auto	bestDistance = std::abs ( x - field.x );

			for ( int at = 1; at <= int ( field.text.size () ); ++at )
			{
				if ( at < int ( field.text.size () ) && ( uint8_t ( field.text[ size_t ( at ) ] ) & 0xC0 ) == 0x80 )
					continue;

				if ( const auto d = std::abs ( x - ( field.x + offsetX ( at ) ) ); d < bestDistance )
				{
					best = at;
					bestDistance = d;
				}
			}

			return best;
		}

		int onEdit ( ImGuiInputTextCallbackData* data )
		{
			field.text.assign ( data->Buf, size_t ( data->BufTextLen ) );

			const auto	mouseX = ImGui::GetMousePos ().x;
			const auto	inside = ImGui::IsMouseHoveringRect ( field.boxMin, field.boxMax );

			if ( inside && ImGui::IsMouseDoubleClicked ( ImGuiMouseButton_Left ) )
			{
				data->SelectAll ();
				data->CursorPos = data->BufTextLen;
				field.anchor = -1;
			}
			else if ( inside && ImGui::IsMouseClicked ( ImGuiMouseButton_Left ) )
			{
				field.anchor = hit ( mouseX );
				data->CursorPos = data->SelectionStart = data->SelectionEnd = field.anchor;
			}
			else if ( field.anchor >= 0 && ImGui::IsMouseDown ( ImGuiMouseButton_Left ) )
			{
				data->SelectionStart = field.anchor;
				data->CursorPos = data->SelectionEnd = hit ( mouseX );
			}
			else
				field.anchor = -1;

			if ( data->CursorPos != field.cursor )
				field.cursorMoved = ImGui::GetTime ();

			field.cursor = data->CursorPos;
			field.selStart = std::min ( data->SelectionStart, data->SelectionEnd );
			field.selEnd = std::max ( data->SelectionStart, data->SelectionEnd );
			return 0;
		}

		// The typed number in the shown unit; false when there is no number
		[[ nodiscard ]] bool parse ( const char* text, float& out )
		{
			char*	end = nullptr;
			out = std::strtof ( text, &end );
			return end != text;
		}

		[[ nodiscard ]] Edit editing ( const char* id, float& value, const SliderRange& range, const Format& format, float width, ImVec2 pos )
		{
			const auto	rowH = theme::px ( theme::heights::row );
			auto*		dl = ImGui::GetWindowDrawList ();
			Edit		edit;

			ImGui::PushStyleColor ( ImGuiCol_FrameBg, IM_COL32 ( 0, 0, 0, 0 ) );
			ImGui::PushStyleColor ( ImGuiCol_Text, IM_COL32 ( 0, 0, 0, 0 ) );
			ImGui::PushStyleColor ( ImGuiCol_TextSelectedBg, IM_COL32 ( 0, 0, 0, 0 ) );
			ImGui::PushStyleColor ( ImGuiCol_InputTextCursor, IM_COL32 ( 0, 0, 0, 0 ) );
			ImGui::PushStyleVar ( ImGuiStyleVar_FramePadding, ImVec2 ( 0.0f, ( rowH - ImGui::GetFontSize () ) * 0.5f ) );
			ImGui::PushStyleVar ( ImGuiStyleVar_FrameBorderSize, 0.0f );

			// The double-click's button is let go first: taking the focus while it is down, ImGui reads the release as a click
			// away and drops it again
			const auto	focusNow = field.focusPending && ! ImGui::IsMouseDown ( ImGuiMouseButton_Left );
			if ( focusNow )
			{
				ImGui::SetKeyboardFocusHere ();
				field.focusPending = false;
			}

			// An id of its own, not the readout's button's, which the double-click left active
			ImGui::PushID ( id );
			ImGui::SetNextItemWidth ( width );
			ImGui::InputText ( "##edit", field.buffer, sizeof ( field.buffer ), ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CallbackAlways, onEdit, nullptr );
			const auto	active = ImGui::IsItemActive ();
			const auto	done = ImGui::IsItemDeactivated ();
			ImGui::PopID ();

			ImGui::PopStyleVar ( 2 );
			ImGui::PopStyleColor ( 4 );

			if ( active )
				field.wasActive = true;

			// Never taken the focus after asking for it: given up, as if Escape
			if ( ! field.focusPending && ! focusNow && ! active && ! field.wasActive )
			{
				field.id = 0;
				return edit;
			}

			if ( done )
			{
				field.id = 0;

				float	typed;
				if ( ! ImGui::IsKeyPressed ( ImGuiKey_Escape ) && parse ( field.buffer, typed ) )
				{
					const auto	v = snap ( typed / format.scale, range );
					edit.changed = v != value;
					edit.committed = true;
					value = v;
				}

				return edit;
			}

			// Selection, text, then the caret, blinking while it rests
			const auto	top = font::centredTop ( font::Face::bold, field.size, pos.y, rowH );
			const auto	lineH = font::lineHeight ( font::Face::bold, field.size );

			if ( field.selEnd > field.selStart )
				dl->AddRectFilled ( ImVec2 ( field.x + offsetX ( field.selStart ), top ), ImVec2 ( field.x + offsetX ( field.selEnd ), top + lineH ),
									draw::color ( theme::colors::accent, selectionAlpha ) );

			font::draw ( dl, ImVec2 ( field.x, top ), font::Face::bold, field.size, draw::color ( theme::colors::text ), field.text, true );

			if ( std::fmod ( ImGui::GetTime () - field.cursorMoved, blinkPeriod ) < blinkOn )
			{
				const auto	x = std::floor ( field.x + offsetX ( field.cursor ) ) + 0.5f;
				dl->AddLine ( ImVec2 ( x, top ), ImVec2 ( x, top + lineH ), draw::color ( theme::colors::text ) );
			}

			return edit;
		}
	}

	Edit valueEdit ( const char* id, float& value, const SliderRange& range, const Format& format, const char* shown, float width )
	{
		const auto	rowH = theme::px ( theme::heights::row );
		const auto	pad = theme::px ( theme::spacing::xs );
		const auto	size = theme::px ( theme::text::body );
		const auto	pos = ImGui::GetCursorScreenPos ();
		const auto	itemId = ImGui::GetID ( id );
		const ImVec2	boxMin ( pos.x - pad, pos.y );
		const ImVec2	boxMax ( pos.x + width + pad, pos.y + rowH );
		const auto	rounding = theme::px ( theme::radius::little );
		auto*		dl = ImGui::GetWindowDrawList ();

		if ( field.id == itemId )
		{
			dl->AddRectFilled ( boxMin, boxMax, draw::color ( theme::shade ( theme::shades::surface ) ), rounding );
			dl->AddRect ( boxMin, boxMax, draw::color ( theme::colors::accent ), rounding );
			field.x = pos.x;
			field.size = size;
			field.boxMin = boxMin;
			field.boxMax = boxMax;
			return editing ( id, value, range, format, width, pos );
		}

		ImGui::InvisibleButton ( id, ImVec2 ( width, rowH ) );

		if ( ImGui::IsItemHovered () )
		{
			ImGui::SetMouseCursor ( ImGuiMouseCursor_TextInput );
			dl->AddRectFilled ( boxMin, boxMax, draw::color ( theme::shade ( theme::shades::surface ) ), rounding );

			// The number alone to edit, in the unit shown and with an ASCII minus
			if ( ImGui::IsMouseDoubleClicked ( ImGuiMouseButton_Left ) )
			{
				field = {};
				field.id = itemId;
				field.focusPending = true;
				field.cursorMoved = ImGui::GetTime ();
				std::snprintf ( field.buffer, sizeof ( field.buffer ), "%.*f", format.decimals, double ( value * format.scale ) );
				field.text = field.buffer;
			}
		}

		char	text[ 32 ];
		if ( shown == nullptr )
		{
			ui::format ( text, sizeof ( text ), value, format );
			shown = text;
		}
		font::draw ( dl, ImVec2 ( pos.x, font::centredTop ( font::Face::bold, size, pos.y, rowH ) ), font::Face::bold, size, draw::color ( theme::colors::text ), shown, true );

		return {};
	}
}
