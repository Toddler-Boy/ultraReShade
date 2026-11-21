/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <locale.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <intrin.h>
#include <Windows.h>

#include <reshade.hpp>
#include <reshade_ultra_json.hpp>

#include "dll_log.hpp"
#include "ultra_json.hpp"

extern std::filesystem::path g_reshade_base_path;

//-----------------------------------------------------------------------------

struct reshade::json::value
{
	type								kind = type::null;
	bool								flag = false;
	int64_t								integer = 0;
	double								number = 0.0;
	std::string							text;		// a string's
	std::string							key;		// a member's own key
	std::vector<std::unique_ptr<value>>	items;		// an array's items or an object's members
	value*								parent = nullptr;
};

//-----------------------------------------------------------------------------

namespace
{
	using Value = reshade::json::value;
	using Type = reshade::json::type;
	using Status = reshade::json::status;
	using Error = reshade::json::error;
	using Segments = std::vector<std::string>;

	constexpr size_t	maxDepth = 512;				// deeper text would run the recursive parser out of stack
	constexpr int64_t	maxFileSize = 1ll << 30;
	constexpr int		replaceAttempts = 10;		// a virus scanner or indexer may hold the old file for a moment
	constexpr DWORD		replaceRetryWait = 10;		// ms
	constexpr size_t	keySetFrom = 16;			// members from which the parser finds duplicate keys through a hash set

	// Made at load: ReShade builds without thread-safe initialization of local statics
	const _locale_t		cNumbers = _create_locale ( LC_NUMERIC, "C" );

#if defined(RESHADE_TEST_APPLICATION)
	std::atomic<reshade::ultra::JsonWriteFault>	writeFault = reshade::ultra::JsonWriteFault::none;
#endif

	struct HandleCloser
	{
		void operator() ( HANDLE handle ) const
		{
			if ( handle != INVALID_HANDLE_VALUE )
				CloseHandle ( handle );
		}
	};

	using UniqueHandle = std::unique_ptr<void, HandleCloser>;

	[[ nodiscard ]] std::string toUtf8 ( std::wstring_view text )
	{
		const auto	size = WideCharToMultiByte ( CP_UTF8, 0, text.data (), static_cast<int> ( text.size () ), nullptr, 0, nullptr, nullptr );
		std::string	result ( static_cast<size_t> ( std::max ( size, 0 ) ), '\0' );
		WideCharToMultiByte ( CP_UTF8, 0, text.data (), static_cast<int> ( text.size () ), result.data (), size, nullptr, nullptr );
		return result;
	}

	[[ nodiscard ]] std::wstring toWide ( std::string_view text )
	{
		const auto		size = MultiByteToWideChar ( CP_UTF8, 0, text.data (), static_cast<int> ( text.size () ), nullptr, 0 );
		std::wstring	result ( static_cast<size_t> ( std::max ( size, 0 ) ), L'\0' );
		MultiByteToWideChar ( CP_UTF8, 0, text.data (), static_cast<int> ( text.size () ), result.data (), size );
		return result;
	}

	void clear ( Error* out )
	{
		if ( out != nullptr )
			*out = {};
	}

	// Always false, so a failing call returns report ( ... )
	bool report ( Error* out, Status code, DWORD systemError, const char* format, ... )
	{
		if ( out == nullptr )
			return false;

		*out = {};
		out->code = code;
		out->system_error = systemError;

		va_list	args;
		va_start ( args, format );
		std::vsnprintf ( out->message, sizeof ( out->message ), format, args );
		va_end ( args );
		return false;
	}

	[[ nodiscard ]] const char* typeName ( Type kind )
	{
		switch ( kind )
		{
			case Type::null:	return "null";
			case Type::boolean:	return "boolean";
			case Type::integer:	return "integer";
			case Type::number:	return "number";
			case Type::string:	return "string";
			case Type::array:	return "array";
			case Type::object:	return "object";
		}

		return "unknown type";
	}

	[[ nodiscard ]] bool isKnown ( Type kind )
	{
		return static_cast<uint32_t> ( kind ) <= static_cast<uint32_t> ( Type::object );
	}

	[[ nodiscard ]] bool isObject ( const Value* v )
	{
		return v != nullptr && v->kind == Type::object;
	}

	[[ nodiscard ]] bool isContainer ( const Value* v )
	{
		return v != nullptr && ( v->kind == Type::object || v->kind == Type::array );
	}

	[[ nodiscard ]] bool isDigit ( char c )
	{
		return c >= '0' && c <= '9';
	}

	//-------------------------------------------------------------------------
	// UTF-8

	// Length of the valid UTF-8 sequence at p, 0 when it is not one (overlong forms, surrogates, beyond U+10FFFF)
	[[ nodiscard ]] size_t utf8Sequence ( const char* p, const char* end )
	{
		const auto	byte = [ p ] ( size_t i ) { return static_cast<unsigned char> ( p[ i ] ); };
		const auto	isTail = [ &byte ] ( size_t i ) { return ( byte ( i ) & 0xC0 ) == 0x80; };
		const auto	left = static_cast<size_t> ( end - p );
		const auto	lead = byte ( 0 );

		if ( lead < 0x80 )
			return 1;

		if ( lead >= 0xC2 && lead <= 0xDF )
			return left >= 2 && isTail ( 1 ) ? 2 : 0;

		if ( lead >= 0xE0 && lead <= 0xEF )
		{
			if ( left < 3 || ! isTail ( 1 ) || ! isTail ( 2 ) || ( lead == 0xE0 && byte ( 1 ) < 0xA0 ) || ( lead == 0xED && byte ( 1 ) > 0x9F ) )
				return 0;

			return 3;
		}

		if ( lead >= 0xF0 && lead <= 0xF4 )
		{
			if ( left < 4 || ! isTail ( 1 ) || ! isTail ( 2 ) || ! isTail ( 3 ) || ( lead == 0xF0 && byte ( 1 ) < 0x90 ) || ( lead == 0xF4 && byte ( 1 ) > 0x8F ) )
				return 0;

			return 4;
		}

		return 0;
	}

	[[ nodiscard ]] bool isValidUtf8 ( std::string_view text )
	{
		for ( auto p = text.data (), end = text.data () + text.size (); p < end; )
		{
			const auto	length = utf8Sequence ( p, end );

			if ( length == 0 )
				return false;

			p += length;
		}

		return true;
	}

	void appendUtf8 ( std::string& out, uint32_t code )
	{
		if ( code < 0x80 )
			out += static_cast<char> ( code );
		else if ( code < 0x800 )
		{
			out += static_cast<char> ( 0xC0 | code >> 6 );
			out += static_cast<char> ( 0x80 | ( code & 0x3F ) );
		}
		else if ( code < 0x10000 )
		{
			out += static_cast<char> ( 0xE0 | code >> 12 );
			out += static_cast<char> ( 0x80 | ( code >> 6 & 0x3F ) );
			out += static_cast<char> ( 0x80 | ( code & 0x3F ) );
		}
		else
		{
			out += static_cast<char> ( 0xF0 | code >> 18 );
			out += static_cast<char> ( 0x80 | ( code >> 12 & 0x3F ) );
			out += static_cast<char> ( 0x80 | ( code >> 6 & 0x3F ) );
			out += static_cast<char> ( 0x80 | ( code & 0x3F ) );
		}
	}

