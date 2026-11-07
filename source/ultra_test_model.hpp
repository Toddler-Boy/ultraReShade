/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

//-----------------------------------------------------------------------------

// The test application's head (res/test_scene/head, the Infinite head scan by Lee Perry-Smith, CC BY 3.0), on the CPU
namespace reshade::ultra
{
	struct TestModelVertex
	{
		float	position[ 3 ];
		float	normal[ 3 ];
		float	uv[ 2 ];
	};

	struct TestModelImage
	{
		uint32_t				width = 0;
		uint32_t				height = 0;
		std::vector<uint8_t>	rgba;
	};

	struct TestModel
	{
		std::vector<TestModelVertex>	vertices;
		std::vector<uint32_t>			indices;
		float							center[ 3 ] {};
		float							radius = 1.0f;
		TestModelImage					colour;
		TestModelImage					normalMap;		// tangent space (green up), no tangents in the mesh
		TestModelImage					specular;
	};

	[[ nodiscard ]] std::optional<TestModel> loadTestHead ( const std::filesystem::path& folder );
}
