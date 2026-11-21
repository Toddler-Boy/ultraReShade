/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <reshade_ultra_ui.h>

#include "ultra_ui_format.hpp"
#include "ultra_ui_hotkey.hpp"
#include "ultra_ui_widgets.hpp"

//-----------------------------------------------------------------------------
// The C structs of reshade_ultra_ui.h as the widgets take them

namespace ultra::ui
{
	[[ nodiscard ]] inline Hotkey hotkeyOf ( const ReShadeUiHotkey* h )
	{
		return h == nullptr ? Hotkey {} : Hotkey { h->vk, ( h->modifiers & RESHADE_UI_MOD_SHIFT ) != 0, ( h->modifiers & RESHADE_UI_MOD_CTRL ) != 0, ( h->modifiers & RESHADE_UI_MOD_ALT ) != 0 };
	}

	[[ nodiscard ]] inline SliderRange rangeOf ( const ReShadeUiRange* r )
	{
		SliderRange	range;
		if ( r == nullptr )
			return range;
		range.lo = r->lo;
		range.hi = r->hi;
		range.step = r->step;
		range.def = r->def;
		range.mid = r->mid;
		range.hasMid = ( r->flags & RESHADE_UI_RANGE_HAS_MID ) != 0;
		range.isMidCentred = ( r->flags & RESHADE_UI_RANGE_MID_CENTRED ) != 0;
		range.log = ( r->flags & RESHADE_UI_RANGE_LOG ) != 0;
		return range;
	}

	[[ nodiscard ]] inline Format formatOf ( const ReShadeUiFormat& f )
	{
		return { f.scale, f.decimals, f.suffix != nullptr ? f.suffix : "", f.sign != 0, f.prefix != nullptr ? f.prefix : "", f.zero };
	}

	[[ nodiscard ]] inline Applies appliesOf ( uint32_t applies )
	{
		return applies == RESHADE_UI_APPLIES_NEXT_START ? Applies::nextStart : applies == RESHADE_UI_APPLIES_AFTER ? Applies::after : Applies::now;
	}
}
