/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Windows.h>
#include <ShellScalingApi.h>
#include <dwmapi.h>

#include "dll_log.hpp"
#include "ultra_ui_theme.hpp"
#include <reshade_ultra_ui.h>

extern HMODULE g_module_handle;

//-----------------------------------------------------------------------------

namespace ultra::ui::theme
{
	namespace
	{
		float	measured = 1.0f;

		struct Size
		{
			const char*	name;
			float*		value;
		};

		struct Color
		{
			const char*	name;
			ImU32*		value;
		};

		// The file's members, in its order, which is the roles' order (include/reshade_ultra_ui.h)
		constexpr Size	sizeTable[] {
			{ "rowHeight", &rowHeight }, { "rowGap", &rowGap }, { "columnGap", &columnGap }, { "indent", &indent }, { "separatorAbove", &separatorAbove },
			{ "headingAbove", &headingAbove }, { "headingBelow", &headingBelow }, { "subheadingAbove", &subheadingAbove }, { "firstRowAbove", &firstRowAbove },
			{ "headingGap", &headingGap }, { "sliderBack", &sliderBack }, { "sliderPad", &sliderPad }, { "valueEditPad", &valueEditPad }, { "switchWidth", &switchWidth },
			{ "switchHeight", &switchHeight }, { "knobSize", &knobSize }, { "choicePad", &choicePad }, { "dropPadLeft", &dropPadLeft }, { "dropPadRight", &dropPadRight },
			{ "menuPad", &menuPad }, { "menuItemPad", &menuItemPad }, { "menuGap", &menuGap }, { "tipPadX", &tipPadX }, { "tipPadY", &tipPadY },
			{ "buttonPadX", &buttonPadX }, { "buttonGap", &buttonGap }, { "keyHeight", &keyHeight }, { "keyPadX", &keyPadX }, { "disclosureSize", &disclosureSize },
			{ "disclosureRadius", &disclosureRadius } };

		constexpr Color	colorTable[] {
			{ "accent", &colors::accent }, { "danger", &colors::danger }, { "warning", &colors::warning }, { "accentHover", &colors::accentHover },
			{ "text", &colors::text }, { "bright", &colors::bright }, { "muted", &colors::muted }, { "nextStart", &colors::nextStart },
			{ "appliesAfter", &colors::appliesAfter }, { "white", &colors::white }, { "separator", &colors::separator }, { "panel", &colors::panel },
			{ "control", &colors::control }, { "controlHover", &colors::controlHover }, { "track", &colors::track }, { "tick", &colors::tick },
			{ "handle", &colors::handle }, { "switchOff", &colors::switchOff }, { "knobOff", &colors::knobOff }, { "knobOn", &colors::knobOn },
			{ "chevron", &colors::chevron }, { "popup", &colors::popup }, { "popupBorder", &colors::popupBorder }, { "popupHover", &colors::popupHover },
			{ "button", &colors::button }, { "buttonHover", &colors::buttonHover }, { "infoDot", &colors::infoDot }, { "infoGlyph", &colors::infoGlyph },
			{ "keycap", &colors::keycap }, { "keycapEdge", &colors::keycapEdge },
			{ "window", &colors::window }, { "windowEdge", &colors::windowEdge }, { "tab", &colors::tab }, { "tabActive", &colors::tabActive }, { "tabHover", &colors::tabHover },
			{ "tabLine", &colors::tabLine }, { "scrollTrack", &colors::scrollTrack }, { "scrollThumb", &colors::scrollThumb }, { "scrollThumbHover", &colors::scrollThumbHover },
			{ "message", &colors::message }, { "messageText", &colors::messageText },
			{ "titleBar", &colors::titleBar }, { "tabFill", &colors::tabFill } };

		constexpr size_t	sizeCount = std::size ( sizeTable );
		constexpr size_t	colorCount = std::size ( colorTable );

		static_assert ( colorCount == RESHADE_UI_COLOR_COUNT );

