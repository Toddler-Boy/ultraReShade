/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include <imgui_internal.h>
#include <stb_image.h>
#include <Windows.h>

#include "dll_log.hpp"
#include "dll_resources.hpp"
#include "ultra_ui_anim.hpp"
#include "ultra_ui_draw.hpp"
#include "ultra_ui_font.hpp"
#include "ultra_ui_image.hpp"
#include "ultra_ui_window.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui::window
{
	namespace
	{
		// The window in back buffer pixels while the user moves or sizes it; the fractions are made from it when they let go
		struct Rect
		{
			float	x = 0.0f;
			float	y = 0.0f;
			float	w = 0.0f;
			float	h = 0.0f;
		};

		// 1 left, 2 right, 4 top, 8 bottom of the window's edge that is held; 16: the tab bar, which moves it
		enum : int { left = 1, right = 2, top = 4, bottom = 8, move = 16 };

		struct Interaction
		{
			Rect	rect;
			float	placedFor[ 2 ] = {};
			float	placedScale = 0.0f;
			int		held = 0;
			bool	isTyping = false;
		} now;

		std::map<std::string, uint32_t, std::less<>>	dots;

		[[ nodiscard ]] float px ( float unscaled )	{ return theme::px ( unscaled ); }

		void place ( const State& state, const ImVec2& display )
		{
			if ( now.placedFor[ 0 ] == display.x && now.placedFor[ 1 ] == display.y && now.placedScale == theme::scale () )
				return;

			now.rect = { state.position[ 0 ] * display.x, state.position[ 1 ] * display.y, state.size[ 0 ] * display.x, state.size[ 1 ] * display.y };
			now.placedFor[ 0 ] = display.x;
			now.placedFor[ 1 ] = display.y;
			now.placedScale = theme::scale ();
		}

		// Inside the display, at least as large as the least the content needs, never larger than the display
		void clamp ( const ImVec2& display )
		{
			now.rect.w = std::clamp ( now.rect.w, std::min ( px ( theme::windowMinWidth ), display.x ), display.x );
			now.rect.h = std::clamp ( now.rect.h, std::min ( px ( theme::windowMinHeight ), display.y ), display.y );
			now.rect.x = std::clamp ( now.rect.x, 0.0f, display.x - now.rect.w );
			now.rect.y = std::clamp ( now.rect.y, 0.0f, display.y - now.rect.h );
		}

		[[ nodiscard ]] bool isSame ( std::string_view a, std::string_view b )
		{
			return a.size () == b.size () && std::equal ( a.begin (), a.end (), b.begin (), [] ( char x, char y ) { return std::tolower ( uint8_t ( x ) ) == std::tolower ( uint8_t ( y ) ); } );
		}

		[[ nodiscard ]] ImGuiMouseCursor cursorOf ( int held )
		{
			const auto	horizontal = ( held & ( left | right ) ) != 0;
			const auto	vertical = ( held & ( top | bottom ) ) != 0;
			if ( horizontal && vertical )
				return ( ( held & left ) != 0 ) == ( ( held & top ) != 0 ) ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW;
			return horizontal ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS;
		}

		// The edges of the window the mouse is at, 0 inside
		[[ nodiscard ]] int edgesAt ( const ImVec2& mouse )
		{
			const auto	band = px ( theme::resizeEdge );
			if ( mouse.x < now.rect.x || mouse.x > now.rect.x + now.rect.w || mouse.y < now.rect.y || mouse.y > now.rect.y + now.rect.h )
				return 0;

			auto	edges = 0;
			edges |= mouse.x < now.rect.x + band ? left : 0;
			edges |= mouse.x > now.rect.x + now.rect.w - band ? right : 0;
			edges |= mouse.y < now.rect.y + band ? top : 0;
			edges |= mouse.y > now.rect.y + now.rect.h - band ? bottom : 0;
			return edges;
		}

		void dragEdges ( const ImVec2& delta, const ImVec2& display )
		{
			const auto	minW = std::min ( px ( theme::windowMinWidth ), display.x );
			const auto	minH = std::min ( px ( theme::windowMinHeight ), display.y );
			auto&		r = now.rect;

			if ( now.held & left )
			{
				const auto	moved = std::clamp ( delta.x, -r.x, r.w - minW );
				r.x += moved;
				r.w -= moved;
			}
			if ( now.held & right )
				r.w = std::clamp ( r.w + delta.x, minW, display.x - r.x );
			if ( now.held & top )
			{
				const auto	moved = std::clamp ( delta.y, -r.y, r.h - minH );
				r.y += moved;
				r.h -= moved;
			}
			if ( now.held & bottom )
				r.h = std::clamp ( r.h + delta.y, minH, display.y - r.y );
		}

		void closeCross ( ImDrawList* dl, ImVec2 centre, float half, ImU32 color )
		{
			dl->AddLine ( ImVec2 ( centre.x - half, centre.y - half ), ImVec2 ( centre.x + half, centre.y + half ), color, px ( theme::closeLine ) );
			dl->AddLine ( ImVec2 ( centre.x - half, centre.y + half ), ImVec2 ( centre.x + half, centre.y - half ), color, px ( theme::closeLine ) );
		}

		// The logo in its tint, a band of logoSweep leaning logoSweepAngle passing across it once every logoSweepPeriod seconds; a
		// grid of quads with the band's colour at their corners, fine enough for the band's soft edges
		void drawLogo ( ImDrawList* list, const Texture& logo, ImVec2 at, ImVec2 size )
		{
			constexpr int	columns = 64;
			constexpr int	rows = 4;

			const auto	lean = std::tan ( theme::logoSweepAngle * 3.14159265f / 180.0f );
			const auto	half = std::max ( 1.0f, px ( theme::logoSweepWidth ) * 0.5f );
			const auto	seconds = std::max ( 0.001f, theme::logoSweepSeconds );
			const auto	period = std::max ( seconds, theme::logoSweepPeriod );
			const auto	phase = float ( std::fmod ( ImGui::GetTime (), double ( period ) ) ) / seconds;
			const auto	from = -half - size.y * lean;
			const auto	front = from + std::min ( phase, 1.0f ) * ( size.x + half - from );
			const auto	tint = draw::color ( theme::colors::bright );
			const auto	sweep = draw::color ( theme::colors::logoSweep );

			list->PushTexture ( logo.ref () );
			list->PrimReserve ( columns * rows * 6, ( columns + 1 ) * ( rows + 1 ) );
			const auto	first = list->_VtxCurrentIdx;

			for ( int r = 0; r <= rows; ++r )
				for ( int c = 0; c <= columns; ++c )
				{
					const auto	u = float ( c ) / columns;
					const auto	v = float ( r ) / rows;
					const auto	x = u * size.x;
					const auto	y = v * size.y;
					const auto	distance = std::abs ( x - ( size.y - y ) * lean - front ) / half;
					const auto	band = phase < 1.0f ? std::max ( 0.0f, 1.0f - distance * distance ) : 0.0f;
					list->PrimWriteVtx ( ImVec2 ( at.x + x, at.y + y ), ImVec2 ( u, v ), draw::mix ( tint, sweep, band ) );
				}

			for ( int r = 0; r < rows; ++r )
				for ( int c = 0; c < columns; ++c )
				{
					const auto	i = first + unsigned ( r * ( columns + 1 ) + c );
					const auto	below = i + unsigned ( columns + 1 );
					for ( const auto index : { i, i + 1, below + 1, i, below + 1, below } )
						list->PrimWriteIdx ( ImDrawIdx ( index ) );
				}

			list->PopTexture ();
		}

		struct Artwork
		{
			std::vector<uint32_t>	rgba;
			int						width = 0;
			int						height = 0;
		};

		// The start logo's artwork, read from the DLL once; empty when it cannot be
		[[ nodiscard ]] const Artwork& logoArtwork ()
		{
			static const auto	artwork = []
			{
				Artwork		out;
				const auto	png = ::reshade::resources::load_data_resource ( IDB_ULTRA_LOGO );
				auto		channels = 0;
				const auto	pixels = png.data != nullptr ? stbi_load_from_memory ( static_cast<const stbi_uc*> ( png.data ), int ( png.data_size ), &out.width, &out.height, &channels, STBI_rgb_alpha )
													 : nullptr;
				if ( pixels == nullptr )
				{
					::reshade::log::message ( ::reshade::log::level::error, "Failed to read the ultraReShade logo for the overlay's tab bar." );
					return out;
				}

				out.rgba.resize ( size_t ( out.width ) * size_t ( out.height ) );
				std::memcpy ( out.rgba.data (), pixels, out.rgba.size () * 4 );
				stbi_image_free ( pixels );
				return out;
			} ();

			return artwork;
		}
	}

	void setTabDot ( std::string_view label, uint32_t color )
	{
		if ( color == 0 )
			dots.erase ( std::string ( label ) );
		else
			dots[ std::string ( label ) ] = color;
	}

	bool isTyping ()
	{
		return now.isTyping;
	}

	void applyStyle ()
	{
		auto&	style = ImGui::GetStyle ();
		auto*	c = style.Colors;
		const auto	v4 = [] ( ImU32 color ) { return ImGui::ColorConvertU32ToFloat4 ( color ); };

		style.WindowRounding = 0.0f;
		style.WindowBorderSize = 0.0f;
		style.ChildRounding = 0.0f;
		style.ChildBorderSize = 0.0f;
		style.FrameRounding = px ( theme::dropRounding );
		style.PopupRounding = px ( theme::menuRounding );
		style.ScrollbarRounding = px ( theme::scrollbarWidth ) * 0.5f;
		style.ScrollbarSize = px ( theme::scrollbarWidth + 2.0f * theme::scrollbarGap );
		style.ScrollbarPadding = px ( theme::scrollbarGap );
		style.FontScaleMain = theme::scale ();

		c[ ImGuiCol_Text ] = v4 ( theme::colors::text );
		c[ ImGuiCol_TextDisabled ] = v4 ( theme::colors::muted );
		c[ ImGuiCol_WindowBg ] = v4 ( theme::colors::window );
		c[ ImGuiCol_ChildBg ] = ImVec4 ( 0, 0, 0, 0 );
		c[ ImGuiCol_PopupBg ] = v4 ( theme::colors::popup );
		c[ ImGuiCol_Border ] = v4 ( theme::colors::windowEdge );
		c[ ImGuiCol_FrameBg ] = v4 ( theme::colors::control );
		c[ ImGuiCol_FrameBgHovered ] = v4 ( theme::colors::controlHover );
		c[ ImGuiCol_FrameBgActive ] = v4 ( theme::colors::controlHover );
		c[ ImGuiCol_ScrollbarBg ] = v4 ( theme::colors::scrollTrack );
		c[ ImGuiCol_ScrollbarGrab ] = v4 ( theme::colors::scrollThumb );
		c[ ImGuiCol_ScrollbarGrabHovered ] = v4 ( theme::colors::scrollThumbHover );
		c[ ImGuiCol_ScrollbarGrabActive ] = v4 ( theme::colors::scrollThumbHover );
		c[ ImGuiCol_CheckMark ] = v4 ( theme::colors::accent );
		c[ ImGuiCol_SliderGrab ] = v4 ( theme::colors::accent );
		c[ ImGuiCol_SliderGrabActive ] = v4 ( theme::colors::accentHover );
		c[ ImGuiCol_Button ] = v4 ( theme::colors::button );
		c[ ImGuiCol_ButtonHovered ] = v4 ( theme::colors::buttonHover );
		c[ ImGuiCol_ButtonActive ] = v4 ( theme::colors::buttonHover );
		c[ ImGuiCol_Header ] = v4 ( theme::colors::button );
		c[ ImGuiCol_HeaderHovered ] = v4 ( theme::colors::buttonHover );
		c[ ImGuiCol_HeaderActive ] = v4 ( theme::colors::buttonHover );
		c[ ImGuiCol_Separator ] = v4 ( theme::colors::separator );
		c[ ImGuiCol_TextSelectedBg ] = ImGui::ColorConvertU32ToFloat4 ( ( theme::colors::accent & 0x00FFFFFFu ) | 0x70000000u );
		c[ ImGuiCol_NavCursor ] = ImVec4 ( 0, 0, 0, 0 );
	}

	Result draw ( api::effect_runtime* runtime, State& state, std::span<const Tab> tabs )
	{
		Result		result;
		const auto&	io = ImGui::GetIO ();
		const auto	display = io.DisplaySize;

		place ( state, display );

		// Held edges and the tab bar move or size the window while the left button is down
		const auto	mouse = io.MousePos;
		if ( now.held == 0 && ImGui::IsMouseClicked ( ImGuiMouseButton_Left ) && ! ImGui::IsPopupOpen ( "", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel ) )
			now.held = edgesAt ( mouse );

		if ( now.held != 0 && ! ImGui::IsMouseDown ( ImGuiMouseButton_Left ) )
		{
			now.held = 0;
			result.isMovedOrResized = true;
		}
		else if ( now.held == move )
		{
			now.rect.x += io.MouseDelta.x;
			now.rect.y += io.MouseDelta.y;
		}
		else if ( now.held != 0 )
			dragEdges ( io.MouseDelta, display );

		clamp ( display );

		const auto	hoveredEdges = now.held != 0 ? now.held : edgesAt ( mouse );
		if ( hoveredEdges != 0 && hoveredEdges != move )
			ImGui::SetMouseCursor ( cursorOf ( hoveredEdges ) );

		const ImVec2	min ( std::round ( now.rect.x ), std::round ( now.rect.y ) );
		const ImVec2	max ( min.x + std::round ( now.rect.w ), min.y + std::round ( now.rect.h ) );
		const auto		rounding = px ( theme::windowRounding );

		ImGui::SetNextWindowPos ( min );
		ImGui::SetNextWindowSize ( ImVec2 ( max.x - min.x, max.y - min.y ) );
		ImGui::PushStyleVar ( ImGuiStyleVar_WindowPadding, ImVec2 ( 0.0f, 0.0f ) );
		const auto	isOpen = ImGui::Begin ( "###overlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing
											| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground
											| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoNav );
		ImGui::PopStyleVar ();

		if ( isOpen )
		{
			auto*	list = ImGui::GetWindowDrawList ();
			draw::shadow ( list, min, max, rounding );
			const auto	barHeight = px ( theme::tabInsetTop ) + px ( theme::tabHeight ) + px ( theme::tabInsetBottom );
			list->AddRectFilled ( min, max, theme::colors::window, rounding );
			list->AddRect ( ImVec2 ( min.x + 0.5f, min.y + 0.5f ), ImVec2 ( max.x - 0.5f, max.y - 0.5f ), theme::colors::windowEdge, rounding, 1.0f );

			// The tab bar: the tabs in a rounded track, the selected one an accent pill that glides to it
			const auto	size = px ( theme::sizes::body );
			const auto	pad = px ( theme::windowPad );
			const auto	pillInset = px ( theme::tabPillInset );
			const auto	tabPad = px ( theme::tabPadX );
			auto		selected = size_t ( 0 );
			for ( size_t i = 0; i < tabs.size (); ++i )
				if ( isSame ( tabs[ i ].name, state.tab ) )
					selected = i;

			// The logo left of the tabs
			const auto	trackTop = min.y + px ( theme::tabInsetTop );
			const auto	trackH = px ( theme::tabHeight );
			const auto	logoH = px ( theme::logoHeight );
			auto		trackLeft = min.x + pad;
			const auto&	artwork = logoArtwork ();
			if ( const auto* logo = pixelsMask ( runtime, L"ultraReShade logo", artwork.rgba, artwork.width, artwork.height, int ( std::lround ( logoH ) ) ) )
			{
				const ImVec2	at ( std::round ( trackLeft + px ( theme::logoInset ) ),std::round ( trackTop + ( trackH - logoH ) * 0.5f ) );
				const auto		logoW = float ( logo->width ) * logoH / float ( logo->height );
				drawLogo ( list, *logo, at, ImVec2 ( logoW, logoH ) );
				trackLeft += px ( theme::logoInset ) + logoW + px ( theme::logoGap );
			}

			const ImVec2	trackMin ( trackLeft, trackTop );
			const auto		trackBottom = trackTop + trackH;

			std::vector<float>	widths;
			auto				trackRight = trackMin.x + pillInset;
			for ( const auto& tab : tabs )
			{
				const auto	dotRoom = dots.contains ( std::string ( tab.label ) ) ? px ( theme::dotSize ) + px ( theme::dotGap ) : 0.0f;
				widths.push_back ( font::width ( font::Face::medium, size, tab.label ) + dotRoom + 2.0f * tabPad );
				trackRight += widths.back ();
			}
			trackRight += pillInset;
			auto	x = trackMin.x + pillInset;

			if ( ! tabs.empty () )
			{
				list->AddRectFilled ( trackMin, ImVec2 ( trackRight, trackBottom ), theme::colors::titleBar, trackH * 0.5f );

				auto	pillX = x;
				for ( size_t i = 0; i < selected; ++i )
					pillX += widths[ i ];

				// In the window's own coordinates, so a moved window carries the pill along
				const auto	from = min.x + anim::swing ( ImGui::GetID ( "tabPillLeft" ), pillX - min.x ).mid ();
				const auto	to = min.x + anim::swing ( ImGui::GetID ( "tabPillRight" ), pillX + widths[ selected ] - min.x ).mid ();
				const auto	pillH = trackH - 2.0f * pillInset;
				list->AddRectFilled ( ImVec2 ( from, trackMin.y + pillInset ), ImVec2 ( to, trackBottom - pillInset ), theme::colors::accent, pillH * 0.5f );
			}

			for ( size_t i = 0; i < tabs.size (); ++i )
			{
				const auto&	tab = tabs[ i ];
				const auto	dot = dots.find ( tab.label );
				const auto	dotRoom = dot != dots.end () ? px ( theme::dotSize ) + px ( theme::dotGap ) : 0.0f;
				const ImVec2	tabMin ( x, trackMin.y );
				const ImVec2	tabMax ( x + widths[ i ], trackBottom );

				ImGui::SetCursorScreenPos ( tabMin );
				ImGui::PushID ( int ( i ) );
				ImGui::InvisibleButton ( "##tab", ImVec2 ( tabMax.x - tabMin.x, tabMax.y - tabMin.y ) );
				const auto	isHovered = ImGui::IsItemHovered ();
				if ( ImGui::IsItemClicked () && i != selected )
				{
					state.tab = tab.name;
					selected = i;
					result.isTabChanged = true;
				}
				ImGui::PopID ();

				const auto	isSelected = i == selected;
				const auto	color = isSelected ? theme::colors::tabSelectedText : isHovered ? theme::colors::tabHover : theme::colors::tab;
				const auto	textX = x + tabPad;
				if ( dot != dots.end () )
				{
					// A dark ring keeps a green dot visible on the green pill
					const ImVec2	centre ( textX + px ( theme::dotSize ) * 0.5f, trackMin.y + ( trackBottom - trackMin.y ) * 0.5f );
					if ( isSelected )
						list->AddCircleFilled ( centre, px ( theme::dotSize ) * 0.5f + px ( theme::dotRing ), theme::colors::window );
					list->AddCircleFilled ( centre, px ( theme::dotSize ) * 0.5f, dot->second );
				}
				font::draw ( list, ImVec2 ( textX + dotRoom, font::centredTop ( font::Face::medium, size, trackMin.y, trackBottom - trackMin.y ) ), font::Face::medium, size, draw::color ( color ), tab.label );

				x += widths[ i ];
			}
			x = trackRight;

			// The close button at the bar's end
			const auto	closeBox = px ( theme::closeSize ) * 2.0f;
			const ImVec2	closeMin ( max.x - pad - closeBox, trackMin.y + ( trackBottom - trackMin.y - closeBox ) * 0.5f );
			ImGui::SetCursorScreenPos ( closeMin );
			ImGui::InvisibleButton ( "##close", ImVec2 ( closeBox, closeBox ) );
			const auto	isCloseHovered = ImGui::IsItemHovered ();
			// On release, so no item stays held in the frames that do not run while the overlay is closed
			if ( ImGui::IsItemDeactivated () && isCloseHovered )
				result.isCloseWanted = true;
			closeCross ( list, ImVec2 ( closeMin.x + closeBox * 0.5f, closeMin.y + closeBox * 0.5f ), px ( theme::closeSize ) * 0.35f, isCloseHovered ? theme::colors::tabActive : theme::colors::tab );

			// The rest of the bar moves the window
			const auto	moveFrom = std::max ( x, min.x + pad );
			if ( closeMin.x - moveFrom > 1.0f )
			{
				ImGui::SetCursorScreenPos ( ImVec2 ( moveFrom, min.y + px ( theme::resizeEdge ) ) );
				ImGui::InvisibleButton ( "##move", ImVec2 ( closeMin.x - moveFrom, barHeight - px ( theme::resizeEdge ) ) );
				if ( ImGui::IsItemActivated () && now.held == 0 )
					now.held = move;
			}

			// The tab's content below the bar, off the rounded corners
			const ImVec2	contentMin ( min.x + pad, min.y + barHeight );
			const ImVec2	contentSize ( max.x - min.x - 2.0f * pad, max.y - min.y - barHeight - pad );
			ImGui::SetCursorScreenPos ( contentMin );
			if ( ImGui::BeginChild ( "##tab", contentSize, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav ) )
			{
				if ( selected < tabs.size () && tabs[ selected ].draw )
					tabs[ selected ].draw ();

				// The bar's shadow over the content's top, drawn last: into the innermost child the tab made, which ImGui draws after the rest
				auto*	last = ImGui::GetCurrentWindow ();
				while ( ! last->DC.ChildWindows.empty () )
					last = last->DC.ChildWindows.back ();

				const auto	shadowH = px ( theme::barShadowHeight );
				const auto	dark = IM_COL32 ( 0, 0, 0, int ( std::lround ( std::clamp ( theme::barShadowAlpha, 0.0f, 1.0f ) * 255.0f ) ) );
				const auto	clear = IM_COL32 ( 0, 0, 0, 0 );
				const ImVec2	shadowMin ( min.x + 1.0f, contentMin.y );
				const ImVec2	shadowMax ( max.x - 1.0f, contentMin.y + shadowH );
				last->DrawList->PushClipRect ( shadowMin, shadowMax, false );
				last->DrawList->AddRectFilledMultiColor ( shadowMin, shadowMax, dark, dark, clear, clear );
				last->DrawList->PopClipRect ();
			}
			ImGui::EndChild ();
		}
		ImGui::End ();

		if ( result.isMovedOrResized || result.isTabChanged || now.held == 0 )
		{
			state.position[ 0 ] = std::round ( std::clamp ( now.rect.x / display.x, 0.0f, 1.0f ) * 1e6f ) / 1e6f;
			state.position[ 1 ] = std::round ( std::clamp ( now.rect.y / display.y, 0.0f, 1.0f ) * 1e6f ) / 1e6f;
			state.size[ 0 ] = std::round ( std::clamp ( now.rect.w / display.x, 0.0f, 1.0f ) * 1e6f ) / 1e6f;
			state.size[ 1 ] = std::round ( std::clamp ( now.rect.h / display.y, 0.0f, 1.0f ) * 1e6f ) / 1e6f;
		}

		now.isTyping = ImGui::GetIO ().WantTextInput;
		return result;
	}

	//-------------------------------------------------------------------------
	// The cursor

	namespace
	{
		struct CursorPick
		{
			ImGuiMouseCursor	imgui;
			LPCWSTR				id;
			const wchar_t*		name;
		};

		const CursorPick	picks[] {
			{ ImGuiMouseCursor_Arrow, MAKEINTRESOURCEW ( 32512 ), L"arrow" }, { ImGuiMouseCursor_TextInput, MAKEINTRESOURCEW ( 32513 ), L"ibeam" },
			{ ImGuiMouseCursor_ResizeAll, MAKEINTRESOURCEW ( 32646 ), L"all" }, { ImGuiMouseCursor_ResizeNS, MAKEINTRESOURCEW ( 32645 ), L"ns" },
			{ ImGuiMouseCursor_ResizeEW, MAKEINTRESOURCEW ( 32644 ), L"ew" }, { ImGuiMouseCursor_ResizeNESW, MAKEINTRESOURCEW ( 32643 ), L"nesw" },
			{ ImGuiMouseCursor_ResizeNWSE, MAKEINTRESOURCEW ( 32642 ), L"nwse" }, { ImGuiMouseCursor_Hand, MAKEINTRESOURCEW ( 32649 ), L"hand" },
			{ ImGuiMouseCursor_NotAllowed, MAKEINTRESOURCEW ( 32648 ), L"no" } };

		struct CursorImage
		{
			std::vector<uint32_t>	pixels;
			int						size = 0;
			ImVec2					hotspot;
		};

		std::map<std::wstring, CursorImage>	cursorImages;

		// The player's cursor: the scheme's, at the cursor size Windows is set to times the display's scale
		[[ nodiscard ]] int cursorSize ( HWND window )
		{
			DWORD	base = 32;
			DWORD	bytes = sizeof ( base );
			RegGetValueW ( HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize", RRF_RT_REG_DWORD, nullptr, &base, &bytes );
			const auto	previous = SetThreadDpiAwarenessContext ( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
			const auto	dpi = window != nullptr ? GetDpiForWindow ( window ) : 96u;
			SetThreadDpiAwarenessContext ( previous );
			return std::clamp ( int ( std::lround ( float ( base ) * float ( std::max ( dpi, 96u ) ) / 96.0f ) ), 16, 256 );
		}

		// A cursor as pixels, its colour and transparency told from drawing it over black and over white
		[[ nodiscard ]] bool cursorPixels ( LPCWSTR id, int size, CursorImage& out )
		{
			const auto	cursor = static_cast<HCURSOR> ( LoadImageW ( nullptr, id, IMAGE_CURSOR, size, size, LR_SHARED ) );
			if ( cursor == nullptr )
				return false;

			ICONINFO	info {};
			if ( ! GetIconInfo ( cursor, &info ) )
				return false;
			out.hotspot = ImVec2 ( float ( info.xHotspot ), float ( info.yHotspot ) );
			DeleteObject ( info.hbmColor );
			DeleteObject ( info.hbmMask );

			BITMAPINFO	bmi {};
			bmi.bmiHeader.biSize = sizeof ( BITMAPINFOHEADER );
			bmi.bmiHeader.biWidth = size;
			bmi.bmiHeader.biHeight = -size;
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 32;
			bmi.bmiHeader.biCompression = BI_RGB;

			const auto	dc = CreateCompatibleDC ( nullptr );
			void*		bits = nullptr;
			const auto	dib = CreateDIBSection ( dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0 );
			if ( dib == nullptr )
			{
				DeleteDC ( dc );
				return false;
			}

			const auto		old = SelectObject ( dc, dib );
			const auto		count = size_t ( size ) * size_t ( size );
			std::vector<uint32_t>	onBlack ( count ), onWhite ( count );
			for ( int pass = 0; pass < 2; ++pass )
			{
				std::fill_n ( static_cast<uint32_t*> ( bits ), count, pass == 0 ? 0xFF000000u : 0xFFFFFFFFu );
				DrawIconEx ( dc, 0, 0, cursor, size, size, 0, nullptr, DI_NORMAL );
				std::copy_n ( static_cast<uint32_t*> ( bits ), count, ( pass == 0 ? onBlack : onWhite ).begin () );
			}
			SelectObject ( dc, old );
			DeleteObject ( dib );
			DeleteDC ( dc );

			out.size = size;
			out.pixels.resize ( count );
			for ( size_t i = 0; i < count; ++i )
			{
				const auto	b = onBlack[ i ];
				const auto	w = onWhite[ i ];
				// Over black a pixel is colour * alpha, over white colour * alpha + 1 - alpha: their difference is the transparency
				const auto	alpha = 255 - ( int ( ( ( w >> 16 ) & 0xFF ) - ( ( b >> 16 ) & 0xFF ) ) + int ( ( ( w >> 8 ) & 0xFF ) - ( ( b >> 8 ) & 0xFF ) ) + int ( ( w & 0xFF ) - ( b & 0xFF ) ) ) / 3;
				const auto	a = std::clamp ( alpha, 0, 255 );
				const auto	unpremultiply = [ a ] ( uint32_t channel ) { return a == 0 ? 0u : std::min ( 255u, channel * 255u / uint32_t ( a ) ); };
				out.pixels[ i ] = unpremultiply ( ( b >> 16 ) & 0xFF ) | ( unpremultiply ( ( b >> 8 ) & 0xFF ) << 8 ) | ( unpremultiply ( b & 0xFF ) << 16 ) | ( uint32_t ( a ) << 24 );
			}
			return true;
		}
	}

	void drawCursor ( api::effect_runtime* runtime )
	{
		const auto	wanted = ImGui::GetMouseCursor ();
		const auto*	pick = &picks[ 0 ];
		for ( const auto& p : picks )
			if ( p.imgui == wanted )
				pick = &p;

		const auto	hwnd = static_cast<HWND> ( runtime->get_hwnd () );
		const auto	size = cursorSize ( hwnd );
		const auto	key = std::wstring ( pick->name ) + L"@" + std::to_wstring ( size );
		auto [ it, fresh ] = cursorImages.try_emplace ( key );
		if ( fresh && ! cursorPixels ( pick->id, size, it->second ) )
			return;
		if ( it->second.pixels.empty () )
			return;

		const auto*	texture = pixelsImage ( runtime, key, it->second.pixels, it->second.size, it->second.size );
		if ( texture == nullptr )
			return;

		const auto	at = ImGui::GetIO ().MousePos;
		const ImVec2	min ( std::round ( at.x - it->second.hotspot.x ), std::round ( at.y - it->second.hotspot.y ) );
		ImGui::GetForegroundDrawList ()->AddImage ( texture->ref (), min, ImVec2 ( min.x + float ( size ), min.y + float ( size ) ) );
	}

	//-------------------------------------------------------------------------
	// The messages

	namespace
	{
		struct Notice
		{
			Message		message;
			double		until = 0.0;
		};

		std::vector<Notice>	notices;
	}

	void notice ( std::string text, uint32_t color, double seconds )
	{
		notices.push_back ( { { std::move ( text ), color }, ImGui::GetTime () + seconds } );
	}

	void drawMessages ( std::span<const Message> given )
	{
		std::erase_if ( notices, [] ( const Notice& n ) { return n.until < ImGui::GetTime (); } );
		std::vector<Message>	messages ( given.begin (), given.end () );
		for ( const auto& n : notices )
			messages.push_back ( n.message );
		if ( messages.empty () )
			return;

		auto*		dl = ImGui::GetForegroundDrawList ();
		const auto	pad = px ( theme::messagePad );
		const auto	size = px ( theme::sizes::body );
		auto		y = pad;
		for ( const auto& m : messages )
		{
			const auto	w = font::width ( font::Face::regular, size, m.text );
			const auto	h = px ( theme::rowHeight );
			const ImVec2	min ( pad, y );
			const ImVec2	max ( pad + w + pad * 2.0f, y + h );
			draw::shadow ( dl, min, max, px ( theme::dropRounding ) );
			dl->AddRectFilled ( min, max, theme::colors::message, px ( theme::dropRounding ) );
			font::draw ( dl, ImVec2 ( min.x + pad, font::centredTop ( font::Face::regular, size, y, h ) ), font::Face::regular, size, m.color != 0 ? m.color : theme::colors::messageText, m.text );
			y += h + pad * 0.5f;
		}
	}
}
