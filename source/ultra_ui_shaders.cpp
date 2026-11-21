/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <reshade_ultra_ui.h>

#include "addon_manager.hpp"
#include "runtime.hpp"
#include "runtime_internal.hpp"
#include "ultra_splash.hpp"
#include "ultra_ui_layout.hpp"
#include "ultra_ui_shaders.hpp"
#include "ultra_ui_window.hpp"

//-----------------------------------------------------------------------------

namespace reshade::ultra
{
	constexpr size_t	none = std::numeric_limits<size_t>::max ();

	struct Built;

	// What a row of the page knows of the shader it belongs to: the runtime's own indices, so a reload leaves nothing dangling
	struct Ref
	{
		Built*		built = nullptr;
		runtime*	owner = nullptr;
		size_t		technique = none;
		size_t		effect = none;
		size_t		uniform = none;
		size_t		gate = none;			// the uniform of the category's switch the row is shown while it is on
		uint32_t	component = 0;
		std::string	file;
		std::string	name;
		std::string	pending;				// a definition being typed
		bool		isEditing = false;
	};

	struct Built
	{
		runtime*										owner = nullptr;
		uint32_t										generation = ~0u;
		bool											isDirty = true;
		std::deque<Ref>									refs;
		std::deque<std::string>							texts;
		std::deque<std::vector<ReShadeUiItem>>			lists;
		std::deque<std::vector<const char*>>			choices;
		std::deque<std::vector<float>>					values;
		std::vector<ReShadeUiSection>					sections;
		std::vector<size_t>								sectionTechnique;
		std::map<std::string, bool, std::less<>>		opened;		// collapsed until it is opened
		std::vector<std::pair<size_t, std::pair<size_t, uint32_t>>>	buttons;	// effect, uniform, component: set to 0 again at the next frame
		std::string										id = "Shaders";
		ReShadeUiPage									page {};
	};

	struct ShadersAccess
	{
		[[ nodiscard ]] static uniform& uniformOf ( const Ref& ref )	{ return ref.owner->_effects[ ref.effect ].uniforms[ ref.uniform ]; }
		[[ nodiscard ]] static technique& techniqueOf ( const Ref& ref )	{ return ref.owner->_techniques[ ref.technique ]; }

		static float read ( const Ref& ref, uint32_t component )
		{
			auto&	u = uniformOf ( ref );
			reshadefx::constant	v {};
			const auto	n = u.type.components ();
			switch ( u.type.base )
			{
				case reshadefx::type::t_bool:
					ref.owner->get_uniform_value ( u, v.as_uint, n );
					return v.as_uint[ component ] != 0 ? 1.0f : 0.0f;
				case reshadefx::type::t_int:
				case reshadefx::type::t_uint:
					ref.owner->get_uniform_value ( u, v.as_int, n );
					return float ( v.as_int[ component ] );
				default:
					ref.owner->get_uniform_value ( u, v.as_float, n );
					return v.as_float[ component ];
			}
		}

		static void write ( const Ref& ref, uint32_t component, float value )
		{
			auto&	u = uniformOf ( ref );
			reshadefx::constant	v {};
			const auto	n = u.type.components ();
			switch ( u.type.base )
			{
				case reshadefx::type::t_bool:
					ref.owner->get_uniform_value ( u, v.as_uint, n );
					v.as_uint[ component ] = value != 0.0f ? 1 : 0;
					ref.owner->set_uniform_value ( u, v.as_uint, n );
					break;
				case reshadefx::type::t_int:
				case reshadefx::type::t_uint:
					ref.owner->get_uniform_value ( u, v.as_int, n );
					v.as_int[ component ] = int ( std::lround ( value ) );
					ref.owner->set_uniform_value ( u, v.as_int, n );
					break;
				default:
					ref.owner->get_uniform_value ( u, v.as_float, n );
					v.as_float[ component ] = value;
					ref.owner->set_uniform_value ( u, v.as_float, n );
					break;
			}
		}

		[[ nodiscard ]] static float initial ( const uniform& u, uint32_t component )
		{
			switch ( u.type.base )
			{
				case reshadefx::type::t_bool:	return u.initializer_value.as_uint[ component ] != 0 ? 1.0f : 0.0f;
				case reshadefx::type::t_int:
				case reshadefx::type::t_uint:	return float ( u.initializer_value.as_int[ component ] );
				default:						return u.initializer_value.as_float[ component ];
			}
		}

