/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "reshade.hpp"

// ultraReShade's json files for add-ons and the modules they load: read, parse, serialize and write, a value model that
// lives in ReShade's DLL (add-ons hold pointers to it, never C++ objects of their own runtime), defaults and user layers,
// and change notifications. Paths are UTF-8, absolute or relative to ReShade's base path.
//
// Files are written atomically (a temporary file in the same folder, flushed, then renamed over the old one), as UTF-8
// without BOM, LF line ends, one tab per level, members in the order they hold.
//
// Threads: every function may be called from any thread. A value tree is not locked: one thread changes it at a time,
// and nobody reads it meanwhile; different trees are independent. Reading and writing files blocks the calling thread on
// file I/O, so keep it off threads that must not wait (a game's render thread). Change callbacks run on ReShade's
// presenting thread, one at a time; unwatch from another thread waits until a callback running then has returned.

namespace reshade::json
{
	// Opaque, owned by ReShade's DLL. A tree is freed through its root; a member's or item's pointer stays valid until it
	// is removed, its container gets another value, or the tree is freed
	struct value;

	enum class type : uint32_t
	{
		null,
		boolean,
		integer,	// int64; integer literals outside its range read as number
		number,		// double, always written so that it reads back as the same double and as a number
		string,		// UTF-8
		array,
		object		// members keep their order
	};

	enum class status : uint32_t
	{
		ok,
		unavailable,	// a ReShade without these functions
		bad_argument,	// a path, value or layer missing, or a path that is not UTF-8
		missing,		// the file does not exist
		unreadable,		// it exists but can't be read (system_error)
		not_json,		// line and column say where it stops being json
		bad_path,		// the path runs into a value that is neither object nor array, or past an array's end
		write_failed	// the file is as it was before (system_error)
	};

	// Fixed layout, never grows
	struct error
	{
		status		code = status::ok;
		uint32_t	line = 0;			// not_json: from 1
		uint32_t	column = 0;			// not_json: from 1, in characters
		uint32_t	system_error = 0;	// the failed step's Windows error code
		char		message[ 512 ] = {};	// one line for a log, naming the file
	};

	// Paths into a tree: segments separated by '/', a leading '/' optional, the empty path is the value itself. A segment
	// is a member's key, or an item's index when the value there is an array; "~1" stands for '/' and "~0" for '~' in keys
	// (as in JSON Pointer)

	// path: the file as watched, change: what happened to it
	using changed_callback = void ( * ) ( void* context, const char* path, api::file_change change );
}

#if defined(RESHADE_API_LIBRARY)

RESHADE_API_LIBRARY_DECL reshade::json::value* ReShadeJsonCreate ( reshade::json::type type );
RESHADE_API_LIBRARY_DECL reshade::json::value* ReShadeJsonCopy ( const reshade::json::value* source );
RESHADE_API_LIBRARY_DECL void ReShadeJsonFree ( reshade::json::value* root );

RESHADE_API_LIBRARY_DECL reshade::json::type ReShadeJsonGetType ( const reshade::json::value* value );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonGetBool ( const reshade::json::value* value, bool* out );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonGetInt ( const reshade::json::value* value, int64_t* out );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonGetNumber ( const reshade::json::value* value, double* out );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonGetString ( const reshade::json::value* value, const char** text, size_t* length );
RESHADE_API_LIBRARY_DECL size_t ReShadeJsonGetSize ( const reshade::json::value* value );
RESHADE_API_LIBRARY_DECL reshade::json::value* ReShadeJsonAt ( const reshade::json::value* container, size_t index, const char** key );
RESHADE_API_LIBRARY_DECL reshade::json::value* ReShadeJsonFind ( const reshade::json::value* object, const char* key );
RESHADE_API_LIBRARY_DECL reshade::json::value* ReShadeJsonFindPath ( const reshade::json::value* root, const char* path );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonEqual ( const reshade::json::value* a, const reshade::json::value* b );

