/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "reshade_api_resource.hpp"

struct ImDrawList;

namespace reshade::api
{
	struct device;
}

// Top-left corner: the start logo once per game start, then a progress ring while effects compile and the
// overlay is closed.
namespace reshade::ultra
{
	struct splash_state
	{
		api::resource logo_texture = {};
		api::resource_view logo_view = {};
		float logo_width = 0.0f;
		float logo_height = 0.0f;
		float ring_alpha = 0.0f;
		float ring_progress = 0.0f;
	};

	struct splash_frame
	{
		api::device *device;
		ImDrawList *draw_list;
		float width;
		float height;
		float delta_time;
		size_t effect_count;
		size_t remaining_effects;
		bool is_loading;
		bool is_overlay_open;
	};

	[[nodiscard]] bool wants_splash_frame(const splash_state &state, bool is_loading);
	void draw_splash(splash_state &state, const splash_frame &frame);
	void destroy_splash(splash_state &state, api::device *device);

	[[nodiscard]] float compile_progress(size_t effect_count, size_t remaining_effects);
	void draw_window_progress_ring(float progress);
}
