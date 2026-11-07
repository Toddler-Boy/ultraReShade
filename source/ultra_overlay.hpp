/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

//-----------------------------------------------------------------------------

namespace reshade::ultra
{
	// Before an add-on window's Begin: one that has never been shown starts as a tab next to Home
	void dockNewAddonWindow ();

	// Between Begin and End of ultraGame's window: once per run its tab moves to the front and is selected
	void placeUltraGameTab ( bool& isPlaced );

	// After all overlay windows: dragging a docked node by its empty tab bar undocks the whole node again
	void undockDraggedNode ();

	struct DisplayScale
	{
		float	monitorScale = 1.0f;	// the effective DPI of the window's monitor / 96 (200% = 2)
		float	pixelSize = 1.0f;		// back buffer height / the window's height on screen
		float	scale = 1.0f;			// both, in quarter steps
	};

	// The overlay's font scale, measured like ultraGame's panel: from the display the game window is on, never from a
	// config. Without a window (VR) the back buffer height alone decides, as upstream ReShade did
	[[ nodiscard ]] DisplayScale measureDisplayScale ( void* window, float backBufferHeight );
}
