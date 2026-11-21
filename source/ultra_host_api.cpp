/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <reshade_ultra.hpp>
#include <reshade_ultra_host_names.h>

#include "dll_log.hpp"
#include "ini_file.hpp"
#include "runtime.hpp"
#include "runtime_internal.hpp"
#include "ultra_home.hpp"
#include "ultra_host_api.hpp"

//-----------------------------------------------------------------------------

// The form is reshade_ultra_api.h's: a record per name, its cells laid out here (cell 1 on, the runtime first), answered by
// "record.start" and "record.slot". Every call is checked for its cells and its runtime before anything is read or written

#if RESHADE_GUI

// Never defined: cellOf of a name its record does not have does not compile
void notInRecord ();

namespace
{
	namespace names = reshade::host::name;
	namespace cells = reshade::host::cell;

	struct Field
	{
		const char*	name;
		uint32_t	type;
	};

	struct Answer
	{
		const char*				name;
		UgCall					call;
		std::span<const Field>	fields;
	};

	template <size_t N>
	[[ nodiscard ]] consteval uint32_t cellOf ( const Field ( &fields )[ N ], std::string_view name )
	{
		for ( size_t i = 0; i < N; ++i )
			if ( std::string_view ( fields[ i ].name ) == name )
				return uint32_t ( i + 1 );
		notInRecord ();
		return 0;
	}

	constexpr Field	overlayStateFields[] {
		{ cells::runtime, ugPtr }, { cells::overlay_state::open, ugU32 }, { cells::overlay_state::x, ugF32 }, { cells::overlay_state::y, ugF32 },
		{ cells::overlay_state::width, ugF32 }, { cells::overlay_state::height, ugF32 }, { cells::overlay_state::display_width, ugF32 },
		{ cells::overlay_state::display_height, ugF32 }, { cells::overlay_state::tab, ugText } };

	constexpr Field	overlayPlaceFields[] {
		{ cells::runtime, ugPtr }, { cells::overlay_place::x, ugF32 }, { cells::overlay_place::y, ugF32 }, { cells::overlay_place::width, ugF32 },
		{ cells::overlay_place::height, ugF32 }, { cells::overlay_place::done, ugU32 } };

	constexpr Field	overlayTabFields[] { { cells::runtime, ugPtr }, { cells::overlay_tab::name, ugText } };

	constexpr Field	overlayWantedFields[] { { cells::runtime, ugPtr }, { cells::overlay_wanted::on, ugU32 } };

	constexpr Field	screenshotStateFields[] {
		{ cells::runtime, ugPtr }, { cells::screenshot_state::saving, ugU32 }, { cells::screenshot_state::file, ugText }, { cells::screenshot_state::failure, ugU32 },
		{ cells::screenshot_state::path, ugText }, { cells::screenshot_state::failed_ms, ugU32 } };

	constexpr Field	settingsStateFields[] { { cells::runtime, ugPtr }, { cells::settings_state::save_failed, ugU32 }, { cells::settings_state::path, ugText } };

	constexpr Field	effectsStateFields[] {
		{ cells::runtime, ugPtr }, { cells::effects_state::is_loading, ugU32 }, { cells::effects_state::total, ugU32 }, { cells::effects_state::remaining, ugU32 },
		{ cells::effects_state::generation, ugU32 }, { cells::effects_state::has_user_shaders, ugU32 }, { cells::effects_state::has_failures, ugU32 } };

	constexpr Field	effectsTechniqueFields[] { { cells::runtime, ugPtr }, { cells::effects_technique::technique, ugU64 }, { cells::effects_technique::is_route, ugU32 } };

	constexpr Field	effectsUniformFields[] {
		{ cells::runtime, ugPtr }, { cells::effects_uniform::variable, ugU64 }, { cells::effects_uniform::buffer, ugPtr }, { cells::effects_uniform::capacity, ugU32 },
		{ cells::effects_uniform::is_special, ugU32 }, { cells::effects_uniform::is_default, ugU32 }, { cells::effects_uniform::count, ugU32 } };

