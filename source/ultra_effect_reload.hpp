/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <filesystem>
#include <vector>

//-----------------------------------------------------------------------------

// Recompiling effects while the game runs, alongside the add-on reload (development builds only): the folders of each
// runtime's effect search paths are watched for .fx, .fxh and .addonfx files
namespace reshade::ultra
{
#if ULTRARESHADE_DEVELOPMENT
	// Each present, on the runtime's presenting thread: watches the folders searchPaths name, again when they change
	void followEffectSearchPaths ( const void* runtime, const std::vector<std::filesystem::path>& searchPaths );

	// The files changed in the runtime's folders, once none changed for a while and nobody writes them (a rename counts
	// by both names); empty until then
	[[ nodiscard ]] std::vector<std::filesystem::path> takeEffectChanges ( const void* runtime );

	// The runtime is destroyed
	void forgetEffectWatch ( const void* runtime );

	void stopEffectReload ();
#else
	inline void forgetEffectWatch ( const void* ) {}
#endif
}
