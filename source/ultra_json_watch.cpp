/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if RESHADE_ADDON

#include <algorithm>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <intrin.h>
#include <Windows.h>

#include <reshade.hpp>
#include <reshade_ultra_json.hpp>

#include "dll_log.hpp"
#include "ultra_file_watch.hpp"
#include "ultra_json.hpp"

//-----------------------------------------------------------------------------

// One file watch per json file, shared by everyone watching it. A write through ReShadeJsonWriteFile leaves the file's
// size and time behind; a change that finds the file still that way is the writer's own and skips the writer's watches
namespace
{
	struct Subscription
	{
		uint64_t						id = 0;
		std::wstring					key;				// upper case
		std::filesystem::path			file;
		std::string						path;				// UTF-8, as handed to the callback
		void*							owner = nullptr;
		HMODULE							callbackModule = nullptr;
		reshade::json::changed_callback	callback = nullptr;
		void*							context = nullptr;
	};

	struct FileState
	{
		uint64_t	size = 0;
		FILETIME	time {};
	};

	struct OwnWrite
	{
		std::wstring	key;
		void*			owner = nullptr;
		FileState		state;
	};

	std::mutex					lock;
	std::recursive_mutex		deliveryLock;		// held while callbacks run, so unwatch waits for a running one
	std::vector<Subscription>	subscriptions;
	std::vector<OwnWrite>		ownWrites;
	uint64_t					nextId = 1;

	[[ nodiscard ]] std::wstring upper ( std::wstring text )
	{
		CharUpperBuffW ( text.data (), static_cast<DWORD> ( text.size () ) );
		return text;
	}

	[[ nodiscard ]] std::wstring toWide ( const char* text )
	{
		const auto		length = static_cast<int> ( std::strlen ( text ) );
		const auto		size = MultiByteToWideChar ( CP_UTF8, 0, text, length, nullptr, 0 );
		std::wstring	result ( static_cast<size_t> ( std::max ( size, 0 ) ), L'\0' );
		MultiByteToWideChar ( CP_UTF8, 0, text, length, result.data (), size );
		return result;
	}

	[[ nodiscard ]] std::optional<FileState> fileState ( const std::wstring& path )
	{
		WIN32_FILE_ATTRIBUTE_DATA	data {};

		if ( ! GetFileAttributesExW ( path.c_str (), GetFileExInfoStandard, &data ) )
			return std::nullopt;

		return FileState { uint64_t ( data.nFileSizeHigh ) << 32 | data.nFileSizeLow, data.ftLastWriteTime };
	}

	[[ nodiscard ]] bool isSame ( const FileState& a, const FileState& b )
	{
		return a.size == b.size && CompareFileTime ( &a.time, &b.time ) == 0;
	}

	[[ nodiscard ]] bool isWatched ( const std::wstring& key )
	{
		return std::ranges::any_of ( subscriptions, [ &key ] ( const Subscription& s ) { return s.key == key; } );
	}

	[[ nodiscard ]] HMODULE moduleOf ( const void* address )
	{
		HMODULE	module = nullptr;
		GetModuleHandleExW ( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR> ( address ), &module );
		return module;
	}

	// On the presenting thread, from the file watch's delivery
	void onFileChanged ( const char* path, reshade::api::file_change change, const char* )
	{
		const std::scoped_lock	delivery ( deliveryLock );
		const auto				wide = toWide ( path );
		const auto				key = upper ( wide );
		std::vector<uint64_t>	ids;
		{
			const std::scoped_lock	guard ( lock );
			const auto				state = fileState ( wide );
			std::optional<void*>	writer;

			for ( const auto& write : ownWrites )
				if ( write.key == key && state.has_value () && isSame ( write.state, *state ) )
					writer = write.owner;

			std::erase_if ( ownWrites, [ &key ] ( const OwnWrite& write ) { return write.key == key; } );

			for ( const auto& s : subscriptions )
				if ( s.key == key && s.owner != writer )
					ids.push_back ( s.id );
		}

		for ( const auto id : ids )
		{
			reshade::json::changed_callback	callback = nullptr;
			void*							context = nullptr;
			std::string						watchedPath;
			{
				const std::scoped_lock	guard ( lock );
				const auto				it = std::ranges::find ( subscriptions, id, &Subscription::id );

				if ( it == subscriptions.end () )
					continue;

				callback = it->callback;
				context = it->context;
				watchedPath = it->path;
			}

			callback ( context, watchedPath.c_str (), change );
		}
	}

