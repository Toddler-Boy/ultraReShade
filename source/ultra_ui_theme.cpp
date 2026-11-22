/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Windows.h>
#include <ShellScalingApi.h>
#include <dwmapi.h>

#include <reshade.hpp>
#include <reshade_ultra_json.hpp>

#include "dll_log.hpp"
#include "reshade_ultra_ui_theme.h"
#include "ultra_file_watch.hpp"
#include "ultra_home.hpp"
#include "ultra_json.hpp"
#include "ultra_ui_theme.hpp"
#include <reshade_ultra_ui.h>

extern HMODULE g_module_handle;

//-----------------------------------------------------------------------------

namespace ultra::ui::theme
{
	namespace
	{
		float	measured = 1.0f;

		// A value of the file: its name there, the variable, the least it may be (a text size or a duration of 0 breaks what is drawn)
		struct Size
		{
			constexpr Size ( const char* n, float* v, float l = 0.0f ) : name ( n ), value ( v ), least ( l )	{}

			const char*	name;
			float*		value;
			float		least;
		};

		struct Color
		{
			const char*	name;
			ImU32*		value;
		};

		constexpr float	mostSize = 100000.0f;

		// The file's members, in its order, which is the roles' order (include/reshade_ultra_ui.h)
		constexpr Size	sizeTable[] {
			{ "rowHeight", &rowHeight }, { "rowGap", &rowGap }, { "columnGap", &columnGap }, { "indent", &indent }, { "separatorAbove", &separatorAbove },
			{ "headingAbove", &headingAbove }, { "headingBelow", &headingBelow }, { "subheadingAbove", &subheadingAbove }, { "firstRowAbove", &firstRowAbove },
			{ "headingGap", &headingGap }, { "sliderBack", &sliderBack }, { "sliderPad", &sliderPad }, { "valueEditPad", &valueEditPad }, { "switchWidth", &switchWidth },
			{ "switchHeight", &switchHeight }, { "knobSize", &knobSize }, { "choicePad", &choicePad }, { "dropPadLeft", &dropPadLeft }, { "dropPadRight", &dropPadRight },
			{ "menuPad", &menuPad }, { "menuItemPad", &menuItemPad }, { "menuGap", &menuGap }, { "tipPadX", &tipPadX }, { "tipPadY", &tipPadY },
			{ "buttonPadX", &buttonPadX }, { "buttonGap", &buttonGap }, { "keyHeight", &keyHeight }, { "keyPadX", &keyPadX }, { "disclosureSize", &disclosureSize },
			{ "disclosureRadius", &disclosureRadius },

			{ "body", &sizes::body, 1.0f }, { "heading", &sizes::heading, 1.0f }, { "subheading", &sizes::subheading, 1.0f }, { "title", &sizes::title, 1.0f },
			{ "choice", &sizes::choice, 1.0f }, { "value", &sizes::value, 1.0f }, { "key", &sizes::key, 1.0f }, { "tip", &sizes::tip, 1.0f }, { "info", &sizes::info, 1.0f },
			{ "panelInset", &panelInset }, { "panelWidth", &panelWidth, 1.0f }, { "resetWidth", &resetWidth }, { "resetIcon", &resetIcon },
			{ "inlineToggleHeight", &inlineToggleHeight }, { "bipolarMinFill", &bipolarMinFill }, { "dropRounding", &dropRounding }, { "chevronSize", &chevronSize },
			{ "menuRounding", &menuRounding }, { "menuItemRounding", &menuItemRounding }, { "tipMaxWidth", &tipMaxWidth, 1.0f }, { "tipGap", &tipGap },
			{ "shadowSpread", &shadowSpread }, { "shadowOffset", &shadowOffset }, { "shadowAlpha", &shadowAlpha }, { "pickerSquare", &pickerSquare },
			{ "pickerHueWidth", &pickerHueWidth }, { "pickerGap", &pickerGap }, { "pickerMarker", &pickerMarker }, { "swatchEdge", &swatchEdge },
			{ "infoSize", &infoSize }, { "infoGap", &infoGap }, { "keyEdge", &keyEdge }, { "keyRounding", &keyRounding }, { "keyGap", &keyGap }, { "keycapGap", &keycapGap },
			{ "coverWidth", &coverWidth }, { "coverHeight", &coverHeight }, { "coverRounding", &coverRounding }, { "coverGap", &coverGap }, { "chipGap", &chipGap },
			{ "chipEdge", &chipEdge }, { "disabledAlpha", &disabledAlpha }, { "swingSeconds", &swingSeconds, 0.001f }, { "swingStretch", &swingStretch },
			{ "fadeSeconds", &fadeSeconds, 0.001f }, { "tipDelay", &tipDelay }, { "doneSeconds", &doneSeconds }, { "windowRounding", &windowRounding },
			{ "windowPad", &windowPad }, { "windowMinWidth", &windowMinWidth }, { "windowMinHeight", &windowMinHeight }, { "resizeEdge", &resizeEdge },
			{ "tabHeight", &tabHeight }, { "tabPadX", &tabPadX }, { "tabGap", &tabGap }, { "tabLine", &tabLine }, { "scrollbarWidth", &scrollbarWidth },
			{ "scrollbarGap", &scrollbarGap }, { "scrollThumbMin", &scrollThumbMin }, { "closeSize", &closeSize }, { "messagePad", &messagePad }, { "dotSize", &dotSize },
			{ "tipEdge", &tipEdge },
			{ "tabInsetTop", &tabInsetTop }, { "tabPillInset", &tabPillInset }, { "dotGap", &dotGap }, { "dotRing", &dotRing }, { "closeLine", &closeLine },
			{ "pickerLine", &pickerLine }, { "separatorLine", &separatorLine }, { "dragThreshold", &dragThreshold }, { "sliderHandleGrow", &sliderHandleGrow },
			{ "tabInsetBottom", &tabInsetBottom }, { "choiceRowHeight", &choiceRowHeight }, { "logoGap", &logoGap }, { "logoHeight", &logoHeight }, { "logoInset", &logoInset }, { "barShadowHeight", &barShadowHeight }, { "barShadowAlpha", &barShadowAlpha },
			{ "logoSweepAngle", &logoSweepAngle }, { "logoSweepWidth", &logoSweepWidth }, { "logoSweepSeconds", &logoSweepSeconds }, { "logoSweepPeriod", &logoSweepPeriod },
			{ "scrollThumbInset", &scrollThumbInset }, { "closeRounding", &closeRounding } };

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
			{ "titleBar", &colors::titleBar }, { "tabSelectedText", &colors::tabSelectedText }, { "logoSweep", &colors::logoSweep }, { "closeHover", &colors::closeHover }, { "titleBarHover", &colors::titleBarHover }, { "choiceHover", &colors::choiceHover } };

