/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdint>
#include <vector>

#include "ultra_ui_theme.hpp"

//-----------------------------------------------------------------------------
// ReShade's overlay converts vertex colours to the swap chain's colour space but samples textures as they are, so an
// image is converted here to land, times the converted white, where its sRGB colours belong. The overlay's white is the
// same everywhere

namespace ultra::ui
{
	inline constexpr float	overlayWhiteNits = 203.0f;		// HDR reference white as per BT.2408, the one ReShade's overlay uses

	struct OverlayLook
	{
		api::color_space	space = api::color_space::srgb;

		[[ nodiscard ]] bool hdr () const	{ return space == api::color_space::scrgb || space == api::color_space::hdr10_pq || space == api::color_space::hdr10_hlg; }
		[[ nodiscard ]] bool operator== ( const OverlayLook& ) const = default;
	};

	// A swap chain presented: the colour space the overlay goes onto. The built-in add-on's present event
	void notePresent ( api::swapchain* swapchain );

	// What the overlay draws in now: the game's override if it has one, else the swap chain's
	[[ nodiscard ]] OverlayLook overlayLook ( api::effect_runtime* runtime );

	// The game's override of the overlay's colour space for a game that states the wrong one: 0 none, else api::color_space
	// (1 SDR, 2 scRGB, 3 HDR10, 4 HLG), curated in the game's reshade.json or set by the player into reshade-user.json
	[[ nodiscard ]] int overlayColourSpaceOverride ();
	void setOverlayColourSpaceOverride ( int space );

	// While the swap chain may be HDR, or an override is set
	[[ nodiscard ]] bool isOverlayColourSpaceShown ();

	// srgb: n pixels, R in the lowest byte; out: n pixels of four half floats for an hdr () look
	void encodeForOverlay ( const uint32_t* srgb, size_t n, const OverlayLook& look, std::vector<uint16_t>& out );
}
