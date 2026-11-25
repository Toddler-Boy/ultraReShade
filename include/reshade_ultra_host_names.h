/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

// The names ultraReShade answers (reshade::get_host, the form of reshade_ultra_api.h) and the cells of their records: what the add-on
// tells it about what it does that ReShade cannot know. Every record's first cell is the effect runtime (an api::effect_runtime*, as the add-on events
// give it); a call without a known runtime is refused. The three float_ names are about the game's textures, whatever runtime: no runtime cell

#include <stdint.h>

namespace reshade::host
{
	// The bits of "targets.float"'s formats
	namespace float_format
	{
		// RGBA8 typeless, unorm and unorm sRGB
		constexpr uint32_t	rgba8 = 1;
		// RGB10A2 unorm
		constexpr uint32_t	rgb10a2 = 2;
	}

	namespace name
	{
		// The rule for the game's float render targets: which textures get a float clone and which pixel shaders' draws make it
		constexpr const char*	float_targets = "targets.float";
		// Pauses the float clones: the game's own resources are used again, as the game calls
		constexpr const char*	float_paused = "targets.float_paused";
		// The game's texture a float clone stands for
		constexpr const char*	float_source = "targets.float_source";
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

		// "targets.float": the game's 2D textures made in these formats (formats, bits of float_format) are tagged as they are made, left as
		// they are. The first time one is a render target of a draw whose pixel shader's CRC-32 (zlib) is one of the shaders (a pointer
		// to shader_count of them), it gets an RGBA16F clone, and every use of it from then on goes to the clone
		namespace float_targets
		{
			constexpr const char*	formats = "formats";
			constexpr const char*	shaders = "shaders";
			constexpr const char*	shader_count = "shader_count";
		}

		// "targets.float_paused": paused 1 = no use of a resource goes to a clone and no clone is made (textures are still tagged, the
		// clones made stay and are used again by 0). Set between frames
		namespace float_paused
		{
			constexpr const char*	paused = "paused";
		}

		// "targets.float_source": for a resource handle (the clone's, as the add-on events give it) the game's texture it stands for and the
		// format that was made as (an api::format); source 0 for a resource that is no clone
		namespace float_source
		{
			constexpr const char*	resource = "resource";
			constexpr const char*	source = "source";
			constexpr const char*	format = "format";
		}

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
