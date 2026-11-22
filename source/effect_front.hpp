/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "effect_preprocessor.hpp"
#include <cstdint>
#include <string>
#include <string_view>

// What the runtime and the build-time effect compiler (ultra_effect_compile) share, so both give the same text to the compiler
// and name its binary the same
namespace reshadefx
{
	/// <summary>
	/// What the preprocessor is told about the device and the target an effect is built for.
	/// </summary>
	struct builtin_macros
	{
		uint32_t vendor_id = 0;
		uint32_t device_id = 0;
		uint32_t renderer_id = 0;
		uint32_t application = 0;
		bool is_permutation = false;
		bool is_performance_mode = false;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t color_space = 0;
		uint32_t color_format = 0;
		uint32_t color_bit_depth = 0;
	};

	/// <summary>
	/// Defines __RESHADE__, __VENDOR__, BUFFER_COLOR_SPACE and the other macros every effect is preprocessed with.
	/// </summary>
	void add_builtin_macros(preprocessor &pp, const builtin_macros &macros);

	/// <summary>
	/// Appends the macros of the older names of the texture functions, after the include paths and before the effect's file.
	/// </summary>
	void add_conversion_macros(preprocessor &pp);

	/// <summary>
	/// The name of an assembled binary: its entry point and a 64-bit digest of the profile, the compile flags and the HLSL (with no debug
	/// information, which holds the files' folders, it is the same in whichever folder the effect is installed).
	/// </summary>
	std::string binary_key(std::string_view entry_point, std::string_view profile, uint32_t compile_flags, std::string_view hlsl);
}