		//---------------------------------------------------------------------
		// The rows' callbacks

		static float valueGet ( void* c, void* )			{ return read ( *static_cast<Ref*> ( c ), static_cast<Ref*> ( c )->component ); }
		static void valueSet ( void* c, void*, float v )	{ write ( *static_cast<Ref*> ( c ), static_cast<Ref*> ( c )->component, v ); }

		static void valueSave ( void* c, void* )
		{
			const auto&	ref = *static_cast<Ref*> ( c );
			if ( ! uniformOf ( ref ).annotation_as_uint ( "nosave" ) )
				ref.owner->save_effect_settings ();
		}

		static uint32_t colorGet ( void* c )
		{
			const auto&	ref = *static_cast<Ref*> ( c );
			const auto	byte = [ & ] ( uint32_t k ) { return uint32_t ( std::clamp ( read ( ref, k ), 0.0f, 1.0f ) * 255.0f + 0.5f ); };
			return byte ( 0 ) | ( byte ( 1 ) << 8 ) | ( byte ( 2 ) << 16 ) | 0xFF000000u;
		}

		static void colorSet ( void* c, uint32_t color, uint32_t isDone )
		{
			const auto&	ref = *static_cast<Ref*> ( c );
			for ( uint32_t k = 0; k < 3; ++k )
				write ( ref, k, float ( ( color >> ( 8 * k ) ) & 0xFF ) / 255.0f );
			if ( isDone != 0 )
				valueSave ( c, nullptr );
		}

		static uint32_t gateVisible ( void* c, void* )
		{
			const auto&	ref = *static_cast<Ref*> ( c );
			Ref			gate = ref;
			gate.uniform = ref.gate;
			return read ( gate, 0 ) != 0.0f ? 1 : 0;
		}

		static const char* noedit ( void*, void* )			{ return ""; }

		static void pressButton ( void* c, void* )
		{
			const auto&	ref = *static_cast<Ref*> ( c );
			write ( ref, ref.component, 1.0f );
			ref.built->buttons.push_back ( { ref.effect, { ref.uniform, ref.component } } );
		}

		static void resetAll ( void* c, void* )
		{
			auto&	ref = *static_cast<Ref*> ( c );
			auto&	effect = ref.owner->_effects[ ref.effect ];
			for ( auto& u : effect.uniforms )
				if ( u.special == special_uniform::none && ! u.annotation_as_uint ( "noreset" ) )
					ref.owner->reset_uniform_value ( u );

			auto	isReload = false;
			if ( const auto own = ref.owner->_preset_preprocessor_definitions.find ( ref.file ); own != ref.owner->_preset_preprocessor_definitions.end () && ! own->second.empty () )
			{
				own->second.clear ();
				isReload = true;
			}
			ref.owner->save_effect_settings ();
			if ( isReload )
			{
				(void) ref.owner->reload_effect ( ref.effect );
				ref.built->isDirty = true;
			}
		}

		static void switchOn ( void* c, void* )
		{
			auto&	runtime = *static_cast<Ref*> ( c )->owner;
			if ( ! invoke_addon_event<addon_event::reshade_set_effects_state> ( &runtime, true ) )
				runtime._effects_enabled = true;
		}

		static void reload ( void* c, void* )
		{
			auto&	runtime = *static_cast<Ref*> ( c )->owner;
			runtime.load_config ();
			runtime.reload_effects ();
		}

		static uint32_t effectsOff ( void* c, void* )		{ return static_cast<Ref*> ( c )->owner->_effects_enabled ? 0 : 1; }

		// Techniques
		static float techniqueGet ( void* c, void* )		{ return techniqueOf ( *static_cast<Ref*> ( c ) ).enabled ? 1.0f : 0.0f; }

		static void techniqueSet ( void* c, void*, float on )
		{
			const auto&	ref = *static_cast<Ref*> ( c );
			auto&		tech = techniqueOf ( ref );
			if ( tech.annotation_as_int ( "enabled" ) )
				return;
			if ( on != 0.0f )
				ref.owner->enable_technique ( tech );
			else
				ref.owner->disable_technique ( tech );
		}

