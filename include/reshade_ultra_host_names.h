/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

// The names ultraReShade answers (reshade::get_host, the form of reshade_ultra_api.h) and the cells of their records: what the add-on
// tells it that it draws nothing of. Every record's first cell is the effect runtime (an api::effect_runtime*, as the add-on events
// give it); a call without a known runtime is refused

namespace reshade::host
{
	namespace name
	{
		// The add-on's own screenshot files still being written
		constexpr const char*	screenshot_pending = "screenshot.pending";
	}

	namespace cell
	{
		constexpr const char*	runtime = "runtime";

		// "screenshot.pending": how many of the add-on's own files (beside the one ReShade's writer saves) are still being written; the
		// "Saving screenshot" message stays up while there are any
		namespace screenshot_pending
		{
			constexpr const char*	count = "count";
		}
	}
}
