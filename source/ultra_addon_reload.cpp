/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if RESHADE_ADDON

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <Windows.h>
#include <Psapi.h>

#include <reshade.hpp>

#include "addon_manager.hpp"
#include "dll_log.hpp"
#include "ini_file.hpp"
#include "ultra_addon_reload.hpp"

extern HMODULE g_module_handle;
extern std::filesystem::path get_module_path ( HMODULE module );

bool reshade::ultra::isAddonReloadEnabled = false;

//-----------------------------------------------------------------------------

namespace
{
	namespace api = reshade::api;
	using reshade::addon_event;

	// Events take it shared; a reload takes it exclusively, so it never swaps an add-on under a running event
	SRWLOCK				eventLock = SRWLOCK_INIT;
	thread_local int	eventDepth = 0;
	constexpr int		reloadDepth = 1 << 20;		// events the reload itself causes run on its thread without the lock

	constexpr auto		pollInterval = std::chrono::milliseconds ( 500 );
	constexpr wchar_t	copyMarker[] = L".loaded";

	struct TrackedSampler
	{
		api::device*		device = nullptr;
		api::sampler_desc	desc {};
	};

	struct TrackedResource
	{
		api::device*		device = nullptr;
		api::resource_desc	desc {};
		api::resource_usage	initialState = api::resource_usage::undefined;
	};

	struct TrackedView
	{
		api::device*			device = nullptr;
		api::resource			resource {};
		api::resource_usage		usage = api::resource_usage::undefined;
		api::resource_view_desc	desc {};
	};

	struct TrackedQueryHeap
	{
		api::device*	device = nullptr;
		api::query_type	type = api::query_type::occlusion;
		uint32_t		count = 0;
	};

	// Everything alive that the add-ons were told about; pipelines and their layouts are not replayed (their
	// descriptions only live during creation)
	struct LiveObjects
	{
		std::vector<api::device*>							devices;
		std::vector<api::command_queue*>					queues;
		std::vector<api::command_list*>						lists;
		std::vector<api::swapchain*>						swapchains;
		std::vector<api::effect_runtime*>					runtimes;
		std::unordered_map<uint64_t, TrackedSampler>		samplers;
		std::unordered_map<uint64_t, TrackedResource>		resources;
		std::unordered_map<uint64_t, TrackedView>			views;
		std::unordered_map<uint64_t, TrackedQueryHeap>		queryHeaps;
	};

	std::mutex		liveMutex;
	LiveObjects		live;

	// An add-on loaded from a copy of file
	struct WatchedAddon
	{
		std::filesystem::path	file;
		std::filesystem::path	loadedCopy;
		HMODULE					module = nullptr;
		FILETIME				loadedTime {};		// file's write time when the copy was made
		FILETIME				polledTime {};		// file's write time at the last poll: a build writing it changes it
	};

	std::vector<WatchedAddon>				watched;			// the presenting thread's, after loading
	size_t									copyNumber = 0;
	size_t									reloadCount = 0;
	std::atomic_bool						isReloadRequested = false;
	std::chrono::steady_clock::time_point	lastPoll;

	using Callbacks = std::vector<std::pair<uint32_t, void*>>;

	//-------------------------------------------------------------------------

	template <typename T>
	void addObject ( std::vector<T*>& list, T* object )
	{
		const std::scoped_lock	lock ( liveMutex );
		list.push_back ( object );
	}

	template <typename T>
	void removeObject ( std::vector<T*>& list, T* object )
	{
		const std::scoped_lock	lock ( liveMutex );
		std::erase ( list, object );
	}

	template <typename T>
	void removeHandle ( std::unordered_map<uint64_t, T>& map, uint64_t handle )
	{
		const std::scoped_lock	lock ( liveMutex );
		map.erase ( handle );
	}

	void onInitDevice ( api::device* device ) { addObject ( live.devices, device ); }
	void onDestroyDevice ( api::device* device ) { removeObject ( live.devices, device ); }
	void onInitQueue ( api::command_queue* queue ) { addObject ( live.queues, queue ); }
	void onDestroyQueue ( api::command_queue* queue ) { removeObject ( live.queues, queue ); }
	void onInitList ( api::command_list* list ) { addObject ( live.lists, list ); }
	void onDestroyList ( api::command_list* list ) { removeObject ( live.lists, list ); }
	void onInitSwapchain ( api::swapchain* swapchain, bool ) { addObject ( live.swapchains, swapchain ); }
	void onDestroySwapchain ( api::swapchain* swapchain, bool ) { removeObject ( live.swapchains, swapchain ); }
	void onInitRuntime ( api::effect_runtime* runtime ) { addObject ( live.runtimes, runtime ); }
	void onDestroyRuntime ( api::effect_runtime* runtime ) { removeObject ( live.runtimes, runtime ); }

