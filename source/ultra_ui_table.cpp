/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <string>
#include <vector>

#include <Windows.h>

#include <reshade.hpp>
#include <reshade_ultra.hpp>

#include "ultra_ui_colourspace.hpp"
#include "ultra_ui_convert.hpp"
#include "ultra_ui_font.hpp"
#include "ultra_ui_image.hpp"
#include "ultra_ui_layout.hpp"
#include "ultra_ui_texture.hpp"
#include "ultra_ui_theme.hpp"
#include "ultra_ui_widgets.hpp"
#include "ultra_ui_window.hpp"

//-----------------------------------------------------------------------------
// ReShadeGetWidgetTable (include/reshade_ultra_ui.h): ultraReShade's widgets, look, text and images as plain C functions, each one
// the C++ of its own file turned into the C form. The built-in add-on's events keep the textures of a device that goes

namespace ultra::ui
{
namespace
{
	[[ nodiscard ]] uint32_t resultOf ( const Edit& edit )
	{
		return ( edit.changed ? RESHADE_UI_CHANGED : 0u ) | ( edit.committed ? RESHADE_UI_COMMITTED : 0u );
	}

	[[ nodiscard ]] std::wstring wide ( const char* utf8 )
	{
		if ( utf8 == nullptr || *utf8 == 0 )
			return {};
		const auto		n = MultiByteToWideChar ( CP_UTF8, 0, utf8, -1, nullptr, 0 );
		std::wstring	out ( size_t ( n > 1 ? n - 1 : 0 ), L'\0' );
		MultiByteToWideChar ( CP_UTF8, 0, utf8, -1, out.data (), n );
		return out;
	}

	[[ nodiscard ]] font::Face faceOf ( uint32_t face )
	{
		return face < uint32_t ( font::Face::count ) ? font::Face ( face ) : font::Face::regular;
	}

	[[ nodiscard ]] ToggleLook lookOf ( const ReShadeUiToggleLook* look )
	{
		if ( look == nullptr )
			return {};
		return { look->heading != 0, hotkeyOf ( &look->hotkey ), look->tooltip, appliesOf ( look->applies ), look->def != 0 };
	}

	//-------------------------------------------------------------------------
	// The look

	void beginFrameOf ( void* runtime )
	{
		beginFrame ( static_cast<api::effect_runtime*> ( runtime ) );
	}

	void pageDrawOf ( void* runtime, const ReShadeUiPage* page, void* frame )
	{
		drawPage ( static_cast<api::effect_runtime*> ( runtime ), *page, frame );
	}

	uint32_t isTypingOf ()
	{
		return window::isTyping () ? 1 : 0;
	}

	float statusTextOf ( float x, float y, uint32_t face, float s, uint32_t c, const char* text )
	{
		return font::drawMarked ( ImGui::GetWindowDrawList (), ImVec2 ( x, y ), faceOf ( face ), s, c, text != nullptr ? text : "" );
	}

	uint32_t specLayoutOf ()
	{
		return RESHADE_UI_SPEC_LAYOUT;
	}

	uint32_t isMouseOverOf ()
	{
		return isMouseOverOverlay () ? 1 : 0;
	}

	float scaleOf ()												{ return theme::scale (); }
	float screenScale ( void* window, float backBufferHeight )	{ return theme::screenScale ( window, backBufferHeight ); }
	uint32_t color ( uint32_t role )								{ return theme::colorOf ( role ); }
	float size ( uint32_t role )									{ return theme::sizeOf ( role ); }

