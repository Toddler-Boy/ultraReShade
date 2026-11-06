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
}
