/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "effect_front.hpp"
#include "version.h"

void reshadefx::add_builtin_macros(preprocessor &pp, const builtin_macros &macros)
{
	pp.add_macro_definition("__RESHADE__", std::to_string(VERSION_MAJOR * 10000 + VERSION_MINOR * 100 + VERSION_REVISION));
	pp.add_macro_definition("__RESHADE_PERMUTATION__", macros.is_permutation ? "1" : "0");
	pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", macros.is_performance_mode ? "1" : "0");
	pp.add_macro_definition("__VENDOR__", std::to_string(macros.vendor_id));
	pp.add_macro_definition("__DEVICE__", std::to_string(macros.device_id));
	pp.add_macro_definition("__RENDERER__", std::to_string(macros.renderer_id));
	pp.add_macro_definition("__APPLICATION__", std::to_string(macros.application));
	pp.add_macro_definition("BUFFER_WIDTH", std::to_string(macros.width));
	pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(macros.height));
	pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
	pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
	pp.add_macro_definition("BUFFER_COLOR_SPACE", std::to_string(macros.color_space));
	pp.add_macro_definition("BUFFER_COLOR_FORMAT", std::to_string(macros.color_format));
	pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", std::to_string(macros.color_bit_depth));
}

void reshadefx::add_conversion_macros(preprocessor &pp)
{
	pp.append_string(
		"#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\n"
		"#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\n"
		"#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\n"
		"#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\n"
		"#define tex2Dgather0 tex2DgatherR\n"
		"#define tex2Dgather1 tex2DgatherG\n"
		"#define tex2Dgather2 tex2DgatherB\n"
		"#define tex2Dgather3 tex2DgatherA\n");
}

namespace
{
	// FNV-1a, 64 bits: the same on every build of the DLL and of the effect compiler, whatever the bitness
	struct digest
	{
		uint64_t state = 14695981039346656037ull;

		void add(std::string_view bytes)
		{
			for (const char c : bytes)
				state = (state ^ static_cast<unsigned char>(c)) * 1099511628211ull;
		}
	};

	std::string hex(uint64_t value)
	{
		static constexpr char digits[] = "0123456789abcdef";
		std::string text(16, '0');
		for (size_t i = 16; i-- > 0; value >>= 4)
			text[i] = digits[value & 15];
		return text;
	}
}

std::string reshadefx::binary_key(std::string_view entry_point, std::string_view profile, uint32_t compile_flags, std::string_view hlsl)
{
	digest d;
	d.add(profile);
	d.add(";");
	d.add(std::to_string(compile_flags));
	d.add(";");
	d.add(hlsl);

	return std::string(entry_point) + '-' + hex(d.state);
}
