/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <reshade_ultra.hpp>
#include <reshade_ultra_host_names.h>

#include "dll_log.hpp"
#include "runtime.hpp"
#include "ultra_host_api.hpp"
#include "ultra_ui_shaders.hpp"

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

	constexpr Field	screenshotPendingFields[] { { cells::runtime, ugPtr }, { cells::screenshot_pending::count, ugU32 } };
	constexpr Field	effectsTargetFields[] { { cells::runtime, ugPtr }, { cells::effects_target::space, ugU32 } };

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
	// A friend of the runtime: what the answers read and write of it
	struct HostAccess
	{
		static int screenshotPending ( runtime& r, UgCell* c )
		{
			constexpr auto	at = [] ( std::string_view name ) consteval { return cellOf ( screenshotPendingFields, name ); };
			r._screenshot_files_pending = c[ at ( cells::screenshot_pending::count ) ].u32;
			return 1;
		}

		static int effectsTarget ( runtime& r, UgCell* c )
		{
			constexpr auto	at = [] ( std::string_view name ) consteval { return cellOf ( effectsTargetFields, name ); };
			const auto		space = c[ at ( cells::effects_target::space ) ].u32;

			if ( space > uint32_t ( api::color_space::hdr10_hlg ) )
			{
				tell ( "range " + std::string ( names::effects_target ), std::string ( names::effects_target ) + " called with colour space " + std::to_string ( space ) + ", which is none: refused" );
				return 0;
			}

			r._target_color_space = static_cast<api::color_space> ( space );
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

	constexpr const char*	screenshotPendingName = names::screenshot_pending;
	constexpr const char*	effectsTargetName = names::effects_target;

	constexpr Answer	answers[] {
		{ names::screenshot_pending, &call<screenshotPendingFields, screenshotPendingName, &Access::screenshotPending>, screenshotPendingFields },
		{ names::effects_target, &call<effectsTargetFields, effectsTargetName, &Access::effectsTarget>, effectsTargetFields },
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
	forgetShaders ( runtime );

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