	void onInitSampler ( api::device* device, const api::sampler_desc& desc, api::sampler sampler )
	{
		const std::scoped_lock	lock ( liveMutex );
		live.samplers[ sampler.handle ] = { device, desc };
	}

	void onDestroySampler ( api::device*, api::sampler sampler ) { removeHandle ( live.samplers, sampler.handle ); }

	void onInitResource ( api::device* device, const api::resource_desc& desc, const api::subresource_data*, api::resource_usage initialState, api::resource resource )
	{
		const std::scoped_lock	lock ( liveMutex );
		live.resources[ resource.handle ] = { device, desc, initialState };
	}

	void onDestroyResource ( api::device*, api::resource resource ) { removeHandle ( live.resources, resource.handle ); }

	void onInitView ( api::device* device, api::resource resource, api::resource_usage usage, const api::resource_view_desc& desc, api::resource_view view )
	{
		const std::scoped_lock	lock ( liveMutex );
		live.views[ view.handle ] = { device, resource, usage, desc };
	}

	void onDestroyView ( api::device*, api::resource_view view ) { removeHandle ( live.views, view.handle ); }

	void onInitQueryHeap ( api::device* device, api::query_type type, uint32_t count, api::query_heap heap )
	{
		const std::scoped_lock	lock ( liveMutex );
		live.queryHeaps[ heap.handle ] = { device, type, count };
	}

	void onDestroyQueryHeap ( api::device*, api::query_heap heap ) { removeHandle ( live.queryHeaps, heap.handle ); }

	//-------------------------------------------------------------------------

	// Calls only the given add-on's callbacks for one event
	template <addon_event ev, typename... Args>
	void send ( const Callbacks& callbacks, Args&&... args )
	{
		for ( const auto& [ event, callback ] : callbacks )
			if ( event == static_cast<uint32_t> ( ev ) )
				reinterpret_cast<typename reshade::addon_event_traits<ev>::decl> ( callback ) ( args... );
	}

	// The add-on sees everything alive created, in the order a game creates it
	void replayCreation ( const Callbacks& callbacks, const LiveObjects& objects )
	{
		for ( const auto device : objects.devices )
			send<addon_event::init_device> ( callbacks, device );

		for ( const auto queue : objects.queues )
			send<addon_event::init_command_queue> ( callbacks, queue );

		for ( const auto list : objects.lists )
			send<addon_event::init_command_list> ( callbacks, list );

		for ( const auto& [ handle, sampler ] : objects.samplers )
			send<addon_event::init_sampler> ( callbacks, sampler.device, sampler.desc, api::sampler { handle } );

		for ( const auto& [ handle, resource ] : objects.resources )
			send<addon_event::init_resource> ( callbacks, resource.device, resource.desc, static_cast<const api::subresource_data*> ( nullptr ), resource.initialState, api::resource { handle } );

		for ( const auto& [ handle, view ] : objects.views )
			send<addon_event::init_resource_view> ( callbacks, view.device, view.resource, view.usage, view.desc, api::resource_view { handle } );

		for ( const auto& [ handle, heap ] : objects.queryHeaps )
			send<addon_event::init_query_heap> ( callbacks, heap.device, heap.type, heap.count, api::query_heap { handle } );

		for ( const auto swapchain : objects.swapchains )
			send<addon_event::init_swapchain> ( callbacks, swapchain, false );

		// The runtimes' effects are loaded already: the add-on hears that too, as after a game's first load
		for ( const auto runtime : objects.runtimes )
		{
			send<addon_event::init_effect_runtime> ( callbacks, runtime );

			size_t	size = 0;
			runtime->get_current_preset_path ( nullptr, &size );
			std::string	preset ( size, '\0' );
			runtime->get_current_preset_path ( preset.data (), &size );
			preset.resize ( std::min ( size, preset.size () ) );		// filled, the size no longer counts the terminating null

			send<addon_event::reshade_set_current_preset_path> ( callbacks, runtime, preset.c_str () );
			send<addon_event::reshade_reloaded_effects> ( callbacks, runtime );
		}
	}

