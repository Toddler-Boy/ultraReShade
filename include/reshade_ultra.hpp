/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "reshade.hpp"
#include "reshade_ultra_api.h"
#include "reshade_ultra_ui.h"
#include "reshade_ultra_json.hpp"
#include "reshade_ultra_log.hpp"

// ultraReShade's additions to the add-on API: functions an add-on calls (include after reshade.hpp, or instead of it)

#if defined(RESHADE_API_LIBRARY)

RESHADE_API_LIBRARY_DECL uint64_t ReShadeGetResourceStableId ( reshade::api::device* device, reshade::api::resource resource );
RESHADE_API_LIBRARY_DECL bool ReShadeWatchFile ( void* module, const char* path, const char* pattern );
RESHADE_API_LIBRARY_DECL void ReShadeUnwatchFile ( void* module, const char* path, const char* pattern );
RESHADE_API_LIBRARY_DECL void ReShadeFlushLog ();
RESHADE_API_LIBRARY_DECL void ReShadeSetLogoAlert ( bool on );
RESHADE_API_LIBRARY_DECL bool ReShadeRunExclusive ( void* module, void ( *callback ) ( void* context ), void* context );
RESHADE_API_LIBRARY_DECL bool ReShadeReloadAddon ( void* module );
RESHADE_API_LIBRARY_DECL void ReShadeGetBuild ( const char** number, const char** built );
RESHADE_API_LIBRARY_DECL bool ReShadeGetAddonSource ( void* module, wchar_t* path, uint32_t capacity );
RESHADE_API_LIBRARY_DECL UgLookup ReShadeHost ( UgLookup other );
RESHADE_API_LIBRARY_DECL const ReShadeWidgetTable* ReShadeGetWidgetTable ( uint32_t bytes );

#endif

namespace reshade
{
	// The ID ultraReShade gave a render target, depth-stencil or unordered access texture, or a swap chain back buffer:
	// it stays the same when the game creates the resource again in the same role (addon_event::ultra_resource_recreated
	// tells which new resource took it over). Zero for other resources, and on a ReShade without this function
	inline uint64_t get_resource_stable_id ( api::device* device, api::resource resource )
	{
#if defined(RESHADE_API_LIBRARY)
		return ReShadeGetResourceStableId ( device, resource );
#else
		static const auto	func = reinterpret_cast<uint64_t ( * ) ( api::device*, api::resource )> (
			GetProcAddress ( internal::get_reshade_module_handle (), "ReShadeGetResourceStableId" ) );
		return func != nullptr ? func ( device, resource ) : 0;
#endif
	}

	namespace internal
	{
		// pattern null: path is a file
		inline bool watch_path ( const char* path, const char* pattern )
		{
#if defined(RESHADE_API_LIBRARY)
			return ReShadeWatchFile ( nullptr, path, pattern );
#else
			static const auto	func = reinterpret_cast<bool ( * ) ( void*, const char*, const char* )> (
				GetProcAddress ( get_reshade_module_handle (), "ReShadeWatchFile" ) );
			return func != nullptr && func ( get_current_module_handle (), path, pattern );
#endif
		}

		inline void unwatch_path ( const char* path, const char* pattern )
		{
#if defined(RESHADE_API_LIBRARY)
			ReShadeUnwatchFile ( nullptr, path, pattern );
#else
			static const auto	func = reinterpret_cast<void ( * ) ( void*, const char*, const char* )> (
				GetProcAddress ( get_reshade_module_handle (), "ReShadeUnwatchFile" ) );
			if ( func != nullptr )
				func ( get_current_module_handle (), path, pattern );
#endif
		}
	}

	// Changes to the file (UTF-8, absolute or relative to ReShade's base path) arrive as addon_event::ultra_file_changed,
	// for this add-on only. A deleted file stays watched and reports its return. False when its folder can't be watched
	// (the log says why), and on a ReShade without this function. The add-on's watches end when it unloads
	inline bool watch_file ( const char* path ) { return internal::watch_path ( path, nullptr ); }
	inline void unwatch_file ( const char* path ) { internal::unwatch_path ( path, nullptr ); }

	// The same for every file in the folder whose name matches pattern (* and ?, case ignored); subfolders are not watched
	inline bool watch_folder ( const char* folder, const char* pattern = "*" ) { return internal::watch_path ( folder, pattern != nullptr ? pattern : "*" ); }
	inline void unwatch_folder ( const char* folder, const char* pattern = "*" ) { internal::unwatch_path ( folder, pattern != nullptr ? pattern : "*" ); }

	// The start logo red and blinking while on, also after it played: it stays on screen until the add-on turns it off
	// again, and does not play after that. Nothing on a ReShade without this function
	inline void set_logo_alert ( bool on )
	{
#if defined(RESHADE_API_LIBRARY)
		ReShadeSetLogoAlert ( on );
#else
		static const auto	func = reinterpret_cast<void ( * ) ( bool )> ( GetProcAddress ( internal::get_reshade_module_handle (), "ReShadeSetLogoAlert" ) );
		if ( func != nullptr )
			func ( on );
#endif
	}