	constexpr Field	effectsFailedFields[] {
		{ cells::runtime, ugPtr }, { cells::effects_failed::index, ugU32 }, { cells::effects_failed::count, ugU32 }, { cells::effects_failed::file, ugText },
		{ cells::effects_failed::errors, ugText } };

	constexpr Field	effectsSaveFields[] { { cells::runtime, ugPtr } };

	constexpr Field	effectsDefinitionFields[] {
		{ cells::runtime, ugPtr }, { cells::effects_definition::file, ugText }, { cells::effects_definition::index, ugU32 }, { cells::effects_definition::count, ugU32 },
		{ cells::effects_definition::name, ugText }, { cells::effects_definition::hint, ugText }, { cells::effects_definition::value, ugText },
		{ cells::effects_definition::is_own, ugU32 } };

	constexpr Field	effectsDefinitionSetFields[] {
		{ cells::runtime, ugPtr }, { cells::effects_definition_set::file, ugText }, { cells::effects_definition_set::name, ugText },
		{ cells::effects_definition_set::value, ugText }, { cells::effects_definition_set::reverted, ugU32 } };

	std::mutex					toldLock;
	std::set<std::string>		told;

	// A cause once, to the main log
	void tell ( const std::string& cause, const std::string& what )
	{
		{
			std::scoped_lock	held ( toldLock );
			if ( ! told.insert ( cause ).second )
				return;
		}
		reshade::log::message ( reshade::log::level::warning, "Host API: %s", what.c_str () );
	}

	std::mutex												runtimesLock;
	std::vector<std::pair<const void*, reshade::runtime*>>	runtimes;

	[[ nodiscard ]] reshade::runtime* runtimeOf ( const UgCell& cell )
	{
		std::scoped_lock	held ( runtimesLock );
		for ( const auto& [ key, runtime ] : runtimes )
			if ( key == cell.ptr )
				return runtime;
		return nullptr;
	}

	void put ( UgCell& c, uint32_t v )			{ c.u64 = 0; c.u32 = v; }
	void put ( UgCell& c, float v )				{ c.u64 = 0; c.f32 = v; }
	void putText ( UgCell& c, const char* v )	{ c.u64 = 0; c.text = v; }

	[[ nodiscard ]] std::string_view textOf ( const UgCell& c )
	{
		return c.text != nullptr ? std::string_view ( c.text ) : std::string_view ();
	}

	[[ nodiscard ]] std::string orNull ( const char* s )
	{
		return s != nullptr ? s : "(null)";
	}

	[[ nodiscard ]] int refused ( const char* name, uint32_t count, uint32_t needed )
	{
		tell ( std::string ( "short " ) + name + ' ' + std::to_string ( count ),
			   std::string ( name ) + " called with " + std::to_string ( count ) + " cells, its record has " + std::to_string ( needed ) + ": refused" );
		return 0;
	}

	[[ nodiscard ]] int noRuntime ( const char* name )
	{
		tell ( std::string ( "runtime " ) + name, std::string ( name ) + " called without an effect runtime ReShade knows: refused" );
		return 0;
	}
}

namespace reshade::ultra
{
	// A friend of the runtime: the overlay, screenshot and effect state the answers read and write
	struct HostAccess
	{
		static constexpr float	minimumSide = 1.0f;

		static void place ( runtime& r, float x, float y, float width, float height )
		{
			const auto	displayWidth = float ( r._width );
			const auto	displayHeight = float ( r._height );
			width = std::clamp ( width, minimumSide, displayWidth );
			height = std::clamp ( height, minimumSide, displayHeight );
			r._overlay_rect[ 0 ] = std::clamp ( x, 0.0f, displayWidth - width );
			r._overlay_rect[ 1 ] = std::clamp ( y, 0.0f, displayHeight - height );
			r._overlay_rect[ 2 ] = width;
			r._overlay_rect[ 3 ] = height;
			r._overlay_rect_for[ 0 ] = displayWidth;
			r._overlay_rect_for[ 1 ] = displayHeight;
		}

