/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

//-----------------------------------------------------------------------------
// The Shaders tab: every shader of the user's as a section of the page engine (ultra_ui_layout.hpp), built from the runtime's effects and
// techniques again whenever they change

namespace reshade
{
	class runtime;

	namespace ultra
	{
		// Draws the tab's content in the area the window gave it
		void drawShaders ( runtime& runtime );

		// The runtime goes
		void forgetShaders ( runtime* runtime );
	}
}
