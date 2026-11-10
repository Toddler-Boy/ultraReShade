/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if RESHADE_ADDON

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>

#include <reshade.hpp>

#include "dll_log.hpp"
#include "runtime.hpp"
#include "runtime_internal.hpp"
#include "ultra_addon_reload.hpp"
#include "ultra_effect_reload.hpp"
#include "ultra_file_watch.hpp"

extern std::filesystem::path g_reshade_base_path;
extern bool resolve_path ( std::filesystem::path& path, std::error_code& ec, const std::filesystem::path& base );

//-----------------------------------------------------------------------------

namespace
{
	namespace api = reshade::api;
	using Clock = std::chrono::steady_clock;
	using Paths = std::vector<std::filesystem::path>;

	// A deploy writes the add-on and several effect files one after the other: one recompile once they are all there
	constexpr auto		quietTime = std::chrono::seconds ( 1 );
	constexpr auto		writtenCheckInterval = std::chrono::milliseconds ( 100 );
	constexpr std::array	patterns = { L"*.fx", L"*.fxh", L"*.addonfx" };

	struct Watcher
	{
		const void*			runtime = nullptr;
		Paths				searchPaths;		// as configured
		Paths				folders;			// resolved, "**" expanded into every folder below
		Paths				changed;
		Clock::time_point	lastChange;
		Clock::time_point	lastWrittenCheck;
	};

	std::mutex				lock;
	std::vector<Watcher>	watchers;
	Paths					watchedFolders;		// every watcher's folders

	//-------------------------------------------------------------------------

	[[ nodiscard ]] bool isSamePath ( const std::filesystem::path& a, const std::filesystem::path& b )
	{
		return _wcsicmp ( a.c_str (), b.c_str () ) == 0;
	}

	[[ nodiscard ]] bool contains ( const Paths& paths, const std::filesystem::path& path )
	{
		return std::ranges::any_of ( paths, [ &path ] ( const std::filesystem::path& other ) { return isSamePath ( other, path ); } );
	}

	[[ nodiscard ]] bool isSameSpelling ( const Paths& a, const Paths& b )
	{
		return std::ranges::equal ( a, b, [] ( const std::filesystem::path& x, const std::filesystem::path& y ) { return x.native () == y.native (); } );
	}

	void addFolder ( Paths& folders, std::filesystem::path folder )
	{
		folder = folder.lexically_normal ();

		if ( ! folder.has_filename () && folder.has_relative_path () )
			folder = folder.parent_path ();

		if ( ! contains ( folders, folder ) )
			folders.push_back ( std::move ( folder ) );
	}

	// As the runtime finds effect files; a folder created below a "**" path later is not watched
	[[ nodiscard ]] Paths resolveFolders ( const Paths& searchPaths )
	{
		Paths			folders;
		std::error_code	error;

		for ( auto path : searchPaths )
		{
			const auto	isRecursive = path.filename () == L"**";

			if ( isRecursive )
				path.remove_filename ();

			if ( ! resolve_path ( path, error, g_reshade_base_path ) )
				continue;

			addFolder ( folders, path );

			if ( ! isRecursive )
				continue;

			for ( const auto& entry : std::filesystem::recursive_directory_iterator ( path, std::filesystem::directory_options::skip_permission_denied, error ) )
				if ( entry.is_directory ( error ) )
					addFolder ( folders, entry.path () );
		}

		return folders;
	}

	[[ nodiscard ]] Watcher* watcherOf ( const void* runtime )
	{
		const auto	it = std::ranges::find ( watchers, runtime, &Watcher::runtime );
		return it != watchers.end () ? &*it : nullptr;
	}

	void addChange ( const std::filesystem::path& file, Clock::time_point now )
	{
		for ( auto& watcher : watchers )
		{
			if ( ! contains ( watcher.folders, file.parent_path () ) )
				continue;

			if ( ! contains ( watcher.changed, file ) )
				watcher.changed.push_back ( file );

			watcher.lastChange = now;
		}
	}

	// On a presenting thread, inside the file watch's delivery: the runtimes take the changes at their next present
	void onEffectFileChanged ( const char* path, api::file_change, const char* oldPath )
	{
		const std::scoped_lock	guard ( lock );
		const auto				now = Clock::now ();

		addChange ( std::filesystem::u8path ( path ), now );

		if ( oldPath != nullptr )
			addChange ( std::filesystem::u8path ( oldPath ), now );
	}

	void watchFolder ( const std::filesystem::path& folder )
	{
		for ( const auto pattern : patterns )
		{
			if ( reshade::ultra::watchFolder ( folder, pattern, onEffectFileChanged ) )
				continue;

			reshade::log::message ( reshade::log::level::error, "Effects in '%s' don't recompile when they change: the folder is not watched.", folder.u8string ().c_str () );
			return;
		}
	}

