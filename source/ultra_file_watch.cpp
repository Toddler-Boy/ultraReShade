/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if RESHADE_ADDON

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <intrin.h>
#include <Windows.h>

#include <reshade.hpp>
#include <reshade_ultra.hpp>

#include "addon_manager.hpp"
#include "dll_log.hpp"
#include "ultra_file_watch.hpp"

extern std::filesystem::path g_reshade_base_path;

//-----------------------------------------------------------------------------

namespace
{
	namespace api = reshade::api;
	using reshade::ultra::FileChangedCallback;
	using Clock = std::chrono::steady_clock;

	// Editors save through a temporary file and a rename, builds write in pieces: a folder's changes are handed out
	// once it was quiet this long
	constexpr auto		quietTime = std::chrono::milliseconds ( 150 );
	// A file still open for writing holds its folder's changes back, at most this long (a log someone keeps open)
	constexpr auto		maxWriterWait = std::chrono::seconds ( 2 );
	constexpr DWORD		bufferSize = 32 * 1024;		// shares refuse more than 64 KB
	constexpr DWORD		fileFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
	constexpr DWORD		awaitFilter = FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_FILE_NAME;
	constexpr ULONG_PTR	wakeKey = 0;
	constexpr DWORD		joinTimeout = 2000;			// ms: a stuck watcher must not hang the game's exit with it

	struct HandleCloser
	{
		void operator() ( HANDLE handle ) const { CloseHandle ( handle ); }
	};

	struct Watch
	{
		void*				owner = nullptr;		// the add-on's module, or a client's callback
		FileChangedCallback	callback = nullptr;		// ReShade's own clients; add-ons get the event
		std::wstring		folder;					// absolute, normalized
		std::wstring		folderKey;				// upper case
		std::wstring		match;					// upper case: the file's name, or the pattern of a folder watch
		bool				isFolder = false;
	};

	struct Change
	{
		void*				owner = nullptr;
		FileChangedCallback	callback = nullptr;
		std::string			path;
		std::string			oldPath;
		api::file_change	kind = api::file_change::modified;
	};

	// What a name in a folder went through since the folder's last quiet moment
	struct Pending
	{
		std::wstring	name;
		std::wstring	renamedFrom;
		std::wstring	renamedTo;
		bool			exists = false;
	};

	struct KnownFile
	{
		std::wstring	name;
		uint64_t		size = 0;
		FILETIME		time {};
	};

	using KnownFiles = std::map<std::wstring, KnownFile>;		// by upper case name

	struct Folder
	{
		std::wstring							path;
		std::wstring							key;
		std::wstring							openedPath;		// path, or while it doesn't exist the nearest folder above it that does
		std::wstring							awaitedName;	// upper case: the next part of path below openedPath
		ULONG_PTR								id = 0;			// a new one per handle, so a cancelled read's completion finds nothing
		HANDLE									handle = INVALID_HANDLE_VALUE;
		OVERLAPPED								overlapped {};
		std::unique_ptr<void, HandleCloser>		readDone { CreateEventW ( nullptr, TRUE, FALSE, nullptr ) };
		std::unique_ptr<DWORD[]>				buffer = std::make_unique<DWORD[]> ( bufferSize / sizeof ( DWORD ) );
		bool									isReading = false;
		bool									isFresh = false;	// no read completed on this handle yet
		// A fresh handle lists or syncs the folder once its first read is pending, so nothing changes unseen in between;
		// a share starts reporting a little later still, so the folder is synced again when its changes settle
		bool									needsList = false;
		bool									needsResync = false;
		bool									needsLateResync = false;
		KnownFiles								known;			// the files there as of the last settle
		std::vector<Pending>					pending;
		std::wstring							renamedName;	// a rename's old name, until its new name follows
		Clock::time_point						firstChange;
		Clock::time_point						lastChange;
	};

	std::mutex								lock;
	std::vector<Watch>						watches;
	std::vector<std::unique_ptr<Folder>>	folders;		// the watcher thread's, under the lock
	std::vector<Change>						ready;
	std::atomic_bool						hasReady = false;
	HANDLE									port = nullptr;
	HANDLE									thread = nullptr;
	std::vector<HANDLE>						endedThreads;	// ended on their own after the last unwatch, joined later
	ULONG_PTR								nextFolderId = 1;

	//-------------------------------------------------------------------------

	[[ nodiscard ]] std::wstring upper ( std::wstring text )
	{
		CharUpperBuffW ( text.data (), static_cast<DWORD> ( text.size () ) );
		return text;
	}

