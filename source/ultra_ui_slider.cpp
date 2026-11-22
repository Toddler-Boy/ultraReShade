/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cmath>

#include "ultra_ui_anim.hpp"
#include "ultra_ui_draw.hpp"
#include "ultra_ui_widgets.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	namespace
	{
		// The slider an Alt+click just reset: its drag is ignored until the button is released
		ImGuiID	altReset = 0;

		// A 1-pixel line on a pixel, not smeared over two
		[[ nodiscard ]] float crisp ( float x )	{ return std::floor ( x ) + 0.5f; }
	}

	float SliderRange::position ( float v ) const
	{
		v = std::clamp ( v, lo, hi );
		if ( isMidCentred )
			return v <= mid ? 0.5f * ( v - lo ) / ( mid - lo ) : 0.5f + 0.5f * ( v - mid ) / ( hi - mid );
		return log ? std::log ( v / lo ) / std::log ( hi / lo ) : ( v - lo ) / ( hi - lo );
	}

	float SliderRange::valueAt ( float position ) const
	{
		position = std::clamp ( position, 0.0f, 1.0f );
		if ( isMidCentred )
			return position <= 0.5f ? lo + ( mid - lo ) * 2.0f * position : mid + ( hi - mid ) * ( 2.0f * position - 1.0f );
		return log ? lo * std::pow ( hi / lo, position ) : lo + ( hi - lo ) * position;
	}

	// On the step's grid and cut to its decimals, so the settings file gets 0.4, not 0.39999998
	float snap ( float v, const SliderRange& range )
	{
		if ( range.step > 0.0f )
		{
			const auto	scale = std::pow ( 10.0, std::max ( 0.0, std::ceil ( -std::log10 ( double ( range.step ) ) - 1e-6 ) ) );
			const auto	onGrid = double ( range.lo ) + std::round ( ( double ( v ) - range.lo ) / range.step ) * range.step;
			v = float ( std::round ( onGrid * scale ) / scale );
		}

		return std::clamp ( v, range.lo, range.hi );
	}

	Edit slider ( const char* id, float& value, const SliderRange& range, float width )
	{
		const auto	rowH = theme::px ( theme::heights::row );
		const auto	backH = theme::px ( theme::spacing::xl );
		const auto	pad = theme::px ( theme::spacing::xs );
		const auto	pos = ImGui::GetCursorScreenPos ();
		const auto	itemId = ImGui::GetID ( id );
		Edit		edit;

		ImGui::InvisibleButton ( id, ImVec2 ( width, rowH ) );
		const auto	hovered = ImGui::IsItemHovered ();
		const auto	active = ImGui::IsItemActive ();

		if ( hovered || active )
			ImGui::SetMouseCursor ( ImGuiMouseCursor_Hand );

		// The value follows the pointer across the track, snapped to the step
		const ImVec2	bMin ( pos.x, pos.y + ( rowH - backH ) * 0.5f );
		const ImVec2	bMax ( pos.x + width, bMin.y + backH );
		const ImVec2	tMin ( bMin.x + pad, bMin.y + pad );
		const ImVec2	tMax ( bMax.x - pad, bMax.y - pad );
		const auto		trackW = tMax.x - tMin.x;

		if ( resetClicked () )
		{
			altReset = itemId;
			edit.changed = value != range.def;
			edit.committed = true;
			value = range.def;
		}
		else if ( active && altReset != itemId && trackW > 0.0f )
		{
			const auto	v = snap ( range.valueAt ( ( ImGui::GetMousePos ().x - tMin.x ) / trackW ), range );
			edit.changed = v != value;
			value = v;
		}

		if ( ImGui::IsItemDeactivated () )
		{
			edit.committed = altReset != itemId;
			altReset = 0;
		}

		auto*		dl = ImGui::GetWindowDrawList ();
		const auto	trackH = tMax.y - tMin.y;
		const auto	t = range.position ( value );

		dl->AddRectFilled ( bMin, bMax, draw::color ( theme::shade ( theme::shades::surface ) ), backH * 0.5f );
		dl->AddRectFilled ( tMin, tMax, draw::color ( theme::shade ( theme::shades::edge ) ), trackH * 0.5f );

		// Bipolar ranges fill from 0 outward, never thinner than a short rounded line at 0
		const auto	bipolar = range.lo < 0.0f && range.hi > 0.0f;
		const auto	zero = bipolar ? range.position ( 0.0f ) : 0.0f;
		auto		a = tMin.x + trackW * std::min ( zero, t );
		auto		b = tMin.x + trackW * std::max ( zero, t );

		if ( const auto minFill = theme::px ( theme::parts::bipolarMinFill ); bipolar && b - a < minFill )
		{
			const auto	c = tMin.x + trackW * zero;
			a = c - minFill * 0.5f;
			b = c + minFill * 0.5f;
		}

		if ( b > a )
			dl->AddRectFilled ( ImVec2 ( a, tMin.y ), ImVec2 ( b, tMax.y ), draw::color ( theme::colors::accent ), trackH * 0.5f );

		// The midpoint's ticks fill the padding above and below the track
		if ( range.hasMid )
		{
			const auto	x = crisp ( tMin.x + trackW * range.position ( range.mid ) );
			const auto	col = draw::color ( theme::shade ( theme::shades::strong ) );
			dl->AddLine ( ImVec2 ( x, bMin.y ), ImVec2 ( x, tMin.y ), col );
			dl->AddLine ( ImVec2 ( x, tMax.y ), ImVec2 ( x, bMax.y ), col );
		}

		// The handle grows in while hovered or dragged, a pixel past the fill on each side
		if ( const auto f = anim::smoothstep ( anim::fade ( itemId, hovered || active ) ); f > 0.0f )
		{
			const auto	r = ( trackH * 0.5f + theme::px ( theme::effects::sliderHandleGrow ) ) * ( 0.5f + 0.5f * f );
			const auto	edge = trackH * 0.5f;
			const auto	cx = tMin.x + std::clamp ( trackW * t, edge, trackW - edge );
			dl->AddCircleFilled ( ImVec2 ( cx, ( tMin.y + tMax.y ) * 0.5f ), r, draw::color ( theme::colors::text, f ) );
		}

		return edit;
	}
}