	void unwatchFolder ( const std::filesystem::path& folder )
	{
		for ( const auto pattern : patterns )
			reshade::ultra::unwatchFolder ( folder, pattern, onEffectFileChanged );
	}

	// Under the lock
	void syncWatches ()
	{
		Paths	wanted;

		for ( const auto& watcher : watchers )
			for ( const auto& folder : watcher.folders )
				if ( ! contains ( wanted, folder ) )
					wanted.push_back ( folder );

		for ( const auto& folder : watchedFolders )
			if ( ! contains ( wanted, folder ) )
				unwatchFolder ( folder );

		for ( const auto& folder : wanted )
			if ( ! contains ( watchedFolders, folder ) )
				watchFolder ( folder );

		watchedFolders = std::move ( wanted );
	}
}
//-----------------------------------------------------------------------------

void reshade::ultra::followEffectSearchPaths ( const void* runtime, const std::vector<std::filesystem::path>& searchPaths )
{
	const std::scoped_lock	guard ( lock );
	auto					watcher = watcherOf ( runtime );

	if ( watcher != nullptr && isSameSpelling ( watcher->searchPaths, searchPaths ) )
		return;

	if ( watcher == nullptr )
		watcher = &watchers.emplace_back ( Watcher { runtime } );

	watcher->searchPaths = searchPaths;
	watcher->folders = resolveFolders ( searchPaths );
	syncWatches ();

	log::message ( log::level::info, "Effects of runtime %p recompile when a file in their %zu search folders changes.", runtime, watcher->folders.size () );
}
//-----------------------------------------------------------------------------

std::vector<std::filesystem::path> reshade::ultra::takeEffectChanges ( const void* runtime )
{
	const std::scoped_lock	guard ( lock );
	const auto				watcher = watcherOf ( runtime );
	const auto				now = Clock::now ();

	if ( watcher == nullptr || watcher->changed.empty () || now - watcher->lastChange < quietTime || now - watcher->lastWrittenCheck < writtenCheckInterval )
		return {};

	watcher->lastWrittenCheck = now;

	if ( std::ranges::any_of ( watcher->changed, isFileBeingWritten ) )
		return {};

	return std::exchange ( watcher->changed, {} );
}
//-----------------------------------------------------------------------------

void reshade::ultra::forgetEffectWatch ( const void* runtime )
{
	const std::scoped_lock	guard ( lock );
	std::erase_if ( watchers, [ runtime ] ( const Watcher& watcher ) { return watcher.runtime == runtime; } );
	syncWatches ();
}
//-----------------------------------------------------------------------------

void reshade::ultra::stopEffectReload ()
{
	const std::scoped_lock	guard ( lock );
	watchers.clear ();
	syncWatches ();
}
//-----------------------------------------------------------------------------

// Queued like the overlay's reload, so update_effects recompiles right after, outside any add-on event
void reshade::runtime::ultra_reload_changed_effects ()
{
	if ( ! ultra::isAddonReloadEnabled || ultra::isInAddonEvent () )
		return;

	ultra::followEffectSearchPaths ( this, _effect_search_paths );

	if ( is_loading () || _is_in_preset_transition )
		return;

	const auto	changed = ultra::takeEffectChanges ( this );

	if ( changed.empty () )
		return;

	std::vector<size_t>	due;
	auto				isAll = false;
	std::error_code		error;

	// A changed .fx recompiles its own effect; any other change all of them, since every effect's cache key covers the
	// .fxh files of all search paths
	for ( const auto& file : changed )
	{
		const auto	it = std::ranges::find_if ( _effects, [ &file ] ( const effect& effect ) { return isSamePath ( effect.source_file, file ); } );

		if ( file.extension () == L".fxh" || it == _effects.end () || ! std::filesystem::exists ( file, error ) )
			isAll = true;
		else if ( ! it->skipped )
			due.push_back ( static_cast<size_t> ( it - _effects.begin () ) );
	}

	auto	what = "Effect file '" + changed.front ().u8string () + '\'';

	if ( changed.size () > 1 )
		what += " and " + std::to_string ( changed.size () - 1 ) + " more";

	if ( isAll )
	{
		log::message ( log::level::info, "%s changed: all effects of runtime %p recompile.", what.c_str (), this );
		reload_effect_next_frame ( nullptr );
		return;
	}

	if ( due.empty () )
	{
		log::message ( log::level::info, "%s changed: not loaded on runtime %p (disabled), nothing recompiles.", what.c_str (), this );
		return;
	}

	for ( const auto index : due )
		if ( std::ranges::find ( _reload_required_effects, std::make_pair ( index, size_t ( 0 ) ) ) == _reload_required_effects.end () )
			_reload_required_effects.emplace_back ( index, 0 );

	log::message ( log::level::info, "%s changed: %zu of the effects of runtime %p recompile.", what.c_str (), due.size (), this );
}
//-----------------------------------------------------------------------------

#endif
