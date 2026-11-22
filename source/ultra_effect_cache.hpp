/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "effect_pack.hpp"

// Where an effect's assembled binaries come from before the compiler is asked: the set built with the effect (its .fxpack beside it),
// then the cache every game shares
namespace reshade::ultra
{
	// Names another folder for the shared cache (the test application's run)
	inline constexpr wchar_t	shaderCacheVariable[] = L"ULTRAGAME_SHADER_CACHE";

	// The binaries built beside the effect file ('<name>.fxpack', ultra_effect_compile's); null where it has none, and where it does
	// not read (logged once). Read once while the file stays as it is
	[[ nodiscard ]] std::shared_ptr<const reshadefx::binary_pack> prebuiltBinaries ( const std::filesystem::path& effect );

	// The shared cache, %LOCALAPPDATA%\ultraGame\ShaderCache, made with the first save; a binary is named by its key and the D3D compiler's
	// build. A folder or file that cannot be made is logged once, the effects compile all the same
	[[ nodiscard ]] bool loadCachedBinary ( const std::string& key, std::string& binary );
	void saveCachedBinary ( const std::string& key, const std::string& binary );
}