		constexpr size_t	sizeCount = std::size ( sizeTable );
		constexpr size_t	colorCount = std::size ( colorTable );

		static_assert ( sizeCount == RESHADE_UI_SIZE_COUNT );
		static_assert ( colorCount == RESHADE_UI_COLOR_COUNT );

		struct Values
		{
			std::array<float, sizeCount>	sizes {};
			std::array<ImU32, colorCount>	colors {};
		};

		using Problems = std::vector<std::string>;

		Values						shipped;		// the built-in copy's
		std::filesystem::path		followed;		// the checkout's file while it is followed
		std::vector<std::string>	told;			// the problems logged, so a file read again with the same one says nothing

		// A value the file holds right is taken, one it lacks or holds wrong stays what base has. Problems come as phrases that name the value
		void readSizes ( const reshade::json::value* object, Values& v, Problems& problems )
		{
			for ( size_t i = 0; i < sizeCount; ++i )
			{
				const auto&	entry = sizeTable[ i ];
				const auto*	member = ReShadeJsonFind ( object, entry.name );
				auto		number = 0.0;
				if ( member == nullptr )
					problems.push_back ( std::string ( "the size " ) + entry.name + " is missing" );
				else if ( ! ReShadeJsonGetNumber ( member, &number ) )
					problems.push_back ( std::string ( "the size " ) + entry.name + " is no number" );
				else if ( ! ( number >= double ( entry.least ) && number <= double ( mostSize ) ) )
				{
					char	least[ 16 ] {};
					std::snprintf ( least, sizeof least, "%g", double ( entry.least ) );
					problems.push_back ( std::string ( "the size " ) + entry.name + " is not from " + least + " to 100000" );
				}
				else
					v.sizes[ i ] = float ( number );
			}

			for ( size_t i = 0, count = ReShadeJsonGetSize ( object ); i < count; ++i )
			{
				const char*	key = nullptr;
				ReShadeJsonAt ( object, i, &key );
				if ( key != nullptr && std::ranges::none_of ( sizeTable, [ key ] ( const Size& e ) { return std::string_view ( e.name ) == key; } ) )
					problems.push_back ( std::string ( "the size " ) + key + " is not one the overlay knows" );
			}
		}