	//-------------------------------------------------------------------------
	// Values

	[[ nodiscard ]] std::unique_ptr<Value> makeValue ( Type kind )
	{
		auto	v = std::make_unique<Value> ();
		v->kind = kind;
		return v;
	}

	[[ nodiscard ]] std::unique_ptr<Value> clone ( const Value& source )
	{
		auto	v = std::make_unique<Value> ();
		v->kind = source.kind;
		v->flag = source.flag;
		v->integer = source.integer;
		v->number = source.number;
		v->text = source.text;
		v->key = source.key;
		v->items.reserve ( source.items.size () );

		for ( const auto& item : source.items )
		{
			auto	copy = clone ( *item );
			copy->parent = v.get ();
			v->items.push_back ( std::move ( copy ) );
		}

		return v;
	}

	void resetValue ( Value& v, Type kind )
	{
		v.kind = kind;
		v.flag = false;
		v.integer = 0;
		v.number = 0.0;
		v.text.clear ();
		v.items.clear ();
	}

	// target takes source's contents; its own key and place stay
	void moveInto ( Value& target, std::unique_ptr<Value> source )
	{
		target.kind = source->kind;
		target.flag = source->flag;
		target.integer = source->integer;
		target.number = source->number;
		target.text = std::move ( source->text );
		target.items = std::move ( source->items );

		for ( const auto& item : target.items )
			item->parent = &target;
	}

	[[ nodiscard ]] size_t findMember ( const Value& object, std::string_view key )
	{
		for ( size_t i = 0; i < object.items.size (); ++i )
			if ( object.items[ i ]->key == key )
				return i;

		return std::string_view::npos;
	}

	Value& appendItem ( Value& container, std::unique_ptr<Value> item, std::string key = {} )
	{
		item->key = std::move ( key );
		item->parent = &container;
		container.items.push_back ( std::move ( item ) );
		return *container.items.back ();
	}

	// The member gets the value in its place, or is appended
	void setMember ( Value& object, const std::string& key, std::unique_ptr<Value> v )
	{
		if ( const auto index = findMember ( object, key ); index != std::string_view::npos )
			moveInto ( *object.items[ index ], std::move ( v ) );
		else
			appendItem ( object, std::move ( v ), key );
	}

	[[ nodiscard ]] std::optional<int64_t> integralValue ( double number )
	{
		constexpr auto	limit = 9223372036854775808.0;		// 2^63

		if ( ! std::isfinite ( number ) || std::trunc ( number ) != number || number < -limit || number >= limit )
			return std::nullopt;

		return static_cast<int64_t> ( number );
	}

	[[ nodiscard ]] bool isEqual ( const Value& a, const Value& b )
	{
		if ( a.kind != b.kind )
		{
			if ( a.kind == Type::integer && b.kind == Type::number )
				return integralValue ( b.number ) == a.integer;

			if ( a.kind == Type::number && b.kind == Type::integer )
				return integralValue ( a.number ) == b.integer;

			return false;
		}

		switch ( a.kind )
		{
			case Type::null:	return true;
			case Type::boolean:	return a.flag == b.flag;
			case Type::integer:	return a.integer == b.integer;
			case Type::number:	return a.number == b.number;
			case Type::string:	return a.text == b.text;
			case Type::array:
				return a.items.size () == b.items.size ()
					   && std::ranges::equal ( a.items, b.items, [] ( const std::unique_ptr<Value>& x, const std::unique_ptr<Value>& y ) { return isEqual ( *x, *y ); } );
			case Type::object:
				return a.items.size () == b.items.size () && std::ranges::all_of ( a.items, [ &b ] ( const std::unique_ptr<Value>& member )
				{
					const auto	index = findMember ( b, member->key );
					return index != std::string_view::npos && isEqual ( *member, *b.items[ index ] );
				} );
		}

		return false;
	}

	//-------------------------------------------------------------------------
	// Paths

	// '/' separated, a leading '/' optional, ~1 = '/', ~0 = '~'
	[[ nodiscard ]] std::optional<Segments> parsePath ( const char* path )
	{
		Segments	segments;

		if ( path == nullptr )
			return std::nullopt;

		std::string_view	rest ( path );

		if ( rest.starts_with ( '/' ) )
			rest.remove_prefix ( 1 );

		if ( rest.empty () )
			return segments;

		for ( ;; )
		{
			const auto	end = rest.find ( '/' );
			const auto	raw = rest.substr ( 0, end );
			std::string	segment;

			for ( size_t i = 0; i < raw.size (); ++i )
			{
				if ( raw[ i ] != '~' )
				{
					segment += raw[ i ];
					continue;
				}

				if ( i + 1 >= raw.size () || ( raw[ i + 1 ] != '0' && raw[ i + 1 ] != '1' ) )
					return std::nullopt;

				segment += raw[ ++i ] == '0' ? '~' : '/';
			}

			segments.push_back ( std::move ( segment ) );

			if ( end == std::string_view::npos )
				return segments;

			rest.remove_prefix ( end + 1 );
		}
	}

	[[ nodiscard ]] std::string joinPath ( const Segments& segments, size_t count )
	{
		std::string	path;

		for ( size_t i = 0; i < count && i < segments.size (); ++i )
			path += ( i != 0 ? "/" : "" ) + segments[ i ];

		return path;
	}

	// Decimal without leading zeros
	[[ nodiscard ]] std::optional<size_t> parseIndex ( std::string_view segment )
	{
		size_t	index = 0;

		if ( segment.empty () || ( segment.size () > 1 && segment[ 0 ] == '0' ) )
			return std::nullopt;

		const auto [ end, ec ] = std::from_chars ( segment.data (), segment.data () + segment.size (), index );

		if ( ec != std::errc () || end != segment.data () + segment.size () )
			return std::nullopt;

		return index;
	}

	[[ nodiscard ]] Value* childOf ( const Value* v, const std::string& segment )
	{
		if ( isObject ( v ) )
		{
			const auto	index = findMember ( *v, segment );
			return index != std::string_view::npos ? v->items[ index ].get () : nullptr;
		}

		if ( v != nullptr && v->kind == Type::array )
		{
			const auto	index = parseIndex ( segment );
			return index.has_value () && *index < v->items.size () ? v->items[ *index ].get () : nullptr;
		}

		return nullptr;
	}

	[[ nodiscard ]] Value* findPath ( const Value* root, const Segments& segments )
	{
		auto	at = const_cast<Value*> ( root );

		for ( const auto& segment : segments )
			if ( at = childOf ( at, segment ); at == nullptr )
				break;

		return at;
	}

