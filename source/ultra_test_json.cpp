/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RESHADE_TEST_APPLICATION

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include <Windows.h>
#include <shellapi.h>

#include <reshade.hpp>
#include <reshade_ultra_json.hpp>

#include "dll_log.hpp"
#include "ultra_json.hpp"
#include "ultra_test_json.hpp"

extern std::filesystem::path g_reshade_base_path;

//-----------------------------------------------------------------------------

namespace
{
	namespace json = reshade::json;
	using Clock = std::chrono::steady_clock;

	constexpr auto	deliveryTimeout = std::chrono::seconds ( 3 );
	constexpr auto	quietCheck = std::chrono::milliseconds ( 1000 );	// a change that must not come gets this long to show

	struct Checks
	{
		size_t	passed = 0;
		size_t	failed = 0;
	};

	Checks	checks;

	// Only failures go to the log
	void expect ( bool isOk, const char* format, ... )
	{
		if ( isOk )
		{
			++checks.passed;
			return;
		}

		++checks.failed;
		char	text[ 1024 ] {};
		va_list	args;
		va_start ( args, format );
		std::vsnprintf ( text, sizeof ( text ), format, args );
		va_end ( args );
		reshade::log::message ( reshade::log::level::error, "Test application: json: %s", text );
	}

	[[ nodiscard ]] std::filesystem::path testFolder ()
	{
		return g_reshade_base_path / L"json-test";
	}

	[[ nodiscard ]] std::string testPath ( const wchar_t* name )
	{
		return ( testFolder () / name ).u8string ();
	}

	[[ nodiscard ]] std::string compact ( const json::value* v )
	{
		return v != nullptr ? json::serialize ( v, true ) : std::string ( "(none)" );
	}

	// Control characters and bytes above 126 as \xNN, for the log
	[[ nodiscard ]] std::string printable ( std::string_view text )
	{
		std::string	out;

		for ( const auto c : text.substr ( 0, 80 ) )
		{
			if ( c >= 32 && c <= 126 )
				out += c;
			else
			{
				char	hex[ 8 ] {};
				std::snprintf ( hex, sizeof ( hex ), "\\x%02X", static_cast<unsigned char> ( c ) );
				out += hex;
			}
		}

		return out;
	}

	[[ nodiscard ]] json::document parsed ( std::string_view text )
	{
		json::error	error;
		auto		d = json::parse ( text, &error );
		expect ( static_cast<bool> ( d ), "'%s' did not parse: %s", printable ( text ).c_str (), error.message );
		return d;
	}

	[[ nodiscard ]] std::string readBytes ( const std::string& path )
	{
		std::ifstream	file ( std::filesystem::u8path ( path ), std::ios::binary );
		return std::string ( std::istreambuf_iterator<char> ( file ), std::istreambuf_iterator<char> () );
	}

	// Without the API: a plain overwrite, as an editor or another program does it
	[[ nodiscard ]] bool writeBytes ( const std::string& path, std::string_view text )
	{
		const auto	handle = CreateFileW ( std::filesystem::u8path ( path ).c_str (), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );

		if ( handle == INVALID_HANDLE_VALUE )
			return false;

		DWORD		written = 0;
		const auto	isWritten = WriteFile ( handle, text.data (), static_cast<DWORD> ( text.size () ), &written, nullptr ) && written == text.size ();
		CloseHandle ( handle );
		return isWritten;
	}

	[[ nodiscard ]] bool hasTemporaryFiles ()
	{
		std::error_code	ec;

		for ( const auto& entry : std::filesystem::directory_iterator ( testFolder (), ec ) )
			if ( entry.path ().filename ().native ().find ( L".tmp-" ) != std::wstring::npos )
				return true;

		return false;
	}

	//-------------------------------------------------------------------------

