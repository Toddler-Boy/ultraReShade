/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstddef>

//-----------------------------------------------------------------------------
// How a slider shows its value: value times scale, so many decimals, between prefix and suffix; signed shows + on
// positive values; zero, when set, is the text for a value that shows as 0 (a slider whose 0 means off)

namespace ultra::ui
{
	struct Format
	{
		float		scale = 1.0f;
		int			decimals = 0;
		const char*	suffix = "";
		bool		sign = false;
		const char*	prefix = "";
		const char*	zero = nullptr;
	};

	inline constexpr Format	percent { 100.0f, 0, "%" };
	inline constexpr Format	signedPercent { 100.0f, 0, "%", true };
	inline constexpr Format	integer {};
	inline constexpr Format	signedInteger { 1.0f, 0, "", true };
	inline constexpr Format	nits { 1.0f, 0, " nits" };
	inline constexpr Format	stopsPerSecond { 1.0f, 1, " st/s" };

	void format ( char* out, size_t size, float value, const Format& f );
}