	// segments from index from on, below root
	[[ nodiscard ]] Value* makePath ( Value& root, const Segments& segments, size_t from, Error* out )
	{
		auto	at = &root;

		for ( auto i = from; i < segments.size (); ++i )
		{
			const auto&	segment = segments[ i ];

			if ( at->kind == Type::null )
				resetValue ( *at, Type::object );

			if ( at->kind == Type::object )
			{
				const auto	index = findMember ( *at, segment );
				at = index != std::string_view::npos ? at->items[ index ].get () : &appendItem ( *at, makeValue ( Type::null ), segment );
				continue;
			}

			if ( at->kind != Type::array )
			{
				report ( out, Status::bad_path, 0, "The value at '%s' is a %s; it has no members or items.", joinPath ( segments, i ).c_str (), typeName ( at->kind ) );
				return nullptr;
			}

			const auto	index = segment == "-" ? std::optional ( at->items.size () ) : parseIndex ( segment );

			if ( ! index.has_value () )
			{
				report ( out, Status::bad_path, 0, "'%s' is not an index into the array at '%s'.", segment.c_str (), joinPath ( segments, i ).c_str () );
				return nullptr;
			}

			if ( *index > at->items.size () )
			{
				report ( out, Status::bad_path, 0, "Index %zu is past the end of the array at '%s' (%zu items).", *index, joinPath ( segments, i ).c_str (), at->items.size () );
				return nullptr;
			}

			at = *index == at->items.size () ? &appendItem ( *at, makeValue ( Type::null ) ) : at->items[ *index ].get ();
		}

		return at;
	}

	[[ nodiscard ]] bool removeChild ( Value& container, const std::string& segment )
	{
		if ( container.kind == Type::object )
		{
			const auto	index = findMember ( container, segment );

			if ( index == std::string_view::npos )
				return false;

			container.items.erase ( container.items.begin () + static_cast<ptrdiff_t> ( index ) );
			return true;
		}

		const auto	index = container.kind == Type::array ? parseIndex ( segment ) : std::nullopt;

		if ( ! index.has_value () || *index >= container.items.size () )
			return false;

		container.items.erase ( container.items.begin () + static_cast<ptrdiff_t> ( *index ) );
		return true;
	}

	//-------------------------------------------------------------------------
	// Text to values

	class Parser
	{
	public:
		Parser ( const char* text, size_t size ) : begin ( text ), at ( text ), end ( text + size )
		{
			if ( size >= 3 && std::memcmp ( text, "\xEF\xBB\xBF", 3 ) == 0 )
				begin = at = text + 3;
		}

		[[ nodiscard ]] std::unique_ptr<Value> parse ()
		{
			auto	root = std::make_unique<Value> ();
			skipSpace ();

			if ( ! parseValue ( *root, 0 ) )
				return nullptr;

			skipSpace ();

			if ( at != end )
			{
				failAt ( at, "there is more text after the json value" );
				return nullptr;
			}

			return root;
		}

		// what: "The text", or the file's name in quotes
		bool report ( Error* out, const char* what ) const
		{
			const auto	isFailed = ::report ( out, Status::not_json, 0, "%s is not json: line %u, column %u: %s.", what, line, column, problem.c_str () );

			if ( out != nullptr )
			{
				out->line = line;
				out->column = column;
			}

			return isFailed;
		}

	private:
		// Always false; line and column count from 1, the column in characters
		bool failAt ( const char* where, const char* format, ... )
		{
			char	text[ 400 ] {};
			va_list	args;
			va_start ( args, format );
			std::vsnprintf ( text, sizeof ( text ), format, args );
			va_end ( args );
			problem = text;

			auto	lineStart = begin;
			line = 1;

			for ( auto p = begin; p < where; ++p )
				if ( *p == '\n' )
					++line, lineStart = p + 1;

			column = 1;

			for ( auto p = lineStart; p < where; ++p )
				column += ( static_cast<unsigned char> ( *p ) & 0xC0 ) != 0x80;

			return false;
		}

		[[ nodiscard ]] static std::string shown ( char c )
		{
			return c >= 32 && c <= 126 ? std::string ( "'" ) + c + "'" : "a byte " + std::to_string ( static_cast<unsigned char> ( c ) );
		}

		void skipSpace ()
		{
			while ( at < end && ( *at == ' ' || *at == '\t' || *at == '\n' || *at == '\r' ) )
				++at;
		}

		bool parseValue ( Value& v, size_t depth )
		{
			if ( at == end )
				return failAt ( at, "a value was expected, the text ends" );

			switch ( *at )
			{
				case '{':	return parseContainer ( v, depth, Type::object );
				case '[':	return parseContainer ( v, depth, Type::array );
				case '"':	v.kind = Type::string; return parseString ( v.text );
				case 't':	return parseWord ( v, "true", Type::boolean, true );
				case 'f':	return parseWord ( v, "false", Type::boolean, false );
				case 'n':	return parseWord ( v, "null", Type::null, false );
				default:	break;
			}

			if ( *at == '-' || isDigit ( *at ) )
				return parseNumber ( v );

			return failAt ( at, "a value was expected, not %s", shown ( *at ).c_str () );
		}

		bool parseWord ( Value& v, std::string_view word, Type kind, bool flag )
		{
			if ( static_cast<size_t> ( end - at ) < word.size () || std::string_view ( at, word.size () ) != word )
				return failAt ( at, "a value was expected, not %s", shown ( *at ).c_str () );

			v.kind = kind;
			v.flag = flag;
			at += word.size ();
			return true;
		}

		[[ nodiscard ]] bool isDuplicate ( const Value& object, const std::string& key, std::unordered_set<std::string_view>& keys ) const
		{
			if ( object.items.size () < keySetFrom )
				return findMember ( object, key ) != std::string_view::npos;

			if ( keys.empty () )
				for ( const auto& member : object.items )
					keys.insert ( member->key );

			return keys.contains ( key );
		}

		bool parseContainer ( Value& v, size_t depth, Type kind )
		{
			const auto	isObjectKind = kind == Type::object;
			const auto	close = isObjectKind ? '}' : ']';

			if ( depth >= maxDepth )
				return failAt ( at, "values are nested deeper than %zu levels", maxDepth );

			v.kind = kind;
			++at;
			skipSpace ();

			if ( at < end && *at == close )
			{
				++at;
				return true;
			}

			std::unordered_set<std::string_view>	keys;

			for ( ;; )
			{
				auto	item = std::make_unique<Value> ();

				if ( isObjectKind )
				{
					if ( at == end || *at != '"' )
						return failAt ( at, at == end ? "a member's key was expected, the text ends" : "a member's key (a string in double quotes) was expected" );

					const auto	keyAt = at;

					if ( ! parseString ( item->key ) )
						return false;

					if ( isDuplicate ( v, item->key, keys ) )
						return failAt ( keyAt, "the key \"%s\" is there twice in this object", item->key.c_str () );

					skipSpace ();

					if ( at == end || *at != ':' )
						return failAt ( at, "':' was expected after the key" );

					++at;
					skipSpace ();
				}

				if ( ! parseValue ( *item, depth + 1 ) )
					return false;

				item->parent = &v;

				if ( isObjectKind && ! keys.empty () )
					keys.insert ( item->key );

				v.items.push_back ( std::move ( item ) );
				skipSpace ();

				if ( at < end && *at == ',' )
				{
					++at;
					skipSpace ();
					continue;
				}

				if ( at < end && *at == close )
				{
					++at;
					return true;
				}

				if ( at == end )
					return failAt ( at, isObjectKind ? "',' or '}' was expected, the text ends" : "',' or ']' was expected, the text ends" );

				return failAt ( at, isObjectKind ? "',' or '}' was expected after the member, not %s" : "',' or ']' was expected after the item, not %s", shown ( *at ).c_str () );
			}
		}

