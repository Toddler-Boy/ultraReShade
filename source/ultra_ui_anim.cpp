/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <unordered_map>

#include "ultra_ui_anim.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui::anim
{
	namespace
	{
		struct Swing
		{
			float	lead;
			float	tail;
			float	leadStart;
			float	tailStart;
			float	target;
			double	start;
		};

		struct Fade
		{
			float	value;
			double	last;
		};

		std::unordered_map<ImGuiID, Swing>	swings;
		std::unordered_map<ImGuiID, Fade>	fades;
	}

	float smoothstep ( float x )
	{
		x = std::clamp ( x, 0.0f, 1.0f );
		return x * x * ( 3.0f - 2.0f * x );
	}

	Edges swing ( ImGuiID id, float target )
	{
		const auto	now = ImGui::GetTime ();
		auto [ it, fresh ] = swings.try_emplace ( id, Swing { target, target, target, target, target, now } );
		auto&		s = it->second;

		if ( ! fresh && s.target != target )
		{
			s.leadStart = s.lead;
			s.tailStart = s.tail;
			s.target = target;
			s.start = now;
		}

		const auto	progress = float ( ( now - s.start ) / theme::swingSeconds );
		s.lead = s.leadStart + ( target - s.leadStart ) * smoothstep ( progress * ( 1.0f + theme::swingStretch ) );
		s.tail = s.tailStart + ( target - s.tailStart ) * smoothstep ( progress );

		return { std::min ( s.lead, s.tail ), std::max ( s.lead, s.tail ) };
	}

	float fade ( ImGuiID id, bool on, double seconds )
	{
		const auto	now = ImGui::GetTime ();
		auto [ it, fresh ] = fades.try_emplace ( id, Fade { on ? 1.0f : 0.0f, now } );
		auto&		f = it->second;

		// A widget out of view for a while comes back where it should be, not mid-fade
		const auto	step = float ( std::min ( now - f.last, seconds ) / seconds );
		f.value = std::clamp ( f.value + ( on ? step : -step ), 0.0f, 1.0f );
		f.last = now;

		return f.value;
	}
}
