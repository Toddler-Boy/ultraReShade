/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "reshade.hpp"

// ultraReShade's additions to the add-on API: functions an add-on calls (include after reshade.hpp, or instead of it)

#if defined(RESHADE_API_LIBRARY)

RESHADE_API_LIBRARY_DECL uint64_t ReShadeGetResourceStableId ( reshade::api::device* device, reshade::api::resource resource );
RESHADE_API_LIBRARY_DECL bool ReShadeWatchFile ( void* module, const char* path, const char* pattern );
RESHADE_API_LIBRARY_DECL void ReShadeUnwatchFile ( void* module, const char* path, const char* pattern );
RESHADE_API_LIBRARY_DECL void ReShadeFlushLog ();
RESHADE_API_LIBRARY_DECL void ReShadeSetLogoAlert ( bool on );

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

	namespace log
	{
		// Writes the lines ReShade's log still holds in memory to its file now, on the calling thread (a crash handler's
		// last step). Nothing on a ReShade without this function
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
