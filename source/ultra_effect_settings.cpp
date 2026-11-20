/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>

#include <reshade_ultra_json.hpp>

#include "dll_log.hpp"
#include "ini_file.hpp"
#include "ultra_effect_settings.hpp"
#include "ultra_home.hpp"

//-----------------------------------------------------------------------------

namespace
{
	using Value = reshade::json::value;
	using Type = reshade::json::type;

	[[ nodiscard ]] std::optional<std::string> textOf ( const Value* value )
	{
		const char*	text = nullptr;
		size_t		length = 0;

		if ( ReShadeJsonGetString ( value, &text, &length ) )
			return std::string ( text, length );

		if ( int64_t integer = 0; ReShadeJsonGetType ( value ) == Type::integer && ReShadeJsonGetInt ( value, &integer ) )
			return std::to_string ( integer );

		if ( double number = 0.0; ReShadeJsonGetNumber ( value, &number ) )
		{
			char		buffer[ 32 ] {};
			const auto [ end, ec ] = std::to_chars ( buffer, buffer + sizeof ( buffer ), number );
			return std::string ( buffer, end );
		}

		return std::nullopt;
	}

	[[ nodiscard ]] std::optional<double> numberOf ( const Value* value )
	{
		if ( bool flag = false; ReShadeJsonGetBool ( value, &flag ) )
			return flag ? 1.0 : 0.0;

		if ( double number = 0.0; ReShadeJsonGetNumber ( value, &number ) )
			return number;

		return std::nullopt;
	}

	[[ nodiscard ]] std::optional<std::vector<double>> numbersOf ( const Value* value )
	{
		if ( const auto single = numberOf ( value ); single.has_value () )
			return std::vector<double> { *single };

		if ( ReShadeJsonGetType ( value ) != Type::array )
			return std::nullopt;

		std::vector<double>	numbers;

		for ( size_t i = 0, count = ReShadeJsonGetSize ( value ); i < count; ++i )
		{
			const auto	number = numberOf ( ReShadeJsonAt ( value, i, nullptr ) );

			if ( ! number.has_value () )
				return std::nullopt;

			numbers.push_back ( *number );
		}

		return numbers;
	}

	[[ nodiscard ]] reshade::ultra::Definitions readDefinitions ( const Value* object, const char* file, const char* where )
	{
		reshade::ultra::Definitions	definitions;

		if ( object != nullptr && ReShadeJsonGetType ( object ) != Type::object )
		{
			reshade::log::message ( reshade::log::level::error, "'%s': %s is no object of definitions, it is left out.", file, where );
			return definitions;
		}

		for ( size_t i = 0, count = ReShadeJsonGetSize ( object ); i < count; ++i )
		{
			const char*	name = nullptr;
			const auto	value = textOf ( ReShadeJsonAt ( object, i, &name ) );

			if ( value.has_value () )
				definitions.emplace_back ( name, *value );
			else
				reshade::log::message ( reshade::log::level::error, "'%s': the definition %s in %s is neither text nor a number, it is left out.", file, name, where );
		}

		return definitions;
	}

	[[ nodiscard ]] reshade::ultra::EffectLayer readLayer ( const reshade::ini_file& home, bool isUser )
	{
		const auto	member = [ &home, isUser ] ( const char* name ) { return isUser ? home.user_member ( name ) : home.curated_member ( name ); };
		const auto	path = ( isUser ? reshade::ultra::userSettingsPath () : reshade::ultra::curatedSettingsPath () ).u8string ();
		const auto	file = path.c_str ();

		reshade::ultra::EffectLayer	layer;
		const auto					effects = member ( "effects" );

		if ( effects != nullptr && ReShadeJsonGetType ( effects ) != Type::array )
			reshade::log::message ( reshade::log::level::error, "'%s': effects is no array, no effect of it runs.", file );

		for ( size_t i = 0, count = ReShadeJsonGetSize ( effects ); i < count; ++i )
		{
			const auto	item = ReShadeJsonAt ( effects, i, nullptr );
			const auto	technique = textOf ( ReShadeJsonFind ( item, "technique" ) );
			const auto	effectFile = textOf ( ReShadeJsonFind ( item, "file" ) );

			if ( ReShadeJsonGetType ( item ) != Type::object || ! technique.has_value () || ! effectFile.has_value () || technique->empty () || effectFile->empty () )
			{
				reshade::log::message ( reshade::log::level::error, "'%s': effect %zu has no technique or no file, it is left out.", file, i );
				continue;
			}

			reshade::ultra::EffectEntry	entry { *technique, *effectFile };

			// The route's state at start: on unless it says otherwise
			if ( const auto enabled = ReShadeJsonFind ( item, "enabled" ); ! isUser && enabled != nullptr && ! ReShadeJsonGetBool ( enabled, &entry.enabled ) )
				reshade::log::message ( reshade::log::level::error, "'%s': effect %s@%s has an enabled that is not true or false, it starts on.", file, technique->c_str (), effectFile->c_str () );

			if ( isUser )
			{
				bool	enabled = false;

				if ( ! ReShadeJsonGetBool ( ReShadeJsonFind ( item, "enabled" ), &enabled ) )
					reshade::log::message ( reshade::log::level::error, "'%s': effect %s@%s has no enabled (true or false), it is off.", file, technique->c_str (), effectFile->c_str () );

				entry.enabled = enabled;

				if ( const auto key = ReShadeJsonFind ( item, "key" ); key != nullptr )
				{
					const auto	numbers = numbersOf ( key );

					if ( numbers.has_value () && numbers->size () == 4 )
						std::ranges::transform ( *numbers, entry.key, [] ( double number ) { return static_cast<unsigned int> ( number ); } );
					else
						reshade::log::message ( reshade::log::level::error, "'%s': effect %s@%s has a key that is no four numbers, it has none.", file, technique->c_str (), effectFile->c_str () );
				}
			}

			layer.effects.push_back ( std::move ( entry ) );
		}

		const auto	files = member ( "effectFiles" );

		if ( files != nullptr && ReShadeJsonGetType ( files ) != Type::object )
			reshade::log::message ( reshade::log::level::error, "'%s': effectFiles is no object, no effect of it has values.", file );

		for ( size_t i = 0, count = ReShadeJsonGetSize ( files ); i < count; ++i )
		{
			const char*	name = nullptr;
			const auto	settings = ReShadeJsonAt ( files, i, &name );
			auto&		target = layer.files[ name ];
			const auto	values = ReShadeJsonFind ( settings, "values" );

			for ( size_t k = 0, uniforms = ReShadeJsonGetSize ( values ); k < uniforms; ++k )
			{
				const char*	uniform = nullptr;
				const auto	numbers = numbersOf ( ReShadeJsonAt ( values, k, &uniform ) );

				if ( numbers.has_value () )
					target.values[ uniform ] = *numbers;
				else
					reshade::log::message ( reshade::log::level::error, "'%s': %s's value %s is no number or array of numbers, it is left out.", file, name, uniform );
			}

			target.definitions = readDefinitions ( ReShadeJsonFind ( settings, "definitions" ), file, name );
		}

		return layer;
	}

