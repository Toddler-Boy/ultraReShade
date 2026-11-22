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
#include <span>
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

		constexpr double	mostSize = 100000.0;
		constexpr double	mostShade = 1.0;

		// A json object of the file: its name and how many values it holds, taken from numberTable in turn
		struct Section
		{
			const char*	name;
			size_t		count;
			double		most;
		};

		// The file's numbers in its order, sections one after the other; the shades last. Everything before them is the roles' order (include/reshade_ultra_ui_theme.h)
		constexpr Size	numberTable[] {
			{ "xs", &spacing::xs }, { "m", &spacing::m }, { "l", &spacing::l }, { "xl", &spacing::xl },
			{ "small", &radius::little }, { "large", &radius::large },
			{ "line", &lines::line },
			{ "row", &heights::row }, { "large", &heights::large }, { "bar", &heights::bar },
			{ "small", &text::little, 1.0f }, { "body", &text::body, 1.0f }, { "large", &text::large, 1.0f }, { "title", &text::title, 1.0f },
			{ "icon", &icons::icon },
			{ "panelWidth", &parts::panelWidth, 1.0f }, { "tipMaxWidth", &parts::tipMaxWidth, 1.0f }, { "pickerSquare", &parts::pickerSquare },
			{ "pickerMarker", &parts::pickerMarker }, { "coverWidth", &parts::coverWidth }, { "coverHeight", &parts::coverHeight },
			{ "windowMinWidth", &parts::windowMinWidth }, { "windowMinHeight", &parts::windowMinHeight }, { "resizeEdge", &parts::resizeEdge },
			{ "switchWidth", &parts::switchWidth }, { "scrollbarWidth", &parts::scrollbarWidth }, { "scrollThumbMin", &parts::scrollThumbMin },
			{ "bipolarMinFill", &parts::bipolarMinFill }, { "barShadowHeight", &parts::barShadowHeight },
			{ "dragThreshold", &parts::dragThreshold },
			{ "transition", &motion::transition, 0.001f }, { "tipDelay", &motion::tipDelay }, { "doneSeconds", &motion::doneSeconds },
			{ "shadowSpread", &effects::shadowSpread }, { "shadowOffset", &effects::shadowOffset }, { "shadowAlpha", &effects::shadowAlpha },
			{ "disabledAlpha", &effects::disabledAlpha }, { "tipEdge", &effects::tipEdge }, { "sliderHandleGrow", &effects::sliderHandleGrow },
			{ "sweepAngle", &logo::sweepAngle }, { "sweepWidth", &logo::sweepWidth }, { "sweepSeconds", &logo::sweepSeconds }, { "sweepPeriod", &logo::sweepPeriod },
			{ "surface", &shades::surface }, { "raised", &shades::raised }, { "edge", &shades::edge }, { "hover", &shades::hover }, { "strong", &shades::strong },
			{ "muted", &shades::muted }, { "body", &shades::body } };

		constexpr Section	sectionTable[] {
			{ "spacing", 4, mostSize }, { "radius", 2, mostSize }, { "lines", 1, mostSize }, { "heights", 3, mostSize }, { "text", 4, mostSize }, { "icons", 1, mostSize },
			{ "parts", 15, mostSize }, { "motion", 3, mostSize }, { "effects", 6, mostSize }, { "logo", 4, mostSize }, { "shades", 7, mostShade } };

		constexpr Color	colorTable[] {
			{ "background", &colors::background }, { "text", &colors::text }, { "accent", &colors::accent },
			{ "danger", &colors::danger }, { "warning", &colors::warning }, { "appliesAfter", &colors::appliesAfter }, { "logoSweep", &colors::logoSweep } };

		constexpr size_t	numberCount = std::size ( numberTable );
		constexpr size_t	colorCount = std::size ( colorTable );
		constexpr size_t	shadeCount = sectionTable[ std::size ( sectionTable ) - 1 ].count;
		constexpr size_t	sizeCount = numberCount - shadeCount;

		constexpr size_t sectionsTotal ()
		{
			auto	total = size_t ( 0 );
			for ( const auto& section : sectionTable )
				total += section.count;
			return total;
		}

		static_assert ( sectionsTotal () == numberCount );
		static_assert ( sizeCount == RESHADE_UI_SIZE_COUNT );
		static_assert ( colorCount + shadeCount == RESHADE_UI_COLOR_COUNT );

		struct Values
		{
			std::array<float, numberCount>	numbers {};
			std::array<ImU32, colorCount>	colors {};
		};

		using Problems = std::vector<std::string>;

		Values						shipped;		// the built-in copy's
		std::filesystem::path		followed;		// the checkout's file while it is followed
		std::vector<std::string>	told;			// the problems logged, so a file read again with the same one says nothing

		// A value the file holds right is taken, one it lacks or holds wrong stays what base has. Problems come as phrases that name the value
		void readNumbers ( const reshade::json::value* object, std::span<const Size> table, float* out, const char* kind, double most, Problems& problems )
		{
			for ( size_t i = 0; i < table.size (); ++i )
			{
				const auto&	entry = table[ i ];
				const auto*	member = ReShadeJsonFind ( object, entry.name );
				auto		number = 0.0;
				if ( member == nullptr )
					problems.push_back ( std::string ( "the " ) + kind + " " + entry.name + " is missing" );
				else if ( ! ReShadeJsonGetNumber ( member, &number ) )
					problems.push_back ( std::string ( "the " ) + kind + " " + entry.name + " is no number" );
				else if ( ! ( number >= double ( entry.least ) && number <= most ) )
				{
					char	least[ 16 ] {};
					char	greatest[ 16 ] {};
					std::snprintf ( least, sizeof least, "%g", double ( entry.least ) );
					std::snprintf ( greatest, sizeof greatest, "%g", most );
					problems.push_back ( std::string ( "the " ) + kind + " " + entry.name + " is not from " + least + " to " + greatest );
				}
				else
					out[ i ] = float ( number );
			}

			for ( size_t i = 0, count = ReShadeJsonGetSize ( object ); i < count; ++i )
			{
				const char*	key = nullptr;
				ReShadeJsonAt ( object, i, &key );
				if ( key != nullptr && std::ranges::none_of ( table, [ key ] ( const Size& e ) { return std::string_view ( e.name ) == key; } ) )
					problems.push_back ( std::string ( "the " ) + kind + " " + key + " is not one the overlay knows" );
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
				problems.push_back ( "it is no json object" );
				return std::nullopt;
			}

			Values	v = base;
			auto	first = size_t ( 0 );
			for ( const auto& section : sectionTable )
			{
				const auto*	object = ReShadeJsonFind ( root, section.name );
				if ( ReShadeJsonGetType ( object ) == reshade::json::type::object )
					readNumbers ( object, std::span<const Size> ( numberTable ).subspan ( first, section.count ), v.numbers.data () + first, section.name, section.most, problems );
				else
					problems.push_back ( std::string ( "it has no object \"" ) + section.name + "\"" );
				first += section.count;
			}

			const auto*	colorsObject = ReShadeJsonFind ( root, "colors" );
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
			for ( size_t i = 0; i < numberCount; ++i )
				*numberTable[ i ].value = v.numbers[ i ];
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

		std::array<std::string, numberCount>	tunedNames {};
		std::array<Tuned, numberCount>		tunedSizes {};
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

		auto	index = size_t ( 0 );
		for ( const auto& section : sectionTable )
			for ( size_t i = 0; i < section.count; ++i, ++index )
			{
				tunedNames[ index ] = std::string ( section.name ) + " " + numberTable[ index ].name;
				tunedSizes[ index ] = { tunedNames[ index ].c_str (), numberTable[ index ].value, shipped.numbers[ index ] };
			}
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

	ImU32 shade ( float t )
	{
		const auto	u = std::clamp ( t, 0.0f, 1.0f );
		const auto	channel = [ & ] ( int shift )
		{
			const auto	from = float ( ( colors::background >> shift ) & 0xFF );
			const auto	to = float ( ( colors::text >> shift ) & 0xFF );
			return ImU32 ( from + ( to - from ) * u + 0.5f ) << shift;
		};
		return channel ( IM_COL32_R_SHIFT ) | channel ( IM_COL32_G_SHIFT ) | channel ( IM_COL32_B_SHIFT ) | IM_COL32_A_MASK;
	}

	ImU32 accentHover ()
	{
		constexpr auto	towardWhite = 0.2f;
		const auto	channel = [] ( int shift )
		{
			const auto	from = float ( ( colors::accent >> shift ) & 0xFF );
			return ImU32 ( from + ( 255.0f - from ) * towardWhite + 0.5f ) << shift;
		};
		return channel ( IM_COL32_R_SHIFT ) | channel ( IM_COL32_G_SHIFT ) | channel ( IM_COL32_B_SHIFT ) | IM_COL32_A_MASK;
	}

	ImU32 colorOf ( uint32_t role )
	{
		return role < colorCount ? *colorTable[ role ].value : role < colorCount + shadeCount ? shade ( *numberTable[ sizeCount + role - colorCount ].value ) : 0;
	}

	float sizeOf ( uint32_t role )
	{
		return role < sizeCount ? *numberTable[ role ].value : 0.0f;
	}

	std::string fileText ()
	{
		const auto	number = [] ( size_t i )
		{
			char	text[ 32 ] {};
			std::snprintf ( text, sizeof text, "%g", std::round ( double ( *numberTable[ i ].value ) * 1000.0 ) / 1000.0 );
			return std::string ( "\t\t\"" ) + numberTable[ i ].name + "\": " + text;
		};
		const auto	colorLines = [] ()
		{
			std::string	s = "\t\"colors\": {";
			for ( size_t i = 0; i < colorCount; ++i )
			{
				const auto	c = *colorTable[ i ].value;
				char		hex[ 16 ] {};
				std::snprintf ( hex, sizeof hex, "#%02X%02X%02X", unsigned ( ( c >> IM_COL32_R_SHIFT ) & 0xFF ), unsigned ( ( c >> IM_COL32_G_SHIFT ) & 0xFF ),
								unsigned ( ( c >> IM_COL32_B_SHIFT ) & 0xFF ) );
				s += std::string ( i == 0 ? "\n" : ",\n" ) + "\t\t\"" + colorTable[ i ].name + "\": \"" + hex + "\"";
			}
			return s + "\n\t}";
		};

		std::string	s = "{\n";
		auto		first = size_t ( 0 );
		for ( size_t k = 0; k < std::size ( sectionTable ); ++k )
		{
			if ( k + 1 == std::size ( sectionTable ) )
				s += colorLines () + ",\n";
			s += std::string ( "\t\"" ) + sectionTable[ k ].name + "\": {";
			for ( size_t i = 0; i < sectionTable[ k ].count; ++i )
				s += std::string ( i == 0 ? "\n" : ",\n" ) + number ( first + i );
			s += k + 1 == std::size ( sectionTable ) ? "\n\t}\n" : "\n\t},\n";
			first += sectionTable[ k ].count;
		}
		return s + "}\n";
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