	[[ nodiscard ]] std::string toUtf8 ( const std::wstring& text )
	{
		const auto	size = WideCharToMultiByte ( CP_UTF8, 0, text.data (), static_cast<int> ( text.size () ), nullptr, 0, nullptr, nullptr );
		std::string	result ( static_cast<size_t> ( std::max ( size, 0 ) ), '\0' );
		WideCharToMultiByte ( CP_UTF8, 0, text.data (), static_cast<int> ( text.size () ), result.data (), size, nullptr, nullptr );
		return result;
	}

	[[ nodiscard ]] std::wstring fromUtf8 ( const char* text )
	{
		const auto		length = static_cast<int> ( std::strlen ( text ) );
		const auto		size = MultiByteToWideChar ( CP_UTF8, 0, text, length, nullptr, 0 );
		std::wstring	result ( static_cast<size_t> ( std::max ( size, 0 ) ), L'\0' );
		MultiByteToWideChar ( CP_UTF8, 0, text, length, result.data (), size );
		return result;
	}

	// * and ?, both upper case
	[[ nodiscard ]] bool isMatch ( std::wstring_view pattern, std::wstring_view name )
	{
		size_t	p = 0;
		size_t	n = 0;
		size_t	star = std::wstring_view::npos;
		size_t	starName = 0;

		while ( n < name.size () )
		{
			if ( p < pattern.size () && ( pattern[ p ] == L'?' || pattern[ p ] == name[ n ] ) )
				++p, ++n;
			else if ( p < pattern.size () && pattern[ p ] == L'*' )
				star = p++, starName = n;
			else if ( star != std::wstring_view::npos )
				p = star + 1, n = ++starName;
			else
				return false;
		}

		while ( p < pattern.size () && pattern[ p ] == L'*' )
			++p;

		return p == pattern.size ();
	}

	[[ nodiscard ]] bool matches ( const Watch& watch, const std::wstring& upperName )
	{
		return watch.isFolder ? isMatch ( watch.match, upperName ) : watch.match == upperName;
	}

	[[ nodiscard ]] bool isSame ( const Watch& a, const Watch& b )
	{
		return a.owner == b.owner && a.callback == b.callback && a.folderKey == b.folderKey && a.match == b.match && a.isFolder == b.isFolder;
	}

	[[ nodiscard ]] std::wstring joinPath ( const std::wstring& folder, const std::wstring& name )
	{
		return folder.ends_with ( L'\\' ) ? folder + name : folder + L'\\' + name;
	}

	// pattern null: path names a file
	[[ nodiscard ]] std::optional<Watch> makeWatch ( std::filesystem::path path, const wchar_t* pattern )
	{
		if ( path.is_relative () )
			path = g_reshade_base_path / path;

		path = path.lexically_normal ();

		if ( ! path.has_filename () && path.has_relative_path () )
			path = path.parent_path ();

		Watch	watch;

		if ( pattern != nullptr )
		{
			watch.folder = path.native ();
			watch.match = upper ( *pattern != L'\0' ? pattern : L"*" );
			watch.isFolder = true;
		}
		else if ( path.has_filename () && path.has_relative_path () )
		{
			watch.folder = path.parent_path ().native ();
			watch.match = upper ( path.filename ().native () );
		}
		else
		{
			reshade::log::message ( reshade::log::level::error, "File watch: '%s' is not a file.", toUtf8 ( path.native () ).c_str () );
			return std::nullopt;
		}

		watch.folderKey = upper ( watch.folder );
		return watch;
	}

	//-------------------------------------------------------------------------
	// The watcher's side: everything under the lock

	[[ nodiscard ]] bool isAwaiting ( const Folder& folder )
	{
		return folder.openedPath != folder.path;
	}

	[[ nodiscard ]] bool hasWatches ( const Folder& folder )
	{
		return std::ranges::any_of ( watches, [ &folder ] ( const Watch& watch ) { return watch.folderKey == folder.key; } );
	}

	// A folder being deleted can't be opened either: it is gone once the last handle on it closes
	[[ nodiscard ]] bool isMissing ( DWORD error )
	{
		return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_ACCESS_DENIED;
	}