		// The sizes that are no file's: after the tuned ones, in the roles' order
		constexpr float	fixedSizes[] {
			sizes::body, sizes::heading, sizes::subheading, sizes::title, sizes::choice, sizes::value, sizes::key, sizes::tip, sizes::info,
			panelInset, panelWidth, resetWidth, resetIcon, inlineToggleHeight, bipolarMinFill, dropRounding, chevronSize, menuRounding,
			menuItemRounding, tipMaxWidth, tipGap, shadowSpread, shadowOffset, shadowAlpha, pickerSquare, pickerHueWidth, pickerGap, pickerMarker,
			swatchEdge, infoSize, infoGap, keyEdge, keyRounding, keyGap, keycapGap, coverWidth, coverHeight, coverRounding, coverGap, chipGap,
			chipEdge, disabledAlpha, float ( swingSeconds ), swingStretch, float ( fadeSeconds ), float ( tipDelay ), float ( doneSeconds ),
			windowRounding, windowPad, windowMinWidth, windowMinHeight, resizeEdge, tabHeight, tabPadX, tabGap, tabLine, scrollbarWidth, scrollbarGap, scrollThumbMin, closeSize,
			messagePad, dotSize };

		static_assert ( sizeCount + std::size ( fixedSizes ) == RESHADE_UI_SIZE_COUNT );

		struct Values
		{
			std::array<float, sizeCount>	sizes {};
			std::array<ImU32, colorCount>	colors {};
		};

		// A flat object of name: value pairs in order, a text's value without its quotes
		struct Object
		{
			struct Member
			{
				std::string	name;
				std::string	value;
				bool		isText = false;
			};

			std::vector<Member>	members;

			[[ nodiscard ]] const Member* find ( const char* name ) const
			{
				const auto	it = std::ranges::find ( members, std::string_view ( name ), &Member::name );
				return it != members.end () ? &*it : nullptr;
			}
		};

		struct Reader
		{
			std::string_view	text;
			size_t				at = 0;

			void skip ()
			{
				while ( at < text.size () && std::isspace ( uint8_t ( text[ at ] ) ) )
					++at;
			}

			[[ nodiscard ]] bool expect ( char c )
			{
				skip ();
				if ( at >= text.size () || text[ at ] != c )
					return false;
				++at;
				return true;
			}

			[[ nodiscard ]] bool quoted ( std::string& out )
			{
				if ( at >= text.size () || text[ at ] != '"' )
					return false;
				const auto	end = text.find ( '"', at + 1 );
				if ( end == std::string_view::npos )
					return false;
				out = std::string ( text.substr ( at + 1, end - at - 1 ) );
				at = end + 1;
				return true;
			}

			[[ nodiscard ]] bool object ( Object& out )
			{
				if ( ! expect ( '{' ) )
					return false;
				for ( ;; )
				{
					skip ();
					if ( at < text.size () && text[ at ] == '}' )
					{
						++at;
						return true;
					}

					Object::Member	member;
					if ( ! quoted ( member.name ) || ! expect ( ':' ) )
						return false;
					skip ();
					if ( at < text.size () && text[ at ] == '"' )
					{
						member.isText = true;
						if ( ! quoted ( member.value ) )
							return false;
					}
					else
					{
						const auto	start = at;
						while ( at < text.size () && text[ at ] != ',' && text[ at ] != '}' && ! std::isspace ( uint8_t ( text[ at ] ) ) )
							++at;
						member.value = std::string ( text.substr ( start, at - start ) );
					}
					out.members.push_back ( std::move ( member ) );
					skip ();
					if ( at < text.size () && text[ at ] == ',' )
						++at;
				}
			}

			// "name": { ... }
			[[ nodiscard ]] bool member ( const char* name, Object& out )
			{
				skip ();
				const auto	key = std::string ( "\"" ) + name + "\"";
				if ( text.substr ( at, key.size () ) != key )
					return false;
				at += key.size ();
				return expect ( ':' ) && object ( out );
			}
		};

