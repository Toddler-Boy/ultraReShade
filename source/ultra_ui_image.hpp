/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ultra_ui_texture.hpp"

//-----------------------------------------------------------------------------
// Image files (PNG, JPEG, whatever Windows' own decoder reads) as textures, loaded once per device and path, in the
// overlay's colour space: made again when that changes (a game switching to HDR, ReShade's overlay brightness)

namespace ultra::ui
{
	// nullptr when the file is missing or unreadable; asked again only for another path
	[[ nodiscard ]] const Texture* image ( api::effect_runtime* runtime, const std::wstring& path );

	// Pixels made in memory (R in the lowest byte, sRGB) as a texture of the overlay's colour space, made once per device and key; nullptr
	// while the device has none
	[[ nodiscard ]] const Texture* pixelsImage ( api::effect_runtime* runtime, const std::wstring& key, const std::vector<uint32_t>& rgba, int width, int height );

	// A device goes: its images are destroyed now
	void forgetImages ( api::device* device );

	// The file's alpha alone, white, for a tint like the glyphs' (so never converted): height pixels tall, shrunk by area
	// average, at most the file's own height. nullptr as image ()
	[[ nodiscard ]] const Texture* mask ( api::effect_runtime* runtime, const std::wstring& path, int height );

	// The same from pixels made in memory, kept once per device and key
	[[ nodiscard ]] const Texture* pixelsMask ( api::effect_runtime* runtime, const std::wstring& key, const std::vector<uint32_t>& rgba, int width, int height, int shrinkTo );

	// The file's pixels as stored, R in the lowest byte; false when it is missing or unreadable
	[[ nodiscard ]] bool decodeImage ( const std::wstring& path, std::vector<uint32_t>& rgba, int& width, int& height );
}
