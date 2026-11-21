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
		struct Text
		{
			font::Face	face;
			float		size;
			ImU32		color;
		};

		[[ nodiscard ]] Text textOf ( bool isHeading, Applies applies = Applies::now )
		{
			return isHeading ? Text { font::Face::medium, theme::px ( theme::sizes::heading ), theme::colors::bright }
							 : Text { font::Face::regular, theme::px ( theme::sizes::body ), labelColor ( applies ) };
		}

		// Text and its mark when it applies later, then the hotkey as keycaps a space and keyGap after it; the end x
		float drawText ( ImDrawList* dl, ImVec2 pos, const Text& t, const char* text, const Hotkey& hotkey, bool marked = false )
		{
			const auto	rowH = theme::px ( theme::rowHeight );
			const auto	top = font::centredTop ( t.face, t.size, pos.y, rowH );
			auto		x = pos.x + font::draw ( dl, ImVec2 ( pos.x, top ), t.face, t.size, draw::color ( t.color ), text );
			if ( marked )
				x += font::draw ( dl, ImVec2 ( x, top ), t.face, t.size, draw::color ( t.color ), laterMark );

			if ( hotkey )
			{
				x += font::width ( t.face, t.size, " " ) + theme::px ( theme::keyGap );
				x += draw::keycaps ( dl, ImVec2 ( x, pos.y + ( rowH - theme::px ( theme::keyHeight ) ) * 0.5f ), hotkey );
			}

			return x;
		}

		[[ nodiscard ]] float textWidth ( const Text& t, const char* text, const Hotkey& hotkey, bool marked = false )
		{
			auto	w = font::width ( t.face, t.size, text );
			if ( marked )
				w += font::width ( t.face, t.size, laterMark );
			if ( hotkey )
				w += font::width ( t.face, t.size, " " ) + theme::px ( theme::keyGap ) + draw::keycapsWidth ( hotkey );

			return w;
		}
	}

	Edit toggle ( const char* id, bool& on, const char* text, const ToggleLook& look )
	{
		const auto	t = textOf ( look.heading, look.applies );
		const auto	switchW = theme::px ( theme::switchWidth );
		const auto	switchH = theme::px ( theme::switchHeight );
		const auto	gap = theme::px ( theme::headingGap );
		const auto	rowH = theme::px ( theme::rowHeight );
		const auto	textW = textWidth ( t, text, look.hotkey, look.applies != Applies::now );
		const auto	pos = ImGui::GetCursorScreenPos ();
		const auto	itemId = ImGui::GetID ( id );
		Edit		edit;

		ImGui::InvisibleButton ( id, ImVec2 ( switchW + gap + textW, rowH ) );
		if ( ImGui::IsItemHovered () )
			ImGui::SetMouseCursor ( ImGuiMouseCursor_Hand );
		if ( resetClicked () )
		{
			edit.changed = on != look.def;
			edit.committed = edit.changed;
			on = look.def;
		}
		else if ( ImGui::IsItemClicked () )
		{
			on = ! on;
			edit.changed = true;
			edit.committed = true;
		}

		// The knob's two edges swing to the new side; between them it stretches into a pill
		auto*		dl = ImGui::GetWindowDrawList ();
		const auto	edges = anim::swing ( itemId, on ? 1.0f : 0.0f );
		const auto	m = edges.mid ();
		const ImVec2	sMin ( pos.x, pos.y + ( rowH - switchH ) * 0.5f );
		const ImVec2	sMax ( sMin.x + switchW, sMin.y + switchH );

		dl->AddRectFilled ( sMin, sMax, draw::color ( draw::mix ( theme::colors::switchOff, theme::colors::accent, m * m ) ), switchH * 0.5f );

		const auto	knob = theme::px ( theme::knobSize );
		const auto	inset = ( switchH - knob ) * 0.5f;
		const auto	travel = switchW - inset * 2.0f - knob;
		const ImVec2	kMin ( sMin.x + inset + travel * edges.lo, sMin.y + inset );
		const ImVec2	kMax ( kMin.x + knob + travel * ( edges.hi - edges.lo ), kMin.y + knob );

		dl->AddRectFilled ( kMin, kMax, draw::color ( draw::mix ( theme::colors::knobOff, theme::colors::knobOn, m ) ), knob * 0.5f );

		const auto	end = drawText ( dl, ImVec2 ( sMax.x + gap, pos.y ), t, text, look.hotkey, look.applies != Applies::now );

		if ( look.tooltip != nullptr )
		{
			const auto	dot = theme::px ( theme::infoSize );
			draw::infoDot ( dl, ImVec2 ( end + theme::px ( theme::infoGap ) + dot * 0.5f, pos.y + rowH * 0.5f ) );
			tooltip ( itemId, ImGui::IsMouseHoveringRect ( pos, ImVec2 ( end + theme::px ( theme::infoGap ) + dot, pos.y + rowH ) ), look.tooltip );
		}

		return edit;
	}

	void heading ( const char* text, const Hotkey& hotkey )
	{
		const auto	t = textOf ( true );
		const auto	pos = ImGui::GetCursorScreenPos ();

		drawText ( ImGui::GetWindowDrawList (), pos, t, text, hotkey );
		ImGui::Dummy ( ImVec2 ( textWidth ( t, text, hotkey ), theme::px ( theme::rowHeight ) ) );
	}
}
