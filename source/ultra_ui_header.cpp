/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include "ultra_ui_draw.hpp"
#include "ultra_ui_font.hpp"
#include "ultra_ui_widgets.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	namespace
	{
		// Labels in capitals, however the caller spells them
		[[ nodiscard ]] std::string upper ( const std::string& text )
		{
			std::string	out ( text );
			for ( auto& c : out )
				c = char ( std::toupper ( static_cast<unsigned char> ( c ) ) );
			return out;
		}

		// The chips in a row from pos (top left), each with its tooltip; sized as the hotkeys' keycaps
		void chips ( ImDrawList* dl, ImVec2 pos, std::span<const Chip> list )
		{
			const auto	size = theme::px ( theme::sizes::key );
			const auto	h = theme::px ( theme::keyHeight );
			const auto	padX = theme::px ( theme::keyPadX );
			const auto	edge = theme::px ( theme::chipEdge );
			const auto	rounding = theme::px ( theme::keyRounding );
			auto		x = pos.x;

			ImGui::PushID ( "chips" );
			for ( const auto& chip : list )
			{
				const auto		text = upper ( chip.text );
				const auto		w = font::width ( font::Face::semibold, size, text ) + padX * 2.0f;
				const ImVec2	min ( x, pos.y );
				const ImVec2	max ( x + w, pos.y + h );

				// The outline opaque and on pixel centres, so it reads evenly on an HDR swap chain
				if ( chip.filled )
					dl->AddRectFilled ( min, max, draw::color ( theme::colors::white ), rounding );
				else
					dl->AddRect ( ImVec2 ( std::floor ( min.x ) + 0.5f, std::floor ( min.y ) + 0.5f ), ImVec2 ( std::floor ( max.x ) - 0.5f, std::floor ( max.y ) - 0.5f ),
								  draw::color ( theme::colors::white ), rounding, edge );

				font::draw ( dl, ImVec2 ( x + padX, font::centredTop ( font::Face::semibold, size, pos.y, h ) ), font::Face::semibold, size,
							 draw::color ( chip.filled ? theme::colors::panel : theme::colors::white ), text );

				if ( chip.tooltip != nullptr )
					tooltip ( ImGui::GetID ( text.c_str () ), ImGui::IsMouseHoveringRect ( min, max ), chip.tooltip );

				x += w + theme::px ( theme::chipGap );
			}
			ImGui::PopID ();
		}
	}

	int headerTitleHeight ()
	{
		return int ( std::round ( font::lineHeight ( font::Face::semibold, theme::px ( theme::sizes::title ) ) ) );
	}

	void header ( const HeaderInfo& info, float width )
	{
		auto*		dl = ImGui::GetWindowDrawList ();
		const auto	pos = ImGui::GetCursorScreenPos ();
		const auto	titleSize = theme::px ( theme::sizes::title );
		const auto	bodySize = theme::px ( theme::sizes::body );
		const auto	titleH = float ( headerTitleHeight () );
		const auto	lineH = font::lineHeight ( font::Face::regular, bodySize );
		const auto	gap = theme::px ( theme::rowGap );
		const auto	chipsH = info.chips.empty () ? 0.0f : theme::px ( theme::keyHeight ) + gap;
		const auto	lineCount = info.lines[ 2 ] != nullptr ? 3 : 2;
		const auto	textH = titleH + gap + chipsH + lineH * float ( lineCount );
		const auto	hasCover = info.cover != nullptr && info.cover->valid ();

		// Without a cover the text block stands alone at the left
		auto	x = pos.x;
		auto	height = textH;

		if ( hasCover )
		{
			const auto	coverW = theme::px ( theme::coverWidth );
			height = theme::px ( theme::coverHeight );
			dl->AddImageRounded ( info.cover->ref (), pos, ImVec2 ( pos.x + coverW, pos.y + height ), ImVec2 ( 0.0f, 0.0f ), ImVec2 ( 1.0f, 1.0f ),
								  draw::color ( IM_COL32_WHITE ), theme::px ( theme::coverRounding ) );
			x += coverW + theme::px ( theme::coverGap );
		}

		// Title and status lines as one block, centred on the cover
		auto	y = pos.y + ( height - textH ) * 0.5f;

		// The image at its own pixel size when it was shrunk to the line, else stretched to it
		if ( info.titleImage != nullptr )
		{
			const auto&		t = *info.titleImage;
			const auto		k = titleH / float ( t.height );
			const ImVec2	at ( std::round ( x ), std::round ( y ) );
			dl->AddImage ( t.ref (), at, ImVec2 ( at.x + float ( t.width ) * k, at.y + titleH ), ImVec2 ( 0.0f, 0.0f ), ImVec2 ( 1.0f, 1.0f ),
						   draw::color ( theme::colors::bright ) );
		}
		else if ( info.title != nullptr )
			font::draw ( dl, ImVec2 ( x, y ), font::Face::semibold, titleSize, draw::color ( theme::colors::bright ), info.title );
		y += titleH + gap;

		if ( ! info.chips.empty () )
		{
			chips ( dl, ImVec2 ( x, y ), info.chips );
			y += chipsH;
		}

		// Tabular digits: counting frames and changing sizes do not shift the rest of the line
		for ( int i = 0; i < lineCount; ++i )
		{
			if ( info.lines[ i ] != nullptr )
				font::drawMarked ( dl, ImVec2 ( x, y ), font::Face::regular, bodySize, draw::color ( info.lineColors[ i ] ), info.lines[ i ] );
			y += lineH;
		}

		ImGui::Dummy ( ImVec2 ( width, height ) );
	}

	void header ( const ReShadeUiHeader& info, float width )
	{
		std::vector<Chip>	list;
		for ( uint32_t i = 0; info.chips != nullptr && i < info.chipCount; ++i )
			list.push_back ( { info.chips[ i ].text != nullptr ? info.chips[ i ].text : "", info.chips[ i ].filled != 0, info.chips[ i ].tooltip } );

		HeaderInfo	h;
		h.title = info.title;
		h.titleImage = static_cast<const Texture*> ( info.titleImage );
		h.chips = list;
		h.cover = static_cast<const Texture*> ( info.cover );
		for ( size_t i = 0; i < 3; ++i )
		{
			h.lines[ i ] = info.lines[ i ];
			h.lineColors[ i ] = info.lineColors[ i ];
		}
		header ( h, width );
	}
}