	// The reverse: the add-on sees everything alive destroyed, so it frees what it keeps per object
	void replayDestruction ( const Callbacks& callbacks, const LiveObjects& objects )
	{
		for ( auto it = objects.runtimes.rbegin (); it != objects.runtimes.rend (); ++it )
			send<addon_event::destroy_effect_runtime> ( callbacks, *it );

		for ( auto it = objects.swapchains.rbegin (); it != objects.swapchains.rend (); ++it )
			send<addon_event::destroy_swapchain> ( callbacks, *it, false );

		for ( const auto& [ handle, heap ] : objects.queryHeaps )
			send<addon_event::destroy_query_heap> ( callbacks, heap.device, api::query_heap { handle } );

		for ( const auto& [ handle, view ] : objects.views )
			send<addon_event::destroy_resource_view> ( callbacks, view.device, api::resource_view { handle } );

		for ( const auto& [ handle, resource ] : objects.resources )
			send<addon_event::destroy_resource> ( callbacks, resource.device, api::resource { handle } );

		for ( const auto& [ handle, sampler ] : objects.samplers )
			send<addon_event::destroy_sampler> ( callbacks, sampler.device, api::sampler { handle } );

		for ( auto it = objects.lists.rbegin (); it != objects.lists.rend (); ++it )
			send<addon_event::destroy_command_list> ( callbacks, *it );

		for ( auto it = objects.queues.rbegin (); it != objects.queues.rend (); ++it )
			send<addon_event::destroy_command_queue> ( callbacks, *it );

		for ( auto it = objects.devices.rbegin (); it != objects.devices.rend (); ++it )
			send<addon_event::destroy_device> ( callbacks, *it );
	}

	//-------------------------------------------------------------------------

	[[ nodiscard ]] FILETIME writeTime ( const std::filesystem::path& file )
	{
		WIN32_FILE_ATTRIBUTE_DATA	data {};
		return GetFileAttributesExW ( file.c_str (), GetFileExInfoStandard, &data ) ? data.ftLastWriteTime : FILETIME {};
	}

	[[ nodiscard ]] bool isSameTime ( const FILETIME& a, const FILETIME& b )
	{
		return CompareFileTime ( &a, &b ) == 0;
	}

	// Nobody writes the file any more: it opens while denying writers
	[[ nodiscard ]] bool isWritten ( const std::filesystem::path& file )
	{
		const auto	handle = CreateFileW ( file.c_str (), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );

		if ( handle == INVALID_HANDLE_VALUE )
			return false;

		CloseHandle ( handle );
		return true;
	}

	[[ nodiscard ]] reshade::addon_info* registeredAddon ( HMODULE module )
	{
		for ( auto& info : reshade::addon_loaded_info )
			if ( info.handle == module )
				return &info;

		return nullptr;
	}

	// Drops what an unloaded add-on left registered (a well-behaved one unregisters itself while it unloads)
	void forgetAddon ( HMODULE module )
	{
		const auto	info = registeredAddon ( module );

		if ( info == nullptr )
			return;

		reshade::log::message ( reshade::log::level::warning, "Add-on \"%s\" was not unregistered when it unloaded.", info->name.c_str () );

		for ( const auto& [ event, callback ] : info->event_callbacks )
			std::erase ( reshade::addon_event_list[ event ], callback );

		reshade::addon_loaded_info.erase ( reshade::addon_loaded_info.begin () + ( info - reshade::addon_loaded_info.data () ) );
	}

	void logProcessUse ()
	{
		PROCESS_MEMORY_COUNTERS_EX	memory {};
		DWORD						handles = 0;
		GetProcessMemoryInfo ( GetCurrentProcess (), reinterpret_cast<PROCESS_MEMORY_COUNTERS*> ( &memory ), sizeof ( memory ) );
		GetProcessHandleCount ( GetCurrentProcess (), &handles );

		reshade::log::message ( reshade::log::level::info, "Add-on reload %zu: the process uses %.1f MB of private memory and %lu handles.", reloadCount,
								static_cast<double> ( memory.PrivateUsage ) / ( 1024.0 * 1024.0 ), handles );
	}

	// Loads a copy of the add-on's file and lets it initialize; null when it fails or doesn't register
	[[ nodiscard ]] HMODULE loadAddon ( WatchedAddon& addon )
	{
		addon.loadedTime = writeTime ( addon.file );
		addon.loadedCopy = reshade::ultra::addonLoadPath ( addon.file );

		const auto	module = LoadLibraryExW ( addon.loadedCopy.c_str (), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS );

		if ( module == nullptr )
		{
			reshade::log::message ( reshade::log::level::error, "Failed to load add-on from '%s' with error code %lu!", addon.loadedCopy.u8string ().c_str (), GetLastError () );
			return nullptr;
		}

		const auto	init = reinterpret_cast<bool ( * ) ( HMODULE, HMODULE )> ( GetProcAddress ( module, "AddonInit" ) );

		if ( ( init != nullptr && ! init ( module, g_module_handle ) ) || registeredAddon ( module ) == nullptr )
		{
			reshade::log::message ( reshade::log::level::error, "The add-on in '%s' did not initialize or register after reloading.", addon.file.u8string ().c_str () );
			forgetAddon ( module );
			FreeLibrary ( module );
			return nullptr;
		}

		registeredAddon ( module )->file = addon.file.filename ().u8string ();
		return module;
	}

