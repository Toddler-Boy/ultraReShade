/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cstdio>

#include "ultra_ui_draw.hpp"
#include "ultra_ui_font.hpp"
#include "ultra_ui_widgets.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	namespace
	{
		struct Hsv
		{
			float	h = 0.0f;		// 0 to 1 around the wheel
			float	s = 0.0f;
			float	v = 0.0f;
		};

		// The open picker's: its hue and saturation kept while it is open, which a grey or black alone would lose. One is open at a time
		struct Picking
		{
			ImGuiID	id = 0;
			Hsv		hsv;
		} picking;

		[[ nodiscard ]] float channel ( ImU32 c, int shift )
		{
			return float ( ( c >> shift ) & 0xFF ) / 255.0f;
		}

		[[ nodiscard ]] Hsv hsvOf ( ImU32 c )
		{
			Hsv	out;
			ImGui::ColorConvertRGBtoHSV ( channel ( c, IM_COL32_R_SHIFT ), channel ( c, IM_COL32_G_SHIFT ), channel ( c, IM_COL32_B_SHIFT ), out.h, out.s, out.v );
			return out;
		}

		[[ nodiscard ]] ImU32 rgbOf ( const Hsv& x )
		{
			float	r = 0.0f, g = 0.0f, b = 0.0f;
			ImGui::ColorConvertHSVtoRGB ( x.h, x.s, x.v, r, g, b );
			const auto	byte = [] ( float f ) { return int ( std::clamp ( f, 0.0f, 1.0f ) * 255.0f + 0.5f ); };
			return IM_COL32 ( byte ( r ), byte ( g ), byte ( b ), 0xFF );
		}

		// The hex code's ink on the swatch: whichever of black and white reads on it
		[[ nodiscard ]] ImU32 inkOn ( ImU32 c )
		{
			const auto	luminance = 0.2126f * channel ( c, IM_COL32_R_SHIFT ) + 0.7152f * channel ( c, IM_COL32_G_SHIFT ) + 0.0722f * channel ( c, IM_COL32_B_SHIFT );
			return luminance > 0.5f ? IM_COL32_BLACK : theme::colors::white;
		}

		// A spot of the square or the strip followed while it is dragged; true while it moves the colour
		[[ nodiscard ]] bool drag ( const char* id, ImVec2 min, ImVec2 size, ImVec2& at, bool& released )
		{
			ImGui::SetCursorScreenPos ( min );
			ImGui::InvisibleButton ( id, size );
			if ( ImGui::IsItemHovered () || ImGui::IsItemActive () )
				ImGui::SetMouseCursor ( ImGuiMouseCursor_Hand );
			released |= ImGui::IsItemDeactivated ();
			if ( ! ImGui::IsItemActive () )
				return false;

			const auto	mouse = ImGui::GetMousePos ();
			at = ImVec2 ( std::clamp ( ( mouse.x - min.x ) / size.x, 0.0f, 1.0f ), std::clamp ( ( mouse.y - min.y ) / size.y, 0.0f, 1.0f ) );
			return true;
		}

		// The open picker: our own box with its shadow, the square and the strip drawn over it
		[[ nodiscard ]] Edit picker ( const char* popupId, ImU32& color, ImVec2 size )
		{
			Edit	edit;
			if ( ! ImGui::BeginPopup ( popupId, ImGuiWindowFlags_NoMove ) )
				return edit;

			auto*		dl = ImGui::GetWindowDrawList ();
			const auto	min = ImGui::GetCursorScreenPos ();
			const ImVec2	max ( min.x + size.x, min.y + size.y );
			const auto	rounding = theme::px ( theme::menuRounding );
			const auto	gap = theme::px ( theme::pickerGap );
			const auto	side = theme::px ( theme::pickerSquare );
			const auto	hueW = theme::px ( theme::pickerHueWidth );
			const auto	edge = draw::color ( theme::colors::popupBorder );

			draw::shadow ( dl, min, max, rounding );
			dl->AddRectFilled ( min, max, draw::color ( theme::colors::popup ), rounding );
			dl->AddRect ( min, max, edge, rounding );

			auto&			hsv = picking.hsv;
			const ImVec2	sqMin ( min.x + gap, min.y + gap );
			const ImVec2	sqMax ( sqMin.x + side, sqMin.y + side );
			const ImVec2	hueMin ( sqMax.x + gap, sqMin.y );
			const ImVec2	hueMax ( hueMin.x + hueW, sqMax.y );
			auto			released = false;

			if ( ImVec2 at; drag ( "square", sqMin, ImVec2 ( side, side ), at, released ) )
			{
				hsv.s = at.x;
				hsv.v = 1.0f - at.y;
				edit.changed = true;
			}
			if ( ImVec2 at; drag ( "hue", hueMin, ImVec2 ( hueW, side ), at, released ) )
			{
				hsv.h = std::min ( at.y, 0.9999f );
				edit.changed = true;
			}
			if ( edit.changed )
				color = rgbOf ( hsv );
			edit.committed = released;

			// Saturation from white to the full hue left to right, brightness fading to black downwards
			const auto	full = draw::color ( rgbOf ( { hsv.h, 1.0f, 1.0f } ) );
			const auto	white = draw::color ( IM_COL32_WHITE );
			const auto	black = draw::color ( IM_COL32_BLACK );
			const auto	clear = draw::color ( IM_COL32_BLACK_TRANS );
			dl->AddRectFilledMultiColor ( sqMin, sqMax, white, full, full, white );
			dl->AddRectFilledMultiColor ( sqMin, sqMax, clear, clear, black, black );
			dl->AddRect ( sqMin, sqMax, edge );

			constexpr int	hueSteps = 6;
			for ( int i = 0; i < hueSteps; ++i )
			{
				const auto	top = draw::color ( rgbOf ( { float ( i ) / float ( hueSteps ), 1.0f, 1.0f } ) );
				const auto	bottom = draw::color ( rgbOf ( { float ( i + 1 ) / float ( hueSteps ), 1.0f, 1.0f } ) );
				const auto	y0 = hueMin.y + side * float ( i ) / float ( hueSteps );
				const auto	y1 = hueMin.y + side * float ( i + 1 ) / float ( hueSteps );
				dl->AddRectFilledMultiColor ( ImVec2 ( hueMin.x, y0 ), ImVec2 ( hueMax.x, y1 ), top, top, bottom, bottom );
			}
			dl->AddRect ( hueMin, hueMax, edge );

			// The point picked, ringed light over dark so it shows on any colour; the hue as a bar across the strip
			const auto		line = theme::px ( 1.5f );
			const auto		r = theme::px ( theme::pickerMarker );
			const ImVec2	spot ( sqMin.x + side * hsv.s, sqMin.y + side * ( 1.0f - hsv.v ) );
			dl->AddCircle ( spot, r + line, black, 0, line );
			dl->AddCircle ( spot, r, draw::color ( theme::colors::white ), 0, line );

			const auto	y = hueMin.y + side * hsv.h;
			dl->AddRect ( ImVec2 ( hueMin.x - line, y - line * 2.0f ), ImVec2 ( hueMax.x + line, y + line * 2.0f ), black, line, line * 2.0f );
			dl->AddRect ( ImVec2 ( hueMin.x - line, y - line * 2.0f ), ImVec2 ( hueMax.x + line, y + line * 2.0f ), draw::color ( theme::colors::white ), line, line );

			ImGui::SetCursorScreenPos ( min );
			ImGui::Dummy ( size );
			ImGui::EndPopup ();
			return edit;
		}
	}

	Edit colorPicker ( const char* id, ImU32& color, ImU32 def, float width )
	{
		const auto	rowH = theme::px ( theme::rowHeight );
		const auto	pos = ImGui::GetCursorScreenPos ();
		const auto	itemId = ImGui::GetID ( id );

		ImGui::PushID ( id );
		const auto	popupId = "picker";
		const auto	open = ImGui::IsPopupOpen ( popupId );

		Edit	reset;
		if ( ImGui::InvisibleButton ( "swatch", ImVec2 ( width, rowH ) ) && ! open && ! ImGui::GetIO ().KeyAlt )
		{
			ImGui::OpenPopup ( popupId );
			picking = { itemId, hsvOf ( color ) };
		}
		if ( resetClicked () )
		{
			reset.changed = color != def;
			reset.committed = reset.changed;
			color = def;
		}
		const auto	hovered = ImGui::IsItemHovered ();
		if ( hovered )
			ImGui::SetMouseCursor ( ImGuiMouseCursor_Hand );

		auto*		dl = ImGui::GetWindowDrawList ();
		const ImVec2	max ( pos.x + width, pos.y + rowH );
		const auto	rounding = theme::px ( theme::dropRounding );
		dl->AddRectFilled ( pos, max, draw::color ( color ), rounding );
		dl->AddRect ( pos, max, draw::color ( hovered || open ? theme::colors::white : theme::colors::popupBorder ), rounding, theme::px ( theme::swatchEdge ) );

		char	hex[ 8 ];
		std::snprintf ( hex, sizeof hex, "#%02X%02X%02X", unsigned ( ( color >> IM_COL32_R_SHIFT ) & 0xFF ), unsigned ( ( color >> IM_COL32_G_SHIFT ) & 0xFF ),
						unsigned ( ( color >> IM_COL32_B_SHIFT ) & 0xFF ) );
		const auto	size = theme::px ( theme::sizes::choice );
		const auto	textW = font::width ( font::Face::semibold, size, hex );
		font::draw ( dl, ImVec2 ( pos.x + ( width - textW ) * 0.5f, font::centredTop ( font::Face::semibold, size, pos.y, rowH ) ), font::Face::semibold, size,
					 draw::color ( inkOn ( color ) ), hex );

		// The picker opens a gap under the swatch, at its left edge
		const auto		gap = theme::px ( theme::pickerGap );
		const ImVec2	popupSize ( gap * 3.0f + theme::px ( theme::pickerSquare ) + theme::px ( theme::pickerHueWidth ), gap * 2.0f + theme::px ( theme::pickerSquare ) );
		ImGui::SetNextWindowPos ( ImVec2 ( pos.x, max.y + theme::px ( theme::menuGap ) ) );
		ImGui::SetNextWindowSize ( popupSize );
		ImGui::PushStyleVar ( ImGuiStyleVar_WindowPadding, ImVec2 ( 0.0f, 0.0f ) );
		ImGui::PushStyleVar ( ImGuiStyleVar_PopupBorderSize, 0.0f );
		ImGui::PushStyleColor ( ImGuiCol_PopupBg, IM_COL32 ( 0, 0, 0, 0 ) );

		const auto	edit = picking.id == itemId ? picker ( popupId, color, popupSize ) : Edit {};

		ImGui::PopStyleColor ();
		ImGui::PopStyleVar ( 2 );
		ImGui::PopID ();
		return { edit.changed || reset.changed, edit.committed || reset.committed };
	}
}