		[[ nodiscard ]] bool readHex4 ( uint32_t& code )
		{
			if ( end - at < 4 )
				return false;

			code = 0;

			for ( auto i = 0; i < 4; ++i, ++at )
			{
				const auto	c = *at;
				const auto	digit = isDigit ( c ) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;

				if ( digit < 0 )
					return false;

				code = code << 4 | static_cast<uint32_t> ( digit );
			}

			return true;
		}

		bool parseEscape ( std::string& out, const char* start )
		{
			const auto	escapeAt = at++;

			if ( at == end )
				return failAt ( start, "the string has no closing '\"'" );

			switch ( *at++ )
			{
				case '"':	out += '"'; return true;
				case '\\':	out += '\\'; return true;
				case '/':	out += '/'; return true;
				case 'b':	out += '\b'; return true;
				case 'f':	out += '\f'; return true;
				case 'n':	out += '\n'; return true;
				case 'r':	out += '\r'; return true;
				case 't':	out += '\t'; return true;
				case 'u':	break;
				default:	return failAt ( escapeAt, "a backslash followed by %s is not a json escape", shown ( at[ -1 ] ).c_str () );
			}

			uint32_t	code = 0;

			if ( ! readHex4 ( code ) )
				return failAt ( escapeAt, "\\u needs four hex digits" );

			if ( code >= 0xDC00 && code <= 0xDFFF )
				return failAt ( escapeAt, "a \\u escape of a low surrogate needs one of a high surrogate before it" );

			if ( code >= 0xD800 && code <= 0xDBFF )
			{
				uint32_t	low = 0;

				if ( end - at < 6 || at[ 0 ] != '\\' || at[ 1 ] != 'u' )
					return failAt ( escapeAt, "a \\u escape of a high surrogate needs one of a low surrogate after it" );

				at += 2;

				if ( ! readHex4 ( low ) || low < 0xDC00 || low > 0xDFFF )
					return failAt ( escapeAt, "a \\u escape of a high surrogate needs one of a low surrogate after it" );

				code = 0x10000 + ( ( code - 0xD800 ) << 10 ) + ( low - 0xDC00 );
			}

			appendUtf8 ( out, code );
			return true;
		}

		bool parseString ( std::string& out )
		{
			const auto	start = at++;

			for ( ;; )
			{
				const auto	run = at;

				while ( at < end && static_cast<unsigned char> ( *at ) >= 0x20 && static_cast<unsigned char> ( *at ) < 0x80 && *at != '"' && *at != '\\' )
					++at;

				out.append ( run, at );

				if ( at == end )
					return failAt ( start, "the string has no closing '\"'" );

				const auto	c = static_cast<unsigned char> ( *at );

				if ( c == '"' )
				{
					++at;
					return true;
				}

				if ( c == '\\' )
				{
					if ( ! parseEscape ( out, start ) )
						return false;

					continue;
				}

				if ( c < 0x20 )
					return failAt ( at, "a control character (code %u) in a string; it must be written as an escape", c );

				const auto	length = utf8Sequence ( at, end );

				if ( length == 0 )
					return failAt ( at, "the text is not valid UTF-8" );

				out.append ( at, length );
				at += length;
			}
		}

		// Integers stay integers while int64 holds them
		bool parseNumber ( Value& v )
		{
			const auto	start = at;
			auto		isInteger = true;
			const auto	skipDigits = [ this ] { while ( at < end && isDigit ( *at ) ) ++at; };

			if ( *at == '-' )
				++at;

			if ( at == end || ! isDigit ( *at ) )
				return failAt ( start, "a number needs a digit after '-'" );

			if ( *at == '0' && at + 1 < end && isDigit ( at[ 1 ] ) )
				return failAt ( start, "a number can't start with 0 followed by more digits" );

			skipDigits ();

			if ( at < end && *at == '.' )
			{
				isInteger = false;

				if ( ++at == end || ! isDigit ( *at ) )
					return failAt ( at, "a digit was expected after the decimal point" );

				skipDigits ();
			}

			if ( at < end && ( *at == 'e' || *at == 'E' ) )
			{
				isInteger = false;

				if ( ++at < end && ( *at == '+' || *at == '-' ) )
					++at;

				if ( at == end || ! isDigit ( *at ) )
					return failAt ( at, "a digit was expected in the exponent" );

				skipDigits ();
			}

			if ( isInteger )
			{
				int64_t	integer = 0;

				if ( const auto [ ptr, ec ] = std::from_chars ( start, at, integer ); ec == std::errc () )
				{
					v.kind = Type::integer;
					v.integer = integer;
					return true;
				}
			}

			auto	number = 0.0;
			const auto [ ptr, ec ] = std::from_chars ( start, at, number );

			if ( ec == std::errc::result_out_of_range )
			{
				// Too large or too small: the C library tells which (an underflow reads as zero)
				number = _strtod_l ( std::string ( start, at ).c_str (), nullptr, cNumbers );

				if ( std::isinf ( number ) )
					return failAt ( start, "the number is too large for a double" );
			}
			else if ( ec != std::errc () )
				return failAt ( start, "the number can't be read" );

			v.kind = Type::number;
			v.number = number;
			return true;
		}

		const char*	begin;
		const char*	at;
		const char*	end;
		std::string	problem;
		uint32_t	line = 0;
		uint32_t	column = 0;
	};

	//-------------------------------------------------------------------------
	// Values to text

	void appendString ( std::string& out, std::string_view text )
	{
		constexpr char	hex[] = "0123456789abcdef";
		out += '"';

		for ( const auto c : text )
		{
			switch ( c )
			{
				case '"':	out += "\\\""; break;
				case '\\':	out += "\\\\"; break;
				case '\b':	out += "\\b"; break;
				case '\f':	out += "\\f"; break;
				case '\n':	out += "\\n"; break;
				case '\r':	out += "\\r"; break;
				case '\t':	out += "\\t"; break;
				default:
					if ( static_cast<unsigned char> ( c ) < 0x20 )
					{
						out += "\\u00";
						out += hex[ c >> 4 ];
						out += hex[ c & 15 ];
					}
					else
						out += c;
			}
		}

		out += '"';
	}

	// The shortest text that reads back as the same double, with ".0" where it would read as an integer
	void appendNumber ( std::string& out, double number )
	{
		char		buffer[ 32 ] {};
		const auto [ end, ec ] = std::to_chars ( buffer, buffer + sizeof ( buffer ), number );
		const auto	text = std::string_view ( buffer, static_cast<size_t> ( end - buffer ) );
		out += text;

		if ( text.find_first_of ( ".eE" ) == std::string_view::npos )
			out += ".0";
	}