RESHADE_API_LIBRARY_DECL bool ReShadeJsonSetType ( reshade::json::value* value, reshade::json::type type );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonSetBool ( reshade::json::value* value, bool flag );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonSetInt ( reshade::json::value* value, int64_t integer );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonSetNumber ( reshade::json::value* value, double number );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonSetString ( reshade::json::value* value, const char* text, size_t length );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonSetComment ( reshade::json::value* value, const char* text, size_t length );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonAssign ( reshade::json::value* target, const reshade::json::value* source );
RESHADE_API_LIBRARY_DECL reshade::json::value* ReShadeJsonMember ( reshade::json::value* object, const char* key );
RESHADE_API_LIBRARY_DECL reshade::json::value* ReShadeJsonInsert ( reshade::json::value* array, size_t index );
RESHADE_API_LIBRARY_DECL reshade::json::value* ReShadeJsonMakePath ( reshade::json::value* root, const char* path, reshade::json::error* error );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonRemove ( reshade::json::value* object, const char* key );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonRemoveAt ( reshade::json::value* container, size_t index );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonRemovePath ( reshade::json::value* root, const char* path );

RESHADE_API_LIBRARY_DECL reshade::json::value* ReShadeJsonParse ( const char* text, size_t size, reshade::json::error* error );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonSerialize ( const reshade::json::value* value, bool compact, char* text, size_t* size );
RESHADE_API_LIBRARY_DECL reshade::json::value* ReShadeJsonReadFile ( const char* path, reshade::json::error* error );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonWriteFile ( void* owner, const char* path, const reshade::json::value* value, reshade::json::error* error );

RESHADE_API_LIBRARY_DECL reshade::json::value* ReShadeJsonLayerGet ( const reshade::json::value* defaults, const reshade::json::value* user, const char* path );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonLayerSet ( const reshade::json::value* defaults, reshade::json::value* user, const char* path, const reshade::json::value* item, reshade::json::error* error );
RESHADE_API_LIBRARY_DECL bool ReShadeJsonLayerReset ( const reshade::json::value* defaults, reshade::json::value* user, const char* path, reshade::json::error* error );

RESHADE_API_LIBRARY_DECL uint64_t ReShadeJsonWatch ( void* owner, const char* path, reshade::json::changed_callback callback, void* context );
RESHADE_API_LIBRARY_DECL void ReShadeJsonUnwatch ( uint64_t id );

#endif

namespace reshade::json
{
	namespace internal
	{
		struct functions
		{
			value* ( *create ) ( type ) = nullptr;
			value* ( *copy ) ( const value* ) = nullptr;
			void ( *free ) ( value* ) = nullptr;
			type ( *get_type ) ( const value* ) = nullptr;
			bool ( *get_bool ) ( const value*, bool* ) = nullptr;
			bool ( *get_int ) ( const value*, int64_t* ) = nullptr;
			bool ( *get_number ) ( const value*, double* ) = nullptr;
			bool ( *get_string ) ( const value*, const char**, size_t* ) = nullptr;
			size_t ( *get_size ) ( const value* ) = nullptr;
			value* ( *at ) ( const value*, size_t, const char** ) = nullptr;
			value* ( *find ) ( const value*, const char* ) = nullptr;
			value* ( *find_path ) ( const value*, const char* ) = nullptr;
			bool ( *equal ) ( const value*, const value* ) = nullptr;
			bool ( *set_type ) ( value*, type ) = nullptr;
			bool ( *set_bool ) ( value*, bool ) = nullptr;
			bool ( *set_int ) ( value*, int64_t ) = nullptr;
			bool ( *set_number ) ( value*, double ) = nullptr;
			bool ( *set_string ) ( value*, const char*, size_t ) = nullptr;
			bool ( *set_comment ) ( value*, const char*, size_t ) = nullptr;
			bool ( *assign ) ( value*, const value* ) = nullptr;
			value* ( *member ) ( value*, const char* ) = nullptr;
			value* ( *insert ) ( value*, size_t ) = nullptr;
			value* ( *make_path ) ( value*, const char*, error* ) = nullptr;
			bool ( *remove ) ( value*, const char* ) = nullptr;
			bool ( *remove_at ) ( value*, size_t ) = nullptr;
			bool ( *remove_path ) ( value*, const char* ) = nullptr;
			value* ( *parse ) ( const char*, size_t, error* ) = nullptr;
			bool ( *serialize ) ( const value*, bool, char*, size_t* ) = nullptr;
			value* ( *read_file ) ( const char*, error* ) = nullptr;
			bool ( *write_file ) ( void*, const char*, const value*, error* ) = nullptr;
			value* ( *layer_get ) ( const value*, const value*, const char* ) = nullptr;
			bool ( *layer_set ) ( const value*, value*, const char*, const value*, error* ) = nullptr;
			bool ( *layer_reset ) ( const value*, value*, const char*, error* ) = nullptr;
			uint64_t ( *watch ) ( void*, const char*, changed_callback, void* ) = nullptr;
			void ( *unwatch ) ( uint64_t ) = nullptr;
		};

#if defined(RESHADE_API_LIBRARY)
		inline const functions& get ()
		{
			static const functions	table { &ReShadeJsonCreate, &ReShadeJsonCopy, &ReShadeJsonFree, &ReShadeJsonGetType, &ReShadeJsonGetBool, &ReShadeJsonGetInt,
											&ReShadeJsonGetNumber, &ReShadeJsonGetString, &ReShadeJsonGetSize, &ReShadeJsonAt, &ReShadeJsonFind, &ReShadeJsonFindPath,
											&ReShadeJsonEqual, &ReShadeJsonSetType, &ReShadeJsonSetBool, &ReShadeJsonSetInt, &ReShadeJsonSetNumber, &ReShadeJsonSetString,
											&ReShadeJsonSetComment, &ReShadeJsonAssign, &ReShadeJsonMember, &ReShadeJsonInsert, &ReShadeJsonMakePath, &ReShadeJsonRemove, &ReShadeJsonRemoveAt,
											&ReShadeJsonRemovePath, &ReShadeJsonParse, &ReShadeJsonSerialize, &ReShadeJsonReadFile, &ReShadeJsonWriteFile,
											&ReShadeJsonLayerGet, &ReShadeJsonLayerSet, &ReShadeJsonLayerReset, &ReShadeJsonWatch, &ReShadeJsonUnwatch };
			return table;
		}
#else
		template <typename Function>
		void bind_export ( HMODULE module, Function& function, const char* name )
		{
			function = module != nullptr ? reinterpret_cast<Function> ( GetProcAddress ( module, name ) ) : nullptr;
		}

