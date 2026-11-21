/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdint>

#include "ultra_ui_theme.hpp"

//-----------------------------------------------------------------------------
// RGBA textures of our own on the game's device, for ImGui to draw: the glyph atlas and the cover

namespace ultra::ui
{
	struct Texture
	{
		api::device*		device = nullptr;
		api::resource		resource {};
		api::resource_view	view {};
		int					width = 0;
		int					height = 0;

		[[ nodiscard ]] bool valid () const				{ return view.handle != 0; }
		[[ nodiscard ]] ImTextureRef ref () const		{ return ImTextureRef ( ImTextureID ( view.handle ) ); }
	};

	// rgba: width * height pixels, R in the lowest byte
	[[ nodiscard ]] Texture createTexture ( api::device* device, int width, int height, const uint32_t* rgba );

	// rgba: width * height pixels of four half floats, R first
	[[ nodiscard ]] Texture createHalfTexture ( api::device* device, int width, int height, const uint16_t* rgba );

	// The GPU may still read it for a frame or two: freed a few panel frames later, or with its device
	void releaseTexture ( Texture& texture );

	// In a destroy_device event, where nothing is in flight any more
	void destroyTextureNow ( Texture& texture );

	// Once per drawn frame
	void tickTextures ();

	// A device goes: the textures it still holds are destroyed now
	void forgetDevice ( api::device* device );
}