		static void techniqueSave ( void* c, void* )		{ static_cast<Ref*> ( c )->owner->save_effect_settings (); }

		static const char* techniqueLock ( void* c, void* )
		{
			const auto&	tech = techniqueOf ( *static_cast<Ref*> ( c ) );
			return tech.annotation_as_int ( "enabled" ) ? "This shader is always on" : tech.annotation_as_uint ( "noedit" ) ? "" : nullptr;
		}

		static const char* techniqueTitle ( void* c, void* )	{ return static_cast<Ref*> ( c )->name.c_str (); }

		// Definitions
		static const char* definitionGet ( void* c )
		{
			static std::string	text;
			auto&	ref = *static_cast<Ref*> ( c );
			text = ref.isEditing ? ref.pending : ref.owner->find_preprocessor_definition ( ref.file, ref.name, 0b111 ).value_or ( std::string () );
			return text.c_str ();
		}

		static void definitionSet ( void* c, const char* text )
		{
			auto&	ref = *static_cast<Ref*> ( c );
			ref.pending = text;
			ref.isEditing = true;
		}

		[[ nodiscard ]] static bool isOwn ( const Ref& ref )
		{
			const auto	own = ref.owner->_preset_preprocessor_definitions.find ( ref.file );
			return own != ref.owner->_preset_preprocessor_definitions.end ()
				   && std::ranges::any_of ( own->second, [ &ref ] ( const auto& d ) { return d.first == ref.name; } );
		}

		static uint32_t definitionIsSet ( void* c )			{ return isOwn ( *static_cast<Ref*> ( c ) ) ? 1 : 0; }

		// The effect with its own definition set, loaded again; one that no longer compiles goes back to the effect's own
		static void definitionCommit ( void* c )
		{
			auto&	ref = *static_cast<Ref*> ( c );
			if ( ! ref.isEditing )
				return;
			ref.isEditing = false;

			auto&	own = ref.owner->_preset_preprocessor_definitions[ ref.file ];
			const auto	it = std::ranges::find_if ( own, [ &ref ] ( const auto& d ) { return d.first == ref.name; } );
			if ( ref.pending.empty () )
			{
				if ( it == own.end () )
					return;
				own.erase ( it );
			}
			else if ( it != own.end () )
				it->second = ref.pending;
			else
				own.emplace_back ( ref.name, ref.pending );

			ref.owner->save_effect_settings ();
			const auto	reloadSucceeded = ref.owner->_last_reload_successful.load ();
			if ( ! ref.owner->reload_effect ( ref.effect ) && ! ref.pending.empty () )
			{
				std::erase_if ( own, [ &ref ] ( const auto& d ) { return d.first == ref.name; } );
				if ( ref.owner->reload_effect ( ref.effect ) )
				{
					ref.owner->_last_reload_successful = reloadSucceeded;
					ref.owner->_wants_effect_settings_save = true;
					::ultra::ui::window::notice ( "The shader failed to compile after this change, so the value went back to the default.", 0, 5.0 );
				}
			}
			ref.built->isDirty = true;
		}

		// Sections
		static uint32_t isCollapsed ( void* context, const char* label )
		{
			const auto&	opened = static_cast<Built*> ( context )->opened;
			const auto	it = opened.find ( label );
			return it != opened.end () && it->second ? 0 : 1;
		}

		static void setCollapsed ( void* context, const char* label, uint32_t collapsed )
		{
			static_cast<Built*> ( context )->opened[ label ] = collapsed == 0;
		}

		// Two shaders change places: the one dragged moves to where the other is, the ones between each a step
		static void moveSection ( void* context, uint32_t from, uint32_t to )
		{
			auto&	b = *static_cast<Built*> ( context );
			if ( from >= b.sectionTechnique.size () || to >= b.sectionTechnique.size () || b.sectionTechnique[ from ] == none || b.sectionTechnique[ to ] == none )
				return;

			auto&	order = b.owner->_technique_sorting;
			const auto	source = std::ranges::find ( order, b.sectionTechnique[ from ] );
			const auto	target = std::ranges::find ( order, b.sectionTechnique[ to ] );
			if ( source == order.end () || target == order.end () || source == target )
				return;

			auto	indices = order;
			auto	i = size_t ( source - order.begin () );
			const auto	j = size_t ( target - order.begin () );
			for ( ; i < j; ++i )
				std::swap ( indices[ i ], indices[ i + 1 ] );
			for ( ; i > j; --i )
				std::swap ( indices[ i - 1 ], indices[ i ] );
			b.owner->reorder_techniques ( std::move ( indices ) );
			b.owner->save_effect_settings ();
			b.isDirty = true;
		}