	// Under the lock: the file watch ends with the last subscription to the file
	void releaseFileWatches ( const std::vector<Subscription>& removed )
	{
		for ( const auto& s : removed )
			if ( ! isWatched ( s.key ) )
				reshade::ultra::unwatchFile ( s.file, onFileChanged );
	}

	[[ nodiscard ]] uint64_t watch ( void* owner, const char* path, reshade::json::changed_callback callback, void* context )
	{
		const auto	file = reshade::ultra::resolveJsonPath ( path );

		if ( file.empty () || callback == nullptr )
		{
			reshade::log::message ( reshade::log::level::error, file.empty () ? "Json: watch was called without a path." : "Json: watch was called without a callback." );
			return 0;
		}

		const auto				key = upper ( file.native () );
		const std::scoped_lock	guard ( lock );

		if ( ! isWatched ( key ) && ! reshade::ultra::watchFile ( file, onFileChanged ) )
			return 0;

		const auto	id = nextId++;
		subscriptions.push_back ( { id, key, file, file.u8string (), owner, moduleOf ( reinterpret_cast<const void*> ( callback ) ), callback, context } );
		return id;
	}

	void unwatch ( uint64_t id )
	{
		const std::scoped_lock	delivery ( deliveryLock );
		const std::scoped_lock	guard ( lock );
		const auto				it = std::ranges::find ( subscriptions, id, &Subscription::id );

		if ( it == subscriptions.end () )
			return;

		const std::vector<Subscription>	removed { *it };
		subscriptions.erase ( it );
		releaseFileWatches ( removed );
	}
}
//-----------------------------------------------------------------------------

void reshade::ultra::noteJsonWrite ( void* owner, const std::filesystem::path& file )
{
	const auto				key = upper ( file.native () );
	const std::scoped_lock	guard ( lock );

	if ( ! isWatched ( key ) )
		return;

	const auto	state = fileState ( file.native () );

	if ( ! state.has_value () )
		return;

	std::erase_if ( ownWrites, [ &key, owner ] ( const OwnWrite& write ) { return write.key == key && write.owner == owner; } );
	ownWrites.push_back ( { key, owner, *state } );
}
//-----------------------------------------------------------------------------

void reshade::ultra::forgetJsonWatches ( HMODULE module )
{
	const std::scoped_lock	delivery ( deliveryLock );
	const std::scoped_lock	guard ( lock );
	std::vector<Subscription>	removed;

	std::erase_if ( subscriptions, [ module, &removed ] ( const Subscription& s )
	{
		if ( s.owner != module && s.callbackModule != module )
			return false;

		removed.push_back ( s );
		return true;
	} );

	std::erase_if ( ownWrites, [ module ] ( const OwnWrite& write ) { return write.owner == module; } );
	releaseFileWatches ( removed );
}
//-----------------------------------------------------------------------------

// stopFileWatch ends the file watches themselves
void reshade::ultra::stopJsonWatch ()
{
	const std::scoped_lock	delivery ( deliveryLock );
	const std::scoped_lock	guard ( lock );
	subscriptions.clear ();
	ownWrites.clear ();
}
//-----------------------------------------------------------------------------

#if defined(RESHADE_API_LIBRARY_EXPORT)

// owner null: the caller's module
uint64_t ReShadeJsonWatch ( void* owner, const char* path, reshade::json::changed_callback callback, void* context )
{
	return watch ( owner != nullptr ? owner : moduleOf ( _ReturnAddress () ), path, callback, context );
}
//-----------------------------------------------------------------------------

void ReShadeJsonUnwatch ( uint64_t id )
{
	unwatch ( id );
}
//-----------------------------------------------------------------------------

#endif

#endif