	// Opens folder's path, or while it doesn't exist the nearest folder above it, to wait there for it
	[[ nodiscard ]] bool openNearest ( Folder& folder )
	{
		auto	target = std::filesystem::path ( folder.path );

		for ( ;; )
		{
			folder.handle = CreateFileW ( target.c_str (), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
										  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr );

			if ( folder.handle != INVALID_HANDLE_VALUE )
				break;

			const auto	error = GetLastError ();
			const auto	parent = target.parent_path ();

			if ( ! isMissing ( error ) || parent.empty () || parent == target )
			{
				reshade::log::message ( reshade::log::level::error, "File watch: failed to open folder '%s' (error code %lu); changes in '%s' are not reported.",
										toUtf8 ( target.native () ).c_str (), error, toUtf8 ( folder.path ).c_str () );
				return false;
			}

			target = parent;
		}

		folder.id = nextFolderId++;

		if ( CreateIoCompletionPort ( folder.handle, port, folder.id, 0 ) == nullptr )
		{
			reshade::log::message ( reshade::log::level::error, "File watch: failed to watch folder '%s' (error code %lu); changes in it are not reported.",
									toUtf8 ( target.native () ).c_str (), GetLastError () );
			CloseHandle ( folder.handle );
			folder.handle = INVALID_HANDLE_VALUE;
			return false;
		}

		if ( target.native () != folder.path && target.native () != folder.openedPath )
			reshade::log::message ( reshade::log::level::info, "File watch: folder '%s' doesn't exist; '%s' is watched until it does.", toUtf8 ( folder.path ).c_str (),
									toUtf8 ( target.native () ).c_str () );

		folder.openedPath = target.native ();
		folder.awaitedName = upper ( std::filesystem::path ( folder.path ).lexically_relative ( target ).begin ()->native () );
		folder.isFresh = true;
		return true;
	}

	void closeFolder ( Folder& folder )
	{
		if ( folder.handle == INVALID_HANDLE_VALUE )
			return;

		if ( folder.isReading )
		{
			DWORD	bytes = 0;
			CancelIoEx ( folder.handle, &folder.overlapped );
			GetOverlappedResult ( folder.handle, &folder.overlapped, &bytes, TRUE );
			folder.isReading = false;
		}

		CloseHandle ( folder.handle );
		folder.handle = INVALID_HANDLE_VALUE;
	}

	// The feature turns itself off for the folder's files
	void dropFolder ( Folder& folder )
	{
		reshade::log::message ( reshade::log::level::error, "File watch: changes in '%s' are no longer reported.", toUtf8 ( folder.path ).c_str () );
		std::erase_if ( watches, [ &folder ] ( const Watch& watch ) { return watch.folderKey == folder.key; } );
		closeFolder ( folder );
	}

	[[ nodiscard ]] Pending* findPending ( Folder& folder, const std::wstring& name )
	{
		for ( auto& entry : folder.pending )
			if ( CompareStringOrdinal ( entry.name.data (), static_cast<int> ( entry.name.size () ), name.data (), static_cast<int> ( name.size () ), TRUE ) == CSTR_EQUAL )
				return &entry;

		return nullptr;
	}

	[[ nodiscard ]] Pending& pendingFor ( Folder& folder, const std::wstring& name )
	{
		if ( const auto entry = findPending ( folder, name ); entry != nullptr )
			return *entry;

		if ( folder.pending.empty () )
			folder.firstChange = Clock::now ();

		return folder.pending.emplace_back ( Pending { name } );
	}

	[[ nodiscard ]] KnownFiles listFiles ( const Folder& folder )
	{
		KnownFiles			files;
		WIN32_FIND_DATAW	data {};
		const auto			search = FindFirstFileExW ( joinPath ( folder.path, L"*" ).c_str (), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH );

		if ( search == INVALID_HANDLE_VALUE )
			return files;

		do
		{
			if ( ( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) == 0 )
				files.emplace ( upper ( data.cFileName ), KnownFile { data.cFileName, uint64_t ( data.nFileSizeHigh ) << 32 | data.nFileSizeLow, data.ftLastWriteTime } );
		}
		while ( FindNextFileW ( search, &data ) );

		FindClose ( search );
		return files;
	}

	// Changes were missed: every file there counts as modified or created, every known one missing as deleted
	void resync ( Folder& folder )
	{
		const auto	files = listFiles ( folder );

		for ( const auto& [ key, file ] : files )
			pendingFor ( folder, file.name ).exists = true;

		for ( const auto& [ key, file ] : folder.known )
			if ( ! files.contains ( key ) )
				pendingFor ( folder, file.name ).exists = false;

		folder.lastChange = Clock::now ();
	}

	void forgetKnown ( Folder& folder )
	{
		for ( const auto& [ key, file ] : folder.known )
			pendingFor ( folder, file.name ).exists = false;

		folder.lastChange = Clock::now ();
	}

