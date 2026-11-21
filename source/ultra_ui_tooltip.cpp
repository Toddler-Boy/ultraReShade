/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ultra_ui_draw.hpp"
#include "ultra_ui_font.hpp"
#include "ultra_ui_widgets.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	namespace
	{
		std::unordered_map<ImGuiID, double>	hoverSince;

		// Words onto lines no wider than maxWidth; a line break in the text starts a new line
		[[ nodiscard ]] std::vector<std::string> wrap ( std::string_view text, float size, float maxWidth )
		{
			std::vector<std::string>	lines;
			std::string					line;

			while ( ! text.empty () )
			{
				const auto	end = text.find_first_of ( " \n" );
				const auto	breaks = end != std::string_view::npos && text[ end ] == '\n';
				const auto	word = text.substr ( 0, end );
				text = end == std::string_view::npos ? std::string_view () : text.substr ( end + 1 );

				auto	candidate = line.empty () ? std::string ( word ) : line + ' ' + std::string ( word );
				if ( ! line.empty () && font::width ( font::Face::regular, size, candidate ) > maxWidth )
				{
					lines.push_back ( std::move ( line ) );
					candidate = word;
				}
				line = std::move ( candidate );

				if ( breaks )
					lines.push_back ( std::move ( line ) ), line.clear ();
			}

			if ( ! line.empty () )
				lines.push_back ( std::move ( line ) );

			return lines;
		}
	}

	void tooltip ( ImGuiID id, bool hovered, const char* text )
	{
		const auto	now = ImGui::GetTime ();

		if ( ! hovered )
		{
			hoverSince.erase ( id );
			return;
		}

		if ( now - hoverSince.try_emplace ( id, now ).first->second < theme::tipDelay )
			return;

		const auto	size = theme::px ( theme::sizes::tip );
		const auto	padX = theme::px ( theme::tipPadX );
		const auto	padY = theme::px ( theme::tipPadY );
		const auto	lineH = font::lineHeight ( font::Face::regular, size );
		const auto	lines = wrap ( text, size, theme::px ( theme::tipMaxWidth ) - padX * 2.0f );

		float	textW = 0.0f;
		for ( const auto& line : lines )
			textW = std::max ( textW, font::width ( font::Face::regular, size, line ) );

		const ImVec2	box ( textW + padX * 2.0f, lineH * float ( lines.size () ) + padY * 2.0f );
		const auto		mouse = ImGui::GetMousePos ();

		ImGui::SetNextWindowPos ( draw::onScreen ( ImVec2 ( mouse.x, mouse.y + theme::px ( theme::rowHeight ) * 0.5f + theme::px ( theme::tipGap ) ), box ) );
		// Full strength over a dimmed row too: it explains why that row is off
		ImGui::PushStyleVar ( ImGuiStyleVar_Alpha, 1.0f );
		ImGui::PushStyleVar ( ImGuiStyleVar_WindowPadding, ImVec2 ( 0.0f, 0.0f ) );
		ImGui::PushStyleVar ( ImGuiStyleVar_WindowBorderSize, 0.0f );
		ImGui::PushStyleVar ( ImGuiStyleVar_PopupBorderSize, 0.0f );
		ImGui::PushStyleColor ( ImGuiCol_PopupBg, IM_COL32 ( 0, 0, 0, 0 ) );

		if ( ImGui::BeginTooltip () )
		{
			auto*		dl = ImGui::GetWindowDrawList ();
			const auto	min = ImGui::GetCursorScreenPos ();
			const ImVec2	max ( min.x + box.x, min.y + box.y );
			const auto	rounding = theme::px ( theme::menuRounding );

			// The outline opaque, on pixel centres: alpha blends in linear light on an HDR swap chain, where a faint white
			// reads several times stronger and its anti-aliased edge unevenly
			draw::shadow ( dl, min, max, rounding );
			dl->AddRectFilled ( min, max, draw::color ( theme::colors::popup ), rounding );
			const ImVec2	edgeMin ( std::floor ( min.x ) + 0.5f, std::floor ( min.y ) + 0.5f );
			const ImVec2	edgeMax ( std::floor ( max.x ) - 0.5f, std::floor ( max.y ) - 0.5f );
			dl->AddRect ( edgeMin, edgeMax, draw::color ( draw::mix ( theme::colors::popup, theme::colors::white, theme::colors::tipEdge ) ), rounding );

			auto	y = min.y + padY;
			for ( const auto& line : lines )
			{
				font::draw ( dl, ImVec2 ( min.x + padX, y ), font::Face::regular, size, draw::color ( theme::colors::text ), line );
				y += lineH;
			}

			ImGui::Dummy ( box );
			ImGui::EndTooltip ();
		}

		ImGui::PopStyleColor ();
		ImGui::PopStyleVar ( 4 );
	}
}