		// The window from the saved fractions, again whenever the back buffer's size changed
		static void placeForDisplay ( runtime& r )
		{
			if ( r._overlay_rect_for[ 0 ] == float ( r._width ) && r._overlay_rect_for[ 1 ] == float ( r._height ) )
				return;
			place ( r, r._overlay_position[ 0 ] * float ( r._width ), r._overlay_position[ 1 ] * float ( r._height ),
					r._overlay_size[ 0 ] * float ( r._width ), r._overlay_size[ 1 ] * float ( r._height ) );
		}

		static void saveRect ( runtime& r )
		{
			const auto	fraction = [] ( float value ) { return std::round ( std::clamp ( value, 0.0f, 1.0f ) * 1e6f ) / 1e6f; };
			const auto	width = float ( r._width );
			const auto	height = float ( r._height );
			const float	position[ 2 ] { fraction ( r._overlay_rect[ 0 ] / width ), fraction ( r._overlay_rect[ 1 ] / height ) };
			const float	size[ 2 ] { fraction ( r._overlay_rect[ 2 ] / width ), fraction ( r._overlay_rect[ 3 ] / height ) };

			// The pixel rounding of a placement is no move
			const auto	isMoved = [] ( float now, float saved, float display ) { return std::abs ( now - saved ) * display > 1.5f; };
			if ( ! isMoved ( position[ 0 ], r._overlay_position[ 0 ], width ) && ! isMoved ( position[ 1 ], r._overlay_position[ 1 ], height )
				 && ! isMoved ( size[ 0 ], r._overlay_size[ 0 ], width ) && ! isMoved ( size[ 1 ], r._overlay_size[ 1 ], height ) )
				return;

			std::copy_n ( position, 2, r._overlay_position );
			std::copy_n ( size, 2, r._overlay_size );
			r.save_overlay_window ( global_config () );
		}

		[[ nodiscard ]] static effect* effectOf ( runtime& r, std::string_view file )
		{
			const auto	it = std::ranges::find_if ( r._effects, [ file ] ( const effect& e ) { return e.source_file.filename ().u8string () == file; } );
			return it != r._effects.end () ? &*it : nullptr;
		}

		template <typename T>
		[[ nodiscard ]] static bool isElement ( const std::vector<T>& list, const void* p )
		{
			const auto*	first = list.data ();
			return p >= static_cast<const void*> ( first ) && p < static_cast<const void*> ( first + list.size () )
				   && ( static_cast<const char*> ( p ) - reinterpret_cast<const char*> ( first ) ) % sizeof ( T ) == 0;
		}

		[[ nodiscard ]] static const technique* techniqueOf ( runtime& r, uint64_t handle )
		{
			const auto*	p = reinterpret_cast<const void*> ( uintptr_t ( handle ) );
			return r.is_loading () || ! isElement ( r._techniques, p ) ? nullptr : static_cast<const technique*> ( p );
		}

		[[ nodiscard ]] static const uniform* uniformOf ( runtime& r, uint64_t handle )
		{
			if ( r.is_loading () )
				return nullptr;
			const auto*	p = reinterpret_cast<const void*> ( uintptr_t ( handle ) );
			for ( const auto& e : r._effects )
				if ( isElement ( e.uniforms, p ) )
					return static_cast<const uniform*> ( p );
			return nullptr;
		}

		// The editor's rule: a float is the default within three quarters of its step
		[[ nodiscard ]] static bool isDefault ( runtime& r, const uniform& variable )
		{
			reshadefx::constant	value;
			auto				is = true;
			switch ( variable.type.base )
			{
				case reshadefx::type::t_bool:
					r.get_uniform_value ( variable, value.as_uint, variable.type.components () );
					for ( size_t i = 0; is && i < variable.type.components (); i++ )
						is = ( value.as_uint[ i ] != 0 ) == ( variable.initializer_value.as_uint[ i ] != 0 );
					break;
				case reshadefx::type::t_int:
				case reshadefx::type::t_uint:
					r.get_uniform_value ( variable, value.as_int, variable.type.components () );
					is = std::memcmp ( value.as_int, variable.initializer_value.as_int, variable.type.components () * sizeof ( int ) ) == 0;
					break;
				case reshadefx::type::t_float:
				{
					const auto	threshold = variable.annotation_as_float ( "ui_step", 0, 0.001f ) * 0.75f + FLT_EPSILON;
					r.get_uniform_value ( variable, value.as_float, variable.type.components () );
					for ( size_t i = 0; is && i < variable.type.components (); i++ )
						is = std::abs ( value.as_float[ i ] - variable.initializer_value.as_float[ i ] ) < threshold;
					break;
				}
				default:
					break;
			}
			return is;
		}