	// A rename's old and new name come one after the other, though other names may come between them on a share
	void record ( Folder& folder, DWORD action, const std::wstring& name )
	{
		auto&	entry = pendingFor ( folder, name );
		entry.exists = action != FILE_ACTION_REMOVED && action != FILE_ACTION_RENAMED_OLD_NAME;

		if ( action == FILE_ACTION_REMOVED )
			entry.renamedFrom.clear ();

		if ( action == FILE_ACTION_RENAMED_OLD_NAME )
			folder.renamedName = name;
		else if ( action == FILE_ACTION_RENAMED_NEW_NAME && ! folder.renamedName.empty () )
		{
			const auto	oldName = folder.renamedName;
			folder.renamedName.clear ();
			pendingFor ( folder, oldName ).renamedTo = name;
			pendingFor ( folder, name ).renamedFrom = oldName;
		}
	}

	template <typename Visit>
	void forEachNotification ( const Folder& folder, Visit visit )
	{
		auto	entry = reinterpret_cast<const BYTE*> ( folder.buffer.get () );

		for ( ;; )
		{
			const auto	info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*> ( entry );
			visit ( info->Action, std::wstring ( info->FileName, info->FileNameLength / sizeof ( wchar_t ) ) );

			if ( info->NextEntryOffset == 0 )
				break;

			entry += info->NextEntryOffset;
		}
	}

	[[ nodiscard ]] std::optional<api::file_change> kindOf ( const Folder& folder, const Pending& entry )
	{
		const auto	existedBefore = folder.known.contains ( upper ( entry.name ) );

		if ( existedBefore && entry.exists )
			return api::file_change::modified;

		if ( entry.exists )
			return entry.renamedFrom.empty () ? api::file_change::created : api::file_change::renamed;

		if ( existedBefore )
			return api::file_change::deleted;

		return std::nullopt;
	}

	[[ nodiscard ]] bool isDirectory ( const std::wstring& path )
	{
		const auto	attributes = GetFileAttributesW ( path.c_str () );
		return attributes != INVALID_FILE_ATTRIBUTES && ( attributes & FILE_ATTRIBUTE_DIRECTORY ) != 0;
	}

	[[ nodiscard ]] std::optional<KnownFile> fileInfo ( const Folder& folder, const std::wstring& name )
	{
		WIN32_FILE_ATTRIBUTE_DATA	data {};

		if ( ! GetFileAttributesExW ( joinPath ( folder.path, name ).c_str (), GetFileExInfoStandard, &data ) )
			return std::nullopt;

		return KnownFile { name, uint64_t ( data.nFileSizeHigh ) << 32 | data.nFileSizeLow, data.ftLastWriteTime };
	}

	// A share may report a rename as a deletion and a creation: a created file with the size and time a deleted one
	// had is that file renamed
	void pairRenames ( Folder& folder, std::vector<std::optional<api::file_change>>& kinds )
	{
		for ( size_t created = 0; created < folder.pending.size (); ++created )
		{
			if ( kinds[ created ] != api::file_change::created )
				continue;

			const auto	info = fileInfo ( folder, folder.pending[ created ].name );

			for ( size_t deleted = 0; info.has_value () && deleted < folder.pending.size (); ++deleted )
			{
				auto&		entry = folder.pending[ deleted ];
				const auto	it = folder.known.find ( upper ( entry.name ) );

				if ( kinds[ deleted ] != api::file_change::deleted || ! entry.renamedTo.empty () || it == folder.known.end () || it->second.size != info->size
					 || CompareFileTime ( &it->second.time, &info->time ) != 0 )
					continue;

				entry.renamedTo = folder.pending[ created ].name;
				folder.pending[ created ].renamedFrom = entry.name;
				kinds[ created ] = api::file_change::renamed;
				break;
			}
		}
	}

	// Someone has it open for writing: it opens only while sharing write access
	[[ nodiscard ]] bool isBeingWritten ( const std::wstring& path )
	{
		const auto	handle = CreateFileW ( path.c_str (), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );

		if ( handle != INVALID_HANDLE_VALUE )
			CloseHandle ( handle );

		return handle == INVALID_HANDLE_VALUE && GetLastError () == ERROR_SHARING_VIOLATION;
	}

