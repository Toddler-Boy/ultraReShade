/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "reshade.hpp"

// ultraReShade's additions to the add-on API: functions an add-on calls (include after reshade.hpp, or instead of it)

#if defined(RESHADE_API_LIBRARY)

RESHADE_API_LIBRARY_DECL uint64_t ReShadeGetResourceStableId ( reshade::api::device* device, reshade::api::resource resource );

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
}
