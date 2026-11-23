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

		[[ nodiscard ]] Text textOf ( const ToggleLook& look )
		{
			if ( look.heading )
				return { font::Face::medium, theme::px ( theme::text::large ), theme::colors::text };

			return { look.isSub ? font::Face::semibold : font::Face::regular, theme::px ( theme::text::body ), labelColor ( look.applies ) };
		}

		// Text and its mark when it applies later, then the hotkey as keycaps a space and keyGap after it; the end x
		float drawText ( ImDrawList* dl, ImVec2 pos, const Text& t, const char* text, const Hotkey& hotkey, bool marked = false )
		{
			const auto	rowH = theme::px ( theme::heights::row );
			const auto	top = font::centredTop ( t.face, t.size, pos.y, rowH );
			auto		x = pos.x + font::draw ( dl, ImVec2 ( pos.x, top ), t.face, t.size, draw::color ( t.color ), text );
			if ( marked )
				x += font::draw ( dl, ImVec2 ( x, top ), t.face, t.size, draw::color ( t.color ), laterMark );

			if ( hotkey )
			{
				x += font::width ( t.face, t.size, " " ) + theme::px ( theme::spacing::xs );
				x += draw::keycaps ( dl, ImVec2 ( x, pos.y + ( rowH - theme::px ( theme::heights::row ) ) * 0.5f ), hotkey );
			}

			return x;
		}

		[[ nodiscard ]] float textWidth ( const Text& t, const char* text, const Hotkey& hotkey, bool marked = false )
		{
			auto	w = font::width ( t.face, t.size, text );
			if ( marked )
				w += font::width ( t.face, t.size, laterMark );
			if ( hotkey )
				w += font::width ( t.face, t.size, " " ) + theme::px ( theme::spacing::xs ) + draw::keycapsWidth ( hotkey );

			return w;
		}

		void drawInfo ( ImDrawList* dl, ImGuiID itemId, ImVec2 pos, float end, const char* text )
		{
			const auto	rowH = theme::px ( theme::heights::row );
			const auto	dot = theme::px ( theme::icons::icon );

			draw::infoDot ( dl, ImVec2 ( end + theme::px ( theme::spacing::xs ) + dot * 0.5f, pos.y + rowH * 0.5f ) );
			tooltip ( itemId, ImGui::IsMouseHoveringRect ( pos, ImVec2 ( end + theme::px ( theme::spacing::xs ) + dot, pos.y + rowH ) ), text );
		}
	}

	Edit toggle ( const char* id, bool& on, const char* text, const ToggleLook& look )
	{
		const auto	t = textOf ( look );
		const auto	switchW = theme::px ( theme::parts::switchWidth );
		const auto	switchH = theme::px ( theme::heights::row );
		const auto	gap = theme::px ( theme::spacing::m );
		const auto	rowH = theme::px ( theme::heights::row );
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

		dl->AddRectFilled ( sMin, sMax, draw::color ( draw::mix ( theme::shade ( theme::shades::edge ), theme::colors::accent, m * m ) ), switchH * 0.5f );

		const auto	knob = theme::px ( theme::icons::icon );
		const auto	inset = ( switchH - knob ) * 0.5f;
		const auto	travel = switchW - inset * 2.0f - knob;
		const ImVec2	kMin ( sMin.x + inset + travel * edges.lo, sMin.y + inset );
		const ImVec2	kMax ( kMin.x + knob + travel * ( edges.hi - edges.lo ), kMin.y + knob );

		dl->AddRectFilled ( kMin, kMax, draw::color ( draw::mix ( theme::shade ( theme::shades::muted ), theme::colors::text, m ) ), knob * 0.5f );

		const auto	end = drawText ( dl, ImVec2 ( sMax.x + gap, pos.y ), t, text, look.hotkey, look.applies != Applies::now );

		if ( look.tooltip != nullptr )
			drawInfo ( dl, itemId, pos, end, look.tooltip );

		return edit;
	}

	Edit subsection ( const char* id, bool& on, bool hasSwitch, const char* text, const ToggleLook& look )
	{
		auto	sub = look;
		sub.heading = false;
		sub.isSub = true;
		if ( hasSwitch )
			return toggle ( id, on, text, sub );

		const auto	t = textOf ( sub );
		const auto	pos = ImGui::GetCursorScreenPos ();
		const auto	end = drawText ( ImGui::GetWindowDrawList (), pos, t, text, {} );

		ImGui::Dummy ( ImVec2 ( textWidth ( t, text, {} ), theme::px ( theme::heights::row ) ) );
		if ( sub.tooltip != nullptr )
			drawInfo ( ImGui::GetWindowDrawList (), ImGui::GetID ( id ), pos, end, sub.tooltip );

		return {};
	}

	void heading ( const char* text, const Hotkey& hotkey )
	{
		const auto	t = textOf ( ToggleLook { true } );
		const auto	pos = ImGui::GetCursorScreenPos ();

		drawText ( ImGui::GetWindowDrawList (), pos, t, text, hotkey );
		ImGui::Dummy ( ImVec2 ( textWidth ( t, text, hotkey ), theme::px ( theme::heights::row ) ) );
	}
}