		// The index'th effect that did not compile, null for none; count: how many did not
		[[ nodiscard ]] static const effect* failedOf ( runtime& r, size_t index, uint32_t& count )
		{
			const effect*	found = nullptr;
			count = 0;
			if ( r.is_loading () )
				return nullptr;
			for ( const auto& e : r._effects )
				if ( ! e.compiled && ! e.is_route )
				{
					if ( count == index )
						found = &e;
					++count;
				}
			return found;
		}

		static void setDefinition ( runtime& r, std::string_view file, std::string_view name, std::string_view value, bool& reverted )
		{
			auto*	e = effectOf ( r, file );
			if ( e == nullptr || r.is_loading () )
				return;

			auto&	own = r._preset_preprocessor_definitions[ std::string ( file ) ];
			auto	isReload = false;
			auto	modified = std::string ();
			if ( name.empty () )
			{
				isReload = ! own.empty ();
				own.clear ();
			}
			else
			{
				const auto	it = std::ranges::find_if ( own, [ name ] ( const auto& d ) { return d.first == name; } );
				if ( value.empty () )
				{
					if ( it != own.end () )
					{
						own.erase ( it );
						isReload = true;
					}
				}
				else
				{
					isReload = true;
					modified = std::string ( name );
					if ( it != own.end () )
						it->second = std::string ( value );
					else
						own.emplace_back ( std::string ( name ), std::string ( value ) );
				}
			}
			if ( ! isReload )
				return;

			r.save_effect_settings ();
			const auto	index = size_t ( e - r._effects.data () );
			const auto	reloadSucceeded = r._last_reload_successful.load ();

			// A definition that makes the effect not compile goes back to the effect's own, and the effect loads again
			if ( ! r.reload_effect ( index ) && ! modified.empty () )
			{
				std::erase_if ( own, [ &modified ] ( const auto& d ) { return d.first == modified; } );
				if ( r.reload_effect ( index ) )
				{
					r._last_reload_successful = reloadSucceeded;
					reverted = true;

					// Once the reloaded effect has its settings again, its techniques are saved as they are
					r._wants_effect_settings_save = true;
				}
			}
		}

		//---------------------------------------------------------------------
		// The answers

		static int overlayState ( runtime& r, UgCell* c )
		{
			constexpr auto	at = [] ( std::string_view name ) consteval { return cellOf ( overlayStateFields, name ); };
			placeForDisplay ( r );
			put ( c[ at ( cells::overlay_state::open ) ], r._show_overlay ? 1u : 0u );
			put ( c[ at ( cells::overlay_state::x ) ], r._overlay_rect[ 0 ] );
			put ( c[ at ( cells::overlay_state::y ) ], r._overlay_rect[ 1 ] );
			put ( c[ at ( cells::overlay_state::width ) ], r._overlay_rect[ 2 ] );
			put ( c[ at ( cells::overlay_state::height ) ], r._overlay_rect[ 3 ] );
			put ( c[ at ( cells::overlay_state::display_width ) ], float ( r._width ) );
			put ( c[ at ( cells::overlay_state::display_height ) ], float ( r._height ) );
			putText ( c[ at ( cells::overlay_state::tab ) ], r._overlay_tab.c_str () );
			return 1;
		}

