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

#include <imgui_internal.h>

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
			return kind == RESHADE_UI_ITEM_SLIDER || kind == RESHADE_UI_ITEM_OPTIONS || kind == RESHADE_UI_ITEM_FIELD || kind == RESHADE_UI_ITEM_COLOR || kind == RESHADE_UI_ITEM_NUMBER;
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

		[[ nodiscard ]] bool isHeading ( const ReShadeUiItem& item )
		{
			return item.kind == RESHADE_UI_ITEM_SUBHEADING || item.kind == RESHADE_UI_ITEM_SECTION;
		}

		// A subheading shows while a row of its group does
		[[ nodiscard ]] bool groupShown ( const std::vector<const ReShadeUiItem*>& items, size_t at, void* frame )
		{
			for ( auto i = at + 1; i < items.size () && ! isHeading ( *items[ i ] ); ++i )
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

		// The add-on's text for the value, null where the item has none
		[[ nodiscard ]] const char* valueTextOf ( const ReShadeUiItem& item, float v, void* frame )
		{
			return item.valueText != nullptr ? item.valueText ( item.context, frame, v ) : nullptr;
		}

		[[ nodiscard ]] float valueWidth ( const ReShadeUiItem& item, void* frame )
		{
			char		text[ 32 ];
			float		w = 0.0f;
			const auto	f = formatOf ( item.format );

			for ( const auto v : { item.range.lo, item.range.hi } )
			{
				const auto*	own = valueTextOf ( item, v, frame );
				if ( own == nullptr )
					format ( text, sizeof ( text ), v, f );
				w = std::max ( w, font::width ( font::Face::bold, theme::px ( theme::text::body ), own != nullptr ? own : text, true ) );
			}

			return w;
		}

		[[ nodiscard ]] float indentOf ( size_t depth )
		{
			return theme::px ( theme::spacing::indent ) * float ( depth );
		}

		struct Widest
		{
			float	label = 0.0f;
			float	value = 0.0f;
		};

		// The label column's width takes in the indent of its level, so the controls of every level line up
		void widen ( Widest& widest, const std::vector<const ReShadeUiItem*>& rows, size_t depth, void* frame )
		{
			for ( const auto* row : rows )
			{
				const auto&	item = *row;
				if ( ! shown ( item, frame ) )
					continue;
				if ( item.kind == RESHADE_UI_ITEM_SECTION )
					widen ( widest, rowsOf ( item.children, item.childCount ), depth + 1, frame );
				if ( labelled ( item.kind ) )
					widest.label = std::max ( widest.label, indentOf ( depth )
										+ labelWidth ( labelFor ( item, frame ), item.tooltip != nullptr || item.disabledReason != nullptr || item.applies != RESHADE_UI_APPLIES_NOW, appliesOf ( item.applies ) )
										+ ( item.onOff.isBound != 0 ? theme::px ( theme::parts::switchWidth + theme::spacing::m ) : 0.0f ) );
				if ( item.kind == RESHADE_UI_ITEM_SLIDER )
					widest.value = std::max ( widest.value, valueWidth ( item, frame ) );
			}
		}

		// Over the rows shown now, so a game's hidden section does not widen the others
		[[ nodiscard ]] Columns columns ( const ReShadeUiPage& page, float width, void* frame )
		{
			Widest	widest;

			for ( uint32_t s = 0; s < page.sectionCount; ++s )
			{
				const auto&	section = page.sections[ s ];
				if ( shown ( section, frame ) )
					widen ( widest, rowsOf ( section.items, section.itemCount ), 0, frame );
			}

			const auto	labelW = widest.label;
			const auto	valueW = widest.value;

			const auto	g = theme::px ( theme::spacing::l );
			const auto	resetW = theme::px ( theme::spacing::xl );
			const auto	inner = width - theme::px ( theme::spacing::xl );
			Columns		c;

			c.control = std::ceil ( labelW ) + g;
			c.controlWidth = std::max ( inner - c.control - g - valueW - g - resetW, theme::px ( theme::heights::row ) * 2.0f );
			c.value = c.control + c.controlWidth + g;
			c.valueWidth = valueW;
			c.reset = c.value + valueW + g;
			return c;
		}

		void separator ( float width )
		{
			const auto	pos = ImGui::GetCursorScreenPos ();
			const auto	y = std::floor ( pos.y ) + 0.5f;

			ImGui::GetWindowDrawList ()->AddLine ( ImVec2 ( pos.x, y ), ImVec2 ( pos.x + width, y ), draw::color ( theme::shade ( theme::shades::raised ) ) );
			ImGui::Dummy ( ImVec2 ( width, theme::px ( theme::lines::line ) ) );
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
			const auto	typed = valueEdit ( "value", v, range, formatOf ( item.format ), valueTextOf ( item, v, frame ), c.valueWidth );
			if ( typed.changed )
				item.value.set ( item.context, frame, v );
			finish ( typed, item.value, item.context, frame );

			if ( v == range.def || has ( item, RESHADE_UI_ITEM_FLAG_NO_RESET ) )
				return;

			ImGui::SetCursorScreenPos ( ImVec2 ( row.x + c.reset, row.y ) );
			if ( resetButton ( "reset" ) )
			{
				item.value.set ( item.context, frame, item.range.def );
				finish ( { true, true }, item.value, item.context, frame );
			}
		}

		// A value typed in, with the reset arrow after it while off its default
		void numberRow ( const ReShadeUiItem& item, ImVec2 row, const Columns& c, void* frame )
		{
			auto		v = item.value.get ( item.context, frame );
			const auto	range = rangeOf ( &item.range );

			ImGui::SetCursorScreenPos ( ImVec2 ( row.x + c.control, row.y ) );
			const auto	typed = valueEdit ( "value", v, range, formatOf ( item.format ), valueTextOf ( item, v, frame ), c.controlWidth );
			if ( typed.changed )
				item.value.set ( item.context, frame, v );
			finish ( typed, item.value, item.context, frame );

			if ( v == range.def || has ( item, RESHADE_UI_ITEM_FLAG_NO_RESET ) )
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
			const auto	edit = textField ( "##field", text, c.controlWidth );
			if ( edit.changed && item.fieldSet != nullptr )
				item.fieldSet ( item.context, text.c_str () );
			if ( edit.committed && item.fieldCommit != nullptr )
				item.fieldCommit ( item.context );

			if ( item.fieldIsSet != nullptr && item.fieldIsSet ( item.context ) != 0 )
			{
				ImGui::SetCursorScreenPos ( ImVec2 ( row.x + c.reset, row.y ) );
				if ( resetButton ( "reset" ) && item.fieldSet != nullptr )
				{
					item.fieldSet ( item.context, "" );
					if ( item.fieldCommit != nullptr )
						item.fieldCommit ( item.context );
				}
			}
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

		// The text in lines no wider than width, broken at spaces
		[[ nodiscard ]] std::vector<std::string> wrapped ( const char* text, float width, float size )
		{
			std::vector<std::string>	lines;
			std::string					line;
			const std::string			all ( text );
			for ( size_t at = 0; at <= all.size (); )
			{
				auto	end = all.find_first_of ( " \n", at );
				if ( end == std::string::npos )
					end = all.size ();
				const auto	word = all.substr ( at, end - at );
				const auto	next = line.empty () ? word : line + " " + word;
				if ( ! line.empty () && font::width ( font::Face::regular, size, next ) > width )
				{
					lines.push_back ( line );
					line = word;
				}
				else
					line = next;
				if ( end < all.size () && all[ end ] == '\n' )
				{
					lines.push_back ( line );
					line.clear ();
				}
				at = end + 1;
			}
			if ( ! line.empty () || lines.empty () )
				lines.push_back ( line );
			return lines;
		}

		// The row's height: one line, or the lines of a wrapped text
		float textRow ( const ReShadeUiItem& item, const char* line, ImVec2 row, float width, void* frame )
		{
			const auto	size = theme::px ( theme::text::body );
			const auto	rowH = theme::px ( theme::heights::row );
			const auto	tone = has ( item, RESHADE_UI_ITEM_FLAG_HAS_TONE ) && item.tone != nullptr ? item.tone ( item.context, frame ) : theme::shade ( theme::shades::muted );
			auto*		dl = ImGui::GetWindowDrawList ();
			if ( ! has ( item, RESHADE_UI_ITEM_FLAG_WRAP ) )
			{
				font::drawMarked ( dl, ImVec2 ( row.x, font::centredTop ( font::Face::regular, size, row.y, rowH ) ), font::Face::regular, size, draw::color ( tone ), line );
				return rowH;
			}

			auto	y = row.y;
			for ( const auto& part : wrapped ( line, width - theme::px ( theme::spacing::xl ), size ) )
			{
				font::draw ( dl, ImVec2 ( row.x, font::centredTop ( font::Face::regular, size, y, rowH ) ), font::Face::regular, size, draw::color ( tone ), part );
				y += rowH;
			}

			// The tooltip of a wrapped line sits on all of it
			if ( item.tooltip != nullptr && ImGui::IsMouseHoveringRect ( ImVec2 ( row.x, row.y ), ImVec2 ( row.x + width, y ) ) )
				tooltip ( ImGui::GetID ( "wrapped" ), true, item.tooltip );
			return y - row.y;
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
		[[ nodiscard ]] float buttonRow ( const std::vector<const ReShadeUiItem*>& items, size_t first, size_t last, ImVec2 row, float right, const char* dimmed, void* frame )
		{
			const auto	rowH = theme::px ( theme::heights::row );
			const auto	lineStep = rowH + theme::px ( theme::spacing::xs );
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
				const auto*	reason = dimmed != nullptr ? dimmed : reasonOf ( item, frame );
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

				x += w + theme::px ( theme::spacing::m );
			}

			return y - row.y + rowH;
		}

		// Why a section's rows are dimmed: what the section around it says, else its own reason, else that its switch is off
		[[ nodiscard ]] const char* dimReason ( const char* inherited, const char* own, bool isOn, const char* title, std::string& storage )
		{
			if ( inherited != nullptr )
				return inherited;
			if ( own != nullptr || isOn )
				return own;

			storage.assign ( title ).append ( " off" );
			return storage.c_str ();
		}

		// The heading being dragged to another place in the page, and whether it moved: a drag is no click on its arrow
		struct Drag
		{
			int		index = -1;
			bool	isMoved = false;
		} drag;

		void drawItems ( const std::vector<const ReShadeUiItem*>& items, size_t depth, float x, float width, const Columns& c, const char* dimmed, void* frame );

		void drawSubsection ( const ReShadeUiItem& item, size_t depth, float x, float width, const Columns& c, const char* dimmed, void* frame )
		{
			const auto*	own = reasonOf ( item, frame );
			const auto*	lock = item.lockReason != nullptr ? item.lockReason ( item.context, frame ) : nullptr;
			const auto*	title = labelFor ( item, frame );
			const auto	key = item.hotkey != nullptr ? item.hotkey ( item.context ) : ReShadeUiHotkey {};
			const auto	on = item.onOff.isBound == 0 || item.onOff.get ( item.context, frame ) != 0.0f;
			const auto*	reason = dimmed != nullptr ? dimmed : lock != nullptr ? lock : own;
			std::string	storage;
			std::string	offText;
			const auto*	tip = tipWith ( item.tooltip, reason, storage );
			auto		isOn = on;

			const auto	top = ImGui::GetCursorScreenPos ().y;

			ImGui::SetCursorScreenPos ( ImVec2 ( x + indentOf ( depth ), top ) );
			ImGui::BeginDisabled ( reason != nullptr );
			const auto	edit = subsection ( "heading", isOn, item.onOff.isBound != 0, title, { false, hotkeyOf ( &key ), tip, Applies::now, item.onOff.def != 0.0f } );
			ImGui::EndDisabled ();

			if ( edit.changed )
				item.onOff.set ( item.context, frame, isOn ? 1.0f : 0.0f );
			finish ( edit, item.onOff, item.context, frame );

			ImGui::SetCursorScreenPos ( ImVec2 ( x - theme::px ( theme::spacing::xl ), top ) );
			ImGui::Dummy ( ImVec2 ( width, theme::px ( theme::heights::row ) ) );
			gap ( theme::spacing::xs * 0.5f );

			drawItems ( rowsOf ( item.children, item.childCount ), depth + 1, x, width, c, dimReason ( dimmed, own, on, title, offText ), frame );
		}

		// The rows of a section or a nested one, depth levels in: their controls in the page's columns from x, their labels indented. dimmed:
		// why they all are, null while none is
		void drawItems ( const std::vector<const ReShadeUiItem*>& items, size_t depth, float x, float width, const Columns& c, const char* dimmed, void* frame )
		{
			const auto	rowH = theme::px ( theme::heights::row );
			const auto	indent = indentOf ( depth );
			auto		first = true;
			auto		isAfterHeading = false;
			std::string	storage;

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
				gap ( theme::spacing::xs * 0.5f + ( first && item.kind == RESHADE_UI_ITEM_OPTIONS ? theme::spacing::xs : 0.0f )
					  + ( ! first && isHeading ( item ) ? theme::spacing::m : 0.0f )
					  + ( ! first && has ( item, RESHADE_UI_ITEM_FLAG_SET_APART ) && ! isAfterHeading ? theme::spacing::xs : 0.0f ) );
				first = false;
				isAfterHeading = isHeading ( item );

				if ( item.kind == RESHADE_UI_ITEM_SECTION )
				{
					ImGui::PushID ( int ( i ) );
					drawSubsection ( item, depth, x, width, c, dimmed, frame );
					ImGui::PopID ();
					i = next;
					continue;
				}

				const ImVec2	row ( x, ImGui::GetCursorScreenPos ().y );
				const ImVec2	at ( x + indent, row.y );
				const auto*		reason = dimmed != nullptr ? dimmed : reasonOf ( item, frame );
				auto			height = rowH;
				const auto*		tip = tipWith ( item.tooltip, reason, storage, appliesOf ( item.applies ), item.appliesWhen );
				const auto		applies = appliesOf ( item.applies );

				ImGui::PushID ( int ( i ) );
				ImGui::BeginDisabled ( reason != nullptr && item.kind != RESHADE_UI_ITEM_BUTTON );

				switch ( item.kind )
				{
					case RESHADE_UI_ITEM_SLIDER:
						if ( item.onOff.isBound != 0 )
							switchLabel ( item, tip, at, frame );
						else
							label ( at, labelFor ( item, frame ), tip, applies );
						sliderRow ( item, row, c, frame );
						break;
					case RESHADE_UI_ITEM_OPTIONS:
						if ( asChoiceRow ( item ) )
							height = theme::px ( theme::heights::large );
						label ( ImVec2 ( at.x, row.y + ( height - rowH ) * 0.5f ), labelFor ( item, frame ), tip, applies );
						indexRow ( item, row, c, frame );
						break;
					case RESHADE_UI_ITEM_TOGGLE:	toggleRow ( item, tip, at, frame );	break;
					case RESHADE_UI_ITEM_TEXT:		height = textRow ( item, line, at, width - indent, frame );	break;
					case RESHADE_UI_ITEM_NUMBER:	label ( at, labelFor ( item, frame ), tip, applies );	numberRow ( item, row, c, frame );	break;
					case RESHADE_UI_ITEM_SUBHEADING:	subheading ( at, labelFor ( item, frame ) );	break;
					case RESHADE_UI_ITEM_FIELD:		label ( at, labelFor ( item, frame ), tip, applies );	fieldRow ( item, row, c );	break;
					case RESHADE_UI_ITEM_COLOR:		label ( at, labelFor ( item, frame ), tip, applies );	colorRow ( item, row, c );	break;
					case RESHADE_UI_ITEM_BUTTON:
						while ( next < items.size () && items[ next ]->kind == RESHADE_UI_ITEM_BUTTON )
							++next;
						height = buttonRow ( items, i, next, at, row.x - theme::px ( theme::spacing::xl ) + width, dimmed, frame );
						break;
					case RESHADE_UI_ITEM_REPEAT:
					case RESHADE_UI_ITEM_SECTION:	break;
				}

				ImGui::EndDisabled ();
				ImGui::PopID ();

				ImGui::SetCursorScreenPos ( ImVec2 ( row.x - theme::px ( theme::spacing::xl ), row.y ) );
				ImGui::Dummy ( ImVec2 ( width, height ) );
				gap ( theme::spacing::xs * 0.5f );
				i = next;
			}
		}

		void drawSection ( const ReShadeUiPage& page, const ReShadeUiSection& section, uint32_t index, float width, const Columns& c, void* frame )
		{
			const auto*	sectionReason = reasonOf ( section, frame );
			const auto*	lockReason = section.lockReason != nullptr ? section.lockReason ( section.context, frame ) : nullptr;
			const auto*	title = section.title != nullptr ? section.title ( section.context, frame ) : section.label;
			const auto	key = section.hotkey != nullptr ? section.hotkey ( section.context ) : ReShadeUiHotkey {};
			std::string	storage;

			gap ( theme::spacing::m );
			separator ( width );
			gap ( theme::spacing::m );

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
				arrowFrom = ImGui::GetItemRectMax ().x + ( tip != nullptr ? theme::px ( theme::spacing::xs + theme::icons::icon ) : 0.0f );

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

			// The file in the muted colour after the name, which the heading's dragging and collapsing take in too
			if ( section.subtitle != nullptr && *section.subtitle != 0 && section.toggle.isBound != 0 )
			{
				const auto	size = theme::px ( theme::text::body );
				const auto	at = arrowFrom + theme::px ( theme::spacing::m );
				font::draw ( ImGui::GetWindowDrawList (), ImVec2 ( at, font::centredTop ( font::Face::regular, size, head.y, theme::px ( theme::heights::row ) ) ), font::Face::regular, size,
							 draw::color ( theme::shade ( theme::shades::muted ) ), section.subtitle );
				arrowFrom = at;
			}

			// Over another draggable heading while one is dragged: they change places, the dragged one follows
			if ( section.isDraggable != 0 && page.moveSection != nullptr && drag.index >= 0 && drag.index != int ( index )
				 && ImGui::IsMouseHoveringRect ( head, ImVec2 ( head.x + width, head.y + theme::px ( theme::heights::row ) ) ) )
			{
				page.moveSection ( page.context, uint32_t ( drag.index ), index );
				drag.index = int ( index );
			}

			const auto	open = page.isCollapsed ( page.context, section.label ) == 0;
			ImGui::SetCursorScreenPos ( ImVec2 ( arrowFrom, head.y ) );
			const auto	isClicked = collapseArrow ( "collapse", open, std::max ( head.x + width - arrowFrom, theme::px ( theme::heights::row ) ) );
			if ( section.isDraggable != 0 && page.moveSection != nullptr && ImGui::IsItemActive () && ImGui::IsMouseDragging ( ImGuiMouseButton_Left, theme::px ( theme::parts::dragThreshold ) ) )
			{
				drag.index = int ( index );
				drag.isMoved = true;
			}
			if ( isClicked && ! drag.isMoved )
				page.setCollapsed ( page.context, section.label, open ? 1 : 0 );
			ImGui::SetCursorScreenPos ( ImVec2 ( head.x, head.y + theme::px ( theme::heights::row ) ) );

			gap ( theme::spacing::xs );

			if ( ! open )
				return;

			std::string	offText;
			drawItems ( rowsOf ( section.items, section.itemCount ), 0, ImGui::GetCursorScreenPos ().x + theme::px ( theme::spacing::xl ), width, c,
						dimReason ( nullptr, sectionReason, on, title, offText ), frame );
		}

		void drawSections ( const ReShadeUiPage& page, void* frame, float width )
		{
			ImGui::PushStyleVar ( ImGuiStyleVar_ItemSpacing, ImVec2 ( 0.0f, 0.0f ) );
			ImGui::PushStyleVar ( ImGuiStyleVar_DisabledAlpha, theme::effects::disabledAlpha );

			const auto	c = columns ( page, width, frame );

			for ( uint32_t i = 0; i < page.sectionCount; ++i )
			{
				const auto&	section = page.sections[ i ];
				if ( ! shown ( section, frame ) )
					continue;

				ImGui::PushID ( int ( i ) );
				drawSection ( page, section, i, width, c, frame );
				ImGui::PopID ();
			}

			ImGui::PopStyleVar ( 2 );

			if ( ! ImGui::IsMouseDown ( ImGuiMouseButton_Left ) )
				drag = {};
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

	namespace
	{
		// The page's scrollbar thumb, ImGui's own made clear: scrollThumbInset off the track's ends, so the bar's shadow never covers it
		void drawThumb ()
		{
			auto* const	window = ImGui::GetCurrentWindow ();
			if ( ! window->ScrollbarY )
				return;

			const auto&	style = ImGui::GetStyle ();
			const auto	track = ImGui::GetWindowScrollbarRect ( window, ImGuiAxis_Y );
			auto		bb = track;
			bb.Expand ( -std::trunc ( std::min ( style.ScrollbarPadding, std::min ( track.GetWidth (), track.GetHeight () ) * 0.5f ) ) );
			bb.Min.y += theme::px ( theme::spacing::m );
			bb.Max.y -= theme::px ( theme::spacing::m );

			const auto	length = bb.GetHeight ();
			if ( length < 1.0f )
				return;

			const auto	visible = window->InnerRect.GetHeight ();
			const auto	total = std::max ( { window->ContentSize.y + window->WindowPadding.y * 2.0f, visible, 1.0f } );
			const auto	thumb = std::clamp ( std::trunc ( length * visible / total ), std::min ( length, style.GrabMinSize ), length );
			const auto	share = std::clamp ( window->Scroll.y / std::max ( 1.0f, total - visible ), 0.0f, 1.0f );
			const auto	top = bb.Min.y + ( length - thumb ) * share;

			const auto	id = ImGui::GetWindowScrollbarID ( window, ImGuiAxis_Y );
			const auto	color = ImGui::GetActiveID () == id || ImGui::GetHoveredID () == id ? theme::shade ( theme::shades::strong ) : theme::shade ( theme::shades::hover );
			window->DrawList->PushClipRect ( track.Min, track.Max, false );
			window->DrawList->AddRectFilled ( ImVec2 ( bb.Min.x, top ), ImVec2 ( bb.Max.x, top + thumb ), draw::color ( color ), style.ScrollbarRounding );
			window->DrawList->PopClipRect ();
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
			ImGui::GetWindowDrawList ()->AddRectFilled ( min, max, theme::colors::background, style.WindowRounding, ImDrawFlags_RoundBottom );
		}

		// A child filling the tab, padded on all sides; it scrolls when the page is taller or wider than the window
		const auto	inset = theme::px ( theme::spacing::m );
		ImGui::PushStyleVar ( ImGuiStyleVar_WindowPadding, ImVec2 ( inset, inset ) );
		ImGui::PushStyleColor ( ImGuiCol_ChildBg, IM_COL32 ( 0, 0, 0, 0 ) );
		ImGui::PushStyleColor ( ImGuiCol_ScrollbarGrab, IM_COL32 ( 0, 0, 0, 0 ) );
		ImGui::PushStyleColor ( ImGuiCol_ScrollbarGrabHovered, IM_COL32 ( 0, 0, 0, 0 ) );
		ImGui::PushStyleColor ( ImGuiCol_ScrollbarGrabActive, IM_COL32 ( 0, 0, 0, 0 ) );
		const auto	open = ImGui::BeginChild ( page.id, ImVec2 ( 0.0f, 0.0f ), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar );
		ImGui::PopStyleColor ( 4 );
		ImGui::PopStyleVar ();

		if ( open )
		{
			const auto	width = theme::px ( theme::parts::panelWidth );
			if ( page.header != nullptr )
			{
				ReShadeUiHeader	info {};
				info.lineColors[ 0 ] = theme::shade ( theme::shades::body );
				info.lineColors[ 1 ] = info.lineColors[ 2 ] = theme::shade ( theme::shades::muted );
				page.header ( page.context, frame, &info );
				header ( info, width );
			}

			drawSections ( page, frame, width );
			keepScroll ( scrolls[ page.id ] );
			drawThumb ();
		}

		ImGui::EndChild ();
	}
}
