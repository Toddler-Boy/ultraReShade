/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <deque>
#include <iterator>
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
#include "ultra_effect_reload.hpp"
#include "ultra_file_watch.hpp"

extern HMODULE g_module_handle;
extern std::filesystem::path g_reshade_base_path;

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

	constexpr auto		writtenCheckInterval = std::chrono::milliseconds ( 100 );
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
		std::filesystem::path	folder;				// where its copies go: the add-on folder it was found in
		HMODULE					module = nullptr;
		FILETIME				loadedTime {};		// file's write time when the copy was made
		bool					isChanged = false;	// the file watch reported a change since
	};

	std::vector<WatchedAddon>				watched;			// the presenting thread's, after loading
	std::filesystem::path					buildFolder;		// [ADDON] AddonBuildPath, set before loading
	size_t									copyNumber = 0;
	size_t									reloadCount = 0;
	std::atomic_bool						isReloadRequested = false;
	std::chrono::steady_clock::time_point	lastWrittenCheck;

	std::mutex								wantedMutex;
	std::vector<HMODULE>					wanted;				// add-ons that asked to reload

	using Callbacks = std::vector<std::pair<uint32_t, void*>>;

	struct ExclusiveCall
	{
		void ( *callback ) ( void* context ) = nullptr;
		void*		context = nullptr;
		HMODULE		owner = nullptr;
		std::string	caller;				// for the log
		uint64_t	number = 0;
	};

	std::mutex					callMutex;
	std::deque<ExclusiveCall>	calls;
	uint64_t					callCount = 0;
	std::atomic_bool			hasCalls = false;

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

	// name.loaded<process id>-<n> of another process that still runs
	[[ nodiscard ]] bool isCopyOfRunningProcess ( const std::filesystem::path& copy )
	{
		const auto	extension = copy.extension ().native ();
		const auto	dash = extension.find ( L'-' );

		if ( dash == std::wstring::npos )
			return false;

		const auto	start = std::size ( copyMarker ) - 1;
		const auto	id = dash > start ? static_cast<DWORD> ( std::wcstoul ( extension.substr ( start, dash - start ).c_str (), nullptr, 10 ) ) : DWORD ( 0 );

		if ( id == 0 || id == GetCurrentProcessId () )
			return false;

		const auto	process = OpenProcess ( SYNCHRONIZE, FALSE, id );

		if ( process == nullptr )
			return false;

		const auto	isRunning = WaitForSingleObject ( process, 0 ) == WAIT_TIMEOUT;
		CloseHandle ( process );
		return isRunning;
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

	// On the presenting thread, inside the file watch's delivery: the reload waits for pollAddonReload
	void onAddonFileChanged ( const char* path, api::file_change, const char* )
	{
		const auto	file = std::filesystem::u8path ( path );

		for ( auto& addon : watched )
			if ( _wcsicmp ( addon.file.lexically_normal ().c_str (), file.c_str () ) == 0 )
				addon.isChanged = true;
	}

	//-------------------------------------------------------------------------
	// The publish marker: a publish writes "publishing" into the build folder first and removes it last. Meanwhile nothing
	// reloads and no exclusive call runs; its removal has every add-on's file checked, so the set changes as one, whichever
	// changes the file watch delivered when. Only needed while add-ons and what they load must come from one build

	constexpr wchar_t	publishingMarker[] = L"publishing";

	bool	isPublishing = false;		// the presenting thread's, as the file watch's delivery

	void onPublishingChanged ( const char*, api::file_change change, const char* )
	{
		isPublishing = change != api::file_change::deleted;
		reshade::log::message ( reshade::log::level::info, isPublishing ? "A publish of the add-on build folder started: nothing reloads until it is complete."
																		: "The publish of the add-on build folder is complete." );
		if ( ! isPublishing )
			for ( auto& addon : watched )
				addon.isChanged = true;
	}

	void followPublishing ()
	{
		const auto	marker = buildFolder / publishingMarker;
		isPublishing = GetFileAttributesW ( marker.c_str () ) != INVALID_FILE_ATTRIBUTES;

		if ( ! reshade::ultra::watchFile ( marker, onPublishingChanged ) )
			reshade::log::message ( reshade::log::level::error, "'%s' is not watched: add-ons may reload from a publish that is not complete.", marker.u8string ().c_str () );
	}

	void stopFollowingPublishing ()
	{
		if ( ! buildFolder.empty () )
			reshade::ultra::unwatchFile ( buildFolder / publishingMarker, onPublishingChanged );
	}
	//-------------------------------------------------------------------------

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

		reshade::ultra::forgetFileWatches ( module );
		reshade::ultra::forgetExclusiveCalls ( module );
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
		addon.loadedCopy = reshade::ultra::addonLoadPath ( addon.file, addon.folder );

		if ( addon.loadedCopy.empty () )
			return nullptr;

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

	// False: an add-on holding something it can't let go of said so, and stays as it is
	[[ nodiscard ]] bool unloadAddon ( WatchedAddon& addon, const LiveObjects& objects )
	{
		if ( addon.module == nullptr )
			return true;

		const auto	canReload = reinterpret_cast<bool ( * ) ()> ( GetProcAddress ( addon.module, "AddonCanReload" ) );

		if ( canReload != nullptr && ! canReload () )
		{
			reshade::log::message ( reshade::log::level::warning, "The add-on in '%s' can't reload now; it stays as it is.", addon.file.u8string ().c_str () );
			addon.loadedTime = writeTime ( addon.file );
			reshade::ultra::forgetExclusiveCalls ( addon.module );		// they belong to the set that did not come
			return false;
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

		return true;
	}

	void loadAgain ( WatchedAddon& addon, const LiveObjects& objects, std::chrono::steady_clock::time_point start )
	{
		addon.module = loadAddon ( addon );

		if ( addon.module == nullptr )
			return;

		replayCreation ( registeredAddon ( addon.module )->event_callbacks, objects );

		const auto	took = std::chrono::duration_cast<std::chrono::milliseconds> ( std::chrono::steady_clock::now () - start );
		reshade::log::message ( reshade::log::level::info, "Reloaded add-on from '%s' in %lld ms.", addon.file.u8string ().c_str (), static_cast<long long> ( took.count () ) );
	}

	// Nothing runs an add-on event meanwhile: other threads wait at their next event. The GPU is idle first, so no
	// work in flight uses what work frees
	template <typename Work>
	void runExclusively ( const Work& work )
	{
		AcquireSRWLockExclusive ( &eventLock );
		eventDepth += reloadDepth;

		std::vector<api::command_queue*>	queues;
		{
			const std::scoped_lock	lock ( liveMutex );
			queues = live.queues;
		}

		for ( const auto queue : queues )
		{
			queue->flush_immediate_command_list ();
			queue->wait_idle ();
		}

		work ();

		eventDepth -= reloadDepth;
		ReleaseSRWLockExclusive ( &eventLock );
	}

	void reloadAddons ( const std::vector<size_t>& due )
	{
		runExclusively ( [ &due ]
		{
			LiveObjects	objects;
			{
				const std::scoped_lock	lock ( liveMutex );
				objects = live;
			}

			++reloadCount;
			const auto	start = std::chrono::steady_clock::now ();

			// Out from the last loaded, in from the first: an add-on may use those loaded before it
			auto	out = due.rbegin ();
			while ( out != due.rend () && unloadAddon ( watched[ *out ], objects ) )
				++out;

			// Those before one that stays stay too, and try again at the next check
			if ( out != due.rend () )
				for ( auto before = std::next ( out ); before != due.rend (); ++before )
					watched[ *before ].isChanged = true;

			for ( auto in = out.base (); in != due.end (); ++in )
				loadAgain ( watched[ *in ], objects, start );
		} );

		logProcessUse ();
	}

	// The calls queued before this present, in order; one a callback queues waits for the next present
	void runExclusiveCalls ()
	{
		if ( ! hasCalls )
			return;

		uint64_t	last = 0;
		{
			const std::scoped_lock	lock ( callMutex );
			last = callCount;
		}

		runExclusively ( [ last ]
		{
			for ( ;; )
			{
				ExclusiveCall	call;
				{
					const std::scoped_lock	lock ( callMutex );

					if ( calls.empty () || calls.front ().number > last )
						break;

					call = std::move ( calls.front () );
					calls.pop_front ();
					hasCalls = ! calls.empty ();
				}

				const auto	start = std::chrono::steady_clock::now ();
				call.callback ( call.context );

				const auto	took = std::chrono::duration_cast<std::chrono::milliseconds> ( std::chrono::steady_clock::now () - start );
				reshade::log::message ( reshade::log::level::info, "Ran an exclusive call of %s in %lld ms.", call.caller.c_str (), static_cast<long long> ( took.count () ) );
			}
		} );
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

bool reshade::ultra::isInAddonEvent ()
{
	return eventDepth != 0;
}
//-----------------------------------------------------------------------------

void reshade::ultra::startAddonReload ( const std::filesystem::path& addonFolder )
{
	isAddonReloadEnabled = true;
	log::message ( log::level::info, "Add-ons reload when their file changes, effects when theirs do." );

	// Copies of processes that ended go; another running process of the same folder keeps its own
	std::error_code	error;

	for ( const auto& entry : std::filesystem::directory_iterator ( addonFolder, std::filesystem::directory_options::skip_permission_denied, error ) )
		if ( entry.path ().extension ().native ().starts_with ( copyMarker ) && ! isCopyOfRunningProcess ( entry.path () ) )
			DeleteFileW ( entry.path ().c_str () );

	if ( std::filesystem::path build; global_config ().get ( "ADDON", "AddonBuildPath", build ) && ! build.empty () )
	{
		buildFolder = g_reshade_base_path / build;
		log::message ( log::level::info, "Add-ons with a file of their name in '%s' load from that file.", buildFolder.u8string ().c_str () );
		followPublishing ();
	}

	trackObjects ( true );
}
//-----------------------------------------------------------------------------

void reshade::ultra::stopAddonReload ()
{
	if ( ! isAddonReloadEnabled )
		return;

	trackObjects ( false );
	stopEffectReload ();
	stopFollowingPublishing ();

	for ( const auto& addon : watched )
	{
		unwatchFile ( addon.file, onAddonFileChanged );
		DeleteFileW ( addon.loadedCopy.c_str () );
	}

	watched.clear ();

	const std::scoped_lock	lock ( callMutex );
	calls.clear ();
	hasCalls = false;
}
//-----------------------------------------------------------------------------

std::filesystem::path reshade::ultra::addonSource ( const std::filesystem::path& found )
{
	if ( buildFolder.empty () )
		return found;

	auto	built = buildFolder / found.filename ();
	if ( std::error_code error; ! std::filesystem::is_regular_file ( built, error ) )
		return found;

	log::message ( log::level::info, "Add-on '%s' loads from '%s'.", found.u8string ().c_str (), built.u8string ().c_str () );
	return built;
}
//-----------------------------------------------------------------------------

std::filesystem::path reshade::ultra::addonSourceOf ( HMODULE module )
{
	const auto	it = std::ranges::find ( watched, module, &WatchedAddon::module );
	return it != watched.end () ? it->file : get_module_path ( module );
}
//-----------------------------------------------------------------------------

// name.addon64 loads as name.loaded<process id>-<n> in the add-on folder: the add-on still finds the files next to it, files it
// names after itself keep their name (name.log), and several processes of one folder never want the same copy
std::filesystem::path reshade::ultra::addonLoadPath ( const std::filesystem::path& file, const std::filesystem::path& folder )
{
	if ( ! isAddonReloadEnabled )
		return file;

	auto	copy = folder / file.filename ();
	copy.replace_extension ( copyMarker + std::to_wstring ( GetCurrentProcessId () ) + L"-" + std::to_wstring ( ++copyNumber ) );

	if ( ! CopyFileW ( file.c_str (), copy.c_str (), FALSE ) )
	{
		log::message ( log::level::error, "Add-on '%s' is not loaded: its copy '%s' could not be made (error code %lu).", file.u8string ().c_str (), copy.u8string ().c_str (), GetLastError () );
		return {};
	}

	return copy;
}
//-----------------------------------------------------------------------------

void reshade::ultra::watchAddon ( HMODULE module, const std::filesystem::path& file )
{
	if ( ! isAddonReloadEnabled )
		return;

	const auto	loadedCopy = get_module_path ( module );

	if ( const auto info = registeredAddon ( module ); info != nullptr )
		info->file = file.filename ().u8string ();

	if ( ! watchFile ( file, onAddonFileChanged ) )
		log::message ( log::level::error, "Add-on '%s' can't reload: its file is not watched.", file.u8string ().c_str () );

	watched.push_back ( { file, loadedCopy, loadedCopy.parent_path (), module, writeTime ( file ) } );
}
//-----------------------------------------------------------------------------

// A build is done once the file watch reported a change and nobody writes the file any more. The exclusive calls run after
// the reloads, and wait while an add-on's file changed: the call may belong to the add-on that is about to reload
void reshade::ultra::pollAddonReload ()
{
	if ( ! isAddonReloadEnabled || eventDepth != 0 || isPublishing )
		return;

	if ( watched.empty () )
		return runExclusiveCalls ();

	const auto	isRequested = isReloadRequested.exchange ( false );
	const auto	now = std::chrono::steady_clock::now ();
	const auto	isCheckDue = now - lastWrittenCheck >= writtenCheckInterval;
	auto		hasChecked = false;

	std::vector<HMODULE>	asked;
	{
		const std::scoped_lock	lock ( wantedMutex );
		asked.swap ( wanted );
	}

	for ( const auto module : asked )
		if ( std::ranges::none_of ( watched, [ module ] ( const WatchedAddon& addon ) { return addon.module == module; } ) )
			if ( const auto info = registeredAddon ( module ); info != nullptr )
				log::message ( log::level::warning, "Add-on \"%s\" asked to reload, but it loaded from its file and can't.", info->name.c_str () );

	std::vector<size_t>	due;

	for ( size_t i = 0; i < watched.size (); ++i )
	{
		auto&	addon = watched[ i ];

		if ( isRequested || std::ranges::contains ( asked, addon.module ) )
		{
			addon.isChanged = false;
			due.push_back ( i );
			continue;
		}

		if ( ! addon.isChanged || ! isCheckDue )
			continue;

		hasChecked = true;
		const auto	time = writeTime ( addon.file );

		// A deleted file waits for its new copy
		if ( isSameTime ( time, FILETIME {} ) )
			continue;

		if ( isSameTime ( time, addon.loadedTime ) )
			addon.isChanged = false;
		else if ( isWritten ( addon.file ) )
		{
			addon.isChanged = false;
			due.push_back ( i );
		}
	}

	if ( hasChecked )
		lastWrittenCheck = now;

	// The add-ons loaded after one reload with it, once their files are written too
	std::vector<size_t>	batch;

	if ( ! due.empty () )
		for ( auto i = due.front (); i < watched.size (); ++i )
			batch.push_back ( i );

	if ( std::ranges::any_of ( batch, [] ( size_t i ) { return ! isWritten ( watched[ i ].file ); } ) )
	{
		for ( const auto i : due )
			watched[ i ].isChanged = true;
	}
	else if ( ! batch.empty () )
		reloadAddons ( batch );

	if ( std::ranges::none_of ( watched, &WatchedAddon::isChanged ) )
		runExclusiveCalls ();
}
//-----------------------------------------------------------------------------

void reshade::ultra::requestAddonReload ()
{
	isReloadRequested = true;
}
//-----------------------------------------------------------------------------

bool reshade::ultra::requestAddonReload ( HMODULE owner )
{
	if ( ! isAddonReloadEnabled )
		return false;

	const std::scoped_lock	lock ( wantedMutex );
	wanted.push_back ( owner );
	return true;
}
//-----------------------------------------------------------------------------

bool reshade::ultra::runExclusive ( HMODULE owner, void ( *callback ) ( void* context ), void* context )
{
	const auto	info = owner != g_module_handle ? registeredAddon ( owner ) : nullptr;

	if ( ! isAddonReloadEnabled || callback == nullptr || ( info == nullptr && owner != g_module_handle ) )
	{
		log::message ( log::level::error, ! isAddonReloadEnabled ? "An exclusive call was refused: add-ons were not loaded for reloading."
										  : callback == nullptr ? "An exclusive call without a callback was refused."
																: "An exclusive call was refused: its module is not a registered add-on." );
		return false;
	}

	const auto	caller = info != nullptr ? "add-on \"" + info->name + "\"" : std::string ( "ReShade" );

	const std::scoped_lock	lock ( callMutex );
	calls.push_back ( { callback, context, owner, caller, ++callCount } );
	hasCalls = true;
	return true;
}
//-----------------------------------------------------------------------------

void reshade::ultra::forgetExclusiveCalls ( HMODULE owner )
{
	const std::scoped_lock	lock ( callMutex );
	std::erase_if ( calls, [ owner ] ( const ExclusiveCall& call ) { return call.owner == owner; } );
	hasCalls = ! calls.empty ();
}
//-----------------------------------------------------------------------------

