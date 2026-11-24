/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstddef>
#include <cstdint>

//-----------------------------------------------------------------------------

// The rule the add-on states for the game's float render targets (include\reshade_ultra_host_names.h "targets.float"), for the whole
// process. It is copied: nothing here points into the add-on, which states it again each time it loads. The mechanism that follows it
// is the API's own (source\d3d11\ultra_d3d11_float_clone.hpp)
namespace reshade::ultra::floatClone
{
	// True when it differs from the rule stated before: it applies from then on, clones made stay
	[[ nodiscard ]] bool stateRule ( uint32_t formats, const uint32_t* shaders, size_t count );

	// The float_format bits stated, 0 for none
	[[ nodiscard ]] uint32_t ruleFormats ();

	// A pixel shader's CRC-32 (zlib) of its bytecode, and whether the rule names it
	[[ nodiscard ]] uint32_t hashOfCode ( const void* code, size_t size );
	[[ nodiscard ]] bool isTriggerHash ( uint32_t hash );
}