	uint32_t tunedSizeCount ()										{ return uint32_t ( theme::tuned ().size () ); }
	const char* tunedSizeName ( uint32_t i )						{ return i < theme::tuned ().size () ? theme::tuned ()[ i ].name : ""; }
	float tunedSizeValue ( uint32_t i )								{ return i < theme::tuned ().size () ? *theme::tuned ()[ i ].value : 0.0f; }
	float tunedSizeShipped ( uint32_t i )							{ return i < theme::tuned ().size () ? theme::tuned ()[ i ].shipped : 0.0f; }
	void setTunedSize ( uint32_t i, float value )					{ if ( i < theme::tuned ().size () ) *theme::tuned ()[ i ].value = value; }
	uint32_t tunedColorCount ()										{ return uint32_t ( theme::tunedColors ().size () ); }
	const char* tunedColorName ( uint32_t i )						{ return i < theme::tunedColors ().size () ? theme::tunedColors ()[ i ].name : ""; }
	uint32_t tunedColorValue ( uint32_t i )							{ return i < theme::tunedColors ().size () ? *theme::tunedColors ()[ i ].value : 0; }
	uint32_t tunedColorShipped ( uint32_t i )						{ return i < theme::tunedColors ().size () ? theme::tunedColors ()[ i ].shipped : 0; }
	void setTunedColor ( uint32_t i, uint32_t c )					{ if ( i < theme::tunedColors ().size () ) *theme::tunedColors ()[ i ].value = c; }

	void copyOut ( const std::string& text, char* out, uint32_t capacity )
	{
		if ( out == nullptr || capacity == 0 )
			return;
		const auto	n = std::min ( text.size (), size_t ( capacity - 1 ) );
		std::copy_n ( text.data (), n, out );
		out[ n ] = 0;
	}

	uint32_t themeText ( char* out, uint32_t capacity )
	{
		const auto	text = theme::fileText ();
		copyOut ( text, out, capacity );
		return uint32_t ( text.size () );
	}

	uint32_t themeTake ( const char* text, uint32_t length, char* why, uint32_t whyCapacity )
	{
		std::string	reason;
		const auto	isTaken = text != nullptr && theme::take ( std::string_view ( text, length ), reason );
		copyOut ( reason, why, whyCapacity );
		return isTaken ? 1 : 0;
	}

	//-------------------------------------------------------------------------
	// Text and images

	float textWidth ( uint32_t face, float s, const char* text, uint32_t tabular )
	{
		return font::width ( faceOf ( face ), s, text != nullptr ? text : "", tabular != 0 );
	}

	float textDraw ( float x, float y, uint32_t face, float s, uint32_t c, const char* text, uint32_t tabular )
	{
		return font::draw ( ImGui::GetWindowDrawList (), ImVec2 ( x, y ), faceOf ( face ), s, c, text != nullptr ? text : "", tabular != 0 );
	}

	float lineHeight ( uint32_t face, float s )								{ return font::lineHeight ( faceOf ( face ), s ); }
	float centredTop ( uint32_t face, float s, float top, float height )	{ return font::centredTop ( faceOf ( face ), s, top, height ); }
	float baselineInLine ( uint32_t face, float s, float lineHeight )		{ return font::baselineInLine ( faceOf ( face ), s, lineHeight ); }

	void placeLine ( uint32_t face, float s, const char* text, float maxWidth, ReShadeUiGlyph* glyphs, uint32_t capacity, ReShadeUiPlacedLine* out )
	{
		const auto	line = font::placeLine ( faceOf ( face ), s, text != nullptr ? text : "", maxWidth );
		out->face = line.face;
		out->scale = line.scale;
		out->width = line.width;
		out->height = line.height;
		out->ascent = line.ascent;
		out->glyphCount = uint32_t ( line.glyphs.size () );
		for ( size_t i = 0; glyphs != nullptr && i < line.glyphs.size () && i < capacity; ++i )
			glyphs[ i ] = { line.glyphs[ i ].index, line.glyphs[ i ].pen };
	}

	uint32_t rasterizeGlyph ( const ReShadeUiPlacedLine* line, const ReShadeUiGlyph* glyphs, uint32_t i, uint8_t* coverage, int32_t stride, int32_t rows, int32_t left, int32_t top )
	{
		if ( line == nullptr || glyphs == nullptr || i >= line->glyphCount || coverage == nullptr )
			return 0;

		font::PlacedLine	placed;
		placed.face = line->face;
		placed.scale = line->scale;
		placed.width = line->width;
		placed.height = line->height;
		placed.ascent = line->ascent;
		return font::rasterizeGlyph ( placed, { glyphs[ i ].index, glyphs[ i ].pen }, coverage, stride, rows, left, top ) ? 1 : 0;
	}

