/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RESHADE_TEST_APPLICATION

#include <algorithm>
#include <cmath>
#include <execution>
#include <fstream>
#include <iterator>
#include <numbers>
#include <numeric>
#include <optional>

#include <stb_image.h>

#include "dll_log.hpp"
#include "ultra_test_environment.hpp"

//-----------------------------------------------------------------------------

namespace
{
	using Level = reshade::ultra::TestEnvironment::Level;

	constexpr uint32_t	baseWidth = 512;
	constexpr uint32_t	sourceWidth = 128;			// what the rough levels and the irradiance are integrated from
	constexpr size_t	levelCount = 5;
	constexpr float		meanRadiance = 0.25f;		// every panorama is scaled to this mean luminance, so no exposure per file
	constexpr float		pi = std::numbers::pi_v<float>;

	struct Direction
	{
		float	x = 0.0f;
		float	y = 0.0f;
		float	z = 0.0f;
	};

	[[ nodiscard ]] Direction direction ( float u, float v )
	{
		const auto	phi = ( u - 0.5f ) * 2.0f * pi;
		const auto	theta = v * pi;
		return { -std::sin ( theta ) * std::sin ( phi ), std::cos ( theta ), -std::sin ( theta ) * std::cos ( phi ) };
	}

	[[ nodiscard ]] float dot ( const Direction& a, const Direction& b )
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	[[ nodiscard ]] Direction texelDirection ( const Level& level, uint32_t x, uint32_t y )
	{
		return direction ( ( static_cast<float> ( x ) + 0.5f ) / static_cast<float> ( level.width ), ( static_cast<float> ( y ) + 0.5f ) / static_cast<float> ( level.height ) );
	}

	// The solid angle a texel of row y covers
	[[ nodiscard ]] float texelSolidAngle ( const Level& level, uint32_t y )
	{
		const auto	theta = ( static_cast<float> ( y ) + 0.5f ) / static_cast<float> ( level.height ) * pi;
		return ( 2.0f * pi / static_cast<float> ( level.width ) ) * ( pi / static_cast<float> ( level.height ) ) * std::sin ( theta );
	}

	[[ nodiscard ]] Level makeLevel ( uint32_t width )
	{
		Level	level { width, std::max ( width / 2, 1u ), {} };
		level.rgba.resize ( size_t ( level.width ) * level.height * 4 );
		return level;
	}

	// Each texel averages the source texels its area covers (the nearest one when the source is smaller)
	[[ nodiscard ]] Level resample ( const Level& source, uint32_t width )
	{
		auto	out = makeLevel ( width );

		for ( uint32_t y = 0; y < out.height; ++y )
			for ( uint32_t x = 0; x < out.width; ++x )
			{
				const auto	x0 = size_t ( x ) * source.width / out.width;
				const auto	y0 = size_t ( y ) * source.height / out.height;
				const auto	x1 = std::max ( x0 + 1, size_t ( x + 1 ) * source.width / out.width );
				const auto	y1 = std::max ( y0 + 1, size_t ( y + 1 ) * source.height / out.height );
				float		sum[ 4 ] {};

				for ( auto sy = y0; sy < y1; ++sy )
					for ( auto sx = x0; sx < x1; ++sx )
						for ( auto c = 0; c < 4; ++c )
							sum[ c ] += source.rgba[ ( sy * source.width + sx ) * 4 + c ];

				for ( auto c = 0; c < 4; ++c )
					out.rgba[ ( size_t ( y ) * out.width + x ) * 4 + c ] = sum[ c ] / static_cast<float> ( ( x1 - x0 ) * ( y1 - y0 ) );
			}

		return out;
	}

	// The light arriving along each direction through a GGX lobe of that roughness, as a Phong lobe around the mirror
	// direction (a quarter of the half-vector exponent)
	[[ nodiscard ]] Level prefilter ( const Level& source, uint32_t width, float roughness )
	{
		std::vector<Direction>	directions;
		std::vector<float>		solidAngles;

		for ( uint32_t y = 0; y < source.height; ++y )
			for ( uint32_t x = 0; x < source.width; ++x )
			{
				directions.push_back ( texelDirection ( source, x, y ) );
				solidAngles.push_back ( texelSolidAngle ( source, y ) );
			}

		const auto	alpha = roughness * roughness;
		const auto	exponent = std::max ( ( 2.0f / ( alpha * alpha ) - 2.0f ) / 4.0f, 0.0f );
		auto		out = makeLevel ( width );

		std::vector<uint32_t>	rows ( out.height );
		std::iota ( rows.begin (), rows.end (), 0u );

		std::for_each ( std::execution::par, rows.begin (), rows.end (), [ & ] ( uint32_t y ) {
			for ( uint32_t x = 0; x < out.width; ++x )
			{
				const auto	reflected = texelDirection ( out, x, y );
				float		sum[ 3 ] {};
				auto		weightSum = 0.0f;

				for ( size_t i = 0; i < directions.size (); ++i )
				{
					const auto	cosine = dot ( reflected, directions[ i ] );

					if ( cosine <= 0.0f )
						continue;

					const auto	weight = std::pow ( cosine, exponent ) * cosine * solidAngles[ i ];
					weightSum += weight;

					for ( auto c = 0; c < 3; ++c )
						sum[ c ] += source.rgba[ i * 4 + c ] * weight;
				}

				for ( auto c = 0; c < 3; ++c )
					out.rgba[ ( size_t ( y ) * out.width + x ) * 4 + c ] = weightSum > 0.0f ? sum[ c ] / weightSum : 0.0f;

				out.rgba[ ( size_t ( y ) * out.width + x ) * 4 + 3 ] = 1.0f;
			}
		} );

		return out;
	}

