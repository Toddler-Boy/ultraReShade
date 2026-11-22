/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include <Windows.h>

#include "dll_log.hpp"
#include "effect_codegen.hpp"
#include "ultra_effect_cache.hpp"

//-----------------------------------------------------------------------------

namespace
{
	struct PrebuiltFile
	{
		uintmax_t										size = 0;
		std::filesystem::file_time_type					time;
		std::shared_ptr<const reshadefx::binary_pack>	pack;
	};

	std::mutex						prebuiltLock;
	std::map<std::wstring, PrebuiltFile>	prebuilt;

	std::mutex						cacheLock;
	std::filesystem::path			cacheFolder;
	bool							isCacheFolderTried = false;
	std::atomic<bool>				isWriteFailureTold { false };

	[[ nodiscard ]] std::wstring environmentValue ( const wchar_t* name )
	{
		const auto		size = GetEnvironmentVariableW ( name, nullptr, 0 );
		std::wstring	value ( size, L'\0' );
		value.resize ( size != 0 ? GetEnvironmentVariableW ( name, value.data (), size ) : 0 );
		return value;
	}

	// Under the lock; empty where there is no folder, with the reason logged
	[[ nodiscard ]] const std::filesystem::path& cacheFolderMade ()
	{
		if ( isCacheFolderTried )
			return cacheFolder;
		isCacheFolderTried = true;

		auto	folder = std::filesystem::path ( environmentValue ( reshade::ultra::shaderCacheVariable ) );

		if ( folder.empty () )
		{
			const auto	appData = environmentValue ( L"LOCALAPPDATA" );

			if ( appData.empty () )
			{
				reshade::log::message ( reshade::log::level::error, "There is no LOCALAPPDATA folder, so compiled effects are not kept in a cache." );
				return cacheFolder;
			}
			folder = std::filesystem::path ( appData ) / L"ultraGame" / L"ShaderCache";
		}

		std::error_code	error;
		std::filesystem::create_directories ( folder, error );

		if ( error )
		{
			reshade::log::message ( reshade::log::level::error, "The shader cache folder '%s' cannot be made (error %d), so compiled effects are not kept in a cache.", folder.u8string ().c_str (), error.value () );
			return cacheFolder;
		}

		cacheFolder = std::move ( folder );
		return cacheFolder;
	}

	[[ nodiscard ]] std::filesystem::path cachePath ( const std::filesystem::path& folder, const std::string& key )
	{
		return folder / std::filesystem::u8path ( key + '-' + reshadefx::d3d_compiler_stamp () + ".cso" );
	}

	// A DXBC container states its own size
	[[ nodiscard ]] bool isWhole ( const std::string& binary )
	{
		uint32_t	size = 0;
		return binary.size () > 28 && std::memcmp ( binary.data (), "DXBC", 4 ) == 0 && ( std::memcpy ( &size, binary.data () + 24, sizeof size ), size == binary.size () );
	}
}
//-----------------------------------------------------------------------------

std::shared_ptr<const reshadefx::binary_pack> reshade::ultra::prebuiltBinaries ( const std::filesystem::path& effect )
{
	auto	path = effect;
	path.replace_extension ( L".fxpack" );

	std::error_code	error;
	const auto		size = std::filesystem::file_size ( path, error );
	if ( error )
		return nullptr;

	const auto	time = std::filesystem::last_write_time ( path, error );
	if ( error )
		return nullptr;

	std::scoped_lock	held ( prebuiltLock );
	auto&				file = prebuilt[ path.native () ];

	if ( file.size == size && file.time == time )
		return file.pack;

	file = { size, time, nullptr };

	auto		pack = std::make_shared<reshadefx::binary_pack> ();
	std::string	why;

	if ( ! pack->read ( path, why ) )
	{
		log::message ( log::level::error, "The prebuilt set '%s' is damaged (%s), so the effect compiles.", path.u8string ().c_str (), why.empty () ? "cannot be opened" : why.c_str () );
		return nullptr;
	}

	file.pack = std::move ( pack );
	return file.pack;
}
//-----------------------------------------------------------------------------

bool reshade::ultra::loadCachedBinary ( const std::string& key, std::string& binary )
{
	std::filesystem::path	path;
	{
		std::scoped_lock	held ( cacheLock );
		const auto&			folder = cacheFolderMade ();

		if ( folder.empty () )
			return false;
		path = cachePath ( folder, key );
	}

	FILE* const	file = _wfsopen ( path.c_str (), L"rb", _SH_DENYNO );
	if ( file == nullptr )
		return false;

	fseek ( file, 0, SEEK_END );
	const auto	size = static_cast<size_t> ( ftell ( file ) );
	fseek ( file, 0, SEEK_SET );

	binary.resize ( size, '\0' );
	const auto	isRead = fread ( binary.data (), 1, size, file ) == size;
	fclose ( file );

	return isRead && isWhole ( binary );
}
//-----------------------------------------------------------------------------

void reshade::ultra::saveCachedBinary ( const std::string& key, const std::string& binary )
{
	std::filesystem::path	path;
	{
		std::scoped_lock	held ( cacheLock );
		const auto&			folder = cacheFolderMade ();

		if ( folder.empty () )
			return;
		path = cachePath ( folder, key );
	}

	// Another game may be reading or writing the same file: written aside, then moved into place
	auto	aside = path;
	aside += L"." + std::to_wstring ( GetCurrentProcessId () ) + L"-" + std::to_wstring ( GetCurrentThreadId () ) + L".tmp";

	FILE* const	file = _wfsopen ( aside.c_str (), L"wb", _SH_DENYWR );
	auto		isSaved = false;
	auto		code = 0;

	if ( file != nullptr )
	{
		const auto	isWritten = fwrite ( binary.data (), 1, binary.size (), file ) == binary.size ();
		const auto	isClosed = fclose ( file ) == 0;
		code = errno;
		isSaved = isClosed && isWritten;

		if ( isSaved && MoveFileExW ( aside.c_str (), path.c_str (), MOVEFILE_REPLACE_EXISTING ) == 0 )
		{
			code = static_cast<int> ( GetLastError () );
			isSaved = false;
		}

		if ( ! isSaved )
			DeleteFileW ( aside.c_str () );
	}
	else
	{
		code = errno;
	}

	if ( ! isSaved && ! isWriteFailureTold.exchange ( true ) )
		log::message ( log::level::error, "The shader cache file '%s' cannot be written (error %d), so compiled effects are not all kept in the cache.", path.u8string ().c_str (), code );
}