	uint64_t cachedGlyphBytes ()
	{
		return font::cachedGlyphBytes ();
	}

	const void* imageOf ( void* runtime, const char* path )
	{
		return image ( static_cast<api::effect_runtime*> ( runtime ), wide ( path ) );
	}

	const void* maskOf ( void* runtime, const char* path, int32_t height )
	{
		return mask ( static_cast<api::effect_runtime*> ( runtime ), wide ( path ), height );
	}

	uint32_t decode ( const char* path, uint32_t* rgba, uint32_t capacity, int32_t* width, int32_t* height )
	{
		std::vector<uint32_t>	pixels;
		int						w = 0;
		int						h = 0;
		if ( ! decodeImage ( wide ( path ), pixels, w, h ) )
			return 0;

		if ( width != nullptr )
			*width = w;
		if ( height != nullptr )
			*height = h;
		if ( rgba != nullptr )
			std::copy_n ( pixels.begin (), std::min ( pixels.size (), size_t ( capacity ) ), rgba );
		return uint32_t ( pixels.size () );
	}

	int32_t colourSpaceOverride ()						{ return overlayColourSpaceOverride (); }
	void setColourSpaceOverride ( int32_t space )		{ setOverlayColourSpaceOverride ( space ); }
	uint32_t isColourSpaceShown ()						{ return isOverlayColourSpaceShown () ? 1 : 0; }

	//-------------------------------------------------------------------------
	// Widgets

	void labelAt ( float x, float y, const char* text, const char* tooltip, uint32_t applies )
	{
		label ( ImVec2 ( x, y ), text, tooltip, appliesOf ( applies ) );
	}

	float labelWidthOf ( const char* text, uint32_t hasTooltip, uint32_t applies )
	{
		return labelWidth ( text, hasTooltip != 0, appliesOf ( applies ) );
	}

	uint32_t toggleOf ( const char* id, uint32_t* on, const char* text, const ReShadeUiToggleLook* look )
	{
		auto		isOn = *on != 0;
		const auto	edit = toggle ( id, isOn, text, lookOf ( look ) );
		*on = isOn ? 1 : 0;
		return resultOf ( edit );
	}

	uint32_t subsectionOf ( const char* id, uint32_t* on, uint32_t hasSwitch, const char* text, const ReShadeUiToggleLook* look )
	{
		auto		isOn = *on != 0;
		const auto	edit = subsection ( id, isOn, hasSwitch != 0, text, lookOf ( look ) );
		*on = isOn ? 1 : 0;
		return resultOf ( edit );
	}

	void headingOf ( const char* text, const ReShadeUiHotkey* hotkey )
	{
		heading ( text, hotkeyOf ( hotkey ) );
	}

	uint32_t collapseArrowOf ( const char* id, uint32_t open, float width )
	{
		return collapseArrow ( id, open != 0, width ) ? 1 : 0;
	}

	void subheadingAt ( float x, float y, const char* text )
	{
		subheading ( ImVec2 ( x, y ), text );
	}

	uint32_t sliderOf ( const char* id, float* value, const ReShadeUiRange* range, float width )
	{
		return resultOf ( slider ( id, *value, rangeOf ( range ), width ) );
	}

	uint32_t valueEditOf ( const char* id, float* value, const ReShadeUiRange* range, const ReShadeUiFormat* format, float width )
	{
		return resultOf ( valueEdit ( id, *value, rangeOf ( range ), format != nullptr ? formatOf ( *format ) : Format {}, nullptr, width ) );
	}

	uint32_t colorPickerOf ( const char* id, uint32_t* c, uint32_t def, float width )
	{
		ImU32		value = *c;
		const auto	edit = colorPicker ( id, value, def, width );
		*c = value;
		return resultOf ( edit );
	}

	uint32_t textFieldOf ( const char* id, char* text, uint32_t capacity, float width )
	{
		std::string	value ( text );
		const auto	edit = textField ( id, value, width );
		if ( edit.changed )
			copyOut ( value, text, capacity );
		return resultOf ( edit );
	}

	uint32_t resetButtonOf ( const char* id )
	{
		return resetButton ( id ) ? 1 : 0;
	}