		//---------------------------------------------------------------------
		// Building the page

		[[ nodiscard ]] static const char* keep ( Built& b, std::string text )
		{
			return b.texts.emplace_back ( std::move ( text ) ).c_str ();
		}

		[[ nodiscard ]] static Ref& newRef ( Built& b, size_t effect = none, size_t uniform = none, uint32_t component = 0 )
		{
			auto&	ref = b.refs.emplace_back ();
			ref.built = &b;
			ref.owner = b.owner;
			ref.effect = effect;
			ref.uniform = uniform;
			ref.component = component;
			return ref;
		}

		[[ nodiscard ]] static ReShadeUiBinding binding ( float def, float ( *get ) ( void*, void* ), void ( *set ) ( void*, void*, float ), void ( *save ) ( void*, void* ) )
		{
			return { 1, def, get, set, save };
		}

		[[ nodiscard ]] static std::vector<std::string_view> itemsOf ( std::string_view list )
		{
			std::vector<std::string_view>	out;
			for ( size_t at = 0; at < list.size (); )
			{
				const auto	end = list.find ( '\0', at );
				out.push_back ( list.substr ( at, end == std::string_view::npos ? std::string_view::npos : end - at ) );
				if ( end == std::string_view::npos )
					break;
				at = end + 1;
			}
			return out;
		}