	// The folder was quiet long enough: one change per name and watch. A rename between two names a watch matches is a
	// rename for it; otherwise the new name was created and the old one deleted
	void settle ( Folder& folder )
	{
		const auto	now = Clock::now ();

		if ( now - folder.firstChange < maxWriterWait
			 && std::ranges::any_of ( folder.pending, [ &folder ] ( const Pending& entry ) { return entry.exists && isBeingWritten ( joinPath ( folder.path, entry.name ) ); } ) )
		{
			folder.lastChange = now;
			return;
		}

		if ( folder.needsLateResync )
		{
			folder.needsLateResync = false;
			resync ( folder );
		}

		std::vector<std::optional<api::file_change>>	kinds;

		for ( const auto& entry : folder.pending )
			kinds.push_back ( entry.exists && isDirectory ( joinPath ( folder.path, entry.name ) ) ? std::nullopt : kindOf ( folder, entry ) );

		pairRenames ( folder, kinds );

		for ( size_t i = 0; i < folder.pending.size (); ++i )
		{
			const auto&	entry = folder.pending[ i ];

			if ( ! kinds[ i ].has_value () )
				continue;

			const auto	upperName = upper ( entry.name );
			const auto	upperFrom = upper ( entry.renamedFrom );
			const auto	upperTo = upper ( entry.renamedTo );
			auto		isRenamedAway = false;

			if ( const auto target = findPending ( folder, entry.renamedTo ); *kinds[ i ] == api::file_change::deleted && target != nullptr )
				isRenamedAway = kinds[ static_cast<size_t> ( target - folder.pending.data () ) ] == api::file_change::renamed;

			for ( const auto& watch : watches )
			{
				if ( watch.folderKey != folder.key || ! matches ( watch, upperName ) )
					continue;

				auto		change = *kinds[ i ];
				std::string	oldPath;

				if ( change == api::file_change::renamed )
				{
					if ( matches ( watch, upperFrom ) )
						oldPath = toUtf8 ( joinPath ( folder.path, entry.renamedFrom ) );
					else
						change = api::file_change::created;
				}
				else if ( isRenamedAway && matches ( watch, upperTo ) )
					continue;

				ready.push_back ( { watch.owner, watch.callback, toUtf8 ( joinPath ( folder.path, entry.name ) ), std::move ( oldPath ), change } );
			}
		}

		for ( size_t i = 0; i < folder.pending.size (); ++i )
		{
			const auto&	entry = folder.pending[ i ];

			const auto	info = entry.exists && kinds[ i ].has_value () ? fileInfo ( folder, entry.name ) : std::nullopt;

			if ( info.has_value () )
				folder.known.insert_or_assign ( upper ( entry.name ), *info );
			else
				folder.known.erase ( upper ( entry.name ) );
		}

		folder.pending.clear ();
		folder.renamedName.clear ();
		hasReady = ! ready.empty ();
	}

	[[ nodiscard ]] bool isDeletePending ( HANDLE handle )
	{
		FILE_STANDARD_INFO	info {};
		return GetFileInformationByHandleEx ( handle, FileStandardInfo, &info, sizeof ( info ) ) && info.DeletePending;
	}

	// The folder's handle no longer works: opened again, or above it while it doesn't exist
	void reopen ( Folder& folder, DWORD error )
	{
		if ( error == ERROR_SUCCESS )
			reshade::log::message ( reshade::log::level::info, "File watch: folder '%s' was deleted; its files are watched again once it is back.", toUtf8 ( folder.path ).c_str () );
		else
			reshade::log::message ( reshade::log::level::warning, "File watch: watching folder '%s' failed with error code %lu; it is opened again.", toUtf8 ( folder.path ).c_str (), error );

		CloseHandle ( folder.handle );
		folder.handle = INVALID_HANDLE_VALUE;

		if ( ! openNearest ( folder ) )
			dropFolder ( folder );
		else if ( isAwaiting ( folder ) )
			forgetKnown ( folder );
		else
			folder.needsResync = true;
	}

	// Something changed in the folder above: the awaited one may exist now
	void retryAwaited ( Folder& folder )
	{
		CloseHandle ( folder.handle );
		folder.handle = INVALID_HANDLE_VALUE;

		if ( ! openNearest ( folder ) )
			dropFolder ( folder );
		else if ( ! isAwaiting ( folder ) )
		{
			reshade::log::message ( reshade::log::level::info, "File watch: folder '%s' is back.", toUtf8 ( folder.path ).c_str () );
			folder.needsResync = true;
		}
	}

