/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RESHADE_TEST_APPLICATION

// stb_image whole and private to this file: the stb library the runtime links reads PNG only
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "ultra_test_image.hpp"

//-----------------------------------------------------------------------------

namespace
{
	template <typename Pixel>
	void take ( Pixel* pixels, int width, int height, std::vector<Pixel>& into, reshade::ultra::TestPicture& picture )
	{
		if ( pixels == nullptr )
			return;

		picture.width = static_cast<uint32_t> ( width );
		picture.height = static_cast<uint32_t> ( height );
		into.assign ( pixels, pixels + size_t ( width ) * size_t ( height ) * 4 );
		stbi_image_free ( pixels );
	}
}

reshade::ultra::TestPicture reshade::ultra::decodeTestPicture ( std::span<const uint8_t> bytes )
{
	TestPicture	picture;
	auto		width = 0;
	auto		height = 0;
	auto		channels = 0;

	const auto	pixels = stbi_load_from_memory ( bytes.data (), static_cast<int> ( bytes.size () ), &width, &height, &channels, 4 );

	take ( pixels, width, height, picture.rgba, picture );
	return picture;
}

reshade::ultra::TestPicture reshade::ultra::decodeTestPictureFloat ( std::span<const uint8_t> bytes )
{
	TestPicture	picture;
	auto		width = 0;
	auto		height = 0;
	auto		channels = 0;

	const auto	pixels = stbi_loadf_from_memory ( bytes.data (), static_cast<int> ( bytes.size () ), &width, &height, &channels, 4 );

	take ( pixels, width, height, picture.rgbaFloat, picture );
	return picture;
}
//-----------------------------------------------------------------------------

#endif
