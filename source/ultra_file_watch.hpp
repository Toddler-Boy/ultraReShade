/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdint>
#include <filesystem>

#include <Windows.h>

namespace reshade::api
{
	enum class file_change : uint32_t;
}

//-----------------------------------------------------------------------------

// File change notifications (reshade::watch_file, addon_event::ultra_file_changed): one thread watches the folders while
// anyone watches, changes settle there and are handed out at the next present
namespace reshade::ultra
{
	// ReShade's own clients get their changes through a callback instead of the event, at the same time
	using FileChangedCallback = void ( * ) ( const char* path, api::file_change change, const char* oldPath );

	// False when the file's folder can't be watched (logged)
	[[ nodiscard ]] bool watchFile ( const std::filesystem::path& file, FileChangedCallback callback );
	void unwatchFile ( const std::filesystem::path& file, FileChangedCallback callback );

	// The folder's files whose name matches pattern (* and ?)
	[[ nodiscard ]] bool watchFolder ( const std::filesystem::path& folder, const wchar_t* pattern, FileChangedCallback callback );
	void unwatchFolder ( const std::filesystem::path& folder, const wchar_t* pattern, FileChangedCallback callback );

	// Someone has the file open for writing (a missing file is not)
	[[ nodiscard ]] bool isFileBeingWritten ( const std::filesystem::path& file );

	// An add-on that unregisters loses its watches and the changes still waiting for it
	void forgetFileWatches ( HMODULE module );

	// Once per presented frame, on the presenting thread: hands out the changes that settled
	void deliverFileChanges ();

	// The add-ons unload: every watch ends and the watcher thread is joined
	void stopFileWatch ();
}
