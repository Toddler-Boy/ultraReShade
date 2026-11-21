/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdint>
#include <span>
#include <vector>

//-----------------------------------------------------------------------------

// The test application's images in every format stb_image reads (JPEG, HDR, ...); the runtime's own reader is limited to the
// formats of the effects' textures
namespace reshade::ultra
{
	struct TestPicture
	{
		uint32_t				width = 0;
		uint32_t				height = 0;
		std::vector<uint8_t>	rgba;			// 8 bits per channel
		std::vector<float>		rgbaFloat;		// linear, for decodeTestPictureFloat
	};

	// Empty for an image that does not read
	[[ nodiscard ]] TestPicture decodeTestPicture ( std::span<const uint8_t> bytes );
	[[ nodiscard ]] TestPicture decodeTestPictureFloat ( std::span<const uint8_t> bytes );
}
