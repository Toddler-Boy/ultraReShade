/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <span>

//-----------------------------------------------------------------------------
// A key with its modifiers, held to exactly: F6 does not fire while Shift is down, Shift+F6 does. Drawn as one keycap per key

namespace ultra::ui
{
	struct Hotkey
	{
		int		vk = 0;
		bool	shift = false;
		bool	ctrl = false;
		bool	alt = false;

		[[ nodiscard ]] explicit operator bool () const	{ return vk != 0; }
	};

	// The keycaps' labels in order, modifiers first; how many
	int keyNames ( const Hotkey& hotkey, std::span<const char*, 4> out );
}
