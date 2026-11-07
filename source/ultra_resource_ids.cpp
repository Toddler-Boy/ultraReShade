/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <bit>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <Windows.h>

#include <reshade.hpp>
#include <reshade_ultra.hpp>

#include "addon_manager.hpp"

//-----------------------------------------------------------------------------

// Stable resource IDs and the addon_event::ultra_resource_recreated source: a render target, depth-stencil or unordered
// access texture, or a back buffer, created in the role of one destroyed before takes over its ID
namespace
{
	using namespace reshade;

	// Two resources play the same role when everything but their size matches
	struct Role
	{
		api::device*		device = nullptr;
		api::resource_type	type = api::resource_type::unknown;
		api::format			format = api::format::unknown;
		uint16_t			levels = 0;			// 0: a full chain, which follows the size
		uint16_t			layers = 0;			// 0 for 3D textures, where it is part of the size
		uint16_t			samples = 0;
		api::memory_heap	heap = api::memory_heap::unknown;
		api::resource_usage	usage = api::resource_usage::undefined;
		api::resource_flags	flags = api::resource_flags::none;
		uint32_t			backBuffer = 0;		// index + 1 of a swap chain's back buffer

		bool operator== ( const Role& ) const = default;
	};

	struct Tracked
	{
		Role		role;
		uint64_t	id = 0;
		uint64_t	createdAt = 0;			// GetTickCount64
		bool		isUnpaired = false;		// got a new ID, no destroyed resource of its role was waiting
	};

	struct Retired
	{
		Role			role;
		uint64_t		id = 0;
		api::resource	resource {};
	};

	struct Recreated
	{
		api::device*	device = nullptr;
		uint64_t		id = 0;
		api::resource	oldResource {};
		api::resource	newResource {};
	};

	struct Key
	{
		api::device*	device = nullptr;
		uint64_t		handle = 0;

		bool operator== ( const Key& ) const = default;
	};

	struct KeyHash
	{
		size_t operator() ( const Key& key ) const { return size_t ( key.handle ^ reinterpret_cast<uintptr_t> ( key.device ) * 0x9E3779B97F4A7C15ull ); }
	};

	// Alt-tab can keep a role empty for minutes, so destroyed roles wait without a time limit, only up to a count
	constexpr size_t	maxRetired = 256;
	// A game that creates the new resource before it releases the old one does so within this time (ms)
	constexpr uint64_t	pairingWindow = 5000;

	std::mutex											lock;
	std::unordered_map<Key, Tracked, KeyHash>			tracked;
	std::vector<Retired>								retired;		// oldest first
	std::unordered_map<api::swapchain*, std::vector<uint64_t>>	backBuffers;
	uint64_t											nextId = 1;

	[[ nodiscard ]] bool roleOf ( api::device* device, const api::resource_desc& desc, Role& role )
	{
		constexpr auto	targetUsage = api::resource_usage::render_target | api::resource_usage::depth_stencil | api::resource_usage::unordered_access;

		if ( desc.type == api::resource_type::unknown || desc.type == api::resource_type::buffer || static_cast<uint32_t> ( desc.usage & targetUsage ) == 0 )
			return false;

		const auto	is3d = desc.type == api::resource_type::texture_3d;
		const auto	largest = std::max ( { desc.texture.width, desc.texture.height, is3d ? uint32_t ( desc.texture.depth_or_layers ) : 1u } );
		const auto	isFullChain = desc.texture.levels == 0 || desc.texture.levels == std::bit_width ( largest );

		role.device = device;
		role.type = desc.type;
		role.format = desc.texture.format;
		role.levels = isFullChain ? 0 : desc.texture.levels;
		role.layers = is3d ? 0 : desc.texture.depth_or_layers;
		role.samples = desc.texture.samples;
		role.heap = desc.heap;
		role.usage = desc.usage;
		role.flags = desc.flags;
		return true;
	}

	// Under the lock: the new resource takes the ID of the destroyed one of its role that was created first
	void adopt ( const Role& role, api::resource resource, std::vector<Recreated>& recreated )
	{
		auto	best = retired.end ();

		for ( auto it = retired.begin (); it != retired.end (); ++it )
			if ( it->role == role && ( best == retired.end () || it->id < best->id ) )
				best = it;

		auto&	entry = tracked[ { role.device, resource.handle } ];
		entry.role = role;
		entry.createdAt = GetTickCount64 ();
		entry.isUnpaired = best == retired.end ();

		if ( entry.isUnpaired )
			entry.id = nextId++;
		else
		{
			entry.id = best->id;
			recreated.push_back ( { role.device, best->id, best->resource, resource } );
			retired.erase ( best );
		}
	}