		// Looked up once; a function this ReShade lacks stays null
		inline const functions& get ()
		{
			static const functions	table = []
			{
				const auto	module = reshade::internal::get_reshade_module_handle ();
				functions	f;
				bind_export ( module, f.create, "ReShadeJsonCreate" );
				bind_export ( module, f.copy, "ReShadeJsonCopy" );
				bind_export ( module, f.free, "ReShadeJsonFree" );
				bind_export ( module, f.get_type, "ReShadeJsonGetType" );
				bind_export ( module, f.get_bool, "ReShadeJsonGetBool" );
				bind_export ( module, f.get_int, "ReShadeJsonGetInt" );
				bind_export ( module, f.get_number, "ReShadeJsonGetNumber" );
				bind_export ( module, f.get_string, "ReShadeJsonGetString" );
				bind_export ( module, f.get_size, "ReShadeJsonGetSize" );
				bind_export ( module, f.at, "ReShadeJsonAt" );
				bind_export ( module, f.find, "ReShadeJsonFind" );
				bind_export ( module, f.find_path, "ReShadeJsonFindPath" );
				bind_export ( module, f.equal, "ReShadeJsonEqual" );
				bind_export ( module, f.set_type, "ReShadeJsonSetType" );
				bind_export ( module, f.set_bool, "ReShadeJsonSetBool" );
				bind_export ( module, f.set_int, "ReShadeJsonSetInt" );
				bind_export ( module, f.set_number, "ReShadeJsonSetNumber" );
				bind_export ( module, f.set_string, "ReShadeJsonSetString" );
				bind_export ( module, f.set_comment, "ReShadeJsonSetComment" );
				bind_export ( module, f.assign, "ReShadeJsonAssign" );
				bind_export ( module, f.member, "ReShadeJsonMember" );
				bind_export ( module, f.insert, "ReShadeJsonInsert" );
				bind_export ( module, f.make_path, "ReShadeJsonMakePath" );
				bind_export ( module, f.remove, "ReShadeJsonRemove" );
				bind_export ( module, f.remove_at, "ReShadeJsonRemoveAt" );
				bind_export ( module, f.remove_path, "ReShadeJsonRemovePath" );
				bind_export ( module, f.parse, "ReShadeJsonParse" );
				bind_export ( module, f.serialize, "ReShadeJsonSerialize" );
				bind_export ( module, f.read_file, "ReShadeJsonReadFile" );
				bind_export ( module, f.write_file, "ReShadeJsonWriteFile" );
				bind_export ( module, f.layer_get, "ReShadeJsonLayerGet" );
				bind_export ( module, f.layer_set, "ReShadeJsonLayerSet" );
				bind_export ( module, f.layer_reset, "ReShadeJsonLayerReset" );
				bind_export ( module, f.watch, "ReShadeJsonWatch" );
				bind_export ( module, f.unwatch, "ReShadeJsonUnwatch" );
				return f;
			} ();
			return table;
		}
#endif