		static int overlayPlace ( runtime& r, UgCell* c )
		{
			constexpr auto	at = [] ( std::string_view name ) consteval { return cellOf ( overlayPlaceFields, name ); };
			place ( r, c[ at ( cells::overlay_place::x ) ].f32, c[ at ( cells::overlay_place::y ) ].f32, c[ at ( cells::overlay_place::width ) ].f32,
					c[ at ( cells::overlay_place::height ) ].f32 );
			if ( c[ at ( cells::overlay_place::done ) ].u32 != 0 )
				saveRect ( r );
			return 1;
		}

		static int overlayTab ( runtime& r, UgCell* c )
		{
			constexpr auto	at = [] ( std::string_view name ) consteval { return cellOf ( overlayTabFields, name ); };
			const auto		tab = std::string ( textOf ( c[ at ( cells::overlay_tab::name ) ] ) );
			if ( tab == r._overlay_tab )
				return 1;
			r._overlay_tab = tab;
			r.save_overlay_window ( global_config () );
			return 1;
		}

		static int overlayWanted ( runtime& r, UgCell* c )
		{
			constexpr auto	at = [] ( std::string_view name ) consteval { return cellOf ( overlayWantedFields, name ); };
			r._overlay_wanted = c[ at ( cells::overlay_wanted::on ) ].u32 != 0;
			return 1;
		}

		static int screenshotState ( runtime& r, UgCell* c )
		{
			constexpr auto					at = [] ( std::string_view name ) consteval { return cellOf ( screenshotStateFields, name ); };
			static thread_local std::string	file, path;
			file = r._last_screenshot_file.filename ().u8string ();
			path = r._screenshot_path.u8string ();

			const auto	isFailed = ! r._last_screenshot_save_successful;
			const auto	ago = std::chrono::duration_cast<std::chrono::milliseconds> ( r._last_present_time - r._last_screenshot_time ).count ();
			put ( c[ at ( cells::screenshot_state::saving ) ], uint32_t ( std::max ( r._screenshots_saving.load (), 0 ) ) );
			putText ( c[ at ( cells::screenshot_state::file ) ], file.c_str () );
			put ( c[ at ( cells::screenshot_state::failure ) ], ! isFailed ? 0u : r._screenshot_directory_creation_successful ? 1u : 2u );
			putText ( c[ at ( cells::screenshot_state::path ) ], path.c_str () );
			put ( c[ at ( cells::screenshot_state::failed_ms ) ], uint32_t ( std::clamp<long long> ( ago, 0, UINT32_MAX ) ) );
			return 1;
		}

		static int settingsState ( runtime& r, UgCell* c )
		{
			constexpr auto					at = [] ( std::string_view name ) consteval { return cellOf ( settingsStateFields, name ); };
			static thread_local std::string	path;
			path = userSettingsPath ().u8string ();
			put ( c[ at ( cells::settings_state::save_failed ) ], r._settings_save_successful ? 0u : 1u );
			putText ( c[ at ( cells::settings_state::path ) ], path.c_str () );
			return 1;
		}

		static int effectsState ( runtime& r, UgCell* c )
		{
			constexpr auto	at = [] ( std::string_view name ) consteval { return cellOf ( effectsStateFields, name ); };
			const auto		isLoading = r.is_loading ();
			const auto		remaining = r._reload_remaining_effects.load ();
			auto			failures = 0u;
			if ( ! isLoading )
				(void) failedOf ( r, SIZE_MAX, failures );

			put ( c[ at ( cells::effects_state::is_loading ) ], isLoading ? 1u : 0u );
			put ( c[ at ( cells::effects_state::total ) ], uint32_t ( r._effects.size () ) );
			put ( c[ at ( cells::effects_state::remaining ) ], remaining == std::numeric_limits<size_t>::max () ? 0u : uint32_t ( remaining ) );
			put ( c[ at ( cells::effects_state::generation ) ], r._effects_generation.load () );
			put ( c[ at ( cells::effects_state::has_user_shaders ) ], r._has_user_shaders ? 1u : 0u );
			put ( c[ at ( cells::effects_state::has_failures ) ], failures != 0 ? 1u : 0u );
			return 1;
		}

