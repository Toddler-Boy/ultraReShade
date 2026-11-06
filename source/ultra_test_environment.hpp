/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

//-----------------------------------------------------------------------------

// The test scene's light probe: an equirectangular panorama prefiltered for GGX, one roughness step per mip, and its
// irradiance as nine spherical harmonics. Direction mapping: u = 0.5 looks at the camera (-z), v runs from up to down
namespace reshade::ultra
{
	struct TestEnvironment
	{
		struct Level
		{
			uint32_t			width = 0;
			uint32_t			height = 0;
			std::vector<float>	rgba;
		};

		std::vector<Level>						levels;			// level i: roughness i / ( levels - 1 )
		std::array<std::array<float, 4>, 9>		irradiance {};	// rgb per coefficient, the shaders' basis order
	};

	// The panorama in file (Radiance .hdr), scaled to a fixed mean brightness; a studio gradient when file is empty or
	// can't be read (logged as an error)
	[[ nodiscard ]] TestEnvironment makeTestEnvironment ( const std::filesystem::path& file );
}