	void checkNumbers ()
	{
		const auto	d = parsed ( "[0, -0, 1, -1, 9223372036854775807, -9223372036854775808, 9223372036854775808, 1.5, -2.25, 0.1, 1e300, 5e-324, "
								 "1.7976931348623157e308, 2.0, 1E2, -0.0, 1e-400]" );
		const int64_t	integers[] { 0, 0, 1, -1, INT64_MAX, INT64_MIN };
		const double	numbers[] { 9223372036854775808.0, 1.5, -2.25, 0.1, 1e300, 5e-324, 1.7976931348623157e308, 2.0, 100.0, -0.0, 0.0 };

		expect ( json::size ( d.get () ) == std::size ( integers ) + std::size ( numbers ), "the number list has %zu items", json::size ( d.get () ) );

		for ( size_t i = 0; i < std::size ( integers ); ++i )
		{
			int64_t		value = 0;
			const auto	item = json::at ( d.get (), i );
			expect ( json::get_type ( item ) == json::type::integer && json::get_int ( item, &value ) && value == integers[ i ], "item %zu is no integer %lld", i, integers[ i ] );
		}

		for ( size_t i = 0; i < std::size ( numbers ); ++i )
		{
			auto		value = 1.0;
			const auto	item = json::at ( d.get (), std::size ( integers ) + i );
			expect ( json::get_type ( item ) == json::type::number && json::get_number ( item, &value ) && value == numbers[ i ]
					 && std::signbit ( value ) == std::signbit ( numbers[ i ] ), "item %zu is no number %.17g", std::size ( integers ) + i, numbers[ i ] );
		}

		const auto	text = json::serialize ( d.get (), true );
		const auto	expected = "[0, 0, 1, -1, 9223372036854775807, -9223372036854775808, 9223372036854775808.0, 1.5, -2.25, 0.1, 1e+300, 5e-324, "
							   "1.7976931348623157e+308, 2.0, 100.0, -0.0, 0.0]";
		expect ( text == expected, "numbers serialize as '%s'", text.c_str () );

		const auto	again = parsed ( text );
		expect ( json::serialize ( again.get (), true ) == text, "numbers do not read back as written" );

		// Any double's bits and any int64 come back as they went
		std::mt19937_64	random ( 20261116 );
		size_t			doubleMisses = 0;
		size_t			integerMisses = 0;
		auto			number = json::create ( json::type::number );

		for ( auto i = 0; i < 20000; ++i )
		{
			const auto	bits = random ();
			double		value = 0.0;
			std::memcpy ( &value, &bits, sizeof ( value ) );

			if ( ! std::isfinite ( value ) )
				continue;

			(void) json::set_number ( number.get (), value );
			const auto	back = json::parse ( json::serialize ( number.get (), true ) );
			auto		read = 0.0;

			if ( json::get_type ( back.get () ) != json::type::number || ! json::get_number ( back.get (), &read ) || std::memcmp ( &read, &value, sizeof ( read ) ) != 0 )
				++doubleMisses;

			const auto	integer = static_cast<int64_t> ( random () );
			(void) json::set_int ( number.get (), integer );
			const auto	backInteger = json::parse ( json::serialize ( number.get (), true ) );
			int64_t		readInteger = 0;

			if ( json::get_type ( backInteger.get () ) != json::type::integer || ! json::get_int ( backInteger.get (), &readInteger ) || readInteger != integer )
				++integerMisses;
		}

		expect ( doubleMisses == 0 && integerMisses == 0, "%zu random doubles and %zu random integers did not round trip", doubleMisses, integerMisses );
		expect ( ! json::set_number ( number.get (), std::nan ( "" ) ) && ! json::set_number ( number.get (), HUGE_VAL ), "a number that is not finite was taken" );

		int64_t		integer = 0;
		const auto	integral = json::make_number ( 3.0 );
		const auto	fraction = json::make_number ( 3.5 );
		expect ( json::get_int ( integral.get (), &integer ) && integer == 3 && ! json::get_int ( fraction.get (), &integer ), "get_int on numbers is wrong" );
	}