		void readColors ( const reshade::json::value* object, Values& v, Problems& problems )
		{
			for ( size_t i = 0; i < colorCount; ++i )
			{
				const auto&	entry = colorTable[ i ];
				const auto*	member = ReShadeJsonFind ( object, entry.name );
				const char*	text = nullptr;
				size_t		length = 0;
				uint32_t	rrggbb = 0;
				if ( member == nullptr )
					problems.push_back ( std::string ( "the colour " ) + entry.name + " is missing" );
				else if ( ! ReShadeJsonGetString ( member, &text, &length ) || length != 7 || text[ 0 ] != '#'
						  || std::from_chars ( text + 1, text + 7, rrggbb, 16 ).ptr != text + 7 )
					problems.push_back ( std::string ( "the colour " ) + entry.name + " is no \"#RRGGBB\"" );
				else
					v.colors[ i ] = rgb ( rrggbb );
			}

			for ( size_t i = 0, count = ReShadeJsonGetSize ( object ); i < count; ++i )
			{
				const char*	key = nullptr;
				ReShadeJsonAt ( object, i, &key );
				if ( key != nullptr && std::ranges::none_of ( colorTable, [ key ] ( const Color& e ) { return std::string_view ( e.name ) == key; } ) )
					problems.push_back ( std::string ( "the colour " ) + key + " is not one the overlay knows" );
			}
		}

		// The values of a json tree on top of base; nothing for a tree that is no object
		[[ nodiscard ]] std::optional<Values> read ( const reshade::json::value* root, const Values& base, Problems& problems )
		{
			if ( ReShadeJsonGetType ( root ) != reshade::json::type::object )
			{
				problems.push_back ( "it is no object with the objects \"sizes\" and \"colors\"" );
				return std::nullopt;
			}

			Values		v = base;
			const auto*	sizesObject = ReShadeJsonFind ( root, "sizes" );
			const auto*	colorsObject = ReShadeJsonFind ( root, "colors" );

			if ( ReShadeJsonGetType ( sizesObject ) == reshade::json::type::object )
				readSizes ( sizesObject, v, problems );
			else
				problems.push_back ( "it has no object \"sizes\"" );

			if ( ReShadeJsonGetType ( colorsObject ) == reshade::json::type::object )
				readColors ( colorsObject, v, problems );
			else
				problems.push_back ( "it has no object \"colors\"" );

			return v;
		}

		[[ nodiscard ]] std::optional<Values> readText ( std::string_view text, const Values& base, Problems& problems )
		{
			reshade::json::error	error;
			const reshade::ultra::JsonTree	root ( ReShadeJsonParse ( text.data (), text.size (), &error ) );
			if ( root == nullptr )
			{
				problems.push_back ( std::string ( "it is no json: " ) + error.message );
				return std::nullopt;
			}

			return read ( root.get (), base, problems );
		}