		static int effectsTechnique ( runtime& r, UgCell* c )
		{
			constexpr auto	at = [] ( std::string_view name ) consteval { return cellOf ( effectsTechniqueFields, name ); };
			const auto*		technique = techniqueOf ( r, c[ at ( cells::effects_technique::technique ) ].u64 );
			if ( technique == nullptr )
			{
				tell ( names::effects_technique, "effects.technique called with a technique that is none of this runtime's: refused" );
				return 0;
			}
			put ( c[ at ( cells::effects_technique::is_route ) ], technique->is_route ? 1u : 0u );
			return 1;
		}

		static int effectsUniform ( runtime& r, UgCell* c )
		{
			constexpr auto	at = [] ( std::string_view name ) consteval { return cellOf ( effectsUniformFields, name ); };
			const auto*		variable = uniformOf ( r, c[ at ( cells::effects_uniform::variable ) ].u64 );
			if ( variable == nullptr )
			{
				tell ( names::effects_uniform, "effects.uniform called with a variable that is none of this runtime's: refused" );
				return 0;
			}

			auto	count = 0u;
			auto*	buffer = static_cast<uint32_t*> ( const_cast<void*> ( c[ at ( cells::effects_uniform::buffer ) ].ptr ) );
			if ( buffer != nullptr )
			{
				count = std::min<uint32_t> ( c[ at ( cells::effects_uniform::capacity ) ].u32, uint32_t ( variable->type.components () ) );
				std::memcpy ( buffer, variable->initializer_value.as_uint, count * sizeof ( uint32_t ) );
			}
			put ( c[ at ( cells::effects_uniform::is_special ) ], variable->special != special_uniform::none ? 1u : 0u );
			put ( c[ at ( cells::effects_uniform::is_default ) ], isDefault ( r, *variable ) ? 1u : 0u );
			put ( c[ at ( cells::effects_uniform::count ) ], count );
			return 1;
		}

		static int effectsFailed ( runtime& r, UgCell* c )
		{
			constexpr auto					at = [] ( std::string_view name ) consteval { return cellOf ( effectsFailedFields, name ); };
			static thread_local std::string	file;
			auto							count = 0u;
			const auto*						e = failedOf ( r, c[ at ( cells::effects_failed::index ) ].u32, count );
			put ( c[ at ( cells::effects_failed::count ) ], count );
			if ( e == nullptr )
			{
				putText ( c[ at ( cells::effects_failed::file ) ], nullptr );
				putText ( c[ at ( cells::effects_failed::errors ) ], nullptr );
				return 1;
			}
			file = e->source_file.filename ().u8string ();
			putText ( c[ at ( cells::effects_failed::file ) ], file.c_str () );
			putText ( c[ at ( cells::effects_failed::errors ) ], e->errors.c_str () );
			return 1;
		}

		static int effectsSave ( runtime& r, UgCell* )
		{
			r.save_effect_settings ();
			return 1;
		}

		static int effectsDefinition ( runtime& r, UgCell* c )
		{
			constexpr auto					at = [] ( std::string_view name ) consteval { return cellOf ( effectsDefinitionFields, name ); };
			static thread_local std::string	value;
			const auto*						e = r.is_loading () ? nullptr : effectOf ( r, textOf ( c[ at ( cells::effects_definition::file ) ] ) );
			const auto						index = c[ at ( cells::effects_definition::index ) ].u32;
			put ( c[ at ( cells::effects_definition::count ) ], e != nullptr ? uint32_t ( e->definitions.size () ) : 0u );
			putText ( c[ at ( cells::effects_definition::name ) ], nullptr );
			putText ( c[ at ( cells::effects_definition::hint ) ], nullptr );
			putText ( c[ at ( cells::effects_definition::value ) ], nullptr );
			put ( c[ at ( cells::effects_definition::is_own ) ], 0u );
			if ( e == nullptr || index >= e->definitions.size () )
				return 1;

			const auto&	definition = e->definitions[ index ];
			const auto	file = e->source_file.filename ().u8string ();
			const auto	own = r._preset_preprocessor_definitions.find ( file );
			const auto	isOwn = own != r._preset_preprocessor_definitions.end ()
								&& std::ranges::any_of ( own->second, [ &definition ] ( const auto& d ) { return d.first == definition.first; } );
			value = r.find_preprocessor_definition ( file, definition.first, 0b111 ).value_or ( std::string () );
			putText ( c[ at ( cells::effects_definition::name ) ], definition.first.c_str () );
			putText ( c[ at ( cells::effects_definition::hint ) ], definition.second.c_str () );
			putText ( c[ at ( cells::effects_definition::value ) ], value.c_str () );
			put ( c[ at ( cells::effects_definition::is_own ) ], isOwn ? 1u : 0u );
			return 1;
		}