		// The rows of one uniform: a row per component, or the kind's own
		static void addUniform ( Built& b, std::vector<ReShadeUiItem>& items, size_t effectIndex, size_t uniformIndex, size_t gate )
		{
			auto&			effect = b.owner->_effects[ effectIndex ];
			auto&			u = effect.uniforms[ uniformIndex ];
			const auto		type = std::string ( u.annotation_as_string ( "ui_type" ) );
			const auto		n = uint32_t ( u.type.components () );
			const auto		label = std::string ( u.annotation_as_string ( "ui_label" ).empty () ? std::string_view ( u.name ) : u.annotation_as_string ( "ui_label" ) );
			const auto		tooltip = std::string ( u.annotation_as_string ( "ui_tooltip" ) );
			const auto		units = std::string ( u.annotation_as_string ( "ui_units" ) );
			const auto		isNoEdit = u.annotation_as_uint ( "noedit" ) != 0;
			const auto		isNoReset = u.annotation_as_uint ( "noreset" ) != 0;
			const auto		isFloat = u.type.base == reshadefx::type::t_float;
			const auto		isBool = u.type.base == reshadefx::type::t_bool;

			if ( const auto text = u.annotation_as_string ( "ui_text" ); ! text.empty () )
			{
				ReShadeUiItem	t {};
				t.kind = RESHADE_UI_ITEM_TEXT;
				t.flags = RESHADE_UI_ITEM_FLAG_WRAP;
				auto&	textRef = newRef ( b, effectIndex, uniformIndex );
				textRef.name = std::string ( text );
				textRef.gate = gate;
				t.context = &textRef;
				t.line = []( void* c, void* ) { return static_cast<Ref*> ( c )->name.c_str (); };
				t.isVisible = gate != none ? &gateVisible : nullptr;
				items.push_back ( t );
			}

			const auto	rowsBase = [ & ] ( uint32_t component, std::string name )
			{
				ReShadeUiItem	i {};
				auto&			ref = newRef ( b, effectIndex, uniformIndex, component );
				ref.gate = gate;
				i.label = keep ( b, std::move ( name ) );
				i.tooltip = tooltip.empty () ? nullptr : keep ( b, tooltip );
				i.context = &ref;
				i.flags = isNoReset ? RESHADE_UI_ITEM_FLAG_NO_RESET : 0u;
				i.value = binding ( initial ( u, component ), &valueGet, &valueSet, u.annotation_as_uint ( "nosave" ) ? nullptr : &valueSave );
				i.disabledReason = isNoEdit ? &noedit : nullptr;
				i.isVisible = gate != none ? &gateVisible : nullptr;
				return i;
			};

			const auto	componentName = [ & ] ( uint32_t k )
			{
				if ( u.type.is_matrix () )
					return label + " [" + std::to_string ( k / u.type.cols ) + ", " + std::to_string ( k % u.type.cols ) + "]";
				static constexpr const char*	axes[] = { "x", "y", "z", "w" };
				return n > 1 ? label + " " + axes[ std::min ( k, 3u ) ] : label;
			};

			// A float is shown with the digits its step needs
			const auto	step = u.annotation_as_float ( "ui_step", 0, isFloat ? 0.001f : 1.0f );
			auto		decimals = 0;
			if ( isFloat )
				for ( auto x = 1.0f; x * step < 1.0f && decimals < 9; x *= 10.0f )
					++decimals;

			// ui_min and ui_max, both present, make a range
			const auto	lowest = u.annotation_as_float ( "ui_min", 0, std::numeric_limits<float>::quiet_NaN () );
			const auto	highest = u.annotation_as_float ( "ui_max", 0, std::numeric_limits<float>::quiet_NaN () );
			const auto	hasRange = ! std::isnan ( lowest ) && ! std::isnan ( highest );
			const auto	isSlider = type == "slider" || ( type == "drag" && hasRange );

			const auto	format = [ & ] ( ReShadeUiItem& i )
			{
				i.format = { 1.0f, decimals, keep ( b, units ), 0, "", nullptr };
				i.range.step = step;
				i.range.def = i.value.def;
			};

			if ( isBool )
			{
				for ( uint32_t k = 0; k < n; ++k )
				{
					auto	i = rowsBase ( k, componentName ( k ) );
					if ( type == "button" )
					{
						i.kind = RESHADE_UI_ITEM_BUTTON;
						i.flags |= RESHADE_UI_ITEM_FLAG_PRIMARY;
						i.press = &pressButton;
					}
					else
						i.kind = RESHADE_UI_ITEM_TOGGLE;
					items.push_back ( i );
				}
				return;
			}

			if ( type == "list" || type == "combo" || type == "radio" )
			{
				const auto	names = itemsOf ( u.annotation_as_string ( "ui_items" ) );
				auto&		list = b.choices.emplace_back ();
				for ( const auto& name : names )
					list.push_back ( keep ( b, std::string ( name ) ) );
				auto	i = rowsBase ( 0, label );
				i.kind = RESHADE_UI_ITEM_OPTIONS;
				i.choices = list.data ();
				i.choiceCount = uint32_t ( list.size () );
				i.flags |= type == "radio" ? RESHADE_UI_ITEM_FLAG_ROW : 0u;
				items.push_back ( i );
				return;
			}

			if ( isFloat && type == "color" && ( u.type.rows == 3 || u.type.rows == 4 ) )
			{
				auto	i = rowsBase ( 0, label );
				i.kind = RESHADE_UI_ITEM_COLOR;
				i.colorGet = &colorGet;
				i.colorSet = &colorSet;
				{
					const auto	byte = [ & ] ( uint32_t k ) { return uint32_t ( std::clamp ( initial ( u, k ), 0.0f, 1.0f ) * 255.0f + 0.5f ); };
					i.colorDefault = byte ( 0 ) | ( byte ( 1 ) << 8 ) | ( byte ( 2 ) << 16 ) | 0xFF000000u;
				}
				items.push_back ( i );
				if ( u.type.rows == 4 )
				{
					auto	a = rowsBase ( 3, label + " alpha" );
					a.kind = RESHADE_UI_ITEM_SLIDER;
					a.tooltip = nullptr;
					a.range = { 0.0f, 1.0f, 0.01f, a.value.def, 0.0f, 0 };
					a.format = { 100.0f, 0, "%", 0, "", nullptr };
					items.push_back ( a );
				}
				return;
			}

			if ( isFloat && type == "color" && u.type.rows == 1 )
			{
				auto	i = rowsBase ( 0, label );
				i.kind = RESHADE_UI_ITEM_SLIDER;
				i.range = { 0.0f, 1.0f, 0.01f, i.value.def, 0.0f, 0 };
				i.format = { 100.0f, 0, "%", 0, "", nullptr };
				items.push_back ( i );
				return;
			}

			for ( uint32_t k = 0; k < n; ++k )
			{
				auto	i = rowsBase ( k, componentName ( k ) );
				if ( isSlider || ( type == "slider" ) )
				{
					i.kind = RESHADE_UI_ITEM_SLIDER;
					i.range.lo = hasRange ? lowest : 0.0f;
					i.range.hi = hasRange ? highest : 1.0f;
				}
				else
					i.kind = RESHADE_UI_ITEM_NUMBER;
				format ( i );
				items.push_back ( i );
			}
		}