	// Under the lock: the ID waits for the next resource of its role, unless one was created for it already
	void retire ( api::device* device, api::resource resource, std::vector<Recreated>& recreated )
	{
		const auto	it = tracked.find ( { device, resource.handle } );

		if ( it == tracked.end () )
			return;

		const auto	old = it->second;
		tracked.erase ( it );

		const auto	now = GetTickCount64 ();
		Tracked*	heir = nullptr;
		uint64_t	heirHandle = 0;

		for ( auto& [ key, entry ] : tracked )
			if ( entry.isUnpaired && entry.role == old.role && entry.id > old.id && now - entry.createdAt <= pairingWindow && ( heir == nullptr || entry.id < heir->id ) )
				heir = &entry, heirHandle = key.handle;

		if ( heir != nullptr )
		{
			heir->id = old.id;
			heir->isUnpaired = false;
			recreated.push_back ( { device, old.id, resource, api::resource { heirHandle } } );
			return;
		}

		retired.push_back ( { old.role, old.id, resource } );

		if ( retired.size () > maxRetired )
			retired.erase ( retired.begin () );
	}

	void announce ( const std::vector<Recreated>& recreated )
	{
		for ( const auto& r : recreated )
			invoke_addon_event<addon_event::ultra_resource_recreated> ( r.device, r.id, r.oldResource, r.newResource );
	}

	//-----------------------------------------------------------------------------

	void onInitResource ( api::device* device, const api::resource_desc& desc, const api::subresource_data*, api::resource_usage, api::resource resource )
	{
		Role	role;

		if ( resource == 0 || ! roleOf ( device, desc, role ) )
			return;

		std::vector<Recreated>	recreated;
		{
			const std::scoped_lock	guard ( lock );
			adopt ( role, resource, recreated );
		}
		announce ( recreated );
	}

	void onDestroyResource ( api::device* device, api::resource resource )
	{
		std::vector<Recreated>	recreated;
		{
			const std::scoped_lock	guard ( lock );
			retire ( device, resource, recreated );
		}
		announce ( recreated );
	}

	void onInitSwapchain ( api::swapchain* swapchain, bool )
	{
		const auto				device = swapchain->get_device ();
		std::vector<Recreated>	recreated;
		{
			const std::scoped_lock	guard ( lock );
			auto&					handles = backBuffers[ swapchain ];
			handles.clear ();

			for ( uint32_t i = 0; i < swapchain->get_back_buffer_count (); ++i )
			{
				const auto	buffer = swapchain->get_back_buffer ( i );
				Role		role;
				role.device = device;
				role.backBuffer = i + 1;

				// A back buffer the game also created itself (D3D12 on Windows 7) keeps the ID it has
				if ( buffer == 0 || tracked.contains ( { device, buffer.handle } ) )
					continue;

				adopt ( role, buffer, recreated );
				handles.push_back ( buffer.handle );
			}
		}
		announce ( recreated );
	}

	// The back buffers as they were when the swap chain was set up: they may be gone from it by now
	void onDestroySwapchain ( api::swapchain* swapchain, bool )
	{
		const auto				device = swapchain->get_device ();
		std::vector<Recreated>	recreated;
		{
			const std::scoped_lock	guard ( lock );
			const auto				it = backBuffers.find ( swapchain );

			if ( it == backBuffers.end () )
				return;

			for ( const auto handle : it->second )
				retire ( device, api::resource { handle }, recreated );

			backBuffers.erase ( it );
		}
		announce ( recreated );
	}

	void onDestroyDevice ( api::device* device )
	{
		const std::scoped_lock	guard ( lock );
		std::erase_if ( tracked, [ device ] ( const auto& entry ) { return entry.first.device == device; } );
		std::erase_if ( retired, [ device ] ( const Retired& entry ) { return entry.role.device == device; } );
	}
}
//-----------------------------------------------------------------------------

// Registered from load on: an ID has to follow a resource from its creation. Nothing of it runs per frame
void register_addon_resource_ids ()
{
	reshade::register_event<reshade::addon_event::init_resource> ( onInitResource );
	reshade::register_event<reshade::addon_event::destroy_resource> ( onDestroyResource );
	reshade::register_event<reshade::addon_event::init_swapchain> ( onInitSwapchain );
	reshade::register_event<reshade::addon_event::destroy_swapchain> ( onDestroySwapchain );
	reshade::register_event<reshade::addon_event::destroy_device> ( onDestroyDevice );
}

void unregister_addon_resource_ids ()
{
	reshade::unregister_event<reshade::addon_event::init_resource> ( onInitResource );
	reshade::unregister_event<reshade::addon_event::destroy_resource> ( onDestroyResource );
	reshade::unregister_event<reshade::addon_event::init_swapchain> ( onInitSwapchain );
	reshade::unregister_event<reshade::addon_event::destroy_swapchain> ( onDestroySwapchain );
	reshade::unregister_event<reshade::addon_event::destroy_device> ( onDestroyDevice );

	const std::scoped_lock	guard ( lock );
	tracked.clear ();
	retired.clear ();
	backBuffers.clear ();
}
//-----------------------------------------------------------------------------

uint64_t ReShadeGetResourceStableId ( reshade::api::device* device, reshade::api::resource resource )
{
	const std::scoped_lock	guard ( lock );
	const auto				it = tracked.find ( { device, resource.handle } );
	return it != tracked.end () ? it->second.id : 0;
}