	void appendInteger ( std::string& out, int64_t integer )
	{
		char		buffer[ 24 ] {};
		const auto [ end, ec ] = std::to_chars ( buffer, buffer + sizeof ( buffer ), integer );
		out.append ( buffer, end );
	}

	void appendValue ( std::string& out, const Value& v, size_t depth, bool isCompact )
	{
		switch ( v.kind )
		{
			case Type::null:	out += "null"; return;
			case Type::boolean:	out += v.flag ? "true" : "false"; return;
			case Type::integer:	appendInteger ( out, v.integer ); return;
			case Type::number:	appendNumber ( out, v.number ); return;
			case Type::string:	appendString ( out, v.text ); return;
			case Type::array:
			case Type::object:	break;
		}

		const auto	isObjectKind = v.kind == Type::object;

		if ( v.items.empty () )
		{
			out += isObjectKind ? "{}" : "[]";
			return;
		}

		out += isObjectKind ? '{' : '[';

		for ( size_t i = 0; i < v.items.size (); ++i )
		{
			if ( i != 0 )
				out += isCompact ? ", " : ",";

			if ( ! isCompact )
			{
				out += '\n';
				out.append ( depth + 1, '\t' );
			}

			if ( isObjectKind )
			{
				appendString ( out, v.items[ i ]->key );
				out += ": ";
			}

			appendValue ( out, *v.items[ i ], depth + 1, isCompact );
		}

		if ( ! isCompact )
		{
			out += '\n';
			out.append ( depth, '\t' );
		}

		out += isObjectKind ? '}' : ']';
	}

	[[ nodiscard ]] std::string toText ( const Value& v, bool isCompact )
	{
		std::string	text;
		appendValue ( text, v, 0, isCompact );

		if ( ! isCompact )
			text += '\n';

		return text;
	}

	//-------------------------------------------------------------------------
	// Files

	[[ nodiscard ]] Value* readFile ( const char* path, Error* out )
	{
		const auto	file = reshade::ultra::resolveJsonPath ( path );

		if ( file.empty () )
		{
			report ( out, Status::bad_argument, 0, "read_file was called without a path." );
			return nullptr;
		}

		const auto	name = toUtf8 ( file.native () );
		const auto	handle = UniqueHandle ( CreateFileW ( file.c_str (), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
													  FILE_FLAG_SEQUENTIAL_SCAN, nullptr ) );

		if ( handle.get () == INVALID_HANDLE_VALUE )
		{
			const auto	error = GetLastError ();

			if ( error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND )
				report ( out, Status::missing, error, "'%s' does not exist.", name.c_str () );
			else
				report ( out, Status::unreadable, error, "'%s' can't be opened (error code %lu).", name.c_str (), error );

			return nullptr;
		}

		LARGE_INTEGER	size {};

		if ( ! GetFileSizeEx ( handle.get (), &size ) || size.QuadPart > maxFileSize )
		{
			const auto	error = size.QuadPart > maxFileSize ? 0 : GetLastError ();
			report ( out, Status::unreadable, error, error != 0 ? "'%s' can't be read (error code %lu)." : "'%s' is larger than 1 GB.", name.c_str (), error );
			return nullptr;
		}

		std::string	text ( static_cast<size_t> ( size.QuadPart ), '\0' );
		size_t		done = 0;

		while ( done < text.size () )
		{
			DWORD	read = 0;

			if ( ! ReadFile ( handle.get (), text.data () + done, static_cast<DWORD> ( text.size () - done ), &read, nullptr ) )
			{
				const auto	error = GetLastError ();
				report ( out, Status::unreadable, error, "'%s' can't be read (error code %lu).", name.c_str (), error );
				return nullptr;
			}

			if ( read == 0 )
				break;

			done += read;
		}

		text.resize ( done );

		Parser		parser ( text.data (), text.size () );
		auto		root = parser.parse ();

		if ( root == nullptr )
		{
			parser.report ( out, ( "'" + name + "'" ).c_str () );
			return nullptr;
		}

		clear ( out );
		return root.release ();
	}

	[[ nodiscard ]] reshade::ultra::JsonWriteFault injectedFault ()
	{
#if defined(RESHADE_TEST_APPLICATION)
		return writeFault.load ();
#else
		return reshade::ultra::JsonWriteFault::none;
#endif
	}

	[[ nodiscard ]] bool writeAll ( HANDLE handle, std::string_view text )
	{
		while ( ! text.empty () )
		{
			DWORD	written = 0;

			if ( ! WriteFile ( handle, text.data (), static_cast<DWORD> ( std::min<size_t> ( text.size (), 1u << 30 ) ), &written, nullptr ) || written == 0 )
				return false;

			text.remove_prefix ( written );
		}

		return true;
	}

	struct WriteFailure
	{
		const char*	step = nullptr;
		DWORD		error = 0;
	};

	// Closed again on return, so a failed one can be deleted
	[[ nodiscard ]] std::optional<WriteFailure> writeTemporary ( const std::filesystem::path& temporary, std::string_view text )
	{
		const auto	handle = UniqueHandle ( CreateFileW ( temporary.c_str (), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr ) );

		if ( handle.get () == INVALID_HANDLE_VALUE )
			return WriteFailure { "the temporary file can't be made", GetLastError () };

		if ( injectedFault () == reshade::ultra::JsonWriteFault::partialWrite )
		{
			(void) writeAll ( handle.get (), text.substr ( 0, text.size () / 2 ) );
			return WriteFailure { "writing the temporary file failed", ERROR_DISK_FULL };
		}

		if ( ! writeAll ( handle.get (), text ) )
			return WriteFailure { "writing the temporary file failed", GetLastError () };

		if ( ! FlushFileBuffers ( handle.get () ) )
			return WriteFailure { "flushing the temporary file failed", GetLastError () };

		return std::nullopt;
	}