		inline void report_unavailable ( error* out )
		{
			if ( out == nullptr )
				return;

			constexpr char	text[] = "This ReShade has no json functions (ultraReShade's add-on API).";
			*out = {};
			out->code = status::unavailable;
			std::char_traits<char>::copy ( out->message, text, sizeof ( text ) );
		}

		// The calling add-on or module; null lets ReShade find it from the caller's address
		inline void* current_owner ()
		{
#if defined(RESHADE_API_LIBRARY)
			return nullptr;
#else
			return reshade::internal::get_current_module_handle ();
#endif
		}
	}

	// Owns a tree (a root value) and frees it
	class document
	{
	public:
		document () = default;
		explicit document ( value* owned ) : root ( owned ) {}
		document ( document&& other ) noexcept : root ( std::exchange ( other.root, nullptr ) ) {}
		document& operator= ( document&& other ) noexcept { reset ( std::exchange ( other.root, nullptr ) ); return *this; }
		~document () { reset (); }

		document ( const document& ) = delete;
		document& operator= ( const document& ) = delete;

		[[ nodiscard ]] value* get () const { return root; }
		[[ nodiscard ]] value* release () { return std::exchange ( root, nullptr ); }
		[[ nodiscard ]] explicit operator bool () const { return root != nullptr; }

		void reset ( value* other = nullptr )
		{
			if ( root != nullptr && internal::get ().free != nullptr )
				internal::get ().free ( root );

			root = other;
		}

	private:
		value*	root = nullptr;
	};

	// A new empty value (null, false, 0, 0.0, "", [] or {}); empty on a ReShade without these functions
	[[ nodiscard ]] inline document create ( type kind ) { const auto f = internal::get ().create; return document ( f != nullptr ? f ( kind ) : nullptr ); }
	[[ nodiscard ]] inline document copy ( const value* source ) { const auto f = internal::get ().copy; return document ( f != nullptr ? f ( source ) : nullptr ); }

	[[ nodiscard ]] inline type get_type ( const value* v ) { const auto f = internal::get ().get_type; return f != nullptr ? f ( v ) : type::null; }

	// False, out untouched, for a missing value or one of another type. get_int takes a number with an integral value in
	// int64's range, get_number an integer
	[[ nodiscard ]] inline bool get_bool ( const value* v, bool* out ) { const auto f = internal::get ().get_bool; return f != nullptr && f ( v, out ); }
	[[ nodiscard ]] inline bool get_int ( const value* v, int64_t* out ) { const auto f = internal::get ().get_int; return f != nullptr && f ( v, out ); }
	[[ nodiscard ]] inline bool get_number ( const value* v, double* out ) { const auto f = internal::get ().get_number; return f != nullptr && f ( v, out ); }

	// Valid until the value changes or is freed; may hold zero characters
	[[ nodiscard ]] inline bool get_string ( const value* v, std::string_view* out )
	{
		const auto	f = internal::get ().get_string;
		const char*	text = nullptr;
		size_t		length = 0;

		if ( f == nullptr || ! f ( v, &text, &length ) )
			return false;

		*out = std::string_view ( text, length );
		return true;
	}

	[[ nodiscard ]] inline bool get_bool_or ( const value* v, bool fallback ) { auto out = fallback; return get_bool ( v, &out ) ? out : fallback; }
	[[ nodiscard ]] inline int64_t get_int_or ( const value* v, int64_t fallback ) { auto out = fallback; return get_int ( v, &out ) ? out : fallback; }
	[[ nodiscard ]] inline double get_number_or ( const value* v, double fallback ) { auto out = fallback; return get_number ( v, &out ) ? out : fallback; }
	[[ nodiscard ]] inline std::string_view get_string_or ( const value* v, std::string_view fallback ) { auto out = fallback; return get_string ( v, &out ) ? out : fallback; }