	[[ nodiscard ]] std::array<float, 9> shBasis ( const Direction& d )
	{
		return { 0.282095f, 0.488603f * d.y, 0.488603f * d.z, 0.488603f * d.x, 1.092548f * d.x * d.y, 1.092548f * d.y * d.z, 0.315392f * ( 3.0f * d.z * d.z - 1.0f ),
				 1.092548f * d.x * d.z, 0.546274f * ( d.x * d.x - d.y * d.y ) };
	}

	// Radiance projected on the basis, then convolved with the cosine lobe (Ramamoorthi and Hanrahan)
	[[ nodiscard ]] std::array<std::array<float, 4>, 9> irradiance ( const Level& source )
	{
		std::array<std::array<float, 4>, 9>	coefficients {};

		for ( uint32_t y = 0; y < source.height; ++y )
			for ( uint32_t x = 0; x < source.width; ++x )
			{
				const auto	basis = shBasis ( texelDirection ( source, x, y ) );
				const auto	solidAngle = texelSolidAngle ( source, y );
				const auto	texel = &source.rgba[ ( size_t ( y ) * source.width + x ) * 4 ];

				for ( size_t i = 0; i < 9; ++i )
					for ( auto c = 0; c < 3; ++c )
						coefficients[ i ][ c ] += texel[ c ] * basis[ i ] * solidAngle;
			}

		constexpr float	bandScale[ 9 ] = { pi, 2.0f * pi / 3.0f, 2.0f * pi / 3.0f, 2.0f * pi / 3.0f, pi / 4.0f, pi / 4.0f, pi / 4.0f, pi / 4.0f, pi / 4.0f };

		for ( size_t i = 0; i < 9; ++i )
			for ( auto c = 0; c < 3; ++c )
				coefficients[ i ][ c ] *= bandScale[ i ];

		return coefficients;
	}

	// Bright from above, a grey horizon, a dark floor
	[[ nodiscard ]] Level studioGradient ()
	{
		auto	level = makeLevel ( 256 );

		for ( uint32_t y = 0; y < level.height; ++y )
		{
			const auto	up = texelDirection ( level, 0, y ).y;
			const auto	sky = std::clamp ( up, 0.0f, 1.0f );
			const auto	floor = std::clamp ( -up, 0.0f, 1.0f );
			const float	colour[ 3 ] = { 0.5f + 0.6f * sky - 0.42f * floor, 0.5f + 0.56f * sky - 0.42f * floor, 0.5f + 0.5f * sky - 0.43f * floor };

			for ( uint32_t x = 0; x < level.width; ++x )
			{
				const auto	texel = &level.rgba[ ( size_t ( y ) * level.width + x ) * 4 ];
				std::copy ( std::begin ( colour ), std::end ( colour ), texel );
				texel[ 3 ] = 1.0f;
			}
		}

		return level;
	}

	[[ nodiscard ]] std::optional<Level> loadPanorama ( const std::filesystem::path& file )
	{
		std::ifstream	stream ( file, std::ios::binary );

		if ( ! stream )
			return std::nullopt;

		const std::vector<unsigned char>	bytes { std::istreambuf_iterator<char> ( stream ), std::istreambuf_iterator<char> () };
		auto								width = 0;
		auto								height = 0;
		auto								channels = 0;
		const auto							pixels = stbi_loadf_from_memory ( bytes.data (), static_cast<int> ( bytes.size () ), &width, &height, &channels, 4 );

		if ( pixels == nullptr )
			return std::nullopt;

		Level	level { static_cast<uint32_t> ( width ), static_cast<uint32_t> ( height ), { pixels, pixels + size_t ( width ) * height * 4 } };
		stbi_image_free ( pixels );
		return level;
	}
}
//-----------------------------------------------------------------------------

reshade::ultra::TestEnvironment reshade::ultra::makeTestEnvironment ( const std::filesystem::path& file )
{
	auto	panorama = file.empty () ? std::nullopt : loadPanorama ( file );

	if ( ! file.empty () && ! panorama.has_value () )
		reshade::log::message ( reshade::log::level::error, "Test application: the panorama '%s' could not be read, the studio gradient lights the scene.", file.u8string ().c_str () );

	if ( ! panorama.has_value () )
		panorama = studioGradient ();

	TestEnvironment	environment;
	const auto		source = resample ( *panorama, sourceWidth );
	environment.irradiance = irradiance ( source );
	environment.levels.push_back ( resample ( *panorama, baseWidth ) );

	for ( size_t i = 1; i < levelCount; ++i )
		environment.levels.push_back ( prefilter ( source, baseWidth >> i, static_cast<float> ( i ) / static_cast<float> ( levelCount - 1 ) ) );

	// The mean radiance is the first coefficient's share of the whole sphere
	const auto&	ambient = environment.irradiance[ 0 ];
	const auto	luminance = ( 0.2126f * ambient[ 0 ] + 0.7152f * ambient[ 1 ] + 0.0722f * ambient[ 2 ] ) / ( pi * 0.282095f * 4.0f * pi );
	const auto	scale = luminance > 0.0f ? meanRadiance / luminance : 1.0f;

	for ( auto& level : environment.levels )
		for ( size_t i = 0; i < level.rgba.size (); ++i )
			if ( i % 4 != 3 )
				level.rgba[ i ] *= scale;

	for ( auto& coefficient : environment.irradiance )
		for ( auto c = 0; c < 3; ++c )
			coefficient[ c ] *= scale;

	return environment;
}
//-----------------------------------------------------------------------------

#endif