		// Every value of the tables, or why not: a file that lacks one or has one unknown is refused whole
		[[ nodiscard ]] bool parse ( std::string_view text, Values& out, std::string& why )
		{
			Reader	reader { text };
			Object	sizesObject, colorsObject;
			if ( ! reader.expect ( '{' ) || ! reader.member ( "sizes", sizesObject ) || ! reader.expect ( ',' ) || ! reader.member ( "colors", colorsObject ) || ! reader.expect ( '}' ) )
			{
				why = "it is not an object with the objects \"sizes\" and \"colors\", in this order";
				return false;
			}

			for ( size_t i = 0; i < sizeCount; ++i )
			{
				const auto*	m = sizesObject.find ( sizeTable[ i ].name );
				auto		v = -1.0f;
				if ( m == nullptr || m->isText || std::from_chars ( m->value.data (), m->value.data () + m->value.size (), v ).ec != std::errc () || v < 0.0f )
				{
					why = std::string ( "its size " ) + sizeTable[ i ].name + " is no number of 0 or more";
					return false;
				}
				out.sizes[ i ] = v;
			}
			for ( size_t i = 0; i < colorCount; ++i )
			{
				const auto*	m = colorsObject.find ( colorTable[ i ].name );
				uint32_t	rrggbb = 0;
				if ( m == nullptr || ! m->isText || m->value.size () != 7 || m->value[ 0 ] != '#'
					 || std::from_chars ( m->value.data () + 1, m->value.data () + 7, rrggbb, 16 ).ptr != m->value.data () + 7 )
				{
					why = std::string ( "its colour " ) + colorTable[ i ].name + " is no \"#RRGGBB\"";
					return false;
				}
				out.colors[ i ] = rgb ( rrggbb );
			}
			for ( const auto& m : sizesObject.members )
				if ( std::ranges::none_of ( sizeTable, [ &m ] ( const Size& e ) { return m.name == e.name; } ) )
				{
					why = "it has a size " + m.name + " the overlay does not know";
					return false;
				}
			for ( const auto& m : colorsObject.members )
				if ( std::ranges::none_of ( colorTable, [ &m ] ( const Color& e ) { return m.name == e.name; } ) )
				{
					why = "it has a colour " + m.name + " the overlay does not know";
					return false;
				}
			return true;
		}

		void apply ( const Values& v )
		{
			for ( size_t i = 0; i < sizeCount; ++i )
				*sizeTable[ i ].value = v.sizes[ i ];
			for ( size_t i = 0; i < colorCount; ++i )
				*colorTable[ i ].value = v.colors[ i ];
		}

		[[ nodiscard ]] std::string_view builtInText ()
		{
			const auto	info = FindResourceW ( g_module_handle, L"ULTRA_PANEL_THEME", MAKEINTRESOURCEW ( 10 ) );	// RT_RCDATA
			const auto	handle = info != nullptr ? LoadResource ( g_module_handle, info ) : nullptr;
			const auto*	bytes = handle != nullptr ? static_cast<const char*> ( LockResource ( handle ) ) : nullptr;
			return bytes != nullptr ? std::string_view ( bytes, SizeofResource ( g_module_handle, info ) ) : std::string_view ();
		}

		std::array<Tuned, sizeCount>		tunedSizes {};
		std::array<TunedColor, colorCount>	tunedColorTable {};
	}

	float scale ()
	{
		return measured;
	}

	// The monitor's DPI scale (read per-monitor-aware: a DPI-unaware game is told 96) times back buffer pixels per
	// screen pixel, in quarter steps so the fonts stay crisp
	void measureScale ( void* window, float backBufferHeight )
	{
		if ( const auto s = screenScale ( window, backBufferHeight ); s > 0.0f )
			measured = s;
	}