		static int effectsDefinitionSet ( runtime& r, UgCell* c )
		{
			constexpr auto	at = [] ( std::string_view name ) consteval { return cellOf ( effectsDefinitionSetFields, name ); };
			auto			reverted = false;
			setDefinition ( r, textOf ( c[ at ( cells::effects_definition_set::file ) ] ), textOf ( c[ at ( cells::effects_definition_set::name ) ] ),
							textOf ( c[ at ( cells::effects_definition_set::value ) ] ), reverted );
			put ( c[ at ( cells::effects_definition_set::reverted ) ], reverted ? 1u : 0u );
			return 1;
		}
	};
}

namespace
{
	using Access = reshade::ultra::HostAccess;

	// fn reads and writes its record's cells, the runtime found
	template <const auto& fields, const char* const& name, int ( *fn ) ( reshade::runtime& runtime, UgCell* cells )>
	int call ( void*, UgCell* in, uint32_t count )
	{
		constexpr auto	needed = uint32_t ( std::size ( fields ) + 1 );
		if ( in == nullptr || count < needed )
			return refused ( name, count, needed );
		auto*	runtime = runtimeOf ( in[ 1 ] );
		if ( runtime == nullptr )
			return noRuntime ( name );
		return fn ( *runtime, in );
	}

	constexpr const char*	overlayStateName = names::overlay_state;
	constexpr const char*	overlayPlaceName = names::overlay_place;
	constexpr const char*	overlayTabName = names::overlay_tab;
	constexpr const char*	overlayWantedName = names::overlay_wanted;
	constexpr const char*	screenshotStateName = names::screenshot_state;
	constexpr const char*	settingsStateName = names::settings_state;
	constexpr const char*	effectsStateName = names::effects_state;
	constexpr const char*	effectsTechniqueName = names::effects_technique;
	constexpr const char*	effectsUniformName = names::effects_uniform;
	constexpr const char*	effectsFailedName = names::effects_failed;
	constexpr const char*	effectsSaveName = names::effects_save;
	constexpr const char*	effectsDefinitionName = names::effects_definition;
	constexpr const char*	effectsDefinitionSetName = names::effects_definition_set;

	constexpr Answer	answers[] {
		{ names::overlay_state, &call<overlayStateFields, overlayStateName, &Access::overlayState>, overlayStateFields },
		{ names::overlay_place, &call<overlayPlaceFields, overlayPlaceName, &Access::overlayPlace>, overlayPlaceFields },
		{ names::overlay_tab, &call<overlayTabFields, overlayTabName, &Access::overlayTab>, overlayTabFields },
		{ names::overlay_wanted, &call<overlayWantedFields, overlayWantedName, &Access::overlayWanted>, overlayWantedFields },
		{ names::screenshot_state, &call<screenshotStateFields, screenshotStateName, &Access::screenshotState>, screenshotStateFields },
		{ names::settings_state, &call<settingsStateFields, settingsStateName, &Access::settingsState>, settingsStateFields },
		{ names::effects_state, &call<effectsStateFields, effectsStateName, &Access::effectsState>, effectsStateFields },
		{ names::effects_technique, &call<effectsTechniqueFields, effectsTechniqueName, &Access::effectsTechnique>, effectsTechniqueFields },
		{ names::effects_uniform, &call<effectsUniformFields, effectsUniformName, &Access::effectsUniform>, effectsUniformFields },
		{ names::effects_failed, &call<effectsFailedFields, effectsFailedName, &Access::effectsFailed>, effectsFailedFields },
		{ names::effects_save, &call<effectsSaveFields, effectsSaveName, &Access::effectsSave>, effectsSaveFields },
		{ names::effects_definition, &call<effectsDefinitionFields, effectsDefinitionName, &Access::effectsDefinition>, effectsDefinitionFields },
		{ names::effects_definition_set, &call<effectsDefinitionSetFields, effectsDefinitionSetName, &Access::effectsDefinitionSet>, effectsDefinitionSetFields },
	};