	uint32_t choiceOf ( const char* id, int32_t* index, const char* const* names, uint32_t count, int32_t def, float width )
	{
		auto	i = int ( *index );
		const auto	edit = choice ( id, i, std::span<const char* const> ( names, count ), def, width );
		*index = i;
		return resultOf ( edit );
	}

	uint32_t dropdownOf ( const char* id, int32_t* index, const char* const* names, uint32_t count, int32_t def, float width )
	{
		auto	i = int ( *index );
		const auto	edit = dropdown ( id, i, std::span<const char* const> ( names, count ), def, width );
		*index = i;
		return resultOf ( edit );
	}

	uint32_t buttonOf ( const void* key, const char* text, uint32_t primary, const char* done, const ReShadeUiHotkey* hotkey, float progress )
	{
		return button ( key, text, primary != 0, done, hotkeyOf ( hotkey ), progress ) ? 1 : 0;
	}

	float buttonWidthOf ( const void* key, const char* text, const char* done, const ReShadeUiHotkey* hotkey )
	{
		return buttonWidth ( key, text, done, hotkeyOf ( hotkey ) );
	}

	void flashOf ( const void* key )
	{
		flash ( key );
	}

	void tooltipOf ( uint32_t id, uint32_t hovered, const char* text )
	{
		tooltip ( id, hovered != 0, text );
	}

	void headerOf ( const ReShadeUiHeader* info, float width )
	{
		header ( *info, width );
	}

	int32_t headerTitleHeightOf ()
	{
		return headerTitleHeight ();
	}

	const ReShadeWidgetTable	table {
		sizeof ( ReShadeWidgetTable ),
		&beginFrameOf, &scaleOf, &screenScale, &color, &size,
		&tunedSizeCount, &tunedSizeName, &tunedSizeValue, &tunedSizeShipped, &setTunedSize,
		&tunedColorCount, &tunedColorName, &tunedColorValue, &tunedColorShipped, &setTunedColor,
		&themeText, &themeTake,
		&textWidth, &textDraw, &lineHeight, &centredTop, &baselineInLine, &placeLine, &rasterizeGlyph, &cachedGlyphBytes,
		&imageOf, &maskOf, &decode,
		&colourSpaceOverride, &setColourSpaceOverride, &isColourSpaceShown,
		&labelAt, &labelWidthOf, &toggleOf, &headingOf, &collapseArrowOf, &subheadingAt, &sliderOf, &valueEditOf, &colorPickerOf, &textFieldOf,
		&resetButtonOf, &choiceOf, &dropdownOf, &buttonOf, &buttonWidthOf, &flashOf, &tooltipOf, &headerOf, &headerTitleHeightOf, &pageDrawOf, &isMouseOverOf, &isTypingOf, &specLayoutOf, &statusTextOf,
		&subsectionOf
	};

	void onPresent ( reshade::api::command_queue*, reshade::api::swapchain* swapchain, const reshade::api::rect*, const reshade::api::rect*, uint32_t, const reshade::api::rect* )
	{
		notePresent ( swapchain );
	}

	void onDestroyDevice ( reshade::api::device* device )
	{
		font::forgetDevice ( device );
		forgetImages ( device );
		forgetDevice ( device );
	}
}
}

// The built-in add-on's events: the swap chain's colour space for the images, a device's textures going with it
void register_addon_ui ()
{
	ultra::ui::theme::load ();
	reshade::register_event<reshade::addon_event::present> ( ultra::ui::onPresent );
	reshade::register_event<reshade::addon_event::destroy_device> ( ultra::ui::onDestroyDevice );
}

void unregister_addon_ui ()
{
	reshade::unregister_event<reshade::addon_event::present> ( ultra::ui::onPresent );
	reshade::unregister_event<reshade::addon_event::destroy_device> ( ultra::ui::onDestroyDevice );
	ultra::ui::theme::unload ();
}

#if defined(RESHADE_API_LIBRARY_EXPORT)

const ReShadeWidgetTable* ReShadeGetWidgetTable ( uint32_t bytes )
{
	return bytes <= ultra::ui::table.bytes ? &ultra::ui::table : nullptr;
}

#endif
