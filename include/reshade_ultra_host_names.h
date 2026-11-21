/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

// The names ultraReShade answers (reshade::get_host, the form of reshade_ultra_api.h) and the cells of their records: what the add-on
// that draws the overlay needs of the window it is drawn in, the screenshot and settings state, and the effects behind the Shaders tab.
// Every record's first cell is the effect runtime (an api::effect_runtime*, as the add-on events give it); a call without a known
// runtime is refused. Texts the host hands out are valid until the next call of the same name on that thread.
//
// event addon_event::ultra_overlay (reshade_events.hpp) comes once per ImGui frame; frames run while the overlay is open, "overlay.wanted"
// is on, or the start logo wants one.

namespace reshade::host
{
	namespace name
	{
		// The window: where it is, the add-on moves it, the selected tab, frames while it is closed
		constexpr const char*	overlay_state = "overlay.state";
		constexpr const char*	overlay_place = "overlay.place";
		constexpr const char*	overlay_tab = "overlay.tab";
		constexpr const char*	overlay_wanted = "overlay.wanted";

		// The messages: the screenshot being saved, the settings that could not be saved
		constexpr const char*	screenshot_state = "screenshot.state";
		constexpr const char*	settings_state = "settings.state";

		// The add-on's own screenshot files still being written
		constexpr const char*	screenshot_pending = "screenshot.pending";

		// The effects: what the public effect API does not say, and what it does not save
		constexpr const char*	effects_state = "effects.state";
		constexpr const char*	effects_technique = "effects.technique";
		constexpr const char*	effects_uniform = "effects.uniform";
		constexpr const char*	effects_failed = "effects.failed";
		constexpr const char*	effects_save = "effects.save";
		constexpr const char*	effects_definition = "effects.definition";
		constexpr const char*	effects_definition_set = "effects.definition.set";
	}

	namespace cell
	{
		constexpr const char*	runtime = "runtime";

		// "overlay.state": the window in back buffer pixels, the back buffer's size, the selected tab's name
		namespace overlay_state
		{
			constexpr const char*	open = "open";
			constexpr const char*	x = "x";
			constexpr const char*	y = "y";
			constexpr const char*	width = "width";
			constexpr const char*	height = "height";
			constexpr const char*	display_width = "displayWidth";
			constexpr const char*	display_height = "displayHeight";
			constexpr const char*	tab = "tab";
		}

		// "overlay.place": the window moved or resized, the rect clamped to the back buffer; done: the edit is finished, the rect is saved
		namespace overlay_place
		{
			constexpr const char*	x = "x";
			constexpr const char*	y = "y";
			constexpr const char*	width = "width";
			constexpr const char*	height = "height";
			constexpr const char*	done = "done";
		}

		namespace overlay_tab
		{
			constexpr const char*	name = "name";
		}

		namespace overlay_wanted
		{
			constexpr const char*	on = "on";
		}

		// "screenshot.state": saving counts the files of ReShade's own writer; failure is 0 none, 1 internal error, 2 folder not created
		namespace screenshot_state
		{
			constexpr const char*	saving = "saving";
			constexpr const char*	file = "file";
			constexpr const char*	failure = "failure";
			constexpr const char*	path = "path";
			constexpr const char*	failed_ms = "failedMs";
		}

		// "screenshot.pending": how many of the add-on's own files (beside the one ReShade's writer saves) are still being written; the
		// "Saving screenshot" message stays up while there are any
		namespace screenshot_pending
		{
			constexpr const char*	count = "count";
		}

		namespace settings_state
		{
			constexpr const char*	save_failed = "saveFailed";
			constexpr const char*	path = "path";
		}

		// "effects.state": generation changes whenever the effects or their techniques do; nothing of them is read while is_loading
		namespace effects_state
		{
			constexpr const char*	is_loading = "isLoading";
			constexpr const char*	total = "total";
			constexpr const char*	remaining = "remaining";
			constexpr const char*	generation = "generation";
			constexpr const char*	has_user_shaders = "hasUserShaders";
			constexpr const char*	has_failures = "hasFailures";
		}

		namespace effects_technique
		{
			constexpr const char*	technique = "technique";
			constexpr const char*	is_route = "isRoute";
		}

		// "effects.uniform": the uniform's default values as 32-bit words into buffer (capacity of them), count of them written
		namespace effects_uniform
		{
			constexpr const char*	variable = "variable";
			constexpr const char*	buffer = "buffer";
			constexpr const char*	capacity = "capacity";
			constexpr const char*	is_special = "isSpecial";
			constexpr const char*	is_default = "isDefault";
			constexpr const char*	count = "count";
		}

		// "effects.failed": the index'th effect file that did not compile, and how many there are
		namespace effects_failed
		{
			constexpr const char*	index = "index";
			constexpr const char*	count = "count";
			constexpr const char*	file = "file";
			constexpr const char*	errors = "errors";
		}

		// "effects.definition": the index'th preprocessor definition of the effect file; hint is the effect's own default, value what is in
		// effect now, is_own that the user set it
		namespace effects_definition
		{
			constexpr const char*	file = "file";
			constexpr const char*	index = "index";
			constexpr const char*	count = "count";
			constexpr const char*	name = "name";
			constexpr const char*	hint = "hint";
			constexpr const char*	value = "value";
			constexpr const char*	is_own = "isOwn";
		}

		// "effects.definition.set": the effect file loads again with the definition set (empty value: the effect's own; empty name: every
		// definition of the file); reverted: it did not compile with it and loads with the default
		namespace effects_definition_set
		{
			constexpr const char*	file = "file";
			constexpr const char*	name = "name";
			constexpr const char*	value = "value";
			constexpr const char*	reverted = "reverted";
		}
	}
}
