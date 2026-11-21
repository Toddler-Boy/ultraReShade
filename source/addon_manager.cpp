/*
 * Copyright (C) 2021 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "reshade.hpp"
#include "reshade_ultra.hpp"
#include "addon_manager.hpp"
#include "dll_log.hpp"
#include "ini_file.hpp"
#include "ultra_addon_reload.hpp"
#include "ultra_home.hpp"
#include "ultra_file_watch.hpp"
#include <algorithm> // std::find, std::find_if, std::remove, std::remove_if
#include <filesystem>
#include <intrin.h>
#include <Windows.h>

extern void register_addon_depth();
extern void unregister_addon_depth();
extern void register_addon_constant_buffers();
extern void unregister_addon_constant_buffers();
extern void track_constant_buffers(bool is_tracking);
extern void register_addon_resource_ids();
extern void unregister_addon_resource_ids();

extern HMODULE g_module_handle;
extern std::filesystem::path g_target_executable_path;

extern std::filesystem::path get_module_path(HMODULE module);

#if RESHADE_VERBOSE_LOG
static const char *addon_event_to_string(reshade::addon_event ev)
{
	using reshade::addon_event;
	switch (ev)
	{
	case addon_event::init_device: return "init_device";
	case addon_event::create_device: return "create_device";
	case addon_event::destroy_device: return "destroy_device";
	case addon_event::init_command_list: return "init_command_list";
	case addon_event::destroy_command_list: return "destroy_command_list";
	case addon_event::init_command_queue: return "init_command_queue";
	case addon_event::destroy_command_queue: return "destroy_command_queue";
	case addon_event::init_swapchain: return "init_swapchain";
	case addon_event::create_swapchain: return "create_swapchain";
	case addon_event::destroy_swapchain: return "destroy_swapchain";
	case addon_event::init_effect_runtime: return "init_effect_runtime";
	case addon_event::destroy_effect_runtime: return "destroy_effect_runtime";
	case addon_event::init_sampler: return "init_sampler";
	case addon_event::create_sampler: return "create_sampler";
	case addon_event::destroy_sampler: return "destroy_sampler";
	case addon_event::init_resource: return "init_resource";
	case addon_event::create_resource: return "create_resource";
	case addon_event::destroy_resource: return "destroy_resource";
	case addon_event::init_resource_view: return "init_resource_view";
	case addon_event::create_resource_view: return "create_resource_view";
	case addon_event::destroy_resource_view: return "destroy_resource_view";
	case addon_event::map_buffer_region: return "map_buffer_region";
	case addon_event::unmap_buffer_region: return "unmap_buffer_region";
	case addon_event::map_texture_region: return "map_texture_region";
	case addon_event::unmap_texture_region: return "unmap_texture_region";
	case addon_event::update_buffer_region: return "update_buffer_region";
	case addon_event::update_buffer_region_command: return "update_buffer_region_command";
	case addon_event::update_texture_region: return "update_texture_region";
	case addon_event::update_texture_region_command: return "update_texture_region_command";
	case addon_event::init_pipeline: return "init_pipeline";
	case addon_event::create_pipeline: return "create_pipeline";
	case addon_event::destroy_pipeline: return "destroy_pipeline";
	case addon_event::init_pipeline_layout: return "init_pipeline_layout";
	case addon_event::create_pipeline_layout: return "create_pipeline_layout";
	case addon_event::destroy_pipeline_layout: return "destroy_pipeline_layout";
	case addon_event::copy_descriptor_tables: return "copy_descriptor_tables";
	case addon_event::update_descriptor_tables: return "update_descriptor_tables";
	case addon_event::init_query_heap: return "init_query_heap";
	case addon_event::create_query_heap: return "create_query_heap";
	case addon_event::destroy_query_heap: return "destroy_query_heap";
	case addon_event::get_query_heap_results: return "get_query_heap_results";
	case addon_event::barrier: return "barrier";
	case addon_event::begin_render_pass: return "begin_render_pass";
	case addon_event::end_render_pass: return "end_render_pass";
	case addon_event::bind_render_targets_and_depth_stencil: return "bind_render_targets_and_depth_stencil";
	case addon_event::bind_pipeline: return "bind_pipeline";
	case addon_event::bind_pipeline_states: return "bind_pipeline_states";
	case addon_event::bind_viewports: return "bind_viewports";
	case addon_event::bind_scissor_rects: return "bind_scissor_rects";
	case addon_event::push_constants: return "push_constants";
	case addon_event::push_descriptors: return "push_descriptors";
	case addon_event::bind_descriptor_tables: return "bind_descriptor_tables";
	case addon_event::bind_index_buffer: return "bind_index_buffer";
	case addon_event::bind_vertex_buffers: return "bind_vertex_buffers";
	case addon_event::bind_stream_output_buffers: return "bind_stream_output_buffers";
	case addon_event::draw: return "draw";
	case addon_event::draw_indexed: return "draw_indexed";
	case addon_event::dispatch: return "dispatch";
	case addon_event::dispatch_mesh: return "dispatch_mesh";
	case addon_event::dispatch_rays: return "dispatch_rays";
	case addon_event::draw_or_dispatch_indirect: return "draw_or_dispatch_indirect";
	case addon_event::copy_resource: return "copy_resource";
	case addon_event::copy_buffer_region: return "copy_buffer_region";
	case addon_event::copy_buffer_to_texture: return "copy_buffer_to_texture";
	case addon_event::copy_texture_region: return "copy_texture_region";
	case addon_event::copy_texture_to_buffer: return "copy_texture_to_buffer";
	case addon_event::resolve_texture_region: return "resolve_texture_region";
	case addon_event::clear_depth_stencil_view: return "clear_depth_stencil_view";
	case addon_event::clear_render_target_view: return "clear_render_target_view";
	case addon_event::clear_unordered_access_view_uint: return "clear_unordered_access_view_uint";
	case addon_event::clear_unordered_access_view_float: return "clear_unordered_access_view_float";
	case addon_event::generate_mipmaps: return "generate_mipmaps";
	case addon_event::begin_query: return "begin_query";
	case addon_event::end_query: return "end_query";
	case addon_event::copy_query_heap_results: return "copy_query_heap_results";
	case addon_event::copy_acceleration_structure: return "copy_acceleration_structure";
	case addon_event::build_acceleration_structure: return "build_acceleration_structure";
	case addon_event::query_acceleration_structures: return "query_acceleration_structures";
	case addon_event::reset_command_list: return "reset_command_list";
	case addon_event::close_command_list: return "close_command_list";
	case addon_event::execute_command_list: return "execute_command_list";
	case addon_event::execute_secondary_command_list: return "execute_secondary_command_list";
	case addon_event::present: return "present";
	case addon_event::finish_present: return "finish_present";
	case addon_event::set_fullscreen_state: return "set_fullscreen_state";
	case addon_event::reshade_present: return "reshade_present";
	case addon_event::reshade_begin_effects: return "reshade_begin_effects";
	case addon_event::reshade_finish_effects: return "reshade_finish_effects";
	case addon_event::reshade_reloaded_effects: return "reshade_reloaded_effects";
	case addon_event::reshade_set_uniform_value: return "reshade_set_uniform_value";
	case addon_event::reshade_set_technique_state: return "reshade_set_technique_state";
	case addon_event::reshade_overlay: return "reshade_overlay";
	case addon_event::reshade_screenshot: return "reshade_screenshot";
	case addon_event::reshade_render_technique: return "reshade_render_technique";
	case addon_event::reshade_set_effects_state: return "reshade_set_effects_state";
	case addon_event::reshade_set_current_preset_path: return "reshade_set_current_preset_path";
	case addon_event::reshade_reorder_techniques: return "reshade_reorder_techniques";
	case addon_event::reshade_open_overlay: return "reshade_open_overlay";
	case addon_event::reshade_overlay_uniform_variable: return "reshade_overlay_uniform_variable";
	case addon_event::reshade_overlay_technique: return "reshade_overlay_technique";
	}
	return "unknown";
}
#endif

std::vector<void *> reshade::addon_event_list[static_cast<uint32_t>(reshade::addon_event::max)];
std::vector<reshade::addon_info> reshade::addon_loaded_info;
thread_local const reshade::addon_info *reshade::addon_current = nullptr;
static unsigned long s_reference_count = 0;

// [ADDON] KeepLoaded=0 gives upstream's way back: add-ons unload with the last device; read at the first load
[[nodiscard]] static bool wants_addons_kept()
{
	static const bool is_kept = !reshade::global_config().has("ADDON", "KeepLoaded") || reshade::global_config().get("ADDON", "KeepLoaded");
	return is_kept;
}

// [ADDON] GameExes: the exe names add-ons load in, so a game's helpers in its folder (crash handler, video player) load none;
// without the key every process does
[[nodiscard]] static bool is_addon_process()
{
	std::vector<std::string> exes;
	reshade::global_config().get("ADDON", "GameExes", exes);
	std::erase_if(exes, [](const std::string &exe) { return exe.empty(); });
	if (exes.empty())
		return true;

	const std::filesystem::path exe = g_target_executable_path.filename();
	if (std::any_of(exes.cbegin(), exes.cend(), [&exe](const std::string &name) { return _wcsicmp(std::filesystem::u8path(name).filename().c_str(), exe.c_str()) == 0; }))
		return true;

	reshade::log::message(reshade::log::level::info, "No add-on loads in '%s': [ADDON] GameExes does not name it.", exe.u8string().c_str());
	return false;
}

void reshade::load_addons()
{
	// Only load add-ons the first time a reference is added
	if (InterlockedIncrement(&s_reference_count) != 1)
		return;

	const bool is_loading = is_addon_process();

	// Games make and drop devices before the one they keep: add-ons stay, and so does ReShade (a Vulkan layer is freed with its last instance)
	if (is_loading && wants_addons_kept())
	{
		InterlockedIncrement(&s_reference_count);
		if (HMODULE pinned = nullptr; !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(g_module_handle), &pinned))
			log::message(log::level::error, "ReShade could not keep itself loaded (error code %lu): its add-ons may outlive it.", GetLastError());
		log::message(log::level::info, "Add-ons stay loaded until the process ends.");
		ultra::followProcessExit();
	}
	else if (is_loading)
		log::message(log::level::info, "Add-ons unload when the last device goes ([ADDON] KeepLoaded=0).");

	const ini_file &config = global_config();

	// Their events need an add-on of this module to belong to
	{	addon_info &info = addon_loaded_info.emplace_back();
		info.name = "ultraReShade";
		info.api_version = RESHADE_API_VERSION;
		info.external = false;
		info.handle = g_module_handle;
	}

	register_addon_constant_buffers();
	register_addon_resource_ids();

	// reshade.json's list is the load order and the whole truth: built-in add-ons by name, the others by file name in [ADDON] AddonPath
	const json::value *const addons = config.curated_member("addons");
	if (ReShadeJsonGetType(addons) != json::type::array)
	{
		log::message(log::level::error, "reshade.json has no list of add-ons (addons), so none loads!");
		return;
	}

	std::filesystem::path addon_search_path;
	config.get("ADDON", "AddonPath", addon_search_path);
	const bool has_addon_path = addon_search_path.is_absolute();
	if (is_loading)
	{
		if (has_addon_path)
		{
			if (ultra::isDevelopmentOn())
				ultra::startAddonReload(addon_search_path);
			else
				log::message(log::level::info, "Developer mode is off: add-ons load in place and do not reload.");
		}
		else
			log::message(log::level::error, "[ADDON] AddonPath is not an absolute path ('%s'), so no add-on file loads!", addon_search_path.u8string().c_str());
	}

	for (size_t i = 0, count = ReShadeJsonGetSize(addons); i < count; ++i)
	{
		const char *text = nullptr;
		size_t length = 0;
		if (!ReShadeJsonGetString(ReShadeJsonAt(addons, i, nullptr), &text, &length) || length == 0)
		{
			log::message(log::level::error, "Add-on %zu in reshade.json is no name, it is left out.", i);
			continue;
		}

		const std::string name(text, length);
		const std::filesystem::path path = std::filesystem::u8path(name);

		if (path.extension() != L".addon" &&
			path.extension() != L".addon32" &&
			path.extension() != L".addon64")
		{
			if (name == "Generic Depth")
			{
				addon_info &info = addon_loaded_info.emplace_back();
				info.name = name;
				info.api_version = RESHADE_API_VERSION;
				info.external = false;
				info.handle = g_module_handle;

				register_addon_depth();
			}
			else
			{
				log::message(log::level::error, "reshade.json names add-on \"%s\", which is no built-in one and no add-on file, it is left out.", name.c_str());
			}
			continue;
		}

		if (!is_loading || !has_addon_path)
			continue;

		if (path.extension() != L".addon" &&
#ifndef _WIN64
			path.extension() != L".addon32")
#else
			path.extension() != L".addon64")
#endif
		{
			log::message(log::level::error, "Add-on '%s' in reshade.json is not built for this bitness, it is left out.", name.c_str());
			continue;
		}

		// Avoid loading library again that has already been loaded externally
		if (std::any_of(addon_loaded_info.cbegin(), addon_loaded_info.cend(),
				[&path](const addon_info &info) { return path.filename().u8string() == info.file; }))
			continue;

		const std::filesystem::path file = addon_search_path / path;
		log::message(log::level::info, "Loading add-on from '%s' ...", file.u8string().c_str());

		// Use 'LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR' to temporarily add add-on search path to the list of directories 'LoadLibraryEx' will use to resolve DLL dependencies
		const std::filesystem::path source = ultra::addonSource(file);
		const std::filesystem::path load_path = ultra::addonLoadPath(source, file.parent_path());
		if (load_path.empty())
			continue;
		const HMODULE module = LoadLibraryExW(load_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		if (module == nullptr)
		{
			log::message(log::level::error, "Failed to load add-on from '%s' with error code %lu!", file.u8string().c_str(), GetLastError());
			continue;
		}

		// Call optional loading entry point (for add-ons wanting to do more complicated one-time initialization than possible in 'DllMain')
		const auto init_func = reinterpret_cast<bool(*)(HMODULE addon_module, HMODULE reshade_module)>(GetProcAddress(module, "AddonInit"));
		if (init_func != nullptr && !init_func(module, g_module_handle))
		{
			log::message(log::level::error, "Failed to load add-on from '%s' because initialization was not successful!", file.u8string().c_str());

			FreeLibrary(module);
			continue;
		}

		if (addon_info *const registered_info = find_addon(module))
		{
			registered_info->external = false;
			ultra::watchAddon(module, source);
		}
		else
		{
			log::message(log::level::warning, "No add-on was registered by '%s'. Unloading again ...", file.u8string().c_str());

			FreeLibrary(module);
		}
	}
}
void reshade::unload_addons()
{
	// Only unload add-ons after the last reference to the manager was released
	if (InterlockedDecrement(&s_reference_count) != 0)
		return;

	// Create copy of add-on list before unloading, since add-ons call 'ReShadeUnregisterAddon' during 'FreeLibrary', which modifies the list
	const std::vector<addon_info> loaded_info_copy = addon_loaded_info;
	for (const addon_info &info : loaded_info_copy)
	{
		if (info.handle == nullptr || info.handle == g_module_handle)
			continue; // Skip disabled and built-in add-ons

		log::message(log::level::info, "Unloading add-on \"%s\" ...", info.name.c_str());

		const auto module = static_cast<HMODULE>(info.handle);

		// Call optional unloading entry point
		const auto uninit_func = reinterpret_cast<void(*)(HMODULE addon_module, HMODULE reshade_module)>(GetProcAddress(module, "AddonUninit"));
		if (uninit_func != nullptr)
			uninit_func(module, g_module_handle);

		if (info.external)
			continue;

		if (!FreeLibrary(module))
			log::message(log::level::warning, "Failed to unload '%s' with error code %lu!", info.file.c_str(), GetLastError());

		if (addon_info *const registered_info = find_addon(module))
		{
			log::message(log::level::warning, "Add-on \"%s\" was not unregistered!", registered_info->name.c_str());

			// Make add-on external at this point, so that it is not removed, since a consecutive load must be able to find it
			registered_info->external = true;
		}
	}

#if RESHADE_VERBOSE_LOG
	log::message(log::level::info, "Unloading built-in add-ons ...");
#endif

	unregister_addon_depth();
	unregister_addon_constant_buffers();
	unregister_addon_resource_ids();

	ultra::stopAddonReload();
	ultra::stopFileWatch();

	// Remove all unloaded add-ons
	addon_loaded_info.erase(
		std::remove_if(addon_loaded_info.begin(), addon_loaded_info.end(),
			[](const addon_info &info) {
				// There should only be external, disabled and built-in add-ons in the list at this point (any other well behaving add-ons should already have unregistered themselves during unloading)
				assert(info.external || info.handle == nullptr || info.handle == g_module_handle);
				return !info.external;
			}),
		addon_loaded_info.end());
}

reshade::addon_info *reshade::find_addon(const void *address)
{
	if (address == nullptr)
		return nullptr;

	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module))
		return nullptr;

	assert(module != nullptr);

	for (auto it = addon_loaded_info.rbegin(); it != addon_loaded_info.rend(); ++it)
		if (it->handle == module)
			return &(*it);
	return nullptr;
}

#if defined(RESHADE_API_LIBRARY_EXPORT)

[[nodiscard]] static bool is_process_exiting()
{
	const auto shutdown_in_progress = reinterpret_cast<BOOLEAN(NTAPI *)()>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlDllShutdownInProgress")));
	return shutdown_in_progress != nullptr && shutdown_in_progress() != FALSE;
}

bool ReShadeRegisterAddon(void *module, uint32_t api_version)
{
	// Can only register an add-on module once
	if (module == nullptr || module == g_module_handle || reshade::find_addon(module))
	{
		reshade::log::message(reshade::log::level::error, "Failed to register add-on, because it provided an invalid module handle!");
		return false;
	}

	// Check that the requested API version is supported
	if (api_version == 0 || api_version > RESHADE_API_VERSION || (api_version / 10000) != (RESHADE_API_VERSION / 10000))
	{
		reshade::log::message(reshade::log::level::error, "Failed to register add-on, because the requested API version (%u) is not supported (%u)!", api_version, static_cast<uint32_t>(RESHADE_API_VERSION));
		return false;
	}

	const std::filesystem::path path = get_module_path(static_cast<HMODULE>(module));

	reshade::addon_info info;
	info.name = path.stem().u8string();
	info.file = path.filename().u8string();
	info.handle = module;
	info.api_version = api_version;

	if (const char *const *name = reinterpret_cast<const char *const *>(GetProcAddress(static_cast<HMODULE>(module), "NAME")))
		info.name = *name;

	if (std::find_if(reshade::addon_loaded_info.cbegin(), reshade::addon_loaded_info.cend(),
			[&info](const reshade::addon_info &existing_info) {
				return existing_info.name == info.name;
			}) != reshade::addon_loaded_info.cend())
	{
		// Prevent registration if another add-on with the same name already exists
		reshade::log::message(reshade::log::level::error, "Failed to register add-on, because another one with the same name (\"%s\") was already registered!", info.name.c_str());
		return false;
	}

	reshade::log::message(reshade::log::level::info, "Registered add-on \"%s\" using ReShade API version %u.", info.name.c_str(), api_version);

	reshade::addon_loaded_info.push_back(std::move(info));

	return true;
}
void ReShadeUnregisterAddon(void *module)
{
	if (module == nullptr || module == g_module_handle)
		return;

	reshade::addon_info *const info = reshade::find_addon(module);
	if (info == nullptr)
		return;

	// Unregister all event callbacks registered by this add-on
	while (!info->event_callbacks.empty())
	{
		const auto &last_event_callback = info->event_callbacks.back();
		ReShadeUnregisterEvent(static_cast<reshade::addon_event>(last_event_callback.first), last_event_callback.second);
	}

#if RESHADE_GUI
	// Unregister all overlay callbacks associated with this add-on
	while (!info->overlay_callbacks.empty())
	{
		const auto &last_overlay_callback = info->overlay_callbacks.back();
		ReShadeUnregisterOverlay(last_overlay_callback.title.c_str(), last_overlay_callback.callback);
	}
#endif

	// At process exit nothing delivers any more, and a thread ended mid-delivery may hold the watches' locks
	if (!is_process_exiting())
	{
		reshade::ultra::forgetFileWatches(static_cast<HMODULE>(module));
		reshade::ultra::forgetExclusiveCalls(static_cast<HMODULE>(module));
	}

	reshade::log::message(reshade::log::level::info, "Unregistered add-on \"%s\".", info->name.c_str());

	reshade::addon_loaded_info.erase(reshade::addon_loaded_info.begin() + (info - reshade::addon_loaded_info.data()));
}

// module null: the add-on is the caller's module
bool ReShadeRunExclusive(void *module, void(*callback)(void *context), void *context)
{
	const reshade::addon_info *const info = reshade::find_addon(module != nullptr ? module : _ReturnAddress());
	if (info == nullptr)
	{
		reshade::log::message(reshade::log::level::error, "run_exclusive was called by a module that is not a registered add-on.");
		return false;
	}

	return reshade::ultra::runExclusive(static_cast<HMODULE>(info->handle), callback, context);
}

// module null: the add-on is the caller's module
bool ReShadeGetAddonSource(void *module, wchar_t *path, uint32_t capacity)
{
	const reshade::addon_info *const info = reshade::find_addon(module != nullptr ? module : _ReturnAddress());
	if (info == nullptr || path == nullptr)
		return false;

	const std::filesystem::path source = reshade::ultra::addonSourceOf(static_cast<HMODULE>(info->handle));
	if (source.native().size() >= capacity)
		return false;

	source.native().copy(path, source.native().size());
	path[source.native().size()] = L'\0';
	return true;
}

// module null: the add-on is the caller's module
bool ReShadeReloadAddon(void *module)
{
	const reshade::addon_info *const info = reshade::find_addon(module != nullptr ? module : _ReturnAddress());
	if (info == nullptr)
	{
		reshade::log::message(reshade::log::level::error, "reload_addon was called by a module that is not a registered add-on.");
		return false;
	}

	return reshade::ultra::requestAddonReload(static_cast<HMODULE>(info->handle));
}

void ReShadeRegisterEvent(reshade::addon_event ev, void *callback)
{
	ReShadeRegisterEventForAddon(nullptr, ev, callback);
}
void ReShadeRegisterEventForAddon(void *module, reshade::addon_event ev, void *callback)
{
	if (ev >= reshade::addon_event::max)
		return;

	reshade::addon_info *const info = reshade::find_addon(module != nullptr ? module : callback);
	if (info == nullptr)
	{
		reshade::log::message(reshade::log::level::error, "Could not find associated add-on and therefore failed to register an event.");
		return;
	}

	assert(info->handle == module || module == nullptr);

	std::vector<void *> &event_list = reshade::addon_event_list[static_cast<uint32_t>(ev)];
	event_list.push_back(callback);

	info->event_callbacks.emplace_back(static_cast<uint32_t>(ev), callback);

	if (ev == reshade::addon_event::ultra_constant_buffer && event_list.size() == 1)
		track_constant_buffers(true);

#if RESHADE_VERBOSE_LOG
	reshade::log::message(reshade::log::level::debug, "Registered event callback %p for event %s.", callback, addon_event_to_string(ev));
#endif
}
void ReShadeUnregisterEvent(reshade::addon_event ev, void *callback)
{
	ReShadeUnregisterEventForAddon(nullptr, ev, callback);
}
void ReShadeUnregisterEventForAddon(void *module, reshade::addon_event ev, void *callback)
{
	if (ev >= reshade::addon_event::max)
		return;

	reshade::addon_info *const info = reshade::find_addon(module != nullptr ? module : callback);
	if (info == nullptr)
		return; // Do not log an error here, since this may be called if an add-on failed to load

	assert(info->handle == module || module == nullptr);

	std::vector<void *> &event_list = reshade::addon_event_list[static_cast<uint32_t>(ev)];
	event_list.erase(std::remove(event_list.begin(), event_list.end(), callback), event_list.end());

	info->event_callbacks.erase(std::remove(info->event_callbacks.begin(), info->event_callbacks.end(), std::make_pair(static_cast<uint32_t>(ev), callback)), info->event_callbacks.end());

	if (ev == reshade::addon_event::ultra_constant_buffer && event_list.empty())
		track_constant_buffers(false);

#if RESHADE_VERBOSE_LOG
	reshade::log::message(reshade::log::level::debug, "Unregistered event callback %p for event %s.", callback, addon_event_to_string(ev));
#endif
}

#if RESHADE_GUI

void ReShadeRegisterOverlay(const char *title, void(*callback)(reshade::api::effect_runtime *runtime))
{
	ReShadeRegisterOverlayForAddon(nullptr, title, callback);
}
void ReShadeRegisterOverlayForAddon(void *module, const char *title, void(*callback)(reshade::api::effect_runtime *runtime))
{
	if (title == nullptr)
	{
		reshade::log::message(reshade::log::level::error, "An add-on tried to register an overlay without a title.");
		return;
	}

	reshade::addon_info *const info = reshade::find_addon(module != nullptr ? module : reinterpret_cast<void *>(callback));
	if (info == nullptr)
	{
		reshade::log::message(reshade::log::level::error, "Could not find associated add-on and therefore failed to register overlay with title \"%s\".", title);
		return;
	}

	assert(info->handle == module || module == nullptr);

	info->overlay_callbacks.push_back(reshade::addon_info::overlay_callback { title, callback });

#if RESHADE_VERBOSE_LOG
	reshade::log::message(reshade::log::level::debug, "Registered overlay with title \"%s\" and callback %p.", title, reinterpret_cast<void *>(callback));
#endif
}
void ReShadeUnregisterOverlay(const char *title, void(*callback)(reshade::api::effect_runtime *runtime))
{
	ReShadeUnregisterOverlayForAddon(nullptr, title, callback);
}
void ReShadeUnregisterOverlayForAddon(void *module, const char *title, void(*callback)(reshade::api::effect_runtime *runtime))
{
	if (title == nullptr)
		return;

	reshade::addon_info *const info = reshade::find_addon(module != nullptr ? module : reinterpret_cast<void *>(callback));
	if (info == nullptr)
		return; // Do not log an error here, since this may be called if an add-on failed to load

	assert(info->handle == module || module == nullptr);

#if RESHADE_VERBOSE_LOG
	// Log before removing from overlay list below, since pointer to title string may become invalid by the removal
	reshade::log::message(reshade::log::level::debug, "Unregistered overlay with title \"%s\" and callback %p.", title, reinterpret_cast<void *>(callback));
#endif

	info->overlay_callbacks.erase(std::remove_if(info->overlay_callbacks.begin(), info->overlay_callbacks.end(),
		[title, callback](const reshade::addon_info::overlay_callback &item) {
			return item.title == title && item.callback == callback;
		}), info->overlay_callbacks.end());
}

#endif

#endif
