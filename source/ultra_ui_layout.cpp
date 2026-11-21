/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ultra_ui_convert.hpp"
#include "ultra_ui_draw.hpp"
#include "ultra_ui_font.hpp"
#include "ultra_ui_layout.hpp"
#include "ultra_ui_texture.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	namespace
	{
		std::atomic<bool>	mouseOverOverlay { false };

		// x of each column from the row's start, and the control's width; the same for every row of the page
		struct Columns
		{
			float	control;
			float	controlWidth;
			float	value;
			float	valueWidth;
			float	reset;
		};

		[[ nodiscard ]] bool labelled ( uint32_t kind )
		{
			return kind == RESHADE_UI_ITEM_SLIDER || kind == RESHADE_UI_ITEM_OPTIONS || kind == RESHADE_UI_ITEM_FIELD || kind == RESHADE_UI_ITEM_COLOR;
		}

		[[ nodiscard ]] bool has ( const ReShadeUiItem& item, uint32_t flag )	{ return ( item.flags & flag ) != 0; }

		// Up to four single-word names fit a choice row; more, or longer ones, go into a drop-down unless the item asks for the row
		[[ nodiscard ]] bool asChoiceRow ( const ReShadeUiItem& item )
		{
			constexpr size_t	maxChoices = 4;

			return has ( item, RESHADE_UI_ITEM_FLAG_ROW )
				   || ( item.choiceCount <= maxChoices && std::none_of ( item.choices, item.choices + item.choiceCount, [] ( const char* name ) { return std::strchr ( name, ' ' ) != nullptr; } ) );
		}

		template <typename T>
		[[ nodiscard ]] bool shown ( const T& t, void* frame )	{ return t.isVisible == nullptr || t.isVisible ( t.context, frame ) != 0; }

		template <typename T>
		[[ nodiscard ]] const char* reasonOf ( const T& t, void* frame )	{ return t.disabledReason != nullptr ? t.disabledReason ( t.context, frame ) : nullptr; }

		[[ nodiscard ]] const char* labelFor ( const ReShadeUiItem& item, void* frame )
		{
			const auto*	text = item.title != nullptr ? item.title ( item.context, frame ) : item.label;
			return text != nullptr ? text : "";
		}

		// A choice's index for a stored value and back; without values the value is the index
		[[ nodiscard ]] int indexOf ( const ReShadeUiItem& item, float v )
		{
			if ( item.choiceValues == nullptr || item.choiceCount == 0 )
				return int ( std::lround ( v ) );

			// The nearest, so a stored value between two choices still shows one
			uint32_t	best = 0;
			for ( uint32_t i = 1; i < item.choiceCount; ++i )
				if ( std::abs ( item.choiceValues[ i ] - v ) < std::abs ( item.choiceValues[ best ] - v ) )
					best = i;

			return int ( best );
		}

		[[ nodiscard ]] float valueOf ( const ReShadeUiItem& item, int index )
		{
			if ( item.choiceValues != nullptr && index >= 0 && uint32_t ( index ) < item.choiceCount )
				return item.choiceValues[ index ];

			return float ( index );
		}

		// The rows shown now: each repeat replaced by its rows for the entries its list has now
		[[ nodiscard ]] std::vector<const ReShadeUiItem*> rowsOf ( const ReShadeUiItem* items, uint32_t count )
		{
			std::vector<const ReShadeUiItem*>	out;
			out.reserve ( count );
			for ( uint32_t i = 0; i < count; ++i )
			{
				const auto&	item = items[ i ];
				if ( item.kind != RESHADE_UI_ITEM_REPEAT )
				{
					out.push_back ( &item );
					continue;
				}

				const auto	entries = item.repeatCount != nullptr ? item.repeatCount ( item.context ) : 0u;
				for ( uint32_t entry = 0; entry < entries; ++entry )
				{
					uint32_t	n = 0;
					const auto*	rows = item.repeatRows ( item.context, entry, &n );
					for ( uint32_t k = 0; rows != nullptr && k < n; ++k )
						out.push_back ( &rows[ k ] );
				}
			}
			return out;
		}

		// A subheading shows while a row of its group does
		[[ nodiscard ]] bool groupShown ( const std::vector<const ReShadeUiItem*>& items, size_t at, void* frame )
		{
			for ( auto i = at + 1; i < items.size () && items[ i ]->kind != RESHADE_UI_ITEM_SUBHEADING; ++i )
				if ( shown ( *items[ i ], frame ) )
					return true;

			return false;
		}

		// The tooltip with why the row is off ahead of it, then when it takes effect
		[[ nodiscard ]] const char* tipWith ( const char* tooltip, const char* reason, std::string& storage, Applies applies = Applies::now, const char* when = nullptr )
		{
			if ( ( reason == nullptr || *reason == 0 ) && applies == Applies::now )
				return tooltip;

			storage.clear ();
			for ( const auto* part : { reason, tooltip } )
				if ( part != nullptr && *part != 0 )
					storage.append ( storage.empty () ? "" : "\n" ).append ( part );

			if ( applies != Applies::now )
				storage.append ( storage.empty () ? "" : "\n" ).append ( laterMark ).append ( " " )
					   .append ( applies == Applies::nextStart ? "Requires a restart" : when != nullptr ? when : "" );

			return storage.c_str ();
		}

		void gap ( float unscaled )
		{
			ImGui::Dummy ( ImVec2 ( 0.0f, theme::px ( unscaled ) ) );
		}

		[[ nodiscard ]] float valueWidth ( const ReShadeUiItem& item )
		{
			char		text[ 32 ];
			float		w = 0.0f;
			const auto	f = formatOf ( item.format );

			for ( const auto v : { item.range.lo, item.range.hi } )
			{
				format ( text, sizeof ( text ), v, f );
				w = std::max ( w, font::width ( font::Face::bold, theme::px ( theme::sizes::value ), text, true ) );
			}

			return w;
		}

		// Over the rows shown now, so a game's hidden section does not widen the others
		[[ nodiscard ]] Columns columns ( const ReShadeUiPage& page, float width, void* frame )
		{
			float	labelW = 0.0f;
			float	valueW = 0.0f;

			for ( uint32_t s = 0; s < page.sectionCount; ++s )
			{
				const auto&	section = page.sections[ s ];
				if ( ! shown ( section, frame ) )
					continue;

				for ( const auto* row : rowsOf ( section.items, section.itemCount ) )
				{
					const auto&	item = *row;
					if ( ! shown ( item, frame ) )
						continue;
					if ( labelled ( item.kind ) )
						labelW = std::max ( labelW, labelWidth ( labelFor ( item, frame ), item.tooltip != nullptr || item.disabledReason != nullptr || item.applies != RESHADE_UI_APPLIES_NOW,
																 appliesOf ( item.applies ) )
														+ ( item.onOff.isBound != 0 ? theme::px ( theme::switchWidth + theme::headingGap ) : 0.0f ) );
					if ( item.kind == RESHADE_UI_ITEM_SLIDER )
						valueW = std::max ( valueW, valueWidth ( item ) );
				}
			}

			const auto	g = theme::px ( theme::columnGap );
			const auto	resetW = theme::px ( theme::resetWidth );
			const auto	inner = width - theme::px ( theme::indent );
			Columns		c;

			c.control = std::ceil ( labelW ) + g;
			c.controlWidth = std::max ( inner - c.control - g - valueW - g - resetW, theme::px ( theme::rowHeight ) * 2.0f );
			c.value = c.control + c.controlWidth + g;
			c.valueWidth = valueW;
			c.reset = c.value + valueW + g;
			return c;
		}

		void separator ( float width )
		{
			const auto	pos = ImGui::GetCursorScreenPos ();
			const auto	y = std::floor ( pos.y ) + 0.5f;

			ImGui::GetWindowDrawList ()->AddLine ( ImVec2 ( pos.x, y ), ImVec2 ( pos.x + width, y ), draw::color ( theme::colors::separator ) );
			ImGui::Dummy ( ImVec2 ( width, theme::px ( 1.0f ) ) );
		}

		// An edit's end: the value saved when it is finished
		void finish ( const Edit& edit, const ReShadeUiBinding& b, void* context, void* frame )
		{
			if ( edit.committed && b.save != nullptr )
				b.save ( context, frame );
		}

		void sliderRow ( const ReShadeUiItem& item, ImVec2 row, const Columns& c, void* frame )
		{
			auto	v = item.value.get ( item.context, frame );
			auto	range = rangeOf ( &item.range );
			if ( item.top != nullptr )
				range.hi = std::max ( item.top ( item.context ), range.lo + range.step );

			ImGui::SetCursorScreenPos ( ImVec2 ( row.x + c.control, row.y ) );
			const auto	edit = slider ( "slider", v, range, c.controlWidth );
			if ( edit.changed )
				item.value.set ( item.context, frame, v );
			finish ( edit, item.value, item.context, frame );

			ImGui::SetCursorScreenPos ( ImVec2 ( row.x + c.value, row.y ) );
			const auto	typed = valueEdit ( "value", v, range, formatOf ( item.format ), c.valueWidth );
			if ( typed.changed )
				item.value.set ( item.context, frame, v );
			finish ( typed, item.value, item.context, frame );

			if ( v == range.def )
				return;

			ImGui::SetCursorScreenPos ( ImVec2 ( row.x + c.reset, row.y ) );
			if ( resetButton ( "reset" ) )
			{
				item.value.set ( item.context, frame, item.range.def );
				finish ( { true, true }, item.value, item.context, frame );
			}
		}

		void indexRow ( const ReShadeUiItem& item, ImVec2 row, const Columns& c, void* frame )
		{
			auto	index = indexOf ( item, item.value.get ( item.context, frame ) );
			const auto	names = std::span<const char* const> ( item.choices, item.choiceCount );
			const auto	def = indexOf ( item, item.value.def );

			ImGui::SetCursorScreenPos ( ImVec2 ( row.x + c.control, row.y ) );
			const auto	edit = asChoiceRow ( item ) ? choice ( "choice", index, names, def, c.controlWidth ) : dropdown ( "dropdown", index, names, def, c.controlWidth );
			if ( edit.changed )
				item.value.set ( item.context, frame, valueOf ( item, index ) );
			finish ( edit, item.value, item.context, frame );
		}

		void fieldRow ( const ReShadeUiItem& item, ImVec2 row, const Columns& c )
		{
			const auto*	now = item.fieldGet != nullptr ? item.fieldGet ( item.context ) : nullptr;
			std::string	text = now != nullptr ? now : "";
			ImGui::SetCursorScreenPos ( ImVec2 ( row.x + c.control, row.y ) );
			if ( textField ( "##field", text, c.controlWidth ).changed && item.fieldSet != nullptr )
				item.fieldSet ( item.context, text.c_str () );
		}

		// The reset arrow after it while off its default
		void colorRow ( const ReShadeUiItem& item, ImVec2 row, const Columns& c )
		{
			auto	color = item.colorGet != nullptr ? ImU32 ( item.colorGet ( item.context ) ) : ImU32 ( item.colorDefault );
			ImGui::SetCursorScreenPos ( ImVec2 ( row.x + c.control, row.y ) );
			if ( const auto edit = colorPicker ( "color", color, item.colorDefault, c.controlWidth ); ( edit.changed || edit.committed ) && item.colorSet != nullptr )
				item.colorSet ( item.context, color, edit.committed ? 1 : 0 );

			if ( color == item.colorDefault )
				return;

			ImGui::SetCursorScreenPos ( ImVec2 ( row.x + c.reset, row.y ) );
			if ( resetButton ( "reset" ) && item.colorSet != nullptr )
				item.colorSet ( item.context, item.colorDefault, 1 );
		}

		void toggleRow ( const ReShadeUiItem& item, const char* tip, ImVec2 row, void* frame )
		{
			auto	on = item.value.get ( item.context, frame ) != 0.0f;

			const auto	key = item.hotkey != nullptr ? item.hotkey ( item.context ) : ReShadeUiHotkey {};
			ImGui::SetCursorScreenPos ( row );
			const auto	edit = toggle ( "toggle", on, labelFor ( item, frame ), { false, hotkeyOf ( &key ), tip, appliesOf ( item.applies ), item.value.def != 0.0f } );
			if ( edit.changed )
				item.value.set ( item.context, frame, on ? 1.0f : 0.0f );
			finish ( edit, item.value, item.context, frame );
		}

		// A switched slider's label: its switch and label as a toggle's
		void switchLabel ( const ReShadeUiItem& item, const char* tip, ImVec2 row, void* frame )
		{
			auto	on = item.onOff.get ( item.context, frame ) != 0.0f;

			ImGui::SetCursorScreenPos ( row );
			const auto	edit = toggle ( "switch", on, labelFor ( item, frame ), { false, {}, tip, appliesOf ( item.applies ), item.onOff.def != 0.0f } );
			if ( edit.changed )
				item.onOff.set ( item.context, frame, on ? 1.0f : 0.0f );
			finish ( edit, item.onOff, item.context, frame );
		}

		void textRow ( const ReShadeUiItem& item, const char* line, ImVec2 row, void* frame )
		{
			const auto	size = theme::px ( theme::sizes::body );
			const auto	tone = has ( item, RESHADE_UI_ITEM_FLAG_HAS_TONE ) && item.tone != nullptr ? item.tone ( item.context, frame ) : theme::colors::muted;
			font::draw ( ImGui::GetWindowDrawList (), ImVec2 ( row.x, font::centredTop ( font::Face::regular, size, row.y, theme::px ( theme::rowHeight ) ) ),
						 font::Face::regular, size, draw::color ( tone ), line );
		}

		// Below 0 unless a reporting job runs
		[[ nodiscard ]] float progressOf ( const ReShadeUiItem& item )
		{
			return item.isBusy != nullptr && item.isBusy ( item.context ) != 0 && item.progress != nullptr ? item.progress ( item.context ) : -1.0f;
		}

		[[ nodiscard ]] bool isBusy ( const ReShadeUiItem& item )
		{
			return item.isBusy != nullptr && item.isBusy ( item.context ) != 0;
		}

		// Buttons in a run, side by side from the row's start, onto the next line where the row is full; the height used
		[[ nodiscard ]] float buttonRow ( const std::vector<const ReShadeUiItem*>& items, size_t first, size_t last, ImVec2 row, float right, void* frame )
		{
			const auto	rowH = theme::px ( theme::rowHeight );
			const auto	lineStep = rowH + theme::px ( theme::rowGap );
			auto		x = row.x;
			auto		y = row.y;

			for ( auto i = first; i < last; ++i )
			{
				const auto&	item = *items[ i ];
				if ( ! shown ( item, frame ) )
					continue;

				const auto	key = item.hotkey != nullptr ? item.hotkey ( item.context ) : ReShadeUiHotkey {};
				const auto	hotkey = hotkeyOf ( &key );
				const auto	w = buttonWidth ( &item, labelFor ( item, frame ), item.doneText, hotkey );
				if ( x > row.x && x + w > right )
				{
					x = row.x;
					y += lineStep;
				}

				if ( item.takeFinished != nullptr && item.takeFinished ( item.context ) != 0 && item.doneText != nullptr )
					flash ( &item );

				ImGui::PushID ( int ( i ) );
				// Running, a reporting job shows its progress at full strength, any other dims
				const auto	progress = progressOf ( item );
				const auto*	reason = reasonOf ( item, frame );
				std::string	storage;

				ImGui::BeginDisabled ( reason != nullptr || ( isBusy ( item ) && progress < 0.0f ) );
				ImGui::SetCursorScreenPos ( ImVec2 ( x, y ) );
				if ( button ( &item, labelFor ( item, frame ), has ( item, RESHADE_UI_ITEM_FLAG_PRIMARY ), item.doneText, hotkey, progress ) && ! isBusy ( item ) )
				{
					if ( item.press != nullptr )
						item.press ( item.context, frame );
					if ( ! has ( item, RESHADE_UI_ITEM_FLAG_HAS_JOB ) && item.doneText != nullptr )
						flash ( &item );
				}

				if ( const auto* tip = tipWith ( item.tooltip, reason, storage ) )
					tooltip ( ImGui::GetItemID (), ImGui::IsItemHovered ( ImGuiHoveredFlags_AllowWhenDisabled ), tip );
				ImGui::EndDisabled ();
				ImGui::PopID ();

				x += w + theme::px ( theme::buttonGap );
			}

			return y - row.y + rowH;
		}

		// A section hung under another is hidden, its own collapsed state kept, while that one is collapsed or switched off
		[[ nodiscard ]] bool hiddenUnder ( const ReShadeUiPage& page, const ReShadeUiSection& section, void* frame )
		{
			if ( section.parent == nullptr )
				return false;

			const ReShadeUiSection*	parent = nullptr;
			for ( uint32_t i = 0; i < page.sectionCount && parent == nullptr; ++i )
				if ( page.sections[ i ].label != nullptr && std::strcmp ( page.sections[ i ].label, section.parent ) == 0 )
					parent = &page.sections[ i ];

			const auto	off = parent != nullptr && parent->toggle.isBound != 0 && parent->toggle.get ( parent->context, frame ) == 0.0f;
			if ( page.note != nullptr )
			{
				static std::map<std::string, bool, std::less<>>	logged;
				if ( const auto [ it, fresh ] = logged.try_emplace ( section.label, off ); fresh || it->second != off )
				{
					it->second = off;
					page.note ( page.context, section.label, section.parent, off ? 1 : 0 );
				}
			}
			return off || page.isCollapsed ( page.context, section.parent ) != 0;
		}

		void drawSection ( const ReShadeUiPage& page, const ReShadeUiSection& section, float width, const Columns& c, void* frame )
		{
			const auto*	sectionReason = reasonOf ( section, frame );
			const auto*	lockReason = section.lockReason != nullptr ? section.lockReason ( section.context, frame ) : nullptr;
			const auto*	title = section.title != nullptr ? section.title ( section.context, frame ) : section.label;
			const auto	key = section.hotkey != nullptr ? section.hotkey ( section.context ) : ReShadeUiHotkey {};
			std::string	storage;

			gap ( theme::separatorAbove );
			separator ( width );
			gap ( theme::headingAbove );

			const auto	head = ImGui::GetCursorScreenPos ();
			auto		on = true;
			auto		arrowFrom = head.x;		// the heading row's part that collapses it: past the switch and its info dot
			if ( section.toggle.isBound != 0 )
			{
				on = section.toggle.get ( section.context, frame ) != 0.0f;
				const auto*	reason = lockReason != nullptr ? lockReason : sectionReason;
				const auto*	tip = tipWith ( section.tooltip, reason, storage );

				ImGui::BeginDisabled ( lockReason != nullptr || sectionReason != nullptr );
				const auto	edit = toggle ( "on", on, title, { true, hotkeyOf ( &key ), tip, Applies::now, section.toggle.def != 0.0f } );
				ImGui::EndDisabled ();
				arrowFrom = ImGui::GetItemRectMax ().x + ( tip != nullptr ? theme::px ( theme::infoGap + theme::infoSize ) : 0.0f );

				if ( edit.changed )
					section.toggle.set ( section.context, frame, on ? 1.0f : 0.0f );
				finish ( edit, section.toggle, section.context, frame );
			}
			else
			{
				ImGui::BeginDisabled ( sectionReason != nullptr );
				heading ( title, hotkeyOf ( &key ) );
				ImGui::EndDisabled ();
			}

			const auto	open = page.isCollapsed ( page.context, section.label ) == 0;
			ImGui::SetCursorScreenPos ( ImVec2 ( arrowFrom, head.y ) );
			if ( collapseArrow ( "collapse", open, std::max ( head.x + width - arrowFrom, theme::px ( theme::rowHeight ) ) ) )
				page.setCollapsed ( page.context, section.label, open ? 1 : 0 );
			ImGui::SetCursorScreenPos ( ImVec2 ( head.x, head.y + theme::px ( theme::rowHeight ) ) );

			gap ( theme::headingBelow );

			// Off, dimmed by what it depends on, or collapsed: the heading alone, its rows not built
			if ( ! on || ! open || sectionReason != nullptr )
				return;

			const auto	rowH = theme::px ( theme::rowHeight );
			const auto	x = ImGui::GetCursorScreenPos ().x + theme::px ( theme::indent );
			const auto	items = rowsOf ( section.items, section.itemCount );
			auto		first = true;
			auto		isAfterHeading = false;

			for ( size_t i = 0; i < items.size (); )
			{
				const auto&	item = *items[ i ];
				auto		next = i + 1;
				const auto*	line = item.kind == RESHADE_UI_ITEM_TEXT && item.line != nullptr ? item.line ( item.context, frame ) : nullptr;

				if ( ! shown ( item, frame ) || ( item.kind == RESHADE_UI_ITEM_TEXT && ( line == nullptr || *line == 0 ) )
					 || ( item.kind == RESHADE_UI_ITEM_SUBHEADING && ! groupShown ( items, i, frame ) ) )
				{
					i = next;
					continue;
				}

				// A choice or drop-down right under its heading gets a little more room above it, a group's heading after
				// other rows more
				gap ( theme::rowGap * 0.5f + ( first && item.kind == RESHADE_UI_ITEM_OPTIONS ? theme::firstRowAbove : 0.0f )
					  + ( ! first && item.kind == RESHADE_UI_ITEM_SUBHEADING ? theme::subheadingAbove : 0.0f )
					  + ( ! first && has ( item, RESHADE_UI_ITEM_FLAG_SET_APART ) && ! isAfterHeading ? theme::rowGap : 0.0f ) );
				first = false;
				isAfterHeading = item.kind == RESHADE_UI_ITEM_SUBHEADING;

				const ImVec2	row ( x, ImGui::GetCursorScreenPos ().y );
				const auto*		reason = reasonOf ( item, frame );
				auto			height = rowH;
				const auto*		tip = tipWith ( item.tooltip, reason, storage, appliesOf ( item.applies ), item.appliesWhen );
				const auto		applies = appliesOf ( item.applies );

				ImGui::PushID ( int ( i ) );
				ImGui::BeginDisabled ( reason != nullptr && item.kind != RESHADE_UI_ITEM_BUTTON );

				switch ( item.kind )
				{
					case RESHADE_UI_ITEM_SLIDER:
						if ( item.onOff.isBound != 0 )
							switchLabel ( item, tip, row, frame );
						else
							label ( row, labelFor ( item, frame ), tip, applies );
						sliderRow ( item, row, c, frame );
						break;
					case RESHADE_UI_ITEM_OPTIONS:	label ( row, labelFor ( item, frame ), tip, applies );	indexRow ( item, row, c, frame );	break;
					case RESHADE_UI_ITEM_TOGGLE:	toggleRow ( item, tip, row, frame );	break;
					case RESHADE_UI_ITEM_TEXT:		textRow ( item, line, row, frame );	break;
					case RESHADE_UI_ITEM_SUBHEADING:	subheading ( row, labelFor ( item, frame ) );	break;
					case RESHADE_UI_ITEM_FIELD:		label ( row, labelFor ( item, frame ), tip, applies );	fieldRow ( item, row, c );	break;
					case RESHADE_UI_ITEM_COLOR:		label ( row, labelFor ( item, frame ), tip, applies );	colorRow ( item, row, c );	break;
					case RESHADE_UI_ITEM_BUTTON:
						while ( next < items.size () && items[ next ]->kind == RESHADE_UI_ITEM_BUTTON )
							++next;
						height = buttonRow ( items, i, next, row, row.x - theme::px ( theme::indent ) + width, frame );
						break;
					case RESHADE_UI_ITEM_REPEAT:	break;
				}

				ImGui::EndDisabled ();
				ImGui::PopID ();

				ImGui::SetCursorScreenPos ( ImVec2 ( row.x - theme::px ( theme::indent ), row.y ) );
				ImGui::Dummy ( ImVec2 ( width, height ) );
				gap ( theme::rowGap * 0.5f );
				i = next;
			}
		}

		void drawSections ( const ReShadeUiPage& page, void* frame, float width )
		{
			ImGui::PushStyleVar ( ImGuiStyleVar_ItemSpacing, ImVec2 ( 0.0f, 0.0f ) );
			ImGui::PushStyleVar ( ImGuiStyleVar_DisabledAlpha, theme::disabledAlpha );

			const auto	c = columns ( page, width, frame );

			for ( uint32_t i = 0; i < page.sectionCount; ++i )
			{
				const auto&	section = page.sections[ i ];
				if ( ! shown ( section, frame ) || hiddenUnder ( page, section, frame ) )
					continue;

				ImGui::PushID ( int ( i ) );
				drawSection ( page, section, width, c, frame );
				ImGui::PopID ();
			}

			ImGui::PopStyleVar ( 2 );
		}

		// Reopened, ImGui clamps the child's scroll to content it has not measured yet: the top. Ours goes back once the content is that
		// tall again, normally the next frame
		struct Scroll
		{
			std::chrono::steady_clock::time_point	lastDrawn;
			float									kept = 0.0f;
			int										restoring = 0;
		};

		void keepScroll ( Scroll& scroll )
		{
			// Wall-clock time: no ImGui frames run while the overlay is closed, so ImGui's own counters stand still
			constexpr int	retryFrames = 30;
			constexpr auto	closedAfter = std::chrono::milliseconds ( 250 );

			const auto	now = std::chrono::steady_clock::now ();
			if ( now - scroll.lastDrawn > closedAfter && scroll.kept > 0.0f )
				scroll.restoring = retryFrames;
			scroll.lastDrawn = now;

			if ( scroll.restoring > 0 )
			{
				if ( ImGui::GetScrollMaxY () >= scroll.kept || --scroll.restoring == 0 )
				{
					ImGui::SetScrollY ( std::min ( scroll.kept, ImGui::GetScrollMaxY () ) );
					scroll.restoring = 0;
				}
				return;
			}

			scroll.kept = ImGui::GetScrollY ();
		}
	}

	void beginFrame ( api::effect_runtime* runtime )
	{
		theme::measureScale ( runtime != nullptr ? runtime->get_hwnd () : nullptr, ImGui::GetIO ().DisplaySize.y );
		font::prepare ( runtime != nullptr ? runtime->get_device () : nullptr );
		tickTextures ();
	}

	void forgetMouseOver ()
	{
		mouseOverOverlay.store ( false, std::memory_order_relaxed );
	}

	bool isMouseOverOverlay ()
	{
		return mouseOverOverlay.load ( std::memory_order_relaxed );
	}

	void drawPage ( api::effect_runtime* runtime, const ReShadeUiPage& page, void* frame )
	{
		static std::map<std::string, Scroll, std::less<>>	scrolls;

		beginFrame ( runtime );
		mouseOverOverlay.store ( ImGui::GetIO ().WantCaptureMouse, std::memory_order_relaxed );

		// Our own background over the tab's whole inner area, so ImGui's style never shows through
		{
			const auto&		style = ImGui::GetStyle ();
			const auto		at = ImGui::GetCursorScreenPos ();
			const auto		avail = ImGui::GetContentRegionAvail ();
			const ImVec2	min ( at.x - style.WindowPadding.x, at.y - style.WindowPadding.y );
			const ImVec2	max ( at.x + avail.x + style.WindowPadding.x, at.y + avail.y + style.WindowPadding.y );
			ImGui::GetWindowDrawList ()->AddRectFilled ( min, max, theme::colors::panel, style.WindowRounding, ImDrawFlags_RoundBottom );
		}

		// A child filling the tab, padded on all sides; it scrolls when the page is taller or wider than the window
		const auto	inset = theme::px ( theme::panelInset );
		ImGui::PushStyleVar ( ImGuiStyleVar_WindowPadding, ImVec2 ( inset, inset ) );
		ImGui::PushStyleColor ( ImGuiCol_ChildBg, IM_COL32 ( 0, 0, 0, 0 ) );
		const auto	open = ImGui::BeginChild ( page.id, ImVec2 ( 0.0f, 0.0f ), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar );
		ImGui::PopStyleColor ();
		ImGui::PopStyleVar ();

		if ( open )
		{
			const auto	width = theme::px ( theme::panelWidth );
			if ( page.header != nullptr )
			{
				ReShadeUiHeader	info {};
				info.lineColors[ 0 ] = theme::colors::text;
				info.lineColors[ 1 ] = info.lineColors[ 2 ] = theme::colors::muted;
				page.header ( page.context, frame, &info );
				header ( info, width );
			}

			drawSections ( page, frame, width );
			keepScroll ( scrolls[ page.id ] );
		}

		ImGui::EndChild ();
	}
}