	void checkStrings ()
	{
		const std::string	utf8 = "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
		const std::string	text = R"(["\"\\\/\b\f\n\r\t", "Aé€😀", ")" + utf8 + R"(", "a\u0000b", "\u001f", {"kéy": 1}])";
		const auto			d = parsed ( text );
		std::string_view	value;

		expect ( json::get_string ( json::at ( d.get (), 0 ), &value ) && value == "\"\\/\b\f\n\r\t", "escapes read as '%s'", printable ( value ).c_str () );
		expect ( json::get_string ( json::at ( d.get (), 1 ), &value ) && value == "A" + utf8, "\\u escapes read as '%s'", printable ( value ).c_str () );
		expect ( json::get_string ( json::at ( d.get (), 2 ), &value ) && value == utf8, "raw UTF-8 reads as '%s'", printable ( value ).c_str () );
		expect ( json::get_string ( json::at ( d.get (), 3 ), &value ) && value == std::string_view ( "a\0b", 3 ), "\\u0000 reads as '%s'", printable ( value ).c_str () );
		expect ( json::find ( json::at ( d.get (), 5 ), "k\xC3\xA9y" ) != nullptr, "a key with an escape is not found by its UTF-8" );

		const auto	written = json::serialize ( d.get (), true );
		const auto	expected = R"(["\"\\/\b\f\n\r\t", "A)" + utf8 + R"(", ")" + utf8 + R"(", "a\u0000b", "\u001f", {"k)" + std::string ( "\xC3\xA9" ) + R"(y": 1}])";
		expect ( written == expected, "strings serialize as '%s'", printable ( written ).c_str () );

		const auto	again = parsed ( written );
		expect ( json::equal ( again.get (), d.get () ), "strings do not read back as written" );

		auto	s = json::create ( json::type::string );
		expect ( ! json::set_string ( s.get (), "\xC3\x28" ) && ! json::set_string ( s.get (), "\xED\xA0\x80" ), "invalid UTF-8 was taken as a string" );
	}

	void checkStructure ()
	{
		const auto	d = parsed ( R"({"name":"x","n":1,"f":2.5,"t":true,"z":null,"e":{},"a":[],"list":[1,{"k":"v"}],"order":{"z":1,"a":2,"m":3}})" );
		const auto	expected = "{\n\t\"name\": \"x\",\n\t\"n\": 1,\n\t\"f\": 2.5,\n\t\"t\": true,\n\t\"z\": null,\n\t\"e\": {},\n\t\"a\": [],\n\t\"list\": [\n\t\t1,\n\t\t{\n"
							   "\t\t\t\"k\": \"v\"\n\t\t}\n\t],\n\t\"order\": {\n\t\t\"z\": 1,\n\t\t\"a\": 2,\n\t\t\"m\": 3\n\t}\n}\n";
		const auto	text = json::serialize ( d.get () );
		expect ( text == expected, "the file form is '%s'", printable ( text ).c_str () );
		expect ( json::serialize ( parsed ( text ).get () ) == text, "the file form does not read back as written" );

		std::string	keys;
		const auto	order = json::find ( d.get (), "order" );

		for ( size_t i = 0; i < json::size ( order ); ++i )
		{
			const char*	key = nullptr;
			(void) json::at ( order, i, &key );
			keys += key != nullptr ? key : "?";
		}

		expect ( keys == "zam", "members come in the order '%s'", keys.c_str () );

		std::string_view	value;
		expect ( json::get_string ( json::find_path ( d.get (), "list/1/k" ), &value ) && value == "v" && json::find_path ( d.get (), "/list/1/k" ) == json::find_path ( d.get (), "list/1/k" ),
				 "find_path does not find list/1/k" );
		expect ( json::find_path ( d.get (), "list/2" ) == nullptr && json::find_path ( d.get (), "list/01" ) == nullptr && json::find_path ( d.get (), "n/x" ) == nullptr,
				 "find_path finds what is not there" );
		expect ( json::find_path ( d.get (), "" ) == d.get (), "the empty path is not the value itself" );

		const auto	escaped = parsed ( R"({"a/b": {"~x": 1}})" );
		expect ( json::get_int_or ( json::find_path ( escaped.get (), "a~1b/~0x" ), 0 ) == 1, "~1 and ~0 in a path do not stand for / and ~" );

		const auto	deep = std::string ( 500, '[' ) + std::string ( 500, ']' );
		expect ( json::serialize ( parsed ( deep ).get (), true ) == deep, "500 nested arrays do not round trip" );

		auto		doc = parsed ( R"({"arr": [1], "n": 5})" );
		json::error	error;
		expect ( json::set_int ( json::make_path ( doc.get (), "arr/-" ), 2 ) && json::set_int ( json::make_path ( doc.get (), "arr/2" ), 3 ), "make_path does not append" );
		expect ( json::make_path ( doc.get (), "arr/5", &error ) == nullptr && error.code == json::status::bad_path, "make_path past an array's end: %s", error.message );
		expect ( json::make_path ( doc.get (), "n/x", &error ) == nullptr && error.code == json::status::bad_path, "make_path into an integer: %s", error.message );
		expect ( json::set_bool ( json::make_path ( doc.get (), "new/deep/key" ), true ), "make_path does not make objects" );
		expect ( compact ( doc.get () ) == R"({"arr": [1, 2, 3], "n": 5, "new": {"deep": {"key": true}}})", "after make_path: %s", compact ( doc.get () ).c_str () );

		expect ( json::remove_path ( doc.get (), "arr/0" ) && json::remove ( doc.get (), "n" ) && ! json::remove ( doc.get (), "n" ) && json::remove_at ( json::find ( doc.get (), "arr" ), 1 ),
				 "remove calls fail" );
		expect ( compact ( doc.get () ) == R"({"arr": [2], "new": {"deep": {"key": true}}})", "after removing: %s", compact ( doc.get () ).c_str () );

		const auto	inserted = json::insert ( json::find ( doc.get (), "arr" ), 0 );
		expect ( json::set_string ( inserted, "first" ) && json::insert ( json::find ( doc.get (), "arr" ), 9 ) == nullptr, "insert fails" );
		expect ( json::assign ( doc.get (), json::find ( doc.get (), "new" ) ), "assign from inside fails" );
		expect ( compact ( doc.get () ) == R"({"deep": {"key": true}})", "after assign from inside: %s", compact ( doc.get () ).c_str () );

		auto	fresh = json::create ( json::type::null );
		expect ( json::set_int ( json::member ( fresh.get (), "x" ), 1 ) && json::get_type ( fresh.get () ) == json::type::object, "member on null does not make an object" );

		const auto	member = json::copy ( json::find ( fresh.get (), "x" ) );
		expect ( json::get_int_or ( member.get (), 0 ) == 1, "copy of a member fails" );

		expect ( json::equal ( parsed ( R"({"a": 1, "b": [1, 2]})" ).get (), parsed ( R"({"b": [1, 2.0], "a": 1.0})" ).get () ), "equal objects in another order are not equal" );
		expect ( ! json::equal ( parsed ( "[1, 2]" ).get (), parsed ( "[2, 1]" ).get () ) && ! json::equal ( parsed ( "1" ).get (), parsed ( "1.5" ).get () ), "different values are equal" );
	}

	void checkBigFile ()
	{
		auto	root = json::create ( json::type::array );

		for ( auto i = 0; i < 100000; ++i )
		{
			const auto	item = json::append ( root.get () );
			(void) json::set_int ( json::member ( item, "id" ), i );
			(void) json::set_string ( json::member ( item, "name" ), "item " + std::to_string ( i ) + " \xC3\xA9" );
			(void) json::set_number ( json::member ( item, "value" ), i * 0.37 );
			const auto	flags = json::member ( item, "flags" );
			(void) json::set_bool ( json::append ( flags ), i % 2 == 0 );
			(void) json::set_type ( json::append ( flags ), json::type::null );
		}

		const auto	path = testPath ( L"big.json" );
		const auto	start = Clock::now ();
		json::error	error;
		const auto	isWritten = json::write_file ( path.c_str (), root.get (), &error );
		const auto	written = Clock::now ();
		const auto	back = json::read_file ( path.c_str (), &error );
		const auto	read = Clock::now ();

		expect ( isWritten && back && json::equal ( back.get (), root.get () ), "the big file does not round trip: %s", error.message );

		std::error_code	ec;
		const auto		size = std::filesystem::file_size ( std::filesystem::u8path ( path ), ec );
		reshade::log::message ( reshade::log::level::info, "Test application: json: a %.1f MB file written in %lld ms, read in %lld ms.", double ( size ) / ( 1024.0 * 1024.0 ),
								static_cast<long long> ( std::chrono::duration_cast<std::chrono::milliseconds> ( written - start ).count () ),
								static_cast<long long> ( std::chrono::duration_cast<std::chrono::milliseconds> ( read - written ).count () ) );
	}

	void checkMalformed ()
	{
		struct Case
		{
			std::string	text;
			uint32_t	line = 0;
			uint32_t	column = 0;
		};

		const Case	cases[] {
			{ R"({"a": 1,})", 1, 9 },
			{ "[1, 2", 1, 6 },
			{ "{\n\t\"a\": tru\n}", 2, 7 },
			{ R"({"a": 01})", 1, 7 },
			{ R"("abc)", 1, 1 },
			{ R"({"a":1 "b":2})", 1, 8 },
			{ "[1] x", 1, 5 },
			{ R"({"a":1,"a":2})", 1, 8 },
			{ "\"\x01\"", 1, 2 },
			{ R"(["\ud800"])", 1, 3 },
			{ R"(["\x"])", 1, 3 },
			{ "\"\xC3\x28\"", 1, 2 },
			{ "[\"\xC3\xA9\", x]", 1, 7 },
			{ "", 1, 1 },
			{ "   ", 1, 4 },
			{ "1e999", 1, 1 },
			{ "-", 1, 1 },
			{ "1.", 1, 3 },
			{ "{\"a\" 1}", 1, 6 },
			{ "\r\n[\r\n1,\r\n]", 4, 1 },
			{ std::string ( 600, '[' ) + std::string ( 600, ']' ), 1, 513 },
		};

		for ( const auto& c : cases )
		{
			json::error	error;
			const auto	d = json::parse ( c.text, &error );
			expect ( ! d && error.code == json::status::not_json && error.line == c.line && error.column == c.column && error.message[ 0 ] != '\0',
					 "'%s' gave line %u, column %u instead of %u, %u: %s", printable ( c.text ).c_str (), error.line, error.column, c.line, c.column, error.message );
		}

		json::error	example;
		(void) json::parse ( cases[ 2 ].text, &example );
		reshade::log::message ( reshade::log::level::info, "Test application: json: a malformed text reads as: %s", example.message );

		const auto	bad = testPath ( L"bad.json" );
		json::error	error;
		expect ( writeBytes ( bad, "{\n\t\"a\": [1, 2,]\n}\n" ), "bad.json can't be written" );
		expect ( ! json::read_file ( bad.c_str (), &error ) && error.code == json::status::not_json && error.line == 2 && error.column == 13
				 && std::strstr ( error.message, "bad.json" ) != nullptr, "a malformed file reads as: %s", error.message );

		expect ( ! json::read_file ( testPath ( L"nothing-here.json" ).c_str (), &error ) && error.code == json::status::missing, "a missing file reads as: %s", error.message );
		expect ( ! json::read_file ( testPath ( L"no-folder\\x.json" ).c_str (), &error ) && error.code == json::status::missing, "a file in a missing folder reads as: %s", error.message );

		const auto	locked = testPath ( L"locked.json" );
		expect ( writeBytes ( locked, "{}" ), "locked.json can't be written" );
		const auto	handle = CreateFileW ( std::filesystem::u8path ( locked ).c_str (), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
		expect ( ! json::read_file ( locked.c_str (), &error ) && error.code == json::status::unreadable && error.system_error == ERROR_SHARING_VIOLATION,
				 "a file open elsewhere without sharing reads as: %s", error.message );
		CloseHandle ( handle );

		expect ( json::read_file ( locked.c_str (), &error ) && error.code == json::status::ok, "a closed file does not read: %s", error.message );
		auto	flag = false;
		expect ( json::parse ( "\xEF\xBB\xBF{}" ) && ! json::get_bool ( json::make_int ( 1 ).get (), &flag ), "a BOM is not skipped, or a wrong type reads" );
	}

	void checkAtomicWrite ()
	{
		const auto	path = testPath ( L"atomic.json" );
		const auto	first = parsed ( R"({"version": 1, "list": [1, 2, 3]})" );
		const auto	second = parsed ( R"({"version": 2})" );
		const auto	firstText = "{\n\t\"version\": 1,\n\t\"list\": [\n\t\t1,\n\t\t2,\n\t\t3\n\t]\n}\n";
		json::error	error;

		expect ( json::write_file ( path.c_str (), first.get (), &error ), "atomic.json is not written: %s", error.message );
		expect ( readBytes ( path ) == firstText, "the written file is '%s'", printable ( readBytes ( path ) ).c_str () );

		for ( const auto fault : { reshade::ultra::JsonWriteFault::partialWrite, reshade::ultra::JsonWriteFault::beforeReplace } )
		{
			reshade::ultra::setJsonWriteFault ( fault );
			const auto	isWritten = json::write_file ( path.c_str (), second.get (), &error );
			reshade::ultra::setJsonWriteFault ( reshade::ultra::JsonWriteFault::none );

			expect ( ! isWritten && error.code == json::status::write_failed && error.system_error != 0, "a failing write (fault %d) reported: %s", static_cast<int> ( fault ), error.message );
			expect ( readBytes ( path ) == firstText && ! hasTemporaryFiles (), "a failing write (fault %d) left '%s'", static_cast<int> ( fault ), printable ( readBytes ( path ) ).c_str () );
		}

		// Open without delete sharing, the old file can't be replaced
		const auto	handle = CreateFileW ( std::filesystem::u8path ( path ).c_str (), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
		const auto	isWritten = json::write_file ( path.c_str (), second.get (), &error );
		CloseHandle ( handle );

		expect ( ! isWritten && error.code == json::status::write_failed && readBytes ( path ) == firstText && ! hasTemporaryFiles (), "a write over a held file reported: %s", error.message );
		reshade::log::message ( reshade::log::level::info, "Test application: json: a write over a held file reads as: %s", error.message );

		expect ( json::write_file ( path.c_str (), second.get (), &error ) && readBytes ( path ) == "{\n\t\"version\": 2\n}\n", "the write after it fails: %s", error.message );
		expect ( json::write_file ( testPath ( L"sub\\deeper\\nested.json" ).c_str (), second.get (), &error ), "a write into missing folders fails: %s", error.message );
		expect ( ! json::write_file ( path.c_str (), nullptr, &error ) && error.code == json::status::bad_argument, "a write without a value reported: %s", error.message );
	}

	void checkLayers ()
	{
		const auto	defaults = parsed ( R"({"bloom": {"strength": 1.0, "radius": 3}, "streaks": {"kinds": [{"len": 1}, {"len": 2}]}, "name": "x", "list": [1, 2, 3]})" );
		auto		user = parsed ( "{}" );
		json::error	error;

		const auto	get = [ &defaults, &user ] ( const char* path ) { return compact ( json::layer_get ( defaults.get (), user.get (), path ).get () ); };
		const auto	set = [ &defaults, &user, &error ] ( const char* path, const json::document& item ) { return json::layer_set ( defaults.get (), user.get (), path, item.get (), &error ); };
		const auto	reset = [ &defaults, &user, &error ] ( const char* path ) { return json::layer_reset ( defaults.get (), user.get (), path, &error ); };
		const auto	isUser = [ &user ] ( const char* expected, const char* step )
		{
			const auto	text = compact ( user.get () );
			expect ( text == expected, "after %s the user layer is %s, not %s", step, text.c_str (), expected );
		};

		expect ( get ( "bloom/strength" ) == "1.0" && get ( "missing" ) == "(none)", "a default does not read through an empty user layer" );

		expect ( set ( "bloom/strength", json::make_number ( 2.0 ) ), "layer_set fails: %s", error.message );
		isUser ( R"({"bloom": {"strength": 2.0}})", "setting bloom/strength" );
		expect ( get ( "bloom" ) == R"({"strength": 2.0, "radius": 3})" && get ( "bloom/radius" ) == "3", "objects do not merge: %s", get ( "bloom" ).c_str () );

		expect ( set ( "bloom/strength", json::make_number ( 1.0 ) ), "layer_set fails: %s", error.message );
		isUser ( "{}", "setting bloom/strength back to its default" );

		expect ( set ( "bloom/strength", json::make_int ( 2 ) ) && set ( "bloom/strength", json::make_int ( 1 ) ), "layer_set fails: %s", error.message );
		isUser ( "{}", "setting the integer 1 over the default 1.0" );

		expect ( set ( "list/1", json::make_int ( 9 ) ), "layer_set into an array fails: %s", error.message );
		isUser ( R"({"list": [1, 9, 3]})", "setting list/1" );
		expect ( set ( "list/1", json::make_int ( 2 ) ), "layer_set into an array fails: %s", error.message );
		isUser ( "{}", "setting list/1 back" );

		expect ( set ( "newgroup/x", json::make_int ( 5 ) ) && get ( "newgroup/x" ) == "5", "a member the defaults lack is not set: %s", error.message );
		isUser ( R"({"newgroup": {"x": 5}})", "setting newgroup/x" );
		expect ( reset ( "newgroup/x" ), "layer_reset fails: %s", error.message );
		isUser ( "{}", "resetting newgroup/x" );

		expect ( set ( "name", json::make_string ( "y" ) ) && reset ( "name" ), "a string is not set and reset: %s", error.message );
		isUser ( "{}", "setting and resetting name" );

		expect ( set ( "bloom", json::make_int ( 7 ) ), "a scalar over an object fails: %s", error.message );
		isUser ( R"({"bloom": 7})", "setting bloom to a scalar" );
		expect ( get ( "bloom/radius" ) == "(none)", "a member reads through a scalar that replaced its object" );
		expect ( ! set ( "bloom/radius", json::make_int ( 4 ) ) && error.code == json::status::bad_path, "a path through a scalar reported: %s", error.message );
		expect ( reset ( "bloom" ), "layer_reset fails: %s", error.message );
		isUser ( "{}", "resetting bloom" );

		expect ( set ( "bloom", parsed ( R"({"strength": 1.0, "radius": 5})" ) ), "an object over an object fails: %s", error.message );
		isUser ( R"({"bloom": {"radius": 5}})", "setting bloom to an object" );
		expect ( get ( "bloom" ) == R"({"strength": 1.0, "radius": 5})", "an object set does not merge: %s", get ( "bloom" ).c_str () );
		expect ( reset ( "bloom" ), "layer_reset fails: %s", error.message );

		user = parsed ( R"({"streaks": {"kinds": [{"len": 7}]}})" );
		expect ( get ( "streaks/kinds" ) == R"([{"len": 7}])" && get ( "streaks/kinds/1" ) == "(none)", "arrays merge instead of replacing: %s", get ( "streaks/kinds" ).c_str () );
		expect ( set ( "streaks/kinds/0/len", json::make_int ( 1 ) ), "a change inside a user array fails: %s", error.message );
		isUser ( R"({"streaks": {"kinds": [{"len": 1}]}})", "changing an item of a user array" );

		const auto	a = parsed ( R"({"a": 1, "b": 2})" );
		const auto	b = parsed ( R"({"c": 3, "a": 5})" );
		expect ( compact ( json::layer_get ( a.get (), b.get () ).get () ) == R"({"a": 5, "b": 2, "c": 3})", "the merge order is wrong" );
		expect ( compact ( json::layer_get ( nullptr, b.get (), "c" ).get () ) == "3" && compact ( json::layer_get ( a.get (), nullptr, "b" ).get () ) == "2", "a missing layer is wrong" );

		auto	empty = json::create ( json::type::null );
		expect ( json::layer_set ( defaults.get (), empty.get (), "bloom/radius", json::make_int ( 4 ).get (), &error ) && compact ( empty.get () ) == R"({"bloom": {"radius": 4}})",
				 "a null user layer is not taken as empty: %s %s", compact ( empty.get () ).c_str (), error.message );

		user = parsed ( "{}" );
		expect ( json::layer_set ( defaults.get (), user.get (), "", json::copy ( defaults.get () ).get (), &error ), "setting the whole document fails: %s", error.message );
		isUser ( "{}", "setting the defaults as the whole document" );
		expect ( ! set ( "a/~2", json::make_int ( 1 ) ) && error.code == json::status::bad_path, "a malformed path reported: %s", error.message );
	}

	//-------------------------------------------------------------------------
	// Change notifications, across presents: two watchers of one file with owners of their own

	enum class Phase
	{
		start,
		listening,		// the folder watch starts reading
		ownWrite,		// owner A wrote: only B is told
		otherWrite,		// someone without the API wrote: both are told
		secondOwner,	// owner B wrote: only A is told
		unwatched,		// B unwatched, someone wrote: only A is told
		done
	};

	struct Watching
	{
		Phase				phase = Phase::start;
		Clock::time_point	since;
		uint64_t			idA = 0;
		uint64_t			idB = 0;
		int					countA = 0;
		int					countB = 0;
		DWORD				presentThread = 0;
		bool				isOffThread = false;
		std::string			path;
	};

	Watching	watching;
	int			ownerA = 0;		// their addresses are the owners
	int			ownerB = 0;

	void onChanged ( void* context, const char* path, reshade::api::file_change change )
	{
		++*static_cast<int*> ( context );
		watching.isOffThread = watching.isOffThread || GetCurrentThreadId () != watching.presentThread;
		reshade::log::message ( reshade::log::level::info, "Test application: json: watcher %c told of '%s' (change %u).", context == &watching.countA ? 'A' : 'B', path,
								static_cast<uint32_t> ( change ) );
	}

	void enter ( Phase phase )
	{
		watching.phase = phase;
		watching.since = Clock::now ();
	}

	// Waited long enough for the expected counts, and for any unexpected change to show
	[[ nodiscard ]] bool isSettled ( int expectedA, int expectedB )
	{
		const auto	elapsed = Clock::now () - watching.since;
		return elapsed >= quietCheck && ( ( watching.countA >= expectedA && watching.countB >= expectedB ) || elapsed >= deliveryTimeout );
	}

	void expectCounts ( int expectedA, int expectedB, const char* step )
	{
		expect ( watching.countA == expectedA && watching.countB == expectedB, "after %s watcher A was told %d times and B %d times, not %d and %d", step, watching.countA,
				 watching.countB, expectedA, expectedB );
	}

	void writeAs ( int* owner, int version )
	{
		json::error	error;
		const auto	d = json::make_int ( version );
		expect ( json::write_file ( watching.path.c_str (), d.get (), &error, owner ), "watched.json is not written: %s", error.message );
	}

	void stepWatching ()
	{
		switch ( watching.phase )
		{
			case Phase::start:
				watching.path = testPath ( L"watched.json" );
				writeAs ( &ownerA, 0 );
				watching.idA = json::watch ( watching.path.c_str (), onChanged, &watching.countA, &ownerA );
				watching.idB = json::watch ( watching.path.c_str (), onChanged, &watching.countB, &ownerB );
				expect ( watching.idA != 0 && watching.idB != 0 && watching.idA != watching.idB, "watching watched.json fails" );
				enter ( Phase::listening );
				return;

			case Phase::listening:
				if ( Clock::now () - watching.since < std::chrono::milliseconds ( 500 ) )
					return;

				writeAs ( &ownerA, 1 );
				enter ( Phase::ownWrite );
				return;

			case Phase::ownWrite:
				if ( ! isSettled ( 0, 1 ) )
					return;

				expectCounts ( 0, 1, "a write by owner A" );
				expect ( writeBytes ( watching.path, "2" ), "watched.json is not written without the API" );
				enter ( Phase::otherWrite );
				return;

			case Phase::otherWrite:
				if ( ! isSettled ( 1, 2 ) )
					return;

				expectCounts ( 1, 2, "a write without the API" );
				writeAs ( &ownerB, 3 );
				enter ( Phase::secondOwner );
				return;

			case Phase::secondOwner:
				if ( ! isSettled ( 2, 2 ) )
					return;

				expectCounts ( 2, 2, "a write by owner B" );
				json::unwatch ( watching.idB );
				expect ( writeBytes ( watching.path, "4" ), "watched.json is not written without the API" );
				enter ( Phase::unwatched );
				return;

			case Phase::unwatched:
				if ( ! isSettled ( 3, 2 ) )
					return;

				expectCounts ( 3, 2, "B unwatched and a write without the API" );
				json::unwatch ( watching.idA );
				expect ( ! watching.isOffThread, "a change callback ran on another thread than the presenting one" );
				enter ( Phase::done );
				reshade::log::message ( checks.failed == 0 ? reshade::log::level::info : reshade::log::level::error, "Test application: json: %zu checks passed, %zu failed.",
										checks.passed, checks.failed );
				return;

			case Phase::done:
				return;
		}
	}
}
//-----------------------------------------------------------------------------

bool reshade::ultra::isJsonTestRequested ()
{
	static const auto	isRequested = []
	{
		auto		argc = 0;
		const auto	argv = CommandLineToArgvW ( GetCommandLineW (), &argc );
		auto		isFound = false;

		for ( auto i = 1; argv != nullptr && i < argc; ++i )
			isFound = isFound || _wcsicmp ( argv[ i ], L"-json-tests" ) == 0;

		LocalFree ( argv );
		return isFound;
	} ();

	return isRequested;
}
//-----------------------------------------------------------------------------

void reshade::ultra::stepJsonTests ( void* window )
{
	if ( watching.phase == Phase::start )
	{
		watching.presentThread = GetCurrentThreadId ();

		std::error_code	ec;
		std::filesystem::remove_all ( testFolder (), ec );
		std::filesystem::create_directories ( testFolder (), ec );

		const auto	start = Clock::now ();
		checkNumbers ();
		checkStrings ();
		checkStructure ();
		checkMalformed ();
		checkAtomicWrite ();
		checkLayers ();
		checkBigFile ();

		reshade::log::message ( reshade::log::level::info, "Test application: json: %zu checks in %lld ms, the change notifications follow.", checks.passed + checks.failed,
								static_cast<long long> ( std::chrono::duration_cast<std::chrono::milliseconds> ( Clock::now () - start ).count () ) );
	}

	if ( watching.phase == Phase::done )
		return;

	stepWatching ();

	if ( watching.phase == Phase::done )
		PostMessageW ( static_cast<HWND> ( window ), WM_CLOSE, 0, 0 );
}
//-----------------------------------------------------------------------------

void reshade::ultra::finishJsonTests ()
{
	if ( isJsonTestRequested () && watching.phase != Phase::done )
		reshade::log::message ( reshade::log::level::error, "Test application: json: the run ended before the change notification checks were done." );
}
//-----------------------------------------------------------------------------

#endif