	void completed ( ULONG_PTR key, const OVERLAPPED* overlapped, DWORD bytes, DWORD error )
	{
		const auto	it = std::ranges::find_if ( folders, [ key ] ( const std::unique_ptr<Folder>& folder ) { return folder->id == key; } );

		if ( it == folders.end () || overlapped != &( *it )->overlapped || error == ERROR_OPERATION_ABORTED )
			return;

		auto&	folder = **it;
		folder.isReading = false;
		folder.isFresh = false;

		if ( isAwaiting ( folder ) )
		{
			auto	isMentioned = error != ERROR_SUCCESS || bytes == 0;

			if ( ! isMentioned )
				forEachNotification ( folder, [ &folder, &isMentioned ] ( DWORD, const std::wstring& name ) { isMentioned = isMentioned || upper ( name ) == folder.awaitedName; } );

			if ( isMentioned )
				retryAwaited ( folder );

			return;
		}

		if ( error == ERROR_SUCCESS && bytes != 0 )
		{
			forEachNotification ( folder, [ &folder ] ( DWORD action, const std::wstring& name ) { record ( folder, action, name ); } );
			folder.lastChange = Clock::now ();
		}
		// A share reports a deleted folder as access denied
		else if ( error == ERROR_ACCESS_DENIED || isDeletePending ( folder.handle ) )
			reopen ( folder, ERROR_SUCCESS );
		else if ( error == ERROR_SUCCESS || error == ERROR_NOTIFY_ENUM_DIR )
		{
			reshade::log::message ( reshade::log::level::warning, "File watch: too many changes at once in '%s'; its files are reported as modified.", toUtf8 ( folder.path ).c_str () );
			folder.needsResync = true;
		}
		else
			reopen ( folder, error );
	}

	void startRead ( Folder& folder )
	{
		folder.overlapped = {};
		folder.overlapped.hEvent = folder.readDone.get ();

		if ( ReadDirectoryChangesW ( folder.handle, folder.buffer.get (), bufferSize, FALSE, isAwaiting ( folder ) ? awaitFilter : fileFilter, nullptr, &folder.overlapped, nullptr ) )
		{
			folder.isReading = true;
			return;
		}

		const auto	error = GetLastError ();

		if ( ! folder.isFresh )
		{
			reopen ( folder, error );
			return;
		}

		reshade::log::message ( reshade::log::level::error, "File watch: failed to watch folder '%s' (error code %lu).", toUtf8 ( folder.openedPath ).c_str (), error );
		dropFolder ( folder );
	}

	// Folders nobody watches close, every other one has a read pending
	void reconcile ()
	{
		std::erase_if ( folders, [] ( const std::unique_ptr<Folder>& folder )
		{
			if ( hasWatches ( *folder ) )
				return false;

			closeFolder ( *folder );
			return true;
		} );

		for ( const auto& folder : folders )
		{
			while ( folder->handle != INVALID_HANDLE_VALUE && ! folder->isReading )
				startRead ( *folder );

			if ( ! folder->isReading || isAwaiting ( *folder ) )
				continue;

			if ( folder->needsList )
				folder->known = listFiles ( *folder );

			if ( folder->needsResync )
			{
				resync ( *folder );
				folder->needsLateResync = true;
			}

			folder->needsList = folder->needsResync = false;
		}
	}

	void settleQuietFolders ()
	{
		const auto	now = Clock::now ();

		for ( const auto& folder : folders )
			if ( ( ! folder->pending.empty () || folder->needsLateResync ) && now - folder->lastChange >= quietTime )
				settle ( *folder );
	}

	[[ nodiscard ]] DWORD nextTimeout ()
	{
		auto		timeout = static_cast<DWORD> ( INFINITE );
		const auto	now = Clock::now ();

		for ( const auto& folder : folders )
		{
			if ( folder->pending.empty () && ! folder->needsLateResync )
				continue;

			const auto	left = std::chrono::ceil<std::chrono::milliseconds> ( folder->lastChange + quietTime - now ).count ();
			timeout = std::min ( timeout, static_cast<DWORD> ( std::max<long long> ( left, 1 ) ) );
		}

		return timeout;
	}

	// Ends by itself once nothing is watched
	DWORD WINAPI watchThread ( void* parameter )
	{
		const auto	completionPort = static_cast<HANDLE> ( parameter );
		SetThreadPriority ( GetCurrentThread (), THREAD_PRIORITY_BELOW_NORMAL );

		std::unique_lock	guard ( lock );

		for ( ;; )
		{
			reconcile ();

			if ( watches.empty () )
				break;

			const auto	timeout = nextTimeout ();
			guard.unlock ();

			DWORD		bytes = 0;
			ULONG_PTR	key = 0;
			OVERLAPPED*	overlapped = nullptr;
			const auto	error = GetQueuedCompletionStatus ( completionPort, &bytes, &key, &overlapped, timeout ) ? ERROR_SUCCESS : GetLastError ();

			guard.lock ();

			if ( overlapped != nullptr )
				completed ( key, overlapped, bytes, error );

			settleQuietFolders ();
		}

		for ( const auto& folder : folders )
			closeFolder ( *folder );

		folders.clear ();
		CloseHandle ( completionPort );
		port = nullptr;
		endedThreads.push_back ( thread );
		thread = nullptr;

		reshade::log::message ( reshade::log::level::info, "File watch: nothing is watched any more, its thread ends." );
		return 0;
	}

