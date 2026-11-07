/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RESHADE_TEST_APPLICATION

#include <algorithm>
#include <cmath>
#include <fstream>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <stb_image.h>

#include "ultra_test_model.hpp"

//-----------------------------------------------------------------------------

namespace
{
	[[ nodiscard ]] std::vector<uint8_t> readFile ( const std::filesystem::path& path )
	{
		std::ifstream	file ( path, std::ios::binary );

		return { std::istreambuf_iterator<char> ( file ), std::istreambuf_iterator<char> () };
	}

	[[ nodiscard ]] bool loadImage ( const std::filesystem::path& path, reshade::ultra::TestModelImage& image )
	{
		const auto	bytes = readFile ( path );
		int			width = 0;
		int			height = 0;
		int			channels = 0;
		const auto	pixels = stbi_load_from_memory ( bytes.data (), static_cast<int> ( bytes.size () ), &width, &height, &channels, 4 );

		if ( pixels == nullptr )
			return false;

		image.width = static_cast<uint32_t> ( width );
		image.height = static_cast<uint32_t> ( height );
		image.rgba.assign ( pixels, pixels + size_t ( width ) * size_t ( height ) * 4 );
		stbi_image_free ( pixels );
		return true;
	}

	[[ nodiscard ]] bool readMesh ( const cgltf_data& data, reshade::ultra::TestModel& model )
	{
		if ( data.meshes_count == 0 || data.meshes[ 0 ].primitives_count == 0 )
			return false;

		const auto&		primitive = data.meshes[ 0 ].primitives[ 0 ];
		cgltf_accessor*	positions = nullptr;
		cgltf_accessor*	normals = nullptr;
		cgltf_accessor*	uvs = nullptr;

		for ( cgltf_size i = 0; i < primitive.attributes_count; ++i )
		{
			const auto&	attribute = primitive.attributes[ i ];

			if ( attribute.type == cgltf_attribute_type_position )
				positions = attribute.data;
			else if ( attribute.type == cgltf_attribute_type_normal )
				normals = attribute.data;
			else if ( attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0 )
				uvs = attribute.data;
		}

		if ( positions == nullptr || normals == nullptr || uvs == nullptr || primitive.indices == nullptr )
			return false;

		model.vertices.resize ( positions->count );

		for ( cgltf_size i = 0; i < positions->count; ++i )
		{
			auto&	vertex = model.vertices[ i ];

			if ( ! cgltf_accessor_read_float ( positions, i, vertex.position, 3 ) || ! cgltf_accessor_read_float ( normals, i, vertex.normal, 3 )
				 || ! cgltf_accessor_read_float ( uvs, i, vertex.uv, 2 ) )
				return false;

			// The scan's maps are stored upside down to its uvs (three.js loads them flipped)
			vertex.uv[ 1 ] = 1.0f - vertex.uv[ 1 ];
		}

		model.indices.resize ( primitive.indices->count );

		for ( cgltf_size i = 0; i < primitive.indices->count; ++i )
			model.indices[ i ] = static_cast<uint32_t> ( cgltf_accessor_read_index ( primitive.indices, i ) );

		float	lo[ 3 ] { 1e30f, 1e30f, 1e30f };
		float	hi[ 3 ] { -1e30f, -1e30f, -1e30f };

		for ( const auto& vertex : model.vertices )
			for ( auto axis = 0; axis < 3; ++axis )
			{
				lo[ axis ] = std::min ( lo[ axis ], vertex.position[ axis ] );
				hi[ axis ] = std::max ( hi[ axis ], vertex.position[ axis ] );
			}

		float	radiusSquared = 0.0f;

		for ( auto axis = 0; axis < 3; ++axis )
			model.center[ axis ] = 0.5f * ( lo[ axis ] + hi[ axis ] );

		for ( const auto& vertex : model.vertices )
		{
			const auto	dx = vertex.position[ 0 ] - model.center[ 0 ];
			const auto	dy = vertex.position[ 1 ] - model.center[ 1 ];
			const auto	dz = vertex.position[ 2 ] - model.center[ 2 ];

			radiusSquared = std::max ( radiusSquared, dx * dx + dy * dy + dz * dz );
		}

		model.radius = std::sqrt ( radiusSquared );
		return model.radius > 0.0f;
	}
}
//-----------------------------------------------------------------------------

std::optional<reshade::ultra::TestModel> reshade::ultra::loadTestHead ( const std::filesystem::path& folder )
{
	const auto		glb = readFile ( folder / L"LeePerrySmith.glb" );
	cgltf_options	options {};
	cgltf_data*		data = nullptr;

	if ( glb.empty () || cgltf_parse ( &options, glb.data (), glb.size (), &data ) != cgltf_result_success )
		return std::nullopt;

	TestModel	model;
	const auto	meshRead = cgltf_load_buffers ( &options, data, nullptr ) == cgltf_result_success && readMesh ( *data, model );

	cgltf_free ( data );

	if ( ! meshRead || ! loadImage ( folder / L"Map-COL.jpg", model.colour ) || ! loadImage ( folder / L"Infinite-Level_02_Tangent_SmoothUV.jpg", model.normalMap )
		 || ! loadImage ( folder / L"Map-SPEC.jpg", model.specular ) )
		return std::nullopt;

	return model;
}
//-----------------------------------------------------------------------------

#endif
