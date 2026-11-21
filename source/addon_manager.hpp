/*
 * Copyright (C) 2021 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "addon.hpp"
#include "reshade_events.hpp"
#include "ultra_addon_event.hpp"
#include "ultra_process_exit.hpp"

namespace reshade
{
	/// <summary>
	/// List of add-on event callbacks.
	/// </summary>
	extern std::vector<void *> addon_event_list[];

	/// <summary>
	/// List of currently loaded add-ons.
	/// </summary>
	extern std::vector<addon_info> addon_loaded_info;

	/// <summary>
	/// Pointer to the add-on that is currently executing.
	/// </summary>
	extern thread_local const addon_info *addon_current;

	/// <summary>
	/// Loads any add-ons found in the configured search paths.
	/// </summary>
	void load_addons();

	/// <summary>
	/// Unloads any add-ons previously loaded via <see cref="load_addons"/>.
	/// </summary>
	void unload_addons();

	/// <summary>
	/// Gets the add-on that was loaded at the specified address.
	/// </summary>
	addon_info *find_addon(const void *address);

	/// <summary>
	/// Checks whether any callbacks were registered for the specified <paramref name="ev"/>ent.
	/// </summary>
	template <addon_event ev>
	bool has_addon_event()
	{
		return !addon_event_list[static_cast<uint32_t>(ev)].empty();
	}

	/// <summary>
	/// Calls all registered callbacks for the specified <typeparamref name="ev"/>ent.
	/// </summary>
	template <addon_event ev, typename... Args>
	__forceinline std::enable_if_t<std::is_same_v<typename addon_event_traits<ev>::type, void>, void> invoke_addon_event_callbacks(Args &&... args)
	{
		const std::vector<void *> &event_list = addon_event_list[static_cast<uint32_t>(ev)];
		for (size_t cb = 0, count = event_list.size(); cb < count; ++cb) // Generates better code than ranged-based for loop
		{
			bool first_invocation = false;
			if constexpr (
				ev == addon_event::reshade_present ||
				ev == addon_event::reshade_begin_effects ||
				ev == addon_event::reshade_finish_effects ||
				ev == addon_event::reshade_reloaded_effects ||
				ev == addon_event::reshade_screenshot ||
				ev == addon_event::reshade_render_technique ||
				ev == addon_event::reshade_set_current_preset_path)
			{
				// Prevent recursive invocation of events
				if (nullptr == addon_current)
				{
					addon_current = find_addon(event_list[cb]);
					first_invocation = true;
				}
				else if (find_addon(event_list[cb]) == addon_current)
				{
					continue;
				}
			}

			reinterpret_cast<typename addon_event_traits<ev>::decl>(event_list[cb])(std::forward<Args>(args)...);

			if (first_invocation)
				addon_current = nullptr;
		}
	}
	/// <summary>
	/// Calls registered callbacks for the specified <typeparamref name="ev"/>ent until a callback reports back as having handled this event by returning <see langword="true"/>.
	/// </summary>
	template <addon_event ev, typename... Args>
	__forceinline std::enable_if_t<std::is_same_v<typename addon_event_traits<ev>::type, bool>, bool> invoke_addon_event_callbacks(Args &&... args)
	{
		bool skip = false;
		const std::vector<void *> &event_list = addon_event_list[static_cast<uint32_t>(ev)];
		for (size_t cb = 0, count = event_list.size(); cb < count; ++cb)
		{
			bool first_invocation = false;
			if constexpr (
				ev == addon_event::reshade_set_uniform_value ||
				ev == addon_event::reshade_set_technique_state ||
				ev == addon_event::reshade_set_effects_state ||
				ev == addon_event::reshade_reorder_techniques ||
				ev == addon_event::reshade_open_overlay ||
				ev == addon_event::reshade_overlay_uniform_variable ||
				ev == addon_event::reshade_overlay_technique)
			{
				// Prevent recursive invocation of events
				if (nullptr == addon_current)
				{
					addon_current = find_addon(event_list[cb]);
					first_invocation = true;
				}
				else if (find_addon(event_list[cb]) == addon_current)
				{
					continue;
				}
			}

			if (reinterpret_cast<typename addon_event_traits<ev>::decl>(event_list[cb])(std::forward<Args>(args)...))
				skip = true;

			if (first_invocation)
				addon_current = nullptr;
		}
		return skip;
	}

	/// <summary>
	/// Invokes all registered callbacks for the specified <typeparamref name="ev"/>ent. The gate is zero while the process runs and add-ons are
	/// not reloaded: then it is only the callbacks. Else the process's end and the guard of a reload are met first
	/// </summary>
	template <addon_event ev, typename... Args>
	std::enable_if_t<std::is_same_v<typename addon_event_traits<ev>::type, void>, void> invoke_addon_event(Args &&... args)
	{
		const uint32_t gate = ultra::eventGate.load(std::memory_order_relaxed);
		if (gate == 0) [[likely]]
			return invoke_addon_event_callbacks<ev>(std::forward<Args>(args)...);
		if (gate & ultra::gateExitBegun)
			return;
		const ultra::AddonEventGuard reload_guard;
		invoke_addon_event_callbacks<ev>(std::forward<Args>(args)...);
	}
	/// <summary>
	/// Invokes registered callbacks for the specified <typeparamref name="ev"/>ent until a callback reports back as having handled this event by returning <see langword="true"/>.
	/// </summary>
	template <addon_event ev, typename... Args>
	std::enable_if_t<std::is_same_v<typename addon_event_traits<ev>::type, bool>, bool> invoke_addon_event(Args &&... args)
	{
		const uint32_t gate = ultra::eventGate.load(std::memory_order_relaxed);
		if (gate == 0) [[likely]]
			return invoke_addon_event_callbacks<ev>(std::forward<Args>(args)...);
		if (gate & ultra::gateExitBegun)
			return false;
		const ultra::AddonEventGuard reload_guard;
		return invoke_addon_event_callbacks<ev>(std::forward<Args>(args)...);
	}
}