	// The whole text into a temporary file beside the target, flushed, then renamed over it: the target is always either
	// the old file or the new one
	[[ nodiscard ]] bool writeFile ( void* owner, const char* path, const Value* v, Error* out )
	{
		const auto	file = reshade::ultra::resolveJsonPath ( path );

		if ( file.empty () || v == nullptr )
			return report ( out, Status::bad_argument, 0, file.empty () ? "write_file was called without a path." : "write_file was called without a value." );

		const auto		name = toUtf8 ( file.native () );
		const auto		text = toText ( *v, false );
		std::error_code	ec;

		if ( std::filesystem::create_directories ( file.parent_path (), ec ); ec )
			return report ( out, Status::write_failed, static_cast<DWORD> ( ec.value () ), "'%s' was not written: its folder can't be made (error code %d).", name.c_str (), ec.value () );

		static std::atomic<uint32_t>	counter = 0;
		auto	temporary = file;
		temporary += L".tmp-" + std::to_wstring ( GetCurrentProcessId () ) + L"-" + std::to_wstring ( ++counter );

		const auto	failed = [ out, &name, &temporary ] ( const char* step, DWORD error )
		{
			DeleteFileW ( temporary.c_str () );
			return report ( out, Status::write_failed, error, "'%s' was not written: %s (error code %lu); the file is as it was.", name.c_str (), step, error );
		};

		if ( const auto failure = writeTemporary ( temporary, text ); failure.has_value () )
			return failed ( failure->step, failure->error );

		if ( injectedFault () == reshade::ultra::JsonWriteFault::beforeReplace )
			return failed ( "the temporary file can't replace it", ERROR_ACCESS_DENIED );

		for ( auto attempt = 1;; ++attempt )
		{
			if ( MoveFileExW ( temporary.c_str (), file.c_str (), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) )
				break;

			const auto	error = GetLastError ();

			if ( attempt >= replaceAttempts || ( error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION ) )
				return failed ( "the temporary file can't replace it", error );

			Sleep ( replaceRetryWait );
		}

		reshade::ultra::noteJsonWrite ( owner, file );
		clear ( out );
		return true;
	}

	//-------------------------------------------------------------------------
	// Layers

	struct Layers
	{
		const Value*	user = nullptr;
		const Value*	defaults = nullptr;
	};

	// The user's value replaces the defaults' unless both are objects
	[[ nodiscard ]] Layers stepLayers ( Layers at, const std::string& segment )
	{
		if ( at.user != nullptr && ! ( isObject ( at.user ) && isObject ( at.defaults ) ) )
			return { childOf ( at.user, segment ), nullptr };

		return { childOf ( at.user, segment ), childOf ( at.defaults, segment ) };
	}

	// Both objects: the defaults' members in their order, then the user's own
	[[ nodiscard ]] std::unique_ptr<Value> merge ( const Value& defaults, const Value& user )
	{
		auto	result = makeValue ( Type::object );

		for ( const auto& member : defaults.items )
		{
			const auto	index = findMember ( user, member->key );

			if ( index == std::string_view::npos )
				appendItem ( *result, clone ( *member ), member->key );
			else if ( isObject ( member.get () ) && isObject ( user.items[ index ].get () ) )
				appendItem ( *result, merge ( *member, *user.items[ index ] ), member->key );
			else
				appendItem ( *result, clone ( *user.items[ index ] ), member->key );
		}

		for ( const auto& member : user.items )
			if ( findMember ( defaults, member->key ) == std::string_view::npos )
				appendItem ( *result, clone ( *member ), member->key );

		return result;
	}

	[[ nodiscard ]] std::unique_ptr<Value> effective ( Layers at )
	{
		if ( isObject ( at.user ) && isObject ( at.defaults ) )
			return merge ( *at.defaults, *at.user );

		if ( at.user != nullptr )
			return clone ( *at.user );

		return at.defaults != nullptr ? clone ( *at.defaults ) : nullptr;
	}

	// What a user layer holds so that it gives wanted over defaults; null when it needs nothing
	[[ nodiscard ]] std::unique_ptr<Value> difference ( const Value* defaults, const Value& wanted )
	{
		if ( isObject ( defaults ) && isObject ( &wanted ) )
		{
			auto	result = makeValue ( Type::object );

			for ( const auto& member : wanted.items )
			{
				const auto	index = findMember ( *defaults, member->key );
				auto		part = index == std::string_view::npos ? clone ( *member ) : difference ( defaults->items[ index ].get (), *member );

				if ( part != nullptr )
					appendItem ( *result, std::move ( part ), member->key );
			}

			return result->items.empty () ? nullptr : std::move ( result );
		}

		if ( defaults != nullptr && isEqual ( *defaults, wanted ) )
			return nullptr;

		return clone ( wanted );
	}

	// The user's objects along the path that hold nothing any more go, unless they replace a defaults' value that is no
	// object
	void pruneEmpty ( Value& user, const Value* defaults, const Segments& segments, size_t depth )
	{
		std::vector<Value*>			userChain { &user };
		std::vector<const Value*>	defaultsChain { defaults };

		for ( size_t i = 0; i < depth; ++i )
		{
			userChain.push_back ( childOf ( userChain.back (), segments[ i ] ) );
			defaultsChain.push_back ( childOf ( defaultsChain.back (), segments[ i ] ) );
		}

		for ( auto j = depth; j > 0; --j )
		{
			const auto	node = userChain[ j ];

			if ( ! isObject ( node ) || ! node->items.empty () || ( defaultsChain[ j ] != nullptr && ! isObject ( defaultsChain[ j ] ) ) )
				break;

			(void) removeChild ( *userChain[ j - 1 ], segments[ j - 1 ] );
		}
	}

	// Down the path while both layers hold objects; where they part (the anchor) the effective value is rebuilt with the
	// change and the user layer takes only its difference to the defaults
	[[ nodiscard ]] bool layerSet ( const Value* defaults, Value& user, const Segments& segments, const Value& item, Error* out )
	{
		std::vector<Value*>	userChain { user.kind == Type::null ? nullptr : &user };
		auto				anchorDefaults = defaults;
		size_t				anchor = 0;

		while ( anchor < segments.size () && isObject ( userChain.back () ) && isObject ( anchorDefaults ) )
		{
			userChain.push_back ( childOf ( userChain.back (), segments[ anchor ] ) );
			anchorDefaults = childOf ( anchorDefaults, segments[ anchor ] );
			++anchor;
		}

		std::unique_ptr<Value>	wanted;

		if ( anchor == segments.size () )
			wanted = clone ( item );
		else
		{
			wanted = effective ( { userChain.back (), anchorDefaults } );

			if ( wanted == nullptr )
				wanted = makeValue ( Type::null );

			const auto	leaf = makePath ( *wanted, segments, anchor, out );

			if ( leaf == nullptr )
				return false;

			moveInto ( *leaf, clone ( item ) );
		}

		auto	part = difference ( anchorDefaults, *wanted );

		if ( anchor == 0 )
		{
			if ( part != nullptr )
				moveInto ( user, std::move ( part ) );
			else
				resetValue ( user, isObject ( defaults ) ? Type::object : Type::null );

			return true;
		}

		auto&	parent = *userChain[ anchor - 1 ];

		if ( part != nullptr )
			setMember ( parent, segments[ anchor - 1 ], std::move ( part ) );
		else if ( removeChild ( parent, segments[ anchor - 1 ] ) )
			pruneEmpty ( user, defaults, segments, anchor - 1 );

		return true;
	}
}
//-----------------------------------------------------------------------------

std::filesystem::path reshade::ultra::resolveJsonPath ( const char* path )
{
	if ( path == nullptr || *path == '\0' )
		return {};

	auto	file = std::filesystem::path ( toWide ( path ) );

	if ( file.is_relative () )
		file = g_reshade_base_path / file;

	return file.lexically_normal ();
}
//-----------------------------------------------------------------------------

void reshade::ultra::JsonFree::operator() ( json::value* root ) const
{
	delete root;
}
//-----------------------------------------------------------------------------