	[[ nodiscard ]] const Answer* answerOf ( const char* name )
	{
		if ( name == nullptr )
			return nullptr;
		const auto	it = std::ranges::find_if ( answers, [ name ] ( const Answer& a ) { return std::strcmp ( a.name, name ) == 0; } );
		return it != std::end ( answers ) ? &*it : nullptr;
	}

	uint32_t recordStart ( const char* record, UgCell* out, uint32_t count )
	{
		const auto*	a = answerOf ( record );
		if ( a == nullptr )
		{
			tell ( "start " + orNull ( record ), "record.start: no record \"" + orNull ( record ) + "\"" );
			return 0;
		}

		const auto	cellCount = uint32_t ( a->fields.size () + 1 );
		if ( out == nullptr )
			return cellCount;
		for ( uint32_t i = 0; i < cellCount && i < count; ++i )
			out[ i ].u64 = 0;
		return cellCount;
	}

	uint32_t recordSlot ( const char* record, const char* name, uint32_t type )
	{
		const auto*	a = answerOf ( record );
		if ( a == nullptr || name == nullptr )
		{
			tell ( "slot " + orNull ( record ), "record.slot: no record \"" + orNull ( record ) + "\"" );
			return 0;
		}

		const auto	it = std::ranges::find_if ( a->fields, [ name ] ( const Field& f ) { return std::strcmp ( f.name, name ) == 0; } );
		if ( it == a->fields.end () )
			return 0;
		if ( it->type == type )
			return uint32_t ( it - a->fields.begin () + 1 );
		tell ( std::string ( "type " ) + a->name + "." + name, std::string ( "record.slot: \"" ) + name + "\" of \"" + a->name + "\" is of type " + std::to_string ( it->type )
															   + ", asked for as " + std::to_string ( type ) + ": left out" );
		return 0;
	}

	void* lookup ( const char* name )
	{
		if ( name != nullptr && std::strcmp ( name, "record.start" ) == 0 )
			return reinterpret_cast<void*> ( static_cast<UgRecordStart> ( &recordStart ) );
		if ( name != nullptr && std::strcmp ( name, "record.slot" ) == 0 )
			return reinterpret_cast<void*> ( static_cast<UgRecordSlot> ( &recordSlot ) );
		const auto*	a = answerOf ( name );
		if ( a == nullptr )
		{
			tell ( "name " + orNull ( name ), "no name \"" + orNull ( name ) + "\"" );
			return nullptr;
		}
		return reinterpret_cast<void*> ( a->call );
	}
}

void reshade::ultra::noteRuntime ( runtime* runtime )
{
	std::scoped_lock	held ( runtimesLock );
	runtimes.emplace_back ( static_cast<const void*> ( static_cast<api::effect_runtime*> ( runtime ) ), runtime );
}

void reshade::ultra::forgetRuntime ( runtime* runtime )
{
	std::scoped_lock	held ( runtimesLock );
	std::erase_if ( runtimes, [ runtime ] ( const auto& r ) { return r.second == runtime; } );
}

#else

void reshade::ultra::noteRuntime ( runtime* )
{
}

void reshade::ultra::forgetRuntime ( runtime* )
{
}

#endif

//-----------------------------------------------------------------------------

#if defined(RESHADE_API_LIBRARY_EXPORT)

UgLookup ReShadeHost ( UgLookup )
{
#if RESHADE_GUI
	return &lookup;
#else
	return nullptr;
#endif
}

#endif