	void reloadAddon ( WatchedAddon& addon, const LiveObjects& objects )
	{
		const auto	start = std::chrono::steady_clock::now ();

		if ( addon.module != nullptr )
		{
			// An add-on holding something it can't let go of says so, and stays
			const auto	canReload = reinterpret_cast<bool ( * ) ()> ( GetProcAddress ( addon.module, "AddonCanReload" ) );

			if ( canReload != nullptr && ! canReload () )
			{
				reshade::log::message ( reshade::log::level::warning, "The add-on in '%s' can't reload now; it stays as it is.", addon.file.u8string ().c_str () );
				addon.loadedTime = writeTime ( addon.file );
				return;
			}

			if ( const auto info = registeredAddon ( addon.module ); info != nullptr )
			{
				reshade::log::message ( reshade::log::level::info, "Reloading add-on \"%s\" ...", info->name.c_str () );
				replayDestruction ( info->event_callbacks, objects );
			}

			if ( const auto uninit = reinterpret_cast<void ( * ) ( HMODULE, HMODULE )> ( GetProcAddress ( addon.module, "AddonUninit" ) ); uninit != nullptr )
				uninit ( addon.module, g_module_handle );

			const auto	oldModule = addon.module;
			addon.module = nullptr;
			FreeLibrary ( oldModule );
			forgetAddon ( oldModule );

			if ( GetModuleHandleW ( addon.loadedCopy.c_str () ) != nullptr )
				reshade::log::message ( reshade::log::level::error, "The old add-on in '%s' is still loaded: something else holds it.", addon.loadedCopy.u8string ().c_str () );
			else
				DeleteFileW ( addon.loadedCopy.c_str () );
		}

		addon.module = loadAddon ( addon );

		if ( addon.module == nullptr )
			return;

		replayCreation ( registeredAddon ( addon.module )->event_callbacks, objects );

		const auto	took = std::chrono::duration_cast<std::chrono::milliseconds> ( std::chrono::steady_clock::now () - start );
		reshade::log::message ( reshade::log::level::info, "Reloaded add-on from '%s' in %lld ms.", addon.file.u8string ().c_str (), static_cast<long long> ( took.count () ) );
	}

	// Nothing runs an add-on event meanwhile: other threads wait at their next event. The GPU is idle first, so no
	// work in flight uses what the old add-on frees
	void reloadAddons ( const std::vector<size_t>& due )
	{
		AcquireSRWLockExclusive ( &eventLock );
		eventDepth += reloadDepth;

		LiveObjects	objects;
		{
			const std::scoped_lock	lock ( liveMutex );
			objects = live;
		}

		for ( const auto queue : objects.queues )
		{
			queue->flush_immediate_command_list ();
			queue->wait_idle ();
		}

		++reloadCount;

		for ( const auto index : due )
			reloadAddon ( watched[ index ], objects );

		eventDepth -= reloadDepth;
		ReleaseSRWLockExclusive ( &eventLock );

		logProcessUse ();
	}

	template <addon_event ev, typename T>
	void track ( bool isTracking, T callback )
	{
		if ( isTracking )
			reshade::register_event<ev> ( callback );
		else
			reshade::unregister_event<ev> ( callback );
	}

	void trackObjects ( bool isTracking )
	{
		track<addon_event::init_device> ( isTracking, onInitDevice );
		track<addon_event::destroy_device> ( isTracking, onDestroyDevice );
		track<addon_event::init_command_queue> ( isTracking, onInitQueue );
		track<addon_event::destroy_command_queue> ( isTracking, onDestroyQueue );
		track<addon_event::init_command_list> ( isTracking, onInitList );
		track<addon_event::destroy_command_list> ( isTracking, onDestroyList );
		track<addon_event::init_swapchain> ( isTracking, onInitSwapchain );
		track<addon_event::destroy_swapchain> ( isTracking, onDestroySwapchain );
		track<addon_event::init_effect_runtime> ( isTracking, onInitRuntime );
		track<addon_event::destroy_effect_runtime> ( isTracking, onDestroyRuntime );
		track<addon_event::init_sampler> ( isTracking, onInitSampler );
		track<addon_event::destroy_sampler> ( isTracking, onDestroySampler );
		track<addon_event::init_resource> ( isTracking, onInitResource );
		track<addon_event::destroy_resource> ( isTracking, onDestroyResource );
		track<addon_event::init_resource_view> ( isTracking, onInitView );
		track<addon_event::destroy_resource_view> ( isTracking, onDestroyView );
		track<addon_event::init_query_heap> ( isTracking, onInitQueryHeap );
		track<addon_event::destroy_query_heap> ( isTracking, onDestroyQueryHeap );
	}
}
//-----------------------------------------------------------------------------