	float screenScale ( void* window, float backBufferHeight )
	{
		const auto	hwnd = static_cast<HWND> ( window );
		if ( hwnd == nullptr || backBufferHeight <= 0.0f )
			return 0.0f;

		const auto	previous = SetThreadDpiAwarenessContext ( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
		UINT		dpiX = 96;
		UINT		dpiY = 96;
		RECT		frame {};
		const auto	hasDpi = SUCCEEDED ( GetDpiForMonitor ( MonitorFromWindow ( hwnd, MONITOR_DEFAULTTONEAREST ), MDT_EFFECTIVE_DPI, &dpiX, &dpiY ) );
		const auto	hasFrame = SUCCEEDED ( DwmGetWindowAttribute ( hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof ( frame ) ) ) && frame.bottom > frame.top;
		SetThreadDpiAwarenessContext ( previous );

		const auto	monitorScale = hasDpi ? float ( dpiY ) / 96.0f : 1.0f;
		const auto	pixelSize = hasFrame ? backBufferHeight / float ( frame.bottom - frame.top ) : 1.0f;

		return std::clamp ( std::round ( monitorScale * pixelSize * 4.0f ) / 4.0f, 0.5f, 4.0f );
	}

	void load ()
	{
		Values		v;
		std::string	why;
		const auto	text = builtInText ();
		if ( text.empty () )
			reshade::log::message ( reshade::log::level::error, "The overlay's theme resource is missing: it has no colours and no spacing." );
		else if ( ! parse ( text, v, why ) )
			reshade::log::message ( reshade::log::level::error, "The overlay's built-in theme does not read, it has no colours and no spacing: %s.", why.c_str () );
		else
			apply ( v );

		for ( size_t i = 0; i < sizeCount; ++i )
			tunedSizes[ i ] = { sizeTable[ i ].name, sizeTable[ i ].value, v.sizes[ i ] };
		for ( size_t i = 0; i < colorCount; ++i )
			tunedColorTable[ i ] = { colorTable[ i ].name, colorTable[ i ].value, v.colors[ i ] };
	}

	std::span<const Tuned> tuned ()
	{
		return tunedSizes;
	}

	std::span<const TunedColor> tunedColors ()
	{
		return tunedColorTable;
	}

	ImU32 colorOf ( uint32_t role )
	{
		return role < colorCount ? *colorTable[ role ].value : 0;
	}

	float sizeOf ( uint32_t role )
	{
		if ( role < sizeCount )
			return *sizeTable[ role ].value;
		return role - sizeCount < std::size ( fixedSizes ) ? fixedSizes[ role - sizeCount ] : 0.0f;
	}

	std::string text ()
	{
		std::string	s = "{\n\t\"sizes\": {";
		for ( size_t i = 0; i < sizeCount; ++i )
		{
			char	number[ 32 ] {};
			std::snprintf ( number, sizeof number, "%g", std::round ( double ( *sizeTable[ i ].value ) * 1000.0 ) / 1000.0 );
			s += std::string ( i == 0 ? "\n" : ",\n" ) + "\t\t\"" + sizeTable[ i ].name + "\": " + number;
		}
		s += "\n\t},\n\t\"colors\": {";
		for ( size_t i = 0; i < colorCount; ++i )
		{
			const auto	c = *colorTable[ i ].value;
			char		hex[ 16 ] {};
			std::snprintf ( hex, sizeof hex, "#%02X%02X%02X", unsigned ( ( c >> IM_COL32_R_SHIFT ) & 0xFF ), unsigned ( ( c >> IM_COL32_G_SHIFT ) & 0xFF ),
							unsigned ( ( c >> IM_COL32_B_SHIFT ) & 0xFF ) );
			s += std::string ( i == 0 ? "\n" : ",\n" ) + "\t\t\"" + colorTable[ i ].name + "\": \"" + hex + "\"";
		}
		s += "\n\t}\n}\n";
		return s;
	}

	bool take ( std::string_view text, std::string& why )
	{
		Values	v;
		if ( ! parse ( text, v, why ) )
			return false;
		apply ( v );
		return true;
	}
}