reshade::ultra::JsonTree reshade::ultra::readJsonFile ( const std::filesystem::path& file, json::error& error )
{
	return JsonTree ( readFile ( toUtf8 ( file.native () ).c_str (), &error ) );
}
//-----------------------------------------------------------------------------

bool reshade::ultra::writeJsonFile ( const std::filesystem::path& file, const json::value& root, json::error& error )
{
	return writeFile ( nullptr, toUtf8 ( file.native () ).c_str (), &root, &error );
}
//-----------------------------------------------------------------------------

#if defined(RESHADE_TEST_APPLICATION)

void reshade::ultra::setJsonWriteFault ( JsonWriteFault fault )
{
	writeFault = fault;
}
//-----------------------------------------------------------------------------

#endif

#if defined(RESHADE_API_LIBRARY_EXPORT)

reshade::json::value* ReShadeJsonCreate ( reshade::json::type type )
{
	return isKnown ( type ) ? makeValue ( type ).release () : nullptr;
}
//-----------------------------------------------------------------------------

reshade::json::value* ReShadeJsonCopy ( const reshade::json::value* source )
{
	if ( source == nullptr )
		return nullptr;

	auto	copy = clone ( *source );
	copy->key.clear ();
	return copy.release ();
}
//-----------------------------------------------------------------------------

// A member or item belongs to its tree: freeing it alone would leave the tree pointing at freed memory
void ReShadeJsonFree ( reshade::json::value* root )
{
	if ( root == nullptr )
		return;

	if ( root->parent != nullptr )
	{
		reshade::log::message ( reshade::log::level::error, "Json: a member or item was given to be freed; only a tree's root can be, it stays." );
		return;
	}

	delete root;
}
//-----------------------------------------------------------------------------

reshade::json::type ReShadeJsonGetType ( const reshade::json::value* value )
{
	return value != nullptr ? value->kind : reshade::json::type::null;
}
//-----------------------------------------------------------------------------

bool ReShadeJsonGetBool ( const reshade::json::value* value, bool* out )
{
	if ( value == nullptr || out == nullptr || value->kind != reshade::json::type::boolean )
		return false;

	*out = value->flag;
	return true;
}
//-----------------------------------------------------------------------------

bool ReShadeJsonGetInt ( const reshade::json::value* value, int64_t* out )
{
	if ( value == nullptr || out == nullptr )
		return false;

	const auto	integer = value->kind == reshade::json::type::integer ? std::optional ( value->integer )
						: value->kind == reshade::json::type::number ? integralValue ( value->number ) : std::nullopt;

	if ( ! integer.has_value () )
		return false;

	*out = *integer;
	return true;
}
//-----------------------------------------------------------------------------

bool ReShadeJsonGetNumber ( const reshade::json::value* value, double* out )
{
	if ( value == nullptr || out == nullptr || ( value->kind != reshade::json::type::number && value->kind != reshade::json::type::integer ) )
		return false;

	*out = value->kind == reshade::json::type::number ? value->number : static_cast<double> ( value->integer );
	return true;
}
//-----------------------------------------------------------------------------

bool ReShadeJsonGetString ( const reshade::json::value* value, const char** text, size_t* length )
{
	if ( value == nullptr || text == nullptr || value->kind != reshade::json::type::string )
		return false;

	*text = value->text.c_str ();

	if ( length != nullptr )
		*length = value->text.size ();

	return true;
}
//-----------------------------------------------------------------------------

size_t ReShadeJsonGetSize ( const reshade::json::value* value )
{
	return isContainer ( value ) ? value->items.size () : 0;
}
//-----------------------------------------------------------------------------

reshade::json::value* ReShadeJsonAt ( const reshade::json::value* container, size_t index, const char** key )
{
	if ( key != nullptr )
		*key = nullptr;

	if ( ! isContainer ( container ) || index >= container->items.size () )
		return nullptr;

	const auto	item = container->items[ index ].get ();

	if ( key != nullptr && container->kind == reshade::json::type::object )
		*key = item->key.c_str ();

	return item;
}
//-----------------------------------------------------------------------------

reshade::json::value* ReShadeJsonFind ( const reshade::json::value* object, const char* key )
{
	if ( ! isObject ( object ) || key == nullptr )
		return nullptr;

	const auto	index = findMember ( *object, key );
	return index != std::string_view::npos ? object->items[ index ].get () : nullptr;
}
//-----------------------------------------------------------------------------

reshade::json::value* ReShadeJsonFindPath ( const reshade::json::value* root, const char* path )
{
	const auto	segments = parsePath ( path );
	return root != nullptr && segments.has_value () ? findPath ( root, *segments ) : nullptr;
}
//-----------------------------------------------------------------------------

bool ReShadeJsonEqual ( const reshade::json::value* a, const reshade::json::value* b )
{
	return a != nullptr && b != nullptr && isEqual ( *a, *b );
}
//-----------------------------------------------------------------------------

bool ReShadeJsonSetType ( reshade::json::value* value, reshade::json::type type )
{
	if ( value == nullptr || ! isKnown ( type ) )
		return false;

	resetValue ( *value, type );
	return true;
}
//-----------------------------------------------------------------------------

bool ReShadeJsonSetBool ( reshade::json::value* value, bool flag )
{
	if ( value == nullptr )
		return false;

	resetValue ( *value, reshade::json::type::boolean );
	value->flag = flag;
	return true;
}
//-----------------------------------------------------------------------------

bool ReShadeJsonSetInt ( reshade::json::value* value, int64_t integer )
{
	if ( value == nullptr )
		return false;

	resetValue ( *value, reshade::json::type::integer );
	value->integer = integer;
	return true;
}
//-----------------------------------------------------------------------------

// Json has no infinity or NaN
bool ReShadeJsonSetNumber ( reshade::json::value* value, double number )
{
	if ( value == nullptr || ! std::isfinite ( number ) )
		return false;

	resetValue ( *value, reshade::json::type::number );
	value->number = number;
	return true;
}
//-----------------------------------------------------------------------------

bool ReShadeJsonSetString ( reshade::json::value* value, const char* text, size_t length )
{
	if ( value == nullptr || ( text == nullptr && length != 0 ) )
		return false;

	const auto	view = text != nullptr ? std::string_view ( text, length ) : std::string_view ();

	if ( ! isValidUtf8 ( view ) )
		return false;

	resetValue ( *value, reshade::json::type::string );
	value->text = view;
	return true;
}
//-----------------------------------------------------------------------------

bool ReShadeJsonAssign ( reshade::json::value* target, const reshade::json::value* source )
{
	if ( target == nullptr || source == nullptr )
		return false;

	if ( target != source )
		moveInto ( *target, clone ( *source ) );

	return true;
}
//-----------------------------------------------------------------------------

