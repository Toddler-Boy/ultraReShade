/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <unordered_map>

#include "ultra_ui_draw.hpp"
#include "ultra_ui_font.hpp"
#include "ultra_ui_widgets.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	namespace
	{
		constexpr float	pressedScale = 0.96f;

		// Per button, by its key: a hotkey fires outside the panel's window, where the button's ImGui id is not known
		struct State
		{
			double	doneAt = -1.0e9;
			float	width = 0.0f;		// the widest it has been, so a label that changes never shrinks it
		};

		std::unordered_map<const void*, State>	states;

		[[ nodiscard ]] float keysWidth ( const Hotkey& hotkey )
		{
			return hotkey ? theme::px ( theme::keyGap ) + draw::keycapsWidth ( hotkey ) : 0.0f;
		}
	}

	float buttonWidth ( const void* key, const char* text, const char* done, const Hotkey& hotkey )
	{
		const auto	size = theme::px ( theme::sizes::choice );
		auto		w = font::width ( font::Face::semibold, size, text );
		if ( done != nullptr )
			w = std::max ( w, font::width ( font::Face::semibold, size, done ) );

		auto&	state = states[ key ];
		state.width = std::max ( state.width, w + keysWidth ( hotkey ) + theme::px ( theme::buttonPadX ) * 2.0f );
		return state.width;
	}

	void flash ( const void* key )
	{
		states[ key ].doneAt = ImGui::GetTime ();
	}

	bool button ( const void* key, const char* text, bool primary, const char* done, const Hotkey& hotkey, float progress )
	{
		const auto	w = buttonWidth ( key, text, done, hotkey );
		const auto	h = theme::px ( theme::rowHeight );
		const auto	pos = ImGui::GetCursorScreenPos ();

		ImGui::PushID ( key );
		const auto	clicked = ImGui::InvisibleButton ( "button", ImVec2 ( w, h ) );
		ImGui::PopID ();

		const auto	running = progress >= 0.0f;
		const auto	hovered = ImGui::IsItemHovered () && ! running;

		if ( hovered )
			ImGui::SetMouseCursor ( ImGuiMouseCursor_Hand );

		// Pressed, it shrinks a little about its centre
		const auto	k = ImGui::IsItemActive () && ! running ? pressedScale : 1.0f;
		const ImVec2	c ( pos.x + w * 0.5f, pos.y + h * 0.5f );
		const ImVec2	min ( c.x - w * 0.5f * k, c.y - h * 0.5f * k );
		const ImVec2	max ( c.x + w * 0.5f * k, c.y + h * 0.5f * k );
		const auto		rounding = ( max.y - min.y ) * 0.5f;
		auto*			dl = ImGui::GetWindowDrawList ();

		// Running, the plain button fills with the accent as far as the action got
		if ( running )
		{
			dl->AddRectFilled ( min, max, draw::color ( theme::colors::button ), rounding );
			dl->PushClipRect ( min, ImVec2 ( min.x + ( max.x - min.x ) * std::min ( progress, 1.0f ), max.y ), true );
			dl->AddRectFilled ( min, max, draw::color ( theme::colors::accent ), rounding );
			dl->PopClipRect ();
		}
		else
		{
			const auto	back = primary ? ( hovered ? theme::colors::accentHover : theme::colors::accent ) : ( hovered ? theme::colors::buttonHover : theme::colors::button );
			dl->AddRectFilled ( min, max, draw::color ( back ), rounding );
		}

		// Label and keycaps centred as one in the button
		const auto	showDone = done != nullptr && ImGui::GetTime () - states[ key ].doneAt < theme::doneSeconds;
		const auto*	shown = showDone ? done : text;
		const auto	size = theme::px ( theme::sizes::choice ) * k;
		const auto	inner = ( w - theme::px ( theme::buttonPadX ) * 2.0f ) * k;
		const auto	keysW = keysWidth ( hotkey ) * k;
		const auto	textW = font::width ( font::Face::semibold, size, shown );
		const auto	left = c.x - inner * 0.5f;

		font::draw ( dl, ImVec2 ( left + ( inner - keysW - textW ) * 0.5f, font::centredTop ( font::Face::semibold, size, min.y, max.y - min.y ) ), font::Face::semibold, size,
					 draw::color ( primary || running ? theme::colors::white : theme::colors::text ), shown );

		if ( hotkey )
			draw::keycaps ( dl, ImVec2 ( left + inner - keysW + theme::px ( theme::keyGap ) * k, c.y - theme::px ( theme::keyHeight ) * 0.5f ), hotkey );

		return clicked;
	}
}