	[[ nodiscard ]] bool isIntegral ( double value )
	{
		return std::trunc ( value ) == value && std::abs ( value ) < 9007199254740992.0;	// 2^53
	}

	void setNumber ( Value* target, double value )
	{
		if ( isIntegral ( value ) )
			ReShadeJsonSetInt ( target, static_cast<int64_t> ( value ) );
		else
			ReShadeJsonSetNumber ( target, value );
	}
}
//-----------------------------------------------------------------------------

bool reshade::ultra::EffectSettings::isRouteFile ( const std::string& file ) const
{
	return std::ranges::any_of ( route.effects, [ &file ] ( const EffectEntry& entry ) { return entry.file == file; } );
}
//-----------------------------------------------------------------------------

const reshade::ultra::EffectFileSettings* reshade::ultra::EffectSettings::fileSettings ( const std::string& file ) const
{
	const auto&	files = isRouteFile ( file ) ? route.files : user.files;
	const auto	it = files.find ( file );
	return it != files.end () ? &it->second : nullptr;
}
//-----------------------------------------------------------------------------

reshade::ultra::EffectSettings reshade::ultra::readEffectSettings ( const ini_file& home )
{
	EffectSettings	settings;
	settings.route = readLayer ( home, false );
	settings.user = readLayer ( home, true );
	settings.definitions = readDefinitions ( home.curated_member ( "definitions" ), curatedSettingsPath ().u8string ().c_str (), "definitions" );
	return settings;
}
//-----------------------------------------------------------------------------

reshade::ultra::JsonTree reshade::ultra::effectsJson ( const std::vector<EffectEntry>& effects )
{
	JsonTree	array ( ReShadeJsonCreate ( Type::array ) );

	for ( const auto& entry : effects )
	{
		const auto	item = ReShadeJsonInsert ( array.get (), ReShadeJsonGetSize ( array.get () ) );
		ReShadeJsonSetString ( ReShadeJsonMember ( item, "technique" ), entry.technique.data (), entry.technique.size () );
		ReShadeJsonSetString ( ReShadeJsonMember ( item, "file" ), entry.file.data (), entry.file.size () );
		ReShadeJsonSetBool ( ReShadeJsonMember ( item, "enabled" ), entry.enabled );

		if ( entry.key[ 0 ] != 0 )
		{
			const auto	key = ReShadeJsonMember ( item, "key" );
			ReShadeJsonSetType ( key, Type::array );

			for ( size_t i = 0; i < 4; ++i )
				ReShadeJsonSetInt ( ReShadeJsonInsert ( key, i ), entry.key[ i ] );
		}
	}

	return array;
}
//-----------------------------------------------------------------------------

reshade::ultra::JsonTree reshade::ultra::effectFileJson ( const EffectFileSettings& settings )
{
	JsonTree	object ( ReShadeJsonCreate ( Type::object ) );
	const auto	values = ReShadeJsonMember ( object.get (), "values" );
	ReShadeJsonSetType ( values, Type::object );

	std::vector<const std::string*>	names;
	for ( const auto& [ name, numbers ] : settings.values )
		names.push_back ( &name );
	std::ranges::sort ( names, [] ( const std::string* a, const std::string* b ) { return *a < *b; } );

	for ( const auto name : names )
	{
		const auto&	numbers = settings.values.at ( *name );
		const auto	target = ReShadeJsonMember ( values, name->c_str () );

		if ( numbers.size () == 1 )
		{
			setNumber ( target, numbers.front () );
			continue;
		}

		ReShadeJsonSetType ( target, Type::array );

		for ( size_t i = 0; i < numbers.size (); ++i )
			setNumber ( ReShadeJsonInsert ( target, i ), numbers[ i ] );
	}

	const auto	definitions = ReShadeJsonMember ( object.get (), "definitions" );
	ReShadeJsonSetType ( definitions, Type::object );

	for ( const auto& [ name, value ] : settings.definitions )
		ReShadeJsonSetString ( ReShadeJsonMember ( definitions, name.c_str () ), value.data (), value.size () );

	return object;
}
//-----------------------------------------------------------------------------

double reshade::ultra::shortestDouble ( float value )
{
	char		buffer[ 32 ] {};
	const auto [ end, ec ] = std::to_chars ( buffer, buffer + sizeof ( buffer ), value );
	auto		result = static_cast<double> ( value );
	std::from_chars ( buffer, end, result );
	return result;
}