	// Members or items; 0 for anything else
	[[ nodiscard ]] inline size_t size ( const value* v ) { const auto f = internal::get ().get_size; return f != nullptr ? f ( v ) : 0; }

	// An array's item or an object's member in order (key: the member's key, null for an item); null past the end
	[[ nodiscard ]] inline value* at ( const value* container, size_t index, const char** key = nullptr ) { const auto f = internal::get ().at; return f != nullptr ? f ( container, index, key ) : nullptr; }
	[[ nodiscard ]] inline value* find ( const value* object, const char* key ) { const auto f = internal::get ().find; return f != nullptr ? f ( object, key ) : nullptr; }
	[[ nodiscard ]] inline value* find_path ( const value* root, const char* path ) { const auto f = internal::get ().find_path; return f != nullptr ? f ( root, path ) : nullptr; }

	// Objects compare without regard to member order, integers and numbers by value
	[[ nodiscard ]] inline bool equal ( const value* a, const value* b ) { const auto f = internal::get ().equal; return f != nullptr && f ( a, b ); }

	// The value becomes an empty one of that type, or the scalar given; false for a null pointer, a number that is not
	// finite, invalid UTF-8
	inline bool set_type ( value* v, type kind ) { const auto f = internal::get ().set_type; return f != nullptr && f ( v, kind ); }
	inline bool set_bool ( value* v, bool flag ) { const auto f = internal::get ().set_bool; return f != nullptr && f ( v, flag ); }
	inline bool set_int ( value* v, int64_t integer ) { const auto f = internal::get ().set_int; return f != nullptr && f ( v, integer ); }
	inline bool set_number ( value* v, double number ) { const auto f = internal::get ().set_number; return f != nullptr && f ( v, number ); }
	inline bool set_string ( value* v, std::string_view text ) { const auto f = internal::get ().set_string; return f != nullptr && f ( v, text.data (), text.size () ); }

	// A line comment written after the value in the file (not in the compact form, never read back); empty takes it off, a line break
	// becomes a space. False for a null pointer or invalid UTF-8. Files are read with their line comments skipped
	inline bool set_comment ( value* v, std::string_view text ) { const auto f = internal::get ().set_comment; return f != nullptr && f ( v, text.data (), text.size () ); }

	// A deep copy of source (which may lie inside target)
	inline bool assign ( value* target, const value* source ) { const auto f = internal::get ().assign; return f != nullptr && f ( target, source ); }

	// The member, appended as null if the object lacks it (a null value becomes an empty object first)
	[[ nodiscard ]] inline value* member ( value* object, const char* key ) { const auto f = internal::get ().member; return f != nullptr ? f ( object, key ) : nullptr; }

	// A new null item at index (index == size appends; a null value becomes an empty array first)
	[[ nodiscard ]] inline value* insert ( value* array, size_t index ) { const auto f = internal::get ().insert; return f != nullptr ? f ( array, index ) : nullptr; }
	[[ nodiscard ]] inline value* append ( value* array ) { return insert ( array, size ( array ) ); }

	// The value at path, made where missing: members as null (objects on the way), an index equal to the array's size or
	// "-" appends; null values on the way become objects
	[[ nodiscard ]] inline value* make_path ( value* root, const char* path, error* out = nullptr )
	{
		const auto	f = internal::get ().make_path;

		if ( f == nullptr )
			internal::report_unavailable ( out );

		return f != nullptr ? f ( root, path, out ) : nullptr;
	}

	// False when there was nothing to remove
	inline bool remove ( value* object, const char* key ) { const auto f = internal::get ().remove; return f != nullptr && f ( object, key ); }
	inline bool remove_at ( value* container, size_t index ) { const auto f = internal::get ().remove_at; return f != nullptr && f ( container, index ); }
	inline bool remove_path ( value* root, const char* path ) { const auto f = internal::get ().remove_path; return f != nullptr && f ( root, path ); }

	// Scalars as a document of their own, for set calls that take a value
	[[ nodiscard ]] inline document make_bool ( bool flag ) { auto d = create ( type::boolean ); set_bool ( d.get (), flag ); return d; }
	[[ nodiscard ]] inline document make_int ( int64_t integer ) { auto d = create ( type::integer ); set_int ( d.get (), integer ); return d; }
	[[ nodiscard ]] inline document make_number ( double number ) { auto d = create ( type::number ); return set_number ( d.get (), number ) ? std::move ( d ) : document (); }
	[[ nodiscard ]] inline document make_string ( std::string_view text ) { auto d = create ( type::string ); return set_string ( d.get (), text ) ? std::move ( d ) : document (); }