	//-------------------------------------------------------------------------
	// The callers' side

	void wakeWatcher ()
	{
		if ( port != nullptr )
			PostQueuedCompletionStatus ( port, 0, wakeKey, nullptr );
	}

	// Under the lock
	[[ nodiscard ]] bool ensureThread ()
	{
		if ( thread != nullptr )
			return true;

		std::erase_if ( endedThreads, [] ( HANDLE ended )
		{
			if ( WaitForSingleObject ( ended, 0 ) != WAIT_OBJECT_0 )
				return false;

			CloseHandle ( ended );
			return true;
		} );

		port = CreateIoCompletionPort ( INVALID_HANDLE_VALUE, nullptr, 0, 1 );

		if ( port == nullptr )
		{
			reshade::log::message ( reshade::log::level::error, "File watch: failed to create a completion port (error code %lu); no file is watched.", GetLastError () );
			return false;
		}

		thread = CreateThread ( nullptr, 0, watchThread, port, 0, nullptr );

		if ( thread == nullptr )
		{
			reshade::log::message ( reshade::log::level::error, "File watch: failed to start its thread (error code %lu); no file is watched.", GetLastError () );
			CloseHandle ( port );
			port = nullptr;
			return false;
		}

		reshade::log::message ( reshade::log::level::info, "File watch: started." );
		return true;
	}

	[[ nodiscard ]] bool addWatch ( Watch watch )
	{
		const std::scoped_lock	guard ( lock );

		if ( std::ranges::any_of ( watches, [ &watch ] ( const Watch& other ) { return isSame ( other, watch ); } ) )
			return true;

		if ( ! ensureThread () )
			return false;

		const auto	isOpen = std::ranges::any_of ( folders, [ &watch ] ( const std::unique_ptr<Folder>& folder ) { return folder->key == watch.folderKey; } );

		if ( ! isOpen )
		{
			auto	folder = std::make_unique<Folder> ();
			folder->path = watch.folder;
			folder->key = watch.folderKey;

			if ( ! openNearest ( *folder ) )
			{
				wakeWatcher ();
				return false;
			}

			folder->needsList = ! isAwaiting ( *folder );

			folders.push_back ( std::move ( folder ) );
		}

		watches.push_back ( std::move ( watch ) );
		wakeWatcher ();
		return true;
	}

	void removeWatch ( const Watch& watch )
	{
		const std::scoped_lock	guard ( lock );
		std::erase_if ( watches, [ &watch ] ( const Watch& other ) { return isSame ( other, watch ); } );
		wakeWatcher ();
	}

	[[ nodiscard ]] const reshade::addon_info* addonOf ( void* owner )
	{
		for ( auto it = reshade::addon_loaded_info.rbegin (); it != reshade::addon_loaded_info.rend (); ++it )
			if ( it->handle == owner )
				return &*it;

		return nullptr;
	}

	// ReShade's own clients: the callback owns the watch
	[[ nodiscard ]] bool addClientWatch ( const std::filesystem::path& path, const wchar_t* pattern, FileChangedCallback callback )
	{
		auto	watch = makeWatch ( path, pattern );

		if ( ! watch.has_value () )
			return false;

		watch->owner = reinterpret_cast<void*> ( callback );
		watch->callback = callback;
		return addWatch ( std::move ( *watch ) );
	}

	void removeClientWatch ( const std::filesystem::path& path, const wchar_t* pattern, FileChangedCallback callback )
	{
		auto	watch = makeWatch ( path, pattern );

		if ( ! watch.has_value () )
			return;

		watch->owner = reinterpret_cast<void*> ( callback );
		watch->callback = callback;
		removeWatch ( *watch );
	}
}
//-----------------------------------------------------------------------------

bool reshade::ultra::watchFile ( const std::filesystem::path& file, FileChangedCallback callback )
{
	return addClientWatch ( file, nullptr, callback );
}
//-----------------------------------------------------------------------------

void reshade::ultra::unwatchFile ( const std::filesystem::path& file, FileChangedCallback callback )
{
	removeClientWatch ( file, nullptr, callback );
}
//-----------------------------------------------------------------------------

bool reshade::ultra::watchFolder ( const std::filesystem::path& folder, const wchar_t* pattern, FileChangedCallback callback )
{
	return addClientWatch ( folder, pattern, callback );
}
//-----------------------------------------------------------------------------