		// Each problem once, until it is gone and comes again
		void report ( const std::string& subject, const Problems& problems, reshade::log::level level, const char* outcome )
		{
			std::vector<std::string>	now;
			for ( const auto& problem : problems )
			{
				now.push_back ( subject + ": " + problem + ". " + outcome );
				if ( ! std::ranges::contains ( told, now.back () ) )
					reshade::log::message ( level, "%s", now.back ().c_str () );
			}
			told = std::move ( now );
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

		// The checkout's file: ultraReShade's checkout lies beside the ultraGame one the source folder is
		[[ nodiscard ]] std::filesystem::path checkoutFile ()
		{
			const auto&	source = reshade::ultra::sourceFolder ();
			return source.empty () ? std::filesystem::path () : source.parent_path () / L"ultraReShade" / L"res" / L"panel-theme.json";
		}

		// The saved file's values, on the presenting thread before the next frame is drawn
		void readFollowed ()
		{
			reshade::json::error	error;
			const auto				root = reshade::ultra::readJsonFile ( followed, error );
			const auto				subject = "The overlay's theme file '" + followed.u8string () + "'";
			Problems				problems;

			if ( root == nullptr )
			{
				report ( "The overlay's theme", { std::string ( error.message ) }, reshade::log::level::warning, "The look stays as it is." );
				return;
			}

			if ( const auto v = read ( root.get (), shipped, problems ); v.has_value () )
				apply ( *v );

			report ( subject, problems, reshade::log::level::warning, "The built-in value stays." );
		}

		void onFileChanged ( const char*, api::file_change change, const char* )
		{
			if ( change == api::file_change::deleted )
				report ( "The overlay's theme", { "its file '" + followed.u8string () + "' is gone" }, reshade::log::level::warning, "The look stays as it is." );
			else
				readFollowed ();
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
		const auto	text = builtInText ();
		if ( text.empty () )
			reshade::log::message ( reshade::log::level::error, "The overlay's theme resource is missing: it has no colours and no sizes." );
		else
		{
			Problems	problems;
			if ( const auto v = readText ( text, Values {}, problems ); v.has_value () )
				shipped = *v;

			report ( "The overlay's built-in theme", problems, reshade::log::level::error, "That value is 0." );
		}

		apply ( shipped );

		for ( size_t i = 0; i < sizeCount; ++i )
			tunedSizes[ i ] = { sizeTable[ i ].name, sizeTable[ i ].value, shipped.sizes[ i ] };
		for ( size_t i = 0; i < colorCount; ++i )
			tunedColorTable[ i ] = { colorTable[ i ].name, colorTable[ i ].value, shipped.colors[ i ] };

		const auto	file = checkoutFile ();
		if ( file.empty () )
			return;

		if ( std::error_code error; ! std::filesystem::is_regular_file ( file, error ) )
		{
			reshade::log::message ( reshade::log::level::warning, "The overlay's theme file '%s' is missing, so the built-in theme stays.", file.u8string ().c_str () );
			return;
		}

		followed = file;
		readFollowed ();
		if ( ! reshade::ultra::watchFile ( followed, &onFileChanged ) )
			reshade::log::message ( reshade::log::level::warning, "The overlay's theme file '%s' is not followed: a change of it reaches the game at its next start.", followed.u8string ().c_str () );
	}

	void unload ()
	{
		if ( ! followed.empty () )
			reshade::ultra::unwatchFile ( followed, &onFileChanged );
		followed.clear ();
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
		return role < sizeCount ? *sizeTable[ role ].value : 0.0f;
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
		Problems	problems;
		if ( const auto v = readText ( text, shipped, problems ); v.has_value () )
			apply ( *v );

		if ( ! problems.empty () )
			why = problems.front ();

		return problems.empty ();
	}
}
