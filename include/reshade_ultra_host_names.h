/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

// The names ultraReShade answers (reshade::get_host, the form of reshade_ultra_api.h) and the cells of their records: what the add-on
// tells it about what it does that ReShade cannot know. Every record's first cell is the effect runtime (an api::effect_runtime*, as the add-on events
// give it); a call without a known runtime is refused

namespace reshade::host
{
	namespace name
	{
		// The add-on's own screenshot files still being written
		constexpr const char*	screenshot_pending = "screenshot.pending";
		// How the targets the add-on renders effects onto, other than the back buffer, are encoded
		constexpr const char*	effects_target = "effects.target";
		// Generic Depth reads its [DEPTH] keys of the game's config again
		constexpr const char*	depth_reread = "depth.reread";
		// What Generic Depth saw at the runtime's last effects pass
		constexpr const char*	depth_seen = "depth.seen";
		// The size of one depth buffer Generic Depth saw
		constexpr const char*	depth_candidate = "depth.candidate";
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

		// "effects.target": api::color_space's number (1 sRGB, 2 scRGB, 3 HDR10 PQ, 4 HLG, 0 not stated) the effects built for those
		// targets get as BUFFER_COLOR_SPACE, from the next render on; a number above 4 is refused
		namespace effects_target
		{
			constexpr const char*	space = "space";
		}

		// "depth.reread" has no cell but the runtime. "depth.seen" answers: clears = the clears of the selected depth buffer
		// (counted while DepthCopyBeforeClears is on), candidates = how many depth buffer sizes came into question
		namespace depth_seen
		{
			constexpr const char*	clears = "clears";
			constexpr const char*	candidates = "candidates";
		}

		// "depth.candidate": for index (0 up to candidates - 1, the largest first) it answers the size and 1; past the last it answers 0
		namespace depth_candidate
		{
			constexpr const char*	index = "index";
			constexpr const char*	width = "width";
			constexpr const char*	height = "height";
		}
	}
}