		static void build ( Built& b )
		{
			auto&	r = *b.owner;
			b.refs.clear ();
			b.texts.clear ();
			b.lists.clear ();
			b.choices.clear ();
			b.values.clear ();
			b.sections.clear ();
			b.sectionTechnique.clear ();
			b.isDirty = false;
			b.generation = r._effects_generation.load ();

			const auto	add = [ & ] ( ReShadeUiSection s, std::vector<ReShadeUiItem> items, size_t technique )
			{
				auto&	list = b.lists.emplace_back ( std::move ( items ) );
				s.items = list.data ();
				s.itemCount = uint32_t ( list.size () );
				b.sections.push_back ( s );
				b.sectionTechnique.push_back ( technique );
			};

			// Files that did not compile: the first line of what the compiler said, all of it in the tooltip
			std::vector<ReShadeUiItem>	failed;
			for ( const auto& e : r._effects )
			{
				if ( e.compiled || e.is_route )
					continue;
				ReShadeUiItem	t {};
				t.kind = RESHADE_UI_ITEM_TEXT;
				t.flags = RESHADE_UI_ITEM_FLAG_WRAP | RESHADE_UI_ITEM_FLAG_HAS_TONE;
				t.label = keep ( b, e.source_file.filename ().u8string () );
				t.tooltip = keep ( b, e.errors );
				auto&	ref = newRef ( b );
				ref.file = e.source_file.filename ().u8string () + ": " + e.errors.substr ( 0, e.errors.find ( '\n' ) );
				t.context = &ref;
				t.line = []( void* c, void* ) { return static_cast<Ref*> ( c )->file.c_str (); };
				t.tone = []( void*, void* ) { return ::ultra::ui::theme::colors::danger; };
				failed.push_back ( t );
			}
			if ( ! failed.empty () )
				add ( { keep ( b, "Shader files" ) }, std::move ( failed ), none );

			// All shaders off: one row, one button
			{
				ReShadeUiSection	s {};
				s.label = "Effects are disabled";
				s.context = &newRef ( b );
				s.isVisible = &effectsOff;
				ReShadeUiItem	button {};
				button.kind = RESHADE_UI_ITEM_BUTTON;
				button.flags = RESHADE_UI_ITEM_FLAG_PRIMARY;
				button.label = "Switch on";
				button.context = s.context;
				button.press = &switchOn;
				add ( s, { button }, none );
			}

			{
				ReShadeUiSection	s {};
				s.label = "Shaders";
				ReShadeUiItem		button {};
				button.kind = RESHADE_UI_ITEM_BUTTON;
				button.label = "Reload shaders";
				button.tooltip = "Reads all shaders from their files again";
				button.context = &newRef ( b );
				button.press = &reload;
				add ( s, { button }, none );
			}

			std::set<size_t>	listed;
			for ( const auto index : r._technique_sorting )
			{
				const auto&	tech = r._techniques[ index ];
				const auto&	effect = r._effects[ tech.effect_index ];
				if ( tech.is_route || tech.annotation_as_int ( "hidden" ) != 0 || ! effect.compiled )
					continue;

				auto&		ref = newRef ( b );
				ref.technique = index;
				ref.effect = tech.effect_index;
				ref.file = effect.source_file.filename ().u8string ();
				const auto	label = tech.annotation_as_string ( "ui_label" );
				ref.name = std::string ( label.empty () ? std::string_view ( tech.name ) : label );

				ReShadeUiSection	s {};
				s.label = keep ( b, ref.file + "/" + tech.name );
				s.subtitle = keep ( b, ref.file );
				if ( const auto tip = tech.annotation_as_string ( "ui_tooltip" ); ! tip.empty () )
					s.tooltip = keep ( b, std::string ( tip ) );
				s.context = &ref;
				s.toggle = binding ( 0.0f, &techniqueGet, &techniqueSet, &techniqueSave );
				s.lockReason = &techniqueLock;
				s.title = &techniqueTitle;
				s.isDraggable = 1;

				// The file's values sit under its first shader, in the order the file declares them
				std::vector<ReShadeUiItem>	items;
				if ( listed.insert ( tech.effect_index ).second )
				{
					ReShadeUiItem	all {};
					all.kind = RESHADE_UI_ITEM_BUTTON;
					all.label = "Reset all";
					all.context = &newRef ( b, tech.effect_index );
					static_cast<Ref*> ( all.context )->file = ref.file;
					all.press = &resetAll;
					bool hasValues = false;

					std::string_view	category;
					size_t				gate = none;
					std::vector<ReShadeUiItem>	rows;
					for ( size_t u = 0; u < effect.uniforms.size (); ++u )
					{
						const auto&	variable = effect.uniforms[ u ];
						if ( variable.annotation_as_int ( "hidden" ) || variable.special != special_uniform::none )
							continue;

						if ( const auto c = variable.annotation_as_string ( "ui_category" ); c != category )
						{
							category = c;
							gate = none;
							if ( ! c.empty () )
							{
								ReShadeUiItem	heading {};
								heading.kind = RESHADE_UI_ITEM_SUBHEADING;
								heading.label = keep ( b, std::string ( c ) );
								rows.push_back ( heading );
							}
						}
						addUniform ( b, rows, tech.effect_index, u, gate );
						hasValues = true;
						if ( variable.annotation_as_uint ( "ui_category_toggle" ) != 0 )
							gate = u;
					}
					if ( hasValues )
					{
						items.push_back ( all );
						items.insert ( items.end (), rows.begin (), rows.end () );
					}

					for ( const auto& definition : effect.definitions )
					{
						ReShadeUiItem	field {};
						auto&			d = newRef ( b, tech.effect_index );
						d.file = ref.file;
						d.name = definition.first;
						field.kind = RESHADE_UI_ITEM_FIELD;
						field.label = keep ( b, definition.first );
						field.tooltip = definition.second.empty () ? nullptr : keep ( b, "Default: " + definition.second );
						field.context = &d;
						field.fieldGet = &definitionGet;
						field.fieldSet = &definitionSet;
						field.fieldCommit = &definitionCommit;
						field.fieldIsSet = &definitionIsSet;
						items.push_back ( field );
					}
				}

				add ( s, std::move ( items ), index );
			}

			b.page = {};
			b.page.id = b.id.c_str ();
			b.page.sections = b.sections.data ();
			b.page.sectionCount = uint32_t ( b.sections.size () );
			b.page.context = &b;
			b.page.isCollapsed = &isCollapsed;
			b.page.setCollapsed = &setCollapsed;
			b.page.moveSection = &moveSection;
		}