reshade::ultra::AddonEventGuard::AddonEventGuard () : isCounted ( isAddonReloadEnabled )
{
	if ( isCounted && eventDepth++ == 0 )
		AcquireSRWLockShared ( &eventLock );
}
//-----------------------------------------------------------------------------

reshade::ultra::AddonEventGuard::~AddonEventGuard ()
{
	if ( isCounted && --eventDepth == 0 )
		ReleaseSRWLockShared ( &eventLock );
}
//-----------------------------------------------------------------------------

void reshade::ultra::startAddonReload ( const std::filesystem::path& addonFolder )
{
	isAddonReloadEnabled = global_config ().get ( "ADDON", "ReloadOnChange" );

	if ( ! isAddonReloadEnabled )
		return;

	log::message ( log::level::info, "Add-ons reload when their file changes." );

	// Copies of a previous run are no longer loaded; copies a running game still holds stay
	std::error_code	error;

	for ( const auto& entry : std::filesystem::directory_iterator ( addonFolder, std::filesystem::directory_options::skip_permission_denied, error ) )
		if ( entry.path ().extension ().native ().starts_with ( copyMarker ) )
			DeleteFileW ( entry.path ().c_str () );

	trackObjects ( true );
}
//-----------------------------------------------------------------------------

void reshade::ultra::stopAddonReload ()
{
	if ( ! isAddonReloadEnabled )
		return;

	trackObjects ( false );

	for ( const auto& addon : watched )
		DeleteFileW ( addon.loadedCopy.c_str () );

	watched.clear ();
}
//-----------------------------------------------------------------------------

// name.addon64 loads as name.loaded<n> in the same folder: the add-on still finds the files next to it, and files it
// names after itself keep their name (name.log)
std::filesystem::path reshade::ultra::addonLoadPath ( const std::filesystem::path& file )
{
	if ( ! isAddonReloadEnabled )
		return file;

	auto	copy = file;
	copy.replace_extension ( copyMarker + std::to_wstring ( ++copyNumber ) );

	if ( ! CopyFileW ( file.c_str (), copy.c_str (), FALSE ) )
	{
		log::message ( log::level::error, "Failed to copy add-on '%s' for loading (error code %lu); it loads from its file and can't reload.", file.u8string ().c_str (), GetLastError () );
		return file;
	}

	return copy;
}
//-----------------------------------------------------------------------------

void reshade::ultra::watchAddon ( HMODULE module, const std::filesystem::path& file )
{
	if ( ! isAddonReloadEnabled )
		return;

	const auto	loadedCopy = get_module_path ( module );

	if ( loadedCopy == file )
		return;

	if ( const auto info = registeredAddon ( module ); info != nullptr )
		info->file = file.filename ().u8string ();

	const auto	time = writeTime ( file );
	watched.push_back ( { file, loadedCopy, module, time, time } );
}
//-----------------------------------------------------------------------------

// A build is done once the file's time stayed the same over one poll and nobody writes it
void reshade::ultra::pollAddonReload ()
{
	if ( ! isAddonReloadEnabled || eventDepth != 0 || watched.empty () )
		return;

	const auto	isRequested = isReloadRequested.exchange ( false );
	const auto	now = std::chrono::steady_clock::now ();

	if ( ! isRequested && now - lastPoll < pollInterval )
		return;

	lastPoll = now;

	std::vector<size_t>	due;

	for ( size_t i = 0; i < watched.size (); ++i )
	{
		auto&		addon = watched[ i ];
		const auto	time = writeTime ( addon.file );
		const auto	isChanged = ! isSameTime ( time, FILETIME {} ) && ! isSameTime ( time, addon.loadedTime );

		if ( isRequested || ( isChanged && isSameTime ( time, addon.polledTime ) && isWritten ( addon.file ) ) )
			due.push_back ( i );

		addon.polledTime = time;
	}

	if ( ! due.empty () )
		reloadAddons ( due );
}
//-----------------------------------------------------------------------------

void reshade::ultra::requestAddonReload ()
{
	isReloadRequested = true;
}
//-----------------------------------------------------------------------------

#endif
