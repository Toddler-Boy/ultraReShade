/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <filesystem>
#include <string>

//-----------------------------------------------------------------------------

// Settings from ultraGame: with ultragame.ini at the base path ReShade reads it (curated, never written) with
// ultragame-user.ini on top and writes only into the latter; reshade::ini_file does the layering
namespace reshade::ultra
{
	// An ultragame.ini with [INSTALL] UltraGameConfig=1: ultraGame wrote ReShade's sections into it. Older installs have
	// one with the add-on's section only, they keep their ReShade.ini
	[[ nodiscard ]] bool isCuratedConfigFile ( const std::filesystem::path& path );

	// Decided once, from the base path, which must be set before
	[[ nodiscard ]] bool isConfigLayered ();

	// ultragame or ReShade: global_config () and the effect runtimes' files are named after it
	[[ nodiscard ]] std::string configBaseName ();

	// One of the curated files the effect runtimes use (ultragame.ini, ultragame2.ini, ultragameVR.ini, ...)
	[[ nodiscard ]] bool isCuratedConfig ( const std::filesystem::path& path );
	[[ nodiscard ]] std::filesystem::path userConfigPath ( const std::filesystem::path& curated );

	// The file [INSTALL] BasePath is read from before the base path is known
	[[ nodiscard ]] std::filesystem::path basePathConfig ( const std::filesystem::path& folder );

	// ReShade.log beside the config, also when layered (ultragame.log belongs to the add-on)
	[[ nodiscard ]] std::filesystem::path logPath ();

	// Once the log is open: which files the settings come from
	void logConfigLayers ();
}