	// callback runs once at the next present while no add-on event runs on any thread and the GPU is idle. Development
	// builds of ultraReShade only: false elsewhere, and on a ReShade without this function
	inline bool run_exclusive ( void ( *callback ) ( void* context ), void* context )
	{
#if defined(RESHADE_API_LIBRARY)
		return ReShadeRunExclusive ( nullptr, callback, context );
#else
		static const auto	func = reinterpret_cast<bool ( * ) ( void*, void ( * ) ( void* ), void* )> (
			GetProcAddress ( internal::get_reshade_module_handle (), "ReShadeRunExclusive" ) );
		return func != nullptr && func ( internal::get_current_module_handle (), callback, context );
#endif
	}

	// This add-on unloads and loads again from its file at the next present, as when its file changed. Development builds of
	// ultraReShade only: false elsewhere, and on a ReShade without this function
	inline bool reload_addon ()
	{
#if defined(RESHADE_API_LIBRARY)
		return ReShadeReloadAddon ( nullptr );
#else
		static const auto	func = reinterpret_cast<bool ( * ) ( void* )> ( GetProcAddress ( internal::get_reshade_module_handle (), "ReShadeReloadAddon" ) );
		return func != nullptr && func ( internal::get_current_module_handle () );
#endif
	}

	// ultraReShade's own build: its version number (6.8.0) and the time it was built (Nov 21 2026 14:03:22), as texts that stay while it is
	// loaded. False, nothing set, on a ReShade without this function
	inline bool get_build ( const char** number, const char** built )
	{
#if defined(RESHADE_API_LIBRARY)
		ReShadeGetBuild ( number, built );
		return true;
#else
		static const auto	func = reinterpret_cast<void ( * ) ( const char**, const char** )> ( GetProcAddress ( internal::get_reshade_module_handle (), "ReShadeGetBuild" ) );
		if ( func == nullptr )
			return false;
		func ( number, built );
		return true;
#endif
	}

	// The file the add-on's code was loaded from (module null: this add-on): with Developer mode on, which loads add-ons
	// from copies, the file the copy was made from. False, path empty, where the module is none of an add-on's, the path does not fit
	// capacity (in characters) or this ReShade has no such function
	inline bool get_addon_source ( void* module, wchar_t* path, uint32_t capacity )
	{
#if defined(RESHADE_API_LIBRARY)
		return ReShadeGetAddonSource ( module, path, capacity );
#else
		static const auto	func = reinterpret_cast<bool ( * ) ( void*, wchar_t*, uint32_t )> (
			GetProcAddress ( internal::get_reshade_module_handle (), "ReShadeGetAddonSource" ) );
		if ( func != nullptr && func ( module != nullptr ? module : internal::get_current_module_handle (), path, capacity ) )
			return true;
		if ( path != nullptr && capacity > 0 )
			path[ 0 ] = L'\0';
		return false;
#endif
	}

	// What ultraReShade answers an add-on that draws its overlay, in the form of reshade_ultra_api.h (the names are in
	// reshade_ultra_host_names.h): its lookup, null on a ReShade without it. other: the add-on's own lookup, null where it has none
	inline UgLookup get_host ( UgLookup other = nullptr )
	{
#if defined(RESHADE_API_LIBRARY)
		return ReShadeHost ( other );
#else
		static const auto	func = reinterpret_cast<UgLookup ( * ) ( UgLookup )> ( GetProcAddress ( internal::get_reshade_module_handle (), "ReShadeHost" ) );
		return func != nullptr ? func ( other ) : nullptr;
#endif
	}

	// ultraReShade's widgets, look, text and images (reshade_ultra_ui.h), null on a ReShade without them or with fewer than bytes of them
	inline const ReShadeWidgetTable* get_widget_table ( uint32_t bytes )
	{
#if defined(RESHADE_API_LIBRARY)
		return ReShadeGetWidgetTable ( bytes );
#else
		static const auto	func = reinterpret_cast<ReShadeGetWidgetTableFn> ( GetProcAddress ( internal::get_reshade_module_handle (), RESHADE_UI_TABLE_EXPORT ) );
		return func != nullptr ? func ( bytes ) : nullptr;
#endif
	}

	namespace log
	{
		// Writes the lines every log of the process (reshade.log and the named logs) still holds in memory to its file now,
		// on the calling thread (a crash handler's last step). Nothing on a ReShade without this function
		inline void flush ()
		{
#if defined(RESHADE_API_LIBRARY)
			ReShadeFlushLog ();
#else
			static const auto	func = reinterpret_cast<void ( * ) ()> ( GetProcAddress ( internal::get_reshade_module_handle (), "ReShadeFlushLog" ) );
			if ( func != nullptr )
				func ();
#endif
		}
	}
}
