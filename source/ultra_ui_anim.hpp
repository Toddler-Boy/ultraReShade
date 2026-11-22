/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "ultra_ui_theme.hpp"

//-----------------------------------------------------------------------------
// Animation state kept per ImGui id, advanced on wall time each frame the widget draws

namespace ultra::ui::anim
{
	// A position gliding to its target as ultra-shared's GUI_Toggle knob does: two edges on offset s-curves, the lead
	// landing first, so what spans them stretches while it moves. A new target mid-flight continues from where both are
	struct Edges
	{
		float	lo;
		float	hi;

		[[ nodiscard ]] float mid () const	{ return ( lo + hi ) * 0.5f; }
	};

	[[ nodiscard ]] Edges swing ( ImGuiID id, float target );

	// 0 to 1 while on holds, back to 0 once it stops
	[[ nodiscard ]] float fade ( ImGuiID id, bool on, double seconds = theme::motion::transition );

	[[ nodiscard ]] float smoothstep ( float x );
}