		static std::map<runtime*, std::unique_ptr<Built>>&	builts ()
		{
			static std::map<runtime*, std::unique_ptr<Built>>	all;
			return all;
		}

		static void draw ( runtime& r )
		{
			if ( r._reload_remaining_effects != std::numeric_limits<size_t>::max () )
			{
				drawWindowProgressRing ( compileProgress ( r._effects.size (), r._reload_remaining_effects.load () ) );
				return;
			}

			auto&	slot = builts ()[ &r ];
			if ( slot == nullptr )
			{
				slot = std::make_unique<Built> ();
				slot->owner = &r;
			}
			auto&	b = *slot;

			// A button's value goes back to 0 at the next frame
			for ( const auto& [ effect, rest ] : b.buttons )
				if ( effect < r._effects.size () && rest.first < r._effects[ effect ].uniforms.size () )
				{
					Ref	ref;
					ref.owner = &r;
					ref.effect = effect;
					ref.uniform = rest.first;
					write ( ref, rest.second, 0.0f );
				}
			b.buttons.clear ();

			if ( b.isDirty || b.generation != r._effects_generation.load () )
				build ( b );

			::ultra::ui::drawPage ( &r, b.page, &b );
		}
	};

	void drawShaders ( runtime& r )
	{
		ShadersAccess::draw ( r );
	}

	void forgetShaders ( runtime* r )
	{
		ShadersAccess::builts ().erase ( r );
	}
}