reshade::json::value* ReShadeJsonMember ( reshade::json::value* object, const char* key )
{
	if ( object == nullptr || key == nullptr || ! isValidUtf8 ( key ) )
		return nullptr;

	if ( object->kind == reshade::json::type::null )
		resetValue ( *object, reshade::json::type::object );

	if ( object->kind != reshade::json::type::object )
		return nullptr;

	const auto	index = findMember ( *object, key );
	return index != std::string_view::npos ? object->items[ index ].get () : &appendItem ( *object, makeValue ( reshade::json::type::null ), key );
}
//-----------------------------------------------------------------------------

reshade::json::value* ReShadeJsonInsert ( reshade::json::value* array, size_t index )
{
	if ( array == nullptr )
		return nullptr;

	if ( array->kind == reshade::json::type::null )
		resetValue ( *array, reshade::json::type::array );

	if ( array->kind != reshade::json::type::array || index > array->items.size () )
		return nullptr;

	auto	item = makeValue ( reshade::json::type::null );
	item->parent = array;
	return array->items.insert ( array->items.begin () + static_cast<ptrdiff_t> ( index ), std::move ( item ) )->get ();
}
//-----------------------------------------------------------------------------

reshade::json::value* ReShadeJsonMakePath ( reshade::json::value* root, const char* path, reshade::json::error* error )
{
	const auto	segments = parsePath ( path );

	if ( root == nullptr )
	{
		report ( error, Status::bad_argument, 0, "make_path was called without a value." );
		return nullptr;
	}

	if ( ! segments.has_value () )
	{
		report ( error, Status::bad_path, 0, path == nullptr ? "make_path was called without a path." : "'%s' is not a valid path ('~' stands only before 0 or 1).", path );
		return nullptr;
	}

	if ( std::ranges::any_of ( *segments, [] ( const std::string& segment ) { return ! isValidUtf8 ( segment ); } ) )
	{
		report ( error, Status::bad_argument, 0, "The path is not valid UTF-8." );
		return nullptr;
	}

	const auto	result = makePath ( *root, *segments, 0, error );

	if ( result != nullptr )
		clear ( error );

	return result;
}
//-----------------------------------------------------------------------------

bool ReShadeJsonRemove ( reshade::json::value* object, const char* key )
{
	return isObject ( object ) && key != nullptr && removeChild ( *object, key );
}
//-----------------------------------------------------------------------------

bool ReShadeJsonRemoveAt ( reshade::json::value* container, size_t index )
{
	if ( ! isContainer ( container ) || index >= container->items.size () )
		return false;

	container->items.erase ( container->items.begin () + static_cast<ptrdiff_t> ( index ) );
	return true;
}
//-----------------------------------------------------------------------------

bool ReShadeJsonRemovePath ( reshade::json::value* root, const char* path )
{
	const auto	segments = parsePath ( path );

	if ( root == nullptr || ! segments.has_value () || segments->empty () )
		return false;

	const auto	parentPath = Segments ( segments->begin (), segments->end () - 1 );
	const auto	parent = findPath ( root, parentPath );
	return parent != nullptr && removeChild ( *parent, segments->back () );
}
//-----------------------------------------------------------------------------

reshade::json::value* ReShadeJsonParse ( const char* text, size_t size, reshade::json::error* error )
{
	if ( text == nullptr && size != 0 )
	{
		report ( error, Status::bad_argument, 0, "parse was called without text." );
		return nullptr;
	}

	Parser	parser ( text != nullptr ? text : "", size );
	auto	root = parser.parse ();

	if ( root == nullptr )
	{
		parser.report ( error, "The text" );
		return nullptr;
	}

	clear ( error );
	return root.release ();
}
//-----------------------------------------------------------------------------

// size: the room in text on the way in, the text's size with its terminating zero on the way out
bool ReShadeJsonSerialize ( const reshade::json::value* value, bool compact, char* text, size_t* size )
{
	if ( value == nullptr || size == nullptr )
		return false;

	const auto	result = toText ( *value, compact );
	const auto	needed = result.size () + 1;
	const auto	isFitting = text != nullptr && *size >= needed;

	if ( isFitting )
		std::memcpy ( text, result.c_str (), needed );

	*size = needed;
	return isFitting;
}
//-----------------------------------------------------------------------------

reshade::json::value* ReShadeJsonReadFile ( const char* path, reshade::json::error* error )
{
	return readFile ( path, error );
}
//-----------------------------------------------------------------------------

// owner null: the caller's module
bool ReShadeJsonWriteFile ( void* owner, const char* path, const reshade::json::value* value, reshade::json::error* error )
{
	HMODULE	caller = nullptr;

	if ( owner == nullptr )
		GetModuleHandleExW ( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR> ( _ReturnAddress () ), &caller );

	return writeFile ( owner != nullptr ? owner : caller, path, value, error );
}
//-----------------------------------------------------------------------------

reshade::json::value* ReShadeJsonLayerGet ( const reshade::json::value* defaults, const reshade::json::value* user, const char* path )
{
	const auto	segments = parsePath ( path );

	if ( ! segments.has_value () )
		return nullptr;

	auto	at = Layers { user != nullptr && user->kind != reshade::json::type::null ? user : nullptr, defaults };

	for ( const auto& segment : *segments )
		at = stepLayers ( at, segment );

	auto	result = effective ( at );

	if ( result != nullptr )
		result->key.clear ();

	return result.release ();
}
//-----------------------------------------------------------------------------

bool ReShadeJsonLayerSet ( const reshade::json::value* defaults, reshade::json::value* user, const char* path, const reshade::json::value* item, reshade::json::error* error )
{
	if ( user == nullptr || item == nullptr )
		return report ( error, Status::bad_argument, 0, user == nullptr ? "layer_set was called without a user layer." : "layer_set was called without a value." );

	const auto	segments = parsePath ( path );

	if ( ! segments.has_value () )
		return report ( error, Status::bad_path, 0, path == nullptr ? "layer_set was called without a path." : "'%s' is not a valid path ('~' stands only before 0 or 1).", path );

	if ( ! layerSet ( defaults, *user, *segments, *item, error ) )
		return false;

	clear ( error );
	return true;
}
//-----------------------------------------------------------------------------

bool ReShadeJsonLayerReset ( const reshade::json::value* defaults, reshade::json::value* user, const char* path, reshade::json::error* error )
{
	if ( user == nullptr )
		return report ( error, Status::bad_argument, 0, "layer_reset was called without a user layer." );

	const auto	segments = parsePath ( path );

	if ( ! segments.has_value () )
		return report ( error, Status::bad_path, 0, path == nullptr ? "layer_reset was called without a path." : "'%s' is not a valid path ('~' stands only before 0 or 1).", path );

	if ( const auto fallback = defaults != nullptr ? findPath ( defaults, *segments ) : nullptr; fallback != nullptr )
		return ReShadeJsonLayerSet ( defaults, user, path, fallback, error );

	if ( segments->empty () )
		resetValue ( *user, isObject ( defaults ) ? reshade::json::type::object : reshade::json::type::null );
	else if ( ReShadeJsonRemovePath ( user, path ) )
		pruneEmpty ( *user, defaults, *segments, segments->size () - 1 );

	clear ( error );
	return true;
}
//-----------------------------------------------------------------------------

#endif