	// Text (a UTF-8 BOM is skipped) to a tree; empty with error filled when it is not json
	[[ nodiscard ]] inline document parse ( std::string_view text, error* out = nullptr )
	{
		const auto	f = internal::get ().parse;

		if ( f == nullptr )
			internal::report_unavailable ( out );

		return document ( f != nullptr ? f ( text.data (), text.size (), out ) : nullptr );
	}

	// The file's form (tabs, a line end after the last line), or compact on one line; empty on a ReShade without these
	// functions
	[[ nodiscard ]] inline std::string serialize ( const value* v, bool compact = false )
	{
		const auto	f = internal::get ().serialize;
		size_t		size = 0;

		if ( f == nullptr || v == nullptr )
			return {};

		(void) f ( v, compact, nullptr, &size );
		std::string	text ( size, '\0' );

		if ( size == 0 || ! f ( v, compact, text.data (), &size ) )
			return {};

		text.resize ( size - 1 );
		return text;
	}

	// Empty with error filled: missing, unreadable or not_json (with line and column)
	[[ nodiscard ]] inline document read_file ( const char* path, error* out = nullptr )
	{
		const auto	f = internal::get ().read_file;

		if ( f == nullptr )
			internal::report_unavailable ( out );

		return document ( f != nullptr ? f ( path, out ) : nullptr );
	}

	// Atomic, missing folders made. owner: whose write it is for change notifications, null for the calling add-on or
	// module (a module loaded by an add-on passes the add-on's handle to count as the add-on)
	[[ nodiscard ]] inline bool write_file ( const char* path, const value* v, error* out = nullptr, void* owner = nullptr )
	{
		const auto	f = internal::get ().write_file;

		if ( f == nullptr )
			internal::report_unavailable ( out );

		return f != nullptr && f ( owner != nullptr ? owner : internal::current_owner (), path, v, out );
	}

	// Defaults and user layers: the user's value wins; where both hold an object at the same place they merge member by
	// member (the defaults' order first, then the user's own members); arrays and scalars replace whole. Either layer
	// may be null. A user root that is null counts as empty

	// The effective value at path as a new tree, empty when neither layer has it; the empty path gives the whole merge
	[[ nodiscard ]] inline document layer_get ( const value* defaults, const value* user, const char* path = "" )
	{
		const auto	f = internal::get ().layer_get;
		return document ( f != nullptr ? f ( defaults, user, path ) : nullptr );
	}

	// Makes the effective value at path item (an object item merges over the defaults' object there) and leaves the user
	// layer holding only what differs from the defaults: a value equal to the default is removed from it, with objects
	// left empty by that. bad_path when the effective value on the way is neither object nor array
	[[ nodiscard ]] inline bool layer_set ( const value* defaults, value* user, const char* path, const value* item, error* out = nullptr )
	{
		const auto	f = internal::get ().layer_set;

		if ( f == nullptr )
			internal::report_unavailable ( out );

		return f != nullptr && f ( defaults, user, path, item, out );
	}

	// Back to the default at path: what the user layer holds there goes
	[[ nodiscard ]] inline bool layer_reset ( const value* defaults, value* user, const char* path, error* out = nullptr )
	{
		const auto	f = internal::get ().layer_reset;

		if ( f == nullptr )
			internal::report_unavailable ( out );

		return f != nullptr && f ( defaults, user, path, out );
	}

	// callback (on the presenting thread) whenever the file is created, changed, deleted or renamed onto, except by
	// write_file calls of the same owner (null: the calling add-on or module). 0 when the file can't be watched (the log
	// says why) and on a ReShade without these functions. An add-on's watches end when it unloads; a module unwatches
	// before it unloads
	[[ nodiscard ]] inline uint64_t watch ( const char* path, changed_callback callback, void* context, void* owner = nullptr )
	{
		const auto	f = internal::get ().watch;
		return f != nullptr ? f ( owner != nullptr ? owner : internal::current_owner (), path, callback, context ) : 0;
	}

	inline void unwatch ( uint64_t id ) { const auto f = internal::get ().unwatch; if ( f != nullptr ) f ( id ); }
}
