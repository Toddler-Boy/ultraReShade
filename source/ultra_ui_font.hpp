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
// Text in Inter, a resource of the DLL, drawn through DirectWrite: the text is shaped with the font's own kerning and features, a character
// Inter lacks comes from the system font Windows names for it, one no font has shows Inter's replacement character. Every glyph, of any
// code point, is rendered the first time it is drawn, in the size it is drawn in, into an atlas of ours. Sizes are pixel sizes (already
// scaled); the four faces are the weights 400, 500, 600 and 700 of the variable font

namespace ultra::ui::font
{
	enum class Face : int { regular, medium, semibold, bold, count };

	// Once per drawn frame, before any text: the atlas for this device; a full one starts over here
	void prepare ( api::device* device );

	// Before the frame's draw data is rendered: the glyphs rendered since the last frame go into this device's atlas texture
	void flush ( api::command_list* cmdList, api::device* device );

	// A device goes: its atlas texture is destroyed now
	void forgetDevice ( api::device* device );

	// The font file as the DLL holds it, for ImGui's own font (its text fields edit invisibly in it); empty without it
	[[ nodiscard ]] std::span<const unsigned char> faceData ();

	// pos: the top left of the text box; returns the width drawn. tabular: digits all as wide as the widest
	float draw ( ImDrawList* drawList, ImVec2 pos, Face face, float size, ImU32 color, std::string_view text, bool tabular = false );

	[[ nodiscard ]] float width ( Face face, float size, std::string_view text, bool tabular = false );
	[[ nodiscard ]] float lineHeight ( Face face, float size );

	// The box top that centres capital letters in [top, top + height]: optically centred, not by the line box
	[[ nodiscard ]] float centredTop ( Face face, float size, float top, float height );

	// One line of text laid out for rendering glyph by glyph into a buffer of the caller's, on any thread. A glyph's index holds the font
	// it is of in its high half and the glyph's own in its low half; pen is its place from the text's start
	struct Glyph
	{
		int		index = 0;
		float	pen = 0.0f;
	};

	struct PlacedLine
	{
		int					face = 0;
		float				scale = 0.0f;		// the size in pixels
		std::vector<Glyph>	glyphs;
		int					width = 0;
		int					height = 0;
		int					ascent = 0;
	};

	// Cut and ended with the ellipsis where wider than maxWidth. Without the font: no glyphs. Any thread
	[[ nodiscard ]] PlacedLine placeLine ( Face face, float size, std::string_view text, float maxWidth );

	// A glyph of line into coverage (a byte per pixel, rows of stride), the line's box at left, top: the higher of it and what is
	// there, within the box and the buffer. Rendered once, copied from a cache after; true when copied. Any thread
	[[ nodiscard ]] bool rasterizeGlyph ( const PlacedLine& line, const Glyph& glyph, uint8_t* coverage, int stride, int rows, int left, int top );

	// The bytes of glyph pixels rasterizeGlyph holds
	[[ nodiscard ]] size_t cachedGlyphBytes ();

	// Where CSS puts the baseline in a line box lineHeight times size tall, from the box's top: the face's ascent and
	// descent centred in it
	[[ nodiscard ]] float baselineInLine ( Face face, float size, float lineHeight );
}
