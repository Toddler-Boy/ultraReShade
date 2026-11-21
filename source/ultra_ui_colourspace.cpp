/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <mutex>

#include "ini_file.hpp"
#include "ultra_ui_colourspace.hpp"

//-----------------------------------------------------------------------------
// The curves and matrix as ReShade's imgui_hdr.hlsl has them, so the image matches the overlay around it

namespace ultra::ui
{
	namespace
	{
		[[ nodiscard ]] float pq ( float v )	// 1.0 = 10000 nits
		{
			constexpr float	m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
			const auto		p = std::pow ( std::max ( v, 0.0f ), m1 );
			return std::pow ( ( c1 + c2 * p ) / ( 1.0f + c3 * p ), m2 );
		}

		[[ nodiscard ]] float hlg ( float v )	// 1.0 = 1000 nits
		{
			constexpr float	a = 0.17883277f, b = 0.28466892f, c = 0.559910714626312255859375f;
			v = std::max ( v, 0.0f );
			return v <= 1.0f / 12.0f ? std::sqrt ( 3.0f * v ) : a * std::log ( 12.0f * v - b ) + c;
		}

		[[ nodiscard ]] std::array<float, 3> toBt2020 ( float r, float g, float b )
		{
			return { 0.627403914928436279296875f * r + 0.3292830288410186767578125f * g + 0.0433130674064159393310546875f * b,
					 0.069097287952899932861328125f * r + 0.9195404052734375f * g + 0.011362315155565738677978515625f * b,
					 0.01639143936336040496826171875f * r + 0.08801330626010894775390625f * g + 0.895595252513885498046875f * b };
		}

		// The effect runtime does not say its swap chain's colour space; the last presented one is the one the overlay
		// goes onto. A game switching HDR on or off changes it here, the cover follows
		std::atomic<uint32_t>	presentedSpace { uint32_t ( api::color_space::srgb ) };
		std::atomic<bool>		isPresentedMaybeHdr { false };

		constexpr const char*	overrideSection = "STYLE";
		constexpr const char*	overrideKey = "HdrOverlayOverwriteColorSpaceTo";

		[[ nodiscard ]] uint16_t half ( float f )
		{
			const auto	bits = std::bit_cast<uint32_t> ( f );
			const auto	sign = ( bits >> 16 ) & 0x8000u;
			const auto	exponent = int ( ( bits >> 23 ) & 0xFF ) - 127 + 15;
			const auto	mantissa = bits & 0x7FFFFFu;

			if ( exponent <= 0 )
				return uint16_t ( sign );
			if ( exponent >= 31 )
				return uint16_t ( sign | 0x7BFFu );

			// Rounded to nearest; a mantissa rounding up carries into the exponent
			return uint16_t ( sign + ( uint32_t ( exponent ) << 10 ) + ( ( mantissa + 0x1000u ) >> 13 ) );
		}
	}

	void notePresent ( api::swapchain* swapchain )
	{
		const auto	space = swapchain->get_color_space ();
		const auto	format = swapchain->get_device ()->get_resource_desc ( swapchain->get_current_back_buffer () ).texture.format;
		presentedSpace.store ( uint32_t ( space ), std::memory_order_relaxed );
		isPresentedMaybeHdr.store ( format == api::format::r16g16b16a16_float || space == api::color_space::hdr10_pq, std::memory_order_relaxed );
	}

	OverlayLook overlayLook ( api::effect_runtime* )
	{
		OverlayLook	look;
		look.space = api::color_space ( presentedSpace.load ( std::memory_order_relaxed ) );

		if ( const auto overwrite = overlayColourSpaceOverride (); overwrite != 0 )
			look.space = api::color_space ( overwrite );

		return look;
	}

	int overlayColourSpaceOverride ()
	{
		int	overwrite = 0;
		reshade::global_config ().get ( overrideSection, overrideKey, overwrite );
		return overwrite;
	}

	void setOverlayColourSpaceOverride ( int space )
	{
		reshade::global_config ().set ( overrideSection, overrideKey, space );
	}

	bool isOverlayColourSpaceShown ()
	{
		return isPresentedMaybeHdr.load ( std::memory_order_relaxed ) || overlayColourSpaceOverride () != 0;
	}

	void encodeForOverlay ( const uint32_t* srgb, size_t n, const OverlayLook& look, std::vector<uint16_t>& out )
	{
		// The overlay's white, as its shader makes it: each pixel divided by it, the multiply in the shader undoes that
		const auto	nits = overlayWhiteNits;
		const auto	white = look.space == api::color_space::hdr10_pq ? pq ( nits / 10000.0f ) : look.space == api::color_space::hdr10_hlg ? hlg ( nits / 1000.0f ) : 1.0f;

		std::array<float, 256>	linear;
		for ( size_t i = 0; i < linear.size (); ++i )
			linear[ i ] = std::pow ( float ( i ) / 255.0f, 2.2f );

		out.resize ( n * 4 );
		for ( size_t i = 0; i < n; ++i )
		{
			const auto	p = srgb[ i ];
			float		c[ 3 ] = { linear[ p & 0xFF ], linear[ ( p >> 8 ) & 0xFF ], linear[ ( p >> 16 ) & 0xFF ] };

			if ( look.space == api::color_space::hdr10_pq || look.space == api::color_space::hdr10_hlg )
			{
				const auto	wide = toBt2020 ( c[ 0 ], c[ 1 ], c[ 2 ] );
				for ( int k = 0; k < 3; ++k )
					c[ k ] = look.space == api::color_space::hdr10_pq ? pq ( wide[ size_t ( k ) ] * nits / 10000.0f ) / white : hlg ( wide[ size_t ( k ) ] * nits / 1000.0f ) / white;
			}

			for ( int k = 0; k < 3; ++k )
				out[ i * 4 + size_t ( k ) ] = half ( c[ k ] );
			out[ i * 4 + 3 ] = half ( float ( p >> 24 ) / 255.0f );
		}
	}
}