void reshade::ultra::unwatchFolder ( const std::filesystem::path& folder, const wchar_t* pattern, FileChangedCallback callback )
{
	removeClientWatch ( folder, pattern, callback );
}
//-----------------------------------------------------------------------------

bool reshade::ultra::isFileBeingWritten ( const std::filesystem::path& file )
{
	return isBeingWritten ( file.native () );
}
//-----------------------------------------------------------------------------

void reshade::ultra::forgetFileWatches ( HMODULE module )
{
	const std::scoped_lock	guard ( lock );
	std::erase_if ( watches, [ module ] ( const Watch& watch ) { return watch.owner == module && watch.callback == nullptr; } );
	std::erase_if ( ready, [ module ] ( const Change& change ) { return change.owner == module && change.callback == nullptr; } );
	hasReady = ! ready.empty ();
	wakeWatcher ();
}
//-----------------------------------------------------------------------------

// Add-ons that unloaded meanwhile were forgotten, with their changes
void reshade::ultra::deliverFileChanges ()
{
	if ( ! hasReady.load ( std::memory_order_relaxed ) )
		return;

	std::vector<Change>	changes;
	{
		const std::scoped_lock	guard ( lock );
		changes.swap ( ready );
		hasReady = false;
	}

	const AddonEventGuard	eventGuard;

	for ( const auto& change : changes )
	{
		const auto	oldPath = change.oldPath.empty () ? nullptr : change.oldPath.c_str ();

		if ( change.callback != nullptr )
		{
			change.callback ( change.path.c_str (), change.kind, oldPath );
			continue;
		}

		const auto	info = addonOf ( change.owner );

		if ( info == nullptr )
			continue;

		const auto	callbacks = info->event_callbacks;		// a callback may unregister

		for ( const auto& [ event, callback ] : callbacks )
			if ( event == static_cast<uint32_t> ( addon_event::ultra_file_changed ) )
				reinterpret_cast<addon_event_traits<addon_event::ultra_file_changed>::decl> ( callback ) ( change.path.c_str (), change.kind, oldPath );
	}
}
//-----------------------------------------------------------------------------

void reshade::ultra::stopFileWatch ()
{
	HANDLE	running = nullptr;
	{
		const std::scoped_lock	guard ( lock );
		watches.clear ();
		ready.clear ();
		hasReady = false;
		wakeWatcher ();
		running = thread;
	}

	if ( running != nullptr && WaitForSingleObject ( running, joinTimeout ) != WAIT_OBJECT_0 )
		log::message ( log::level::warning, "File watch: its thread did not end within %lu ms.", joinTimeout );

	const std::scoped_lock	guard ( lock );

	for ( const auto ended : endedThreads )
	{
		WaitForSingleObject ( ended, joinTimeout );
		CloseHandle ( ended );
	}

	endedThreads.clear ();
}
//-----------------------------------------------------------------------------

#if defined(RESHADE_API_LIBRARY_EXPORT)

// module null: the add-on is the caller's module
bool ReShadeWatchFile ( void* module, const char* path, const char* pattern )
{
	const auto	info = reshade::find_addon ( module != nullptr ? module : _ReturnAddress () );

	if ( info == nullptr || path == nullptr )
	{
		reshade::log::message ( reshade::log::level::error, info == nullptr ? "File watch: watch_file was called by a module that is not a registered add-on."
																			: "File watch: watch_file was called without a path." );
		return false;
	}

	const auto	wildcard = pattern != nullptr ? std::optional ( fromUtf8 ( pattern ) ) : std::nullopt;
	auto		watch = makeWatch ( fromUtf8 ( path ), wildcard.has_value () ? wildcard->c_str () : nullptr );

	if ( ! watch.has_value () )
		return false;

	watch->owner = info->handle;
	return addWatch ( std::move ( *watch ) );
}
//-----------------------------------------------------------------------------

void ReShadeUnwatchFile ( void* module, const char* path, const char* pattern )
{
	const auto	info = reshade::find_addon ( module != nullptr ? module : _ReturnAddress () );

	if ( info == nullptr || path == nullptr )
		return;

	const auto	wildcard = pattern != nullptr ? std::optional ( fromUtf8 ( pattern ) ) : std::nullopt;
	auto		watch = makeWatch ( fromUtf8 ( path ), wildcard.has_value () ? wildcard->c_str () : nullptr );

	if ( ! watch.has_value () )
		return;

	watch->owner = info->handle;
	removeWatch ( *watch );
}
//-----------------------------------------------------------------------------

#endif

#endif
