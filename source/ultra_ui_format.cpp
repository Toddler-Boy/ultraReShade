/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <cmath>
#include <cstdio>

#include "ultra_ui_format.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	void format ( char* out, size_t size, float value, const Format& f )
	{
		// Rounded first, so a value that shows as 0 shows without a sign
		const auto	unit = std::pow ( 10.0f, float ( f.decimals ) );
		const auto	shown = std::round ( value * f.scale * unit ) / unit;
		if ( shown == 0.0f && f.zero != nullptr )
		{
			std::snprintf ( out, size, "%s", f.zero );
			return;
		}
		const auto*	sign = shown < 0.0f ? "-" : shown > 0.0f && f.sign ? "+" : "";

		std::snprintf ( out, size, "%s%s%.*f%s", f.prefix, sign, f.decimals, double ( std::abs ( shown ) ), f.suffix );
	}
}
