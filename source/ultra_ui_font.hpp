/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "ultra_ui_theme.hpp"

//-----------------------------------------------------------------------------
// Text in our own embedded faces (Inter, resources of the DLL), rasterized into a glyph atlas of ours. Sizes are pixel sizes (already
// scaled). ASCII: everything we draw is ours

namespace ultra::ui::font
{
	enum class Face : int { regular, medium, semibold, bold, count };

	// Once per drawn frame, before any text: the atlas for this device and the current scale
	void prepare ( api::device* device );

	// A device goes: its atlas texture is destroyed now
	void forgetDevice ( api::device* device );

	// The face's TTF as the DLL holds it, for ImGui's own font (its text fields edit invisibly in our face); empty without it
	[[ nodiscard ]] std::span<const unsigned char> faceData ( Face face );

	// pos: the top left of the text box; returns the width drawn. tabular: digits all as wide as the widest
	float draw ( ImDrawList* drawList, ImVec2 pos, Face face, float size, ImU32 color, std::string_view text, bool tabular = false );

	[[ nodiscard ]] float width ( Face face, float size, std::string_view text, bool tabular = false );
	[[ nodiscard ]] float lineHeight ( Face face, float size );

	// The box top that centres capital letters in [top, top + height]: optically centred, not by the line box
	[[ nodiscard ]] float centredTop ( Face face, float size, float top, float height );

	// One line rasterized on its own at its size, sharp at any size (no atlas): a coverage byte per pixel, the baseline on the
	// row ascent from the top. Cut and ended with "..." where wider than maxWidth. Empty without the faces. Any thread
	struct Line
	{
		std::vector<uint8_t>	coverage;
		int						width = 0;
		int						height = 0;
		int						ascent = 0;
	};

	[[ nodiscard ]] Line rasterize ( Face face, float size, std::string_view text, float maxWidth );

	// The same line laid out as rasterize lays it out, its glyphs to be rasterized one at a time into a buffer of the caller's
	struct Glyph
	{
		int		index = 0;	// the face's
		float	pen = 0.0f;	// from the text's start
	};

	struct PlacedLine
	{
		int					face = 0;
		float				scale = 0.0f;
		std::vector<Glyph>	glyphs;
		int					width = 0;
		int					height = 0;
		int					ascent = 0;
	};

	// Without the faces: no glyphs. Any thread
	[[ nodiscard ]] PlacedLine placeLine ( Face face, float size, std::string_view text, float maxWidth );

	// A glyph of line into coverage (a byte per pixel, rows of stride), the line's box at left, top: the higher of it and what is
	// there, within the box and the buffer. Rasterized once, copied from a cache after; true when copied. Any thread
	[[ nodiscard ]] bool rasterizeGlyph ( const PlacedLine& line, const Glyph& glyph, uint8_t* coverage, int stride, int rows, int left, int top );

	// The bytes of glyph pixels rasterizeGlyph holds
	[[ nodiscard ]] size_t cachedGlyphBytes ();

	// Where CSS puts the baseline in a line box lineHeight times size tall, from the box's top: the face's ascent and
	// descent centred in it
	[[ nodiscard ]] float baselineInLine ( Face face, float size, float lineHeight );
}
