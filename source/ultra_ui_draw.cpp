/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <array>
#include <cmath>
#include <numbers>

#include "ultra_ui_draw.hpp"
#include "ultra_ui_font.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui::draw
{
	ImU32 color ( ImU32 c, float alpha )
	{
		return ImGui::GetColorU32 ( c, alpha );
	}

	ImU32 mix ( ImU32 a, ImU32 b, float t )
	{
		const auto	channel = [ & ] ( int shift )
		{
			const auto	ca = float ( ( a >> shift ) & 0xFF );
			const auto	cb = float ( ( b >> shift ) & 0xFF );
			return ImU32 ( ca + ( cb - ca ) * t + 0.5f ) << shift;
		};
		return channel ( IM_COL32_R_SHIFT ) | channel ( IM_COL32_G_SHIFT ) | channel ( IM_COL32_B_SHIFT ) | channel ( IM_COL32_A_SHIFT );
	}

	void shadow ( ImDrawList* drawList, ImVec2 min, ImVec2 max, float rounding, float alpha )
	{
		// Rings from wide and faint to tight, stacking to a soft falloff; ReShade's ImGui has no shadow primitive
		constexpr int	rings = 6;
		const auto		spread = theme::px ( theme::shadowSpread );
		const auto		offset = theme::px ( theme::shadowOffset );

		drawList->PushClipRectFullScreen ();
		for ( int i = rings; i >= 1; --i )
		{
			const auto	grow = spread * float ( i ) / float ( rings );
			const auto	a = alpha * theme::shadowAlpha / float ( rings );
			drawList->AddRectFilled ( ImVec2 ( min.x - grow, min.y - grow + offset ), ImVec2 ( max.x + grow, max.y + grow + offset ),
									  color ( IM_COL32_BLACK, a ), rounding + grow );
		}
		drawList->PopClipRect ();
	}

	namespace
	{
		[[ nodiscard ]] float keycapWidth ( const char* key )
		{
			return font::width ( font::Face::semibold, theme::px ( theme::sizes::key ), key ) + theme::px ( theme::keyPadX ) * 2.0f;
		}

		float keycap ( ImDrawList* drawList, ImVec2 pos, const char* key )
		{
			const auto	size = theme::px ( theme::sizes::key );
			const auto	w = keycapWidth ( key );
			const auto	h = theme::px ( theme::keyHeight );
			const auto	face = h - theme::px ( theme::keyEdge );
			const auto	rounding = theme::px ( theme::keyRounding );

			drawList->AddRectFilled ( pos, ImVec2 ( pos.x + w, pos.y + h ), color ( theme::colors::keycapEdge ), rounding );
			drawList->AddRectFilled ( pos, ImVec2 ( pos.x + w, pos.y + face ), color ( theme::colors::keycap ), rounding );

			// Centred in the lighter top face, not the whole key
			const auto	textW = font::width ( font::Face::semibold, size, key );
			font::draw ( drawList, ImVec2 ( pos.x + ( w - textW ) * 0.5f, font::centredTop ( font::Face::semibold, size, pos.y, face ) ),
						 font::Face::semibold, size, color ( theme::colors::white ), key );
			return w;
		}
	}

	float keycapsWidth ( const Hotkey& hotkey )
	{
		std::array<const char*, 4>	names {};
		const auto					n = keyNames ( hotkey, names );
		float						w = 0.0f;

		for ( int i = 0; i < n; ++i )
			w += keycapWidth ( names[ size_t ( i ) ] ) + ( i > 0 ? theme::px ( theme::keycapGap ) : 0.0f );

		return w;
	}

	float keycaps ( ImDrawList* drawList, ImVec2 pos, const Hotkey& hotkey )
	{
		std::array<const char*, 4>	names {};
		const auto					n = keyNames ( hotkey, names );
		auto						x = pos.x;

		for ( int i = 0; i < n; ++i )
		{
			if ( i > 0 )
				x += theme::px ( theme::keycapGap );
			x += keycap ( drawList, ImVec2 ( x, pos.y ), names[ size_t ( i ) ] );
		}

		return x - pos.x;
	}

	void infoDot ( ImDrawList* drawList, ImVec2 centre )
	{
		const auto	r = theme::px ( theme::infoSize ) * 0.5f;
		const auto	size = theme::px ( theme::sizes::info );

		drawList->AddCircleFilled ( centre, r, color ( theme::colors::infoDot ) );

		const auto	w = font::width ( font::Face::bold, size, "i" );
		font::draw ( drawList, ImVec2 ( centre.x - w * 0.5f, font::centredTop ( font::Face::bold, size, centre.y - r, r * 2.0f ) ),
					 font::Face::bold, size, color ( theme::colors::infoGlyph ), "i" );
	}

	void resetIcon ( ImDrawList* drawList, ImVec2 centre, float size, ImU32 col )
	{
		constexpr auto	pi = std::numbers::pi_v<float>;
		const auto		r = size * 0.32f;
		const auto		thickness = size * 0.11f;
		const auto		head = size * 0.2f;

		// An arc from the left, over the top, round to the lower right; the arrowhead at its left end points down
		drawList->PathArcTo ( centre, r, pi, pi * 2.25f );
		drawList->PathStroke ( col, thickness );

		const ImVec2	tip ( centre.x - r, centre.y + head * 0.7f );
		drawList->AddTriangleFilled ( ImVec2 ( tip.x - head * 0.6f, centre.y - head * 0.1f ), ImVec2 ( tip.x + head * 0.6f, centre.y - head * 0.1f ), tip, col );
	}

	void chevron ( ImDrawList* drawList, ImVec2 centre, float size, ImU32 col, float open )
	{
		const auto	w = size * 0.32f;
		const auto	h = size * 0.17f * ( 1.0f - 2.0f * open );
		const ImVec2	points[] = { ImVec2 ( centre.x - w, centre.y - h ), ImVec2 ( centre.x, centre.y + h ), ImVec2 ( centre.x + w, centre.y - h ) };

		drawList->AddPolyline ( points, 3, col, size * 0.1f );
	}

	void disclosure ( ImDrawList* drawList, ImVec2 centre, float size, float radius, ImU32 col, float open )
	{
		const auto	angle = open * std::numbers::pi_v<float> * 0.5f;
		const auto	c = std::cos ( angle );
		const auto	sn = std::sin ( angle );
		const auto	at = [ & ] ( float x, float y ) { return ImVec2 ( centre.x + x * c - y * sn, centre.y + x * sn + y * c ); };
		const auto	r = size * 0.5f - radius;
		const ImVec2	points[] { at ( r * 0.6f, 0.0f ), at ( -r * 0.4f, -r * 0.6f ), at ( -r * 0.4f, r * 0.6f ) };

		drawList->AddTriangleFilled ( points[ 0 ], points[ 1 ], points[ 2 ], col );
		if ( radius <= 0.0f )
			return;
		drawList->AddPolyline ( points, 3, col, radius * 2.0f, ImDrawFlags_Closed );
		for ( const auto& p : points )
			drawList->AddCircleFilled ( p, radius, col );
	}
}
