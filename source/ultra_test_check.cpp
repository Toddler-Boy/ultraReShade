/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RESHADE_TEST_APPLICATION

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <sstream>
#include <set>
#include <string>
#include <vector>

#include <Windows.h>
#include <shellapi.h>

#include <reshade.hpp>
#include <reshade_ultra.hpp>
#include <reshade_ultra_host_names.h>
#include <stb_image.h>
#include <stb_image_write.h>
#include <stb_image_write_hdr_png.h>

#include "dll_log.hpp"
#include "runtime.hpp"
#include "ultra_addon_reload.hpp"
#include "ultra_home.hpp"
#include "ultra_log.hpp"
#include "ultra_test_check.hpp"
#include "ultra_test_json.hpp"
#include "ultra_test_run.hpp"

extern HMODULE g_module_handle;
extern std::filesystem::path g_reshade_base_path;

//-----------------------------------------------------------------------------

namespace
{
	struct CaptureState
	{
		std::optional<size_t>	frame;
		std::filesystem::path	path;
		const reshade::ultra::TestFrames*	frames = nullptr;
		size_t								presents = 0;
		bool								isWritten = false;
		std::atomic<bool>					isLoaded = false;
	};

	CaptureState	state;

	// -dump-constants <frame> <file.txt>: every constant buffer the frame's draws read, as ultra_constant_buffer hands them over
	struct ConstantsDump
	{
		std::optional<size_t>	frame;
		std::filesystem::path	path;
		std::string				text;
		bool					isListening = false;
		bool					isWritten = false;
	};

	ConstantsDump	dump;

	struct TechniqueToggle
	{
		size_t		frame = 0;
		std::string	effect;
		std::string	technique;
		bool		isDone = false;
	};

	struct EffectChanges
	{
		std::vector<TechniqueToggle>	toggles;
		size_t							reloadEvery = 0;
		size_t							lastFrame = SIZE_MAX;
	};

	EffectChanges	changes;

	// After the effects of a present; a toggle due while the effects load waits until they are loaded
	void applyEffectChanges ( reshade::runtime* runtime, size_t frame )
	{
		for ( auto& toggle : changes.toggles )
		{
			if ( toggle.isDone || toggle.frame > frame || runtime->is_loading () )
				continue;

			toggle.isDone = true;

			const auto	technique = runtime->find_technique ( toggle.effect.c_str (), toggle.technique.c_str () );

			if ( technique.handle == 0 )
			{
				reshade::log::message ( reshade::log::level::error, "Test application: no technique '%s' in '%s' to toggle in frame %zu.", toggle.technique.c_str (), toggle.effect.c_str (), frame );
				continue;
			}

			const auto	isOn = ! runtime->get_technique_state ( technique );
			runtime->set_technique_state ( technique, isOn );
			reshade::log::message ( reshade::log::level::info, "Test application: technique '%s' %s in frame %zu.", toggle.technique.c_str (), isOn ? "on" : "off", frame );
		}

		if ( frame == changes.lastFrame )
			return;

		changes.lastFrame = frame;

		if ( changes.reloadEvery != 0 && frame != 0 && frame % changes.reloadEvery == 0 )
		{
			reshade::log::message ( reshade::log::level::info, "Test application: effects reload in frame %zu.", frame );
			runtime->reload_effect_next_frame ( nullptr );
		}
	}

	void onConstantBuffer ( reshade::api::command_list*, reshade::api::shader_stage stages, reshade::api::pipeline_layout layout, uint32_t param, uint32_t slot, const void* data,
							uint64_t size, reshade::api::resource buffer, uint64_t offset )
	{
		if ( state.frames == nullptr || dump.frame != state.frames->index () )
			return;

		char	line[ 160 ];
		std::snprintf ( line, sizeof ( line ), "stages 0x%x layout %llu param %u slot %u size %llu buffer %llu offset %llu\n", unsigned ( stages ), layout.handle, param, slot, size,
						buffer.handle, offset );
		dump.text += line;

		const auto	floats = static_cast<const float*> ( data );

		for ( uint64_t i = 0; i < size / 4; ++i )
		{
			std::snprintf ( line, sizeof ( line ), i % 4 == 3 || i + 1 == size / 4 ? "%10.5f\n" : "%10.5f ", floats[ i ] );
			dump.text += line;
		}
	}

	// -dump-resource-ids <file.txt>: each frame the stable IDs (and handles) of the back buffer presented and of the
	// render targets and depth buffer bound, and every ultra_resource_recreated event. Once a resource was recreated,
	// a frame that binds a target with an ID not seen before is an error
	struct ResourceIdsDump
	{
		std::filesystem::path	path;
		std::ofstream			file;
		std::set<uint64_t>		knownIds;
		std::string				targets;
		std::string				depth;
		bool					isListening = false;
		bool					hasRecreated = false;
	};

	ResourceIdsDump	ids;

	[[ nodiscard ]] size_t frameIndex ()
	{
		return state.frames != nullptr ? state.frames->index () : 0;
	}

	[[ nodiscard ]] std::string describe ( reshade::api::device* device, reshade::api::resource resource )
	{
		const auto	id = reshade::get_resource_stable_id ( device, resource );
		char		text[ 48 ];
		std::snprintf ( text, sizeof ( text ), " %llu:%llx", id, resource.handle );

		if ( id == 0 )
			reshade::log::message ( reshade::log::level::error, "Test application: resource %llx has no stable ID.", resource.handle );
		else if ( ids.hasRecreated && ! ids.knownIds.contains ( id ) )
			reshade::log::message ( reshade::log::level::error, "Test application: resource %llx has the new stable ID %llu after a recreation.", resource.handle, id );
		else
			ids.knownIds.insert ( id );

		return text;
	}

	void addOnce ( std::string& list, const std::string& entry )
	{
		if ( list.find ( entry ) == std::string::npos )
			list += entry;
	}

	void onBindTargets ( reshade::api::command_list* cmdList, uint32_t count, const reshade::api::resource_view* rtvs, reshade::api::resource_view dsv )
	{
		const auto	device = cmdList->get_device ();

		for ( uint32_t i = 0; i < count; ++i )
			if ( rtvs[ i ] != 0 )
				addOnce ( ids.targets, describe ( device, device->get_resource_from_view ( rtvs[ i ] ) ) );

		if ( dsv != 0 )
			addOnce ( ids.depth, describe ( device, device->get_resource_from_view ( dsv ) ) );
	}

	void onResourceRecreated ( reshade::api::device*, uint64_t id, reshade::api::resource oldResource, reshade::api::resource newResource )
	{
		ids.hasRecreated = true;
		ids.file << "recreated " << id << ": " << std::hex << oldResource.handle << " -> " << newResource.handle << std::dec << " in frame " << frameIndex () << '\n';
	}

	void writeResourceIds ( reshade::runtime* runtime )
	{
		if ( ! ids.isListening )
		{
			ids.isListening = true;
			ids.file.open ( ids.path );

			if ( ! ids.file.good () )
				reshade::log::message ( reshade::log::level::error, "Test application: failed to open '%s' for the resource IDs.", ids.path.u8string ().c_str () );

			reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil> ( onBindTargets );
			reshade::register_event<reshade::addon_event::ultra_resource_recreated> ( onResourceRecreated );
			return;
		}

		const auto	backBuffer = describe ( runtime->get_device (), runtime->get_swapchain ()->get_current_back_buffer () );
		ids.file << "frame " << frameIndex () << " back buffer" << backBuffer << " targets" << ids.targets << " depth" << ids.depth << '\n';
		ids.file.flush ();
		ids.targets.clear ();
		ids.depth.clear ();
	}

	// -watch <file>, -watch-folder <folder> <pattern>: watched through the add-on API from the first present on, until
	// -unwatch-at <frame>. Every change goes to the log and to -dump-file-events <file>, one tab-separated line each:
	// frame, Unix time in ms, change, thread (present or other), path, old path
	struct FileEvents
	{
		std::vector<std::pair<std::string, std::optional<std::string>>>	paths;		// UTF-8; a pattern for a folder
		std::filesystem::path	dumpPath;
		std::ofstream			dump;
		std::optional<size_t>	unwatchAt;
		DWORD					presentThread = 0;
		bool					isListening = false;
		bool					isStopped = false;
	};

	FileEvents	files;

	[[ nodiscard ]] std::string toUtf8 ( const wchar_t* text )
	{
		const auto	size = WideCharToMultiByte ( CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr );
		std::string	result ( static_cast<size_t> ( std::max ( size, 1 ) ), '\0' );
		WideCharToMultiByte ( CP_UTF8, 0, text, -1, result.data (), size, nullptr, nullptr );
		result.pop_back ();
		return result;
	}

	[[ nodiscard ]] std::string absoluteUtf8 ( const wchar_t* path )
	{
		std::error_code	ec;
		return toUtf8 ( std::filesystem::absolute ( path, ec ).c_str () );
	}

	[[ nodiscard ]] const char* toString ( reshade::api::file_change change )
	{
		switch ( change )
		{
			case reshade::api::file_change::created:	return "created";
			case reshade::api::file_change::modified:	return "modified";
			case reshade::api::file_change::deleted:	return "deleted";
			case reshade::api::file_change::renamed:	return "renamed";
		}

		reshade::log::message ( reshade::log::level::error, "Test application: unknown file change %u.", static_cast<uint32_t> ( change ) );
		return "unknown";
	}

	[[ nodiscard ]] uint64_t unixMilliseconds ()
	{
		FILETIME	time {};
		GetSystemTimePreciseAsFileTime ( &time );
		return ( ( uint64_t ( time.dwHighDateTime ) << 32 | time.dwLowDateTime ) - 116444736000000000ull ) / 10000;
	}

	void onFileChanged ( const char* path, reshade::api::file_change change, const char* oldPath )
	{
		const auto	isPresentThread = GetCurrentThreadId () == files.presentThread;

		files.dump << frameIndex () << '\t' << unixMilliseconds () << '\t' << toString ( change ) << '\t' << ( isPresentThread ? "present" : "other" ) << '\t' << path << '\t'
				   << ( oldPath != nullptr ? oldPath : "" ) << '\n';
		files.dump.flush ();

		reshade::log::message ( reshade::log::level::info, "Test application: file %s in frame %zu: '%s'%s%s%s", toString ( change ), frameIndex (), path,
								oldPath != nullptr ? " (from '" : "", oldPath != nullptr ? oldPath : "", oldPath != nullptr ? "')" : "" );

		if ( ! isPresentThread )
			reshade::log::message ( reshade::log::level::error, "Test application: the file change came on thread %lu, not the presenting thread %lu.", GetCurrentThreadId (),
									files.presentThread );
	}

	void watchFiles ( bool isWatching )
	{
		if ( isWatching )
		{
			if ( ! files.dumpPath.empty () )
			{
				files.dump.open ( files.dumpPath );

				if ( ! files.dump.good () )
					reshade::log::message ( reshade::log::level::error, "Test application: failed to open '%s' for the file changes.", files.dumpPath.u8string ().c_str () );
			}

			reshade::register_event<reshade::addon_event::ultra_file_changed> ( onFileChanged );
		}

		for ( const auto& [ path, pattern ] : files.paths )
		{
			if ( ! isWatching && pattern.has_value () )
				reshade::unwatch_folder ( path.c_str (), pattern->c_str () );
			else if ( ! isWatching )
				reshade::unwatch_file ( path.c_str () );
			else if ( pattern.has_value () ? ! reshade::watch_folder ( path.c_str (), pattern->c_str () ) : ! reshade::watch_file ( path.c_str () ) )
				reshade::log::message ( reshade::log::level::error, "Test application: failed to watch '%s'.", path.c_str () );
		}

		files.isListening = isWatching;
		files.isStopped = ! isWatching;
		reshade::log::message ( reshade::log::level::info, "Test application: %zu file watches %s in frame %zu.", files.paths.size (), isWatching ? "start" : "end", frameIndex () );
	}

	// -log-techniques <frame>: the techniques that render in that frame, in render order
	struct TechniqueOrder
	{
		std::optional<size_t>	frame;
		bool					isListening = false;
	};

	TechniqueOrder	order;

	void onTechniqueRendered ( reshade::api::effect_runtime* runtime, reshade::api::effect_technique technique, reshade::api::command_list*, reshade::api::resource_view, reshade::api::resource_view )
	{
		if ( order.frame != frameIndex () )
			return;

		char	name[ 128 ] {};
		runtime->get_technique_name ( technique, name );
		reshade::log::message ( reshade::log::level::info, "Test application: technique '%s' renders in frame %zu.", name, frameIndex () );
	}

	// -run-exclusive-at <frame>: queued after that frame's effects, runs at the next present
	struct ExclusiveCallTest
	{
		std::optional<size_t>	frame;
		bool					isAsked = false;
		bool					isQueued = false;
		bool					hasRun = false;
	};

	ExclusiveCallTest	exclusive;

	void onExclusiveCall ( void* )
	{
		exclusive.hasRun = true;
		const auto	isPresentThread = GetCurrentThreadId () == files.presentThread;
		reshade::log::message ( isPresentThread ? reshade::log::level::info : reshade::log::level::error, "Test application: exclusive call ran in frame %zu on %s thread.", frameIndex (),
								isPresentThread ? "the presenting" : "another" );
	}

	// -check-host-api: every name of the host API (reshade_ultra_host_names.h) found, its record asked, and the calls it must refuse
	// refused (no cells, too few cells, no runtime); a name, a record and a type it does not have are none
	struct HostApiCheck
	{
		bool	isRequested = false;
		bool	isDone = false;
	};

	HostApiCheck	hostApi;

	void failHostApi ( const char* name, const char* what )
	{
		reshade::log::message ( reshade::log::level::error, "Test application: host API \"%s\": %s.", name, what );
	}

	void checkHostApi ( reshade::runtime* runtime )
	{
		namespace names = reshade::host::name;
		const auto	lookup = reshade::get_host ();

		if ( lookup == nullptr )
			return failHostApi ( "ReShadeHost", "no lookup" );

		const auto	start = reinterpret_cast<UgRecordStart> ( lookup ( "record.start" ) );
		const auto	slot = reinterpret_cast<UgRecordSlot> ( lookup ( "record.slot" ) );

		if ( start == nullptr || slot == nullptr )
			return failHostApi ( "record.start", "the record functions are missing" );

		if ( lookup ( "no.such.name" ) != nullptr || start ( "no.such.name", nullptr, 0 ) != 0 || slot ( "no.such.name", "x", ugU32 ) != 0 )
			failHostApi ( "no.such.name", "an unknown name has an answer" );
		if ( slot ( names::screenshot_pending, "no such cell", ugU32 ) != 0 || slot ( names::screenshot_pending, "count", ugF32 ) != 0 || slot ( names::screenshot_pending, "count", ugU32 ) == 0 )
			failHostApi ( names::screenshot_pending, "a cell is not found by its name and type" );

		// Called with a runtime, these answer without changing anything but what they are for
		constexpr const char*	harmless[] { names::screenshot_pending, names::effects_target, names::depth_reread, names::depth_seen };

		for ( const auto name : { names::screenshot_pending, names::effects_target, names::depth_reread, names::depth_seen, names::depth_candidate } )
		{
			const auto	call = reinterpret_cast<UgCall> ( lookup ( name ) );
			const auto	count = start ( name, nullptr, 0 );

			if ( call == nullptr || count < 2 || count > 32 )
			{
				failHostApi ( name, "not found or without a record" );
				continue;
			}

			UgCell	cells[ 32 ] {};

			if ( call ( nullptr, nullptr, 0 ) != 0 || call ( nullptr, cells, count - 1 ) != 0 )
				failHostApi ( name, "a call with too few cells was not refused" );

			if ( start ( name, cells, count ) != count )
				failHostApi ( name, "the record's start does not fill its cells" );

			if ( call ( nullptr, cells, count ) != 0 )
				failHostApi ( name, "a call without a runtime was not refused" );

			if ( std::ranges::any_of ( harmless, [ name ] ( const char* h ) { return std::strcmp ( h, name ) == 0; } ) )
			{
				cells[ 1 ].ptr = static_cast<reshade::api::effect_runtime*> ( runtime );

				if ( call ( nullptr, cells, count ) != 1 )
					failHostApi ( name, "a call with a runtime was refused" );
			}
		}

		reshade::log::message ( reshade::log::level::info, "Test application: the host API's names were checked." );
	}

	void writeToStream ( void* file, void* data, int size )
	{
		static_cast<std::ofstream*> ( file )->write ( static_cast<const char*> ( data ), size );
	}

	template <typename T>
	void put ( std::vector<char>& out, T value )
	{
		const auto	bytes = reinterpret_cast<const char*> ( &value );
		out.insert ( out.end (), bytes, bytes + sizeof ( T ) );
	}

	void putAttribute ( std::vector<char>& out, const char* name, const char* type, const std::vector<char>& value )
	{
		out.insert ( out.end (), name, name + std::strlen ( name ) + 1 );
		out.insert ( out.end (), type, type + std::strlen ( type ) + 1 );
		put ( out, static_cast<int32_t> ( value.size () ) );
		out.insert ( out.end (), value.begin (), value.end () );
	}

	// OpenEXR, uncompressed scanlines of half floats (B, G, R in the file's alphabetical order): scRGB as is,
	// 1.0 = 80 nits, values above 1.0 kept
	void writeExr ( std::ofstream& file, uint32_t width, uint32_t height, const uint16_t* rgba )
	{
		std::vector<char>	header { 0x76, 0x2f, 0x31, 0x01, 2, 0, 0, 0 };
		std::vector<char>	channels;

		for ( const auto name : { "B", "G", "R" } )
		{
			channels.insert ( channels.end (), { name[ 0 ], 0 } );
			put ( channels, int32_t ( 1 ) );		// half
			put ( channels, int32_t ( 0 ) );		// linear flag and reserved
			put ( channels, int32_t ( 1 ) );
			put ( channels, int32_t ( 1 ) );
		}
		channels.push_back ( 0 );

		std::vector<char>	window;
		for ( const auto v : { 0, 0, int32_t ( width ) - 1, int32_t ( height ) - 1 } )
			put ( window, int32_t ( v ) );

		std::vector<char>	one;
		put ( one, 1.0f );
		std::vector<char>	centre;
		put ( centre, 0.0f );
		put ( centre, 0.0f );

		putAttribute ( header, "channels", "chlist", channels );
		putAttribute ( header, "compression", "compression", { 0 } );
		putAttribute ( header, "dataWindow", "box2i", window );
		putAttribute ( header, "displayWindow", "box2i", window );
		putAttribute ( header, "lineOrder", "lineOrder", { 0 } );
		putAttribute ( header, "pixelAspectRatio", "float", one );
		putAttribute ( header, "screenWindowCenter", "v2f", centre );
		putAttribute ( header, "screenWindowWidth", "float", one );
		header.push_back ( 0 );

		const auto	lineBytes = size_t ( width ) * 3 * sizeof ( uint16_t );
		const auto	firstLine = header.size () + size_t ( height ) * sizeof ( uint64_t );

		for ( uint32_t y = 0; y < height; ++y )
			put ( header, uint64_t ( firstLine + y * ( 8 + lineBytes ) ) );

		file.write ( header.data (), std::streamsize ( header.size () ) );

		std::vector<uint16_t>	line ( size_t ( width ) * 3 );

		for ( uint32_t y = 0; y < height; ++y )
		{
			for ( uint32_t x = 0; x < width; ++x )
				for ( auto c = 0; c < 3; ++c )
					line[ size_t ( c ) * width + x ] = rgba[ ( size_t ( y ) * width + x ) * 4 + 2 - c ];

			const int32_t	header2[ 2 ] { int32_t ( y ), int32_t ( lineBytes ) };
			file.write ( reinterpret_cast<const char*> ( header2 ), sizeof ( header2 ) );
			file.write ( reinterpret_cast<const char*> ( line.data () ), std::streamsize ( lineBytes ) );
		}
	}

	// SDR: 8-bit PNG. scRGB (half float): OpenEXR, the file's extension becomes .exr. HDR10 (10-bit, PQ): 16-bit PNG
	// tagged Rec.2020 PQ, the code values as presented
	[[ nodiscard ]] bool writeCapture ( reshade::runtime* runtime )
	{
		uint32_t	width = 0;
		uint32_t	height = 0;
		runtime->get_screenshot_width_and_height ( &width, &height );

		const auto	device = runtime->get_device ();
		const auto	format = reshade::api::format_to_default_typed ( device->get_resource_desc ( runtime->get_swapchain ()->get_current_back_buffer () ).texture.format, 0 );
		const auto	isScRgb = format == reshade::api::format::r16g16b16a16_float;
		const auto	isHdr10 = format == reshade::api::format::r10g10b10a2_unorm || format == reshade::api::format::b10g10r10a2_unorm;
		std::vector<uint8_t>	pixels ( size_t ( width ) * size_t ( height ) * ( isScRgb ? 8 : 4 ) );

		if ( pixels.empty () || ! runtime->capture_screenshot ( pixels.data () ) )
			return false;

		if ( isScRgb )
			state.path.replace_extension ( L".exr" );

		std::error_code	ec;
		std::filesystem::create_directories ( state.path.parent_path (), ec );

		auto	file = std::ofstream ( state.path, std::ios::binary );

		if ( isScRgb )
			writeExr ( file, width, height, reinterpret_cast<const uint16_t*> ( pixels.data () ) );
		else if ( isHdr10 )
		{
			std::vector<uint16_t>	codes ( size_t ( width ) * size_t ( height ) * 3 );

			for ( size_t i = 0; i < size_t ( width ) * size_t ( height ); ++i )
			{
				uint32_t	packed;
				std::memcpy ( &packed, pixels.data () + i * 4, 4 );
				const uint32_t	r = packed & 1023;
				const uint32_t	g = ( packed >> 10 ) & 1023;
				const uint32_t	b = ( packed >> 20 ) & 1023;
				const bool		isBgr = format == reshade::api::format::b10g10r10a2_unorm;

				codes[ i * 3 ] = static_cast<uint16_t> ( ( isBgr ? b : r ) << 6 );
				codes[ i * 3 + 1 ] = static_cast<uint16_t> ( g << 6 );
				codes[ i * 3 + 2 ] = static_cast<uint16_t> ( ( isBgr ? r : b ) << 6 );
			}

			constexpr unsigned char	primariesBt2020 = 9;
			constexpr unsigned char	transferPq = 16;
			stbi_write_hdr_png_to_func ( writeToStream, &file, static_cast<int> ( width ), static_cast<int> ( height ), 3, codes.data (), 0, primariesBt2020, transferPq );
		}
		else
			stbi_write_png_to_func ( writeToStream, &file, static_cast<int> ( width ), static_cast<int> ( height ), 4, pixels.data (), static_cast<int> ( width ) * 4 );

		return file.good () && file.tellp () > 0;
	}

	// The picture as presented, without the overlay: after the add-ons' and ReShade's effects (NR on the Present route)
	void afterEffects ( reshade::runtime* runtime )
	{
		++state.presents;
		state.isLoaded = state.isLoaded || ! runtime->is_loading ();

		if ( state.frames != nullptr && state.isLoaded )
			applyEffectChanges ( runtime, state.frames->index () );

		if ( state.frames != nullptr && state.frame == state.frames->index () && ! state.isWritten )
		{
			state.isWritten = writeCapture ( runtime );
			reshade::log::message ( state.isWritten ? reshade::log::level::info : reshade::log::level::error, "Test application: capture of frame %zu %s '%s'.", *state.frame,
									state.isWritten ? "written to" : "failed for", state.path.u8string ().c_str () );
		}

		// Listening starts once ReShade's add-ons are loaded (the event needs their bookkeeping), so from the first present
		if ( dump.frame.has_value () && ! dump.isListening )
			dump.isListening = true, reshade::register_event<reshade::addon_event::ultra_constant_buffer> ( onConstantBuffer );

		if ( ! ids.path.empty () )
			writeResourceIds ( runtime );

		files.presentThread = GetCurrentThreadId ();

		if ( hostApi.isRequested && ! hostApi.isDone && state.isLoaded )
		{
			hostApi.isDone = true;
			checkHostApi ( runtime );
		}

		if ( order.frame.has_value () && ! order.isListening )
		{
			order.isListening = true;
			reshade::register_event<reshade::addon_event::reshade_render_technique> ( onTechniqueRendered );
		}

		if ( exclusive.frame.has_value () && ! exclusive.isAsked && frameIndex () >= *exclusive.frame )
		{
			exclusive.isAsked = true;
			exclusive.isQueued = reshade::ultra::runExclusive ( g_module_handle, onExclusiveCall, nullptr );

			if ( ! exclusive.isQueued )
				reshade::log::message ( reshade::log::level::error, "Test application: the exclusive call was refused in frame %zu.", frameIndex () );
		}

		if ( reshade::ultra::isJsonTestRequested () )
			reshade::ultra::stepJsonTests ( runtime->get_hwnd () );

		if ( ! files.paths.empty () && ! files.isListening && ! files.isStopped )
			watchFiles ( true );
		else if ( files.isListening && files.unwatchAt.has_value () && frameIndex () >= *files.unwatchAt )
			watchFiles ( false );

		if ( dump.frame.has_value () && state.frames != nullptr && dump.frame == state.frames->index () && ! dump.isWritten )
		{
			std::ofstream	file ( dump.path );
			file << dump.text;
			dump.isWritten = file.good ();
			reshade::log::message ( dump.isWritten ? reshade::log::level::info : reshade::log::level::error, "Test application: constant buffers of frame %zu %s '%s'.", *dump.frame,
									dump.isWritten ? "written to" : "failed for", dump.path.u8string ().c_str () );
		}
	}

	[[ nodiscard ]] bool isError ( const std::string& line )
	{
		return line.find ( "| ERROR |" ) != std::string::npos;
	}

	// This run's lines in the file (it holds the runs before too)
	[[ nodiscard ]] bool logHasErrors ()
	{
		ultralog::flushAll ();
		std::ifstream		log ( reshade::ultra::logPath (), std::ios::binary );
		const std::string	text ( ( std::istreambuf_iterator<char> ( log ) ), std::istreambuf_iterator<char> () );
		std::istringstream	run { std::string ( ultralog::lastRun ( text ) ) };
		std::string			line;

		while ( std::getline ( run, line ) )
			if ( isError ( line ) )
				return true;

		return false;
	}
}
//-----------------------------------------------------------------------------

reshade::ultra::TestCheck::TestCheck ()
{
	auto		argc = 0;
	const auto	argv = CommandLineToArgvW ( GetCommandLineW (), &argc );

	for ( auto i = 1; argv != nullptr && i < argc; ++i )
	{
		if ( _wcsicmp ( argv[ i ], L"-dump-resource-ids" ) == 0 )
		{
			if ( i + 1 >= argc )
			{
				reshade::log::message ( reshade::log::level::error, "Test application: -dump-resource-ids needs a file." );
				isCommandLineValid = false;
				break;
			}

			ids.path = std::filesystem::absolute ( argv[ ++i ] );
			continue;
		}

		if ( _wcsicmp ( argv[ i ], L"-check-host-api" ) == 0 )
		{
			hostApi.isRequested = true;
			continue;
		}

		if ( _wcsicmp ( argv[ i ], L"-log-techniques" ) == 0 )
		{
			wchar_t*	end = nullptr;
			const auto	frame = i + 1 < argc ? std::wcstoull ( argv[ i + 1 ], &end, 10 ) : 0;

			if ( i + 1 >= argc || end == argv[ i + 1 ] || *end != L'\0' )
			{
				reshade::log::message ( reshade::log::level::error, "Test application: -log-techniques needs a frame number." );
				isCommandLineValid = false;
				break;
			}

			order.frame = static_cast<size_t> ( frame );
			++i;
			continue;
		}

		if ( _wcsicmp ( argv[ i ], L"-run-exclusive-at" ) == 0 )
		{
			wchar_t*	end = nullptr;
			const auto	frame = i + 1 < argc ? std::wcstoull ( argv[ i + 1 ], &end, 10 ) : 0;

			if ( i + 1 >= argc || end == argv[ i + 1 ] || *end != L'\0' )
			{
				reshade::log::message ( reshade::log::level::error, "Test application: -run-exclusive-at needs a frame number." );
				isCommandLineValid = false;
				break;
			}

			exclusive.frame = static_cast<size_t> ( frame );
			++i;
			continue;
		}

		if ( _wcsicmp ( argv[ i ], L"-toggle-technique-at" ) == 0 || _wcsicmp ( argv[ i ], L"-reload-effects-every" ) == 0 )
		{
			const auto	isToggle = _wcsicmp ( argv[ i ], L"-toggle-technique-at" ) == 0;
			wchar_t*	end = nullptr;
			const auto	frame = i + 1 < argc ? std::wcstoull ( argv[ i + 1 ], &end, 10 ) : 0;

			if ( i + ( isToggle ? 3 : 1 ) >= argc || end == argv[ i + 1 ] || *end != L'\0' || ( ! isToggle && frame == 0 ) )
			{
				reshade::log::message ( reshade::log::level::error, isToggle ? "Test application: -toggle-technique-at needs a frame, an effect file and a technique."
																			 : "Test application: -reload-effects-every needs a frame count above zero." );
				isCommandLineValid = false;
				break;
			}

			if ( isToggle )
				changes.toggles.push_back ( { static_cast<size_t> ( frame ), toUtf8 ( argv[ i + 2 ] ), toUtf8 ( argv[ i + 3 ] ) } );
			else
				changes.reloadEvery = static_cast<size_t> ( frame );

			i += isToggle ? 3 : 1;
			continue;
		}

		const auto	isWatchFolder = _wcsicmp ( argv[ i ], L"-watch-folder" ) == 0;

		if ( isWatchFolder || _wcsicmp ( argv[ i ], L"-watch" ) == 0 || _wcsicmp ( argv[ i ], L"-dump-file-events" ) == 0 || _wcsicmp ( argv[ i ], L"-unwatch-at" ) == 0 )
		{
			const auto	values = isWatchFolder ? 2 : 1;
			wchar_t*	end = nullptr;
			const auto	isUnwatchAt = _wcsicmp ( argv[ i ], L"-unwatch-at" ) == 0;
			const auto	frame = isUnwatchAt && i + 1 < argc ? std::wcstoull ( argv[ i + 1 ], &end, 10 ) : 0;

			if ( i + values >= argc || ( isUnwatchAt && ( end == argv[ i + 1 ] || *end != L'\0' ) ) )
			{
				reshade::log::message ( reshade::log::level::error, "Test application: %s needs %s.", toUtf8 ( argv[ i ] ).c_str (),
										isWatchFolder ? "a folder and a pattern" : isUnwatchAt ? "a frame number" : "a file" );
				isCommandLineValid = false;
				break;
			}

			if ( isUnwatchAt )
				files.unwatchAt = static_cast<size_t> ( frame );
			else if ( isWatchFolder )
				files.paths.emplace_back ( absoluteUtf8 ( argv[ i + 1 ] ), toUtf8 ( argv[ i + 2 ] ) );
			else if ( _wcsicmp ( argv[ i ], L"-watch" ) == 0 )
				files.paths.emplace_back ( absoluteUtf8 ( argv[ i + 1 ] ), std::nullopt );
			else
				files.dumpPath = std::filesystem::absolute ( argv[ i + 1 ] );

			i += values;
			continue;
		}

		const auto	isCapture = _wcsicmp ( argv[ i ], L"-capture" ) == 0;

		if ( ! isCapture && _wcsicmp ( argv[ i ], L"-dump-constants" ) != 0 )
			continue;

		wchar_t*	end = nullptr;
		const auto	frame = i + 2 < argc ? std::wcstoull ( argv[ i + 1 ], &end, 10 ) : 0;

		if ( i + 2 >= argc || end == argv[ i + 1 ] || *end != L'\0' )
		{
			reshade::log::message ( reshade::log::level::error, "Test application: %s needs a frame number and a file.", isCapture ? "-capture" : "-dump-constants" );
			isCommandLineValid = false;
			break;
		}

		if ( isCapture )
		{
			captureFrame = static_cast<size_t> ( frame );
			capturePath = std::filesystem::absolute ( argv[ i + 2 ] );
		}
		else
		{
			dump.frame = static_cast<size_t> ( frame );
			dump.path = std::filesystem::absolute ( argv[ i + 2 ] );
		}

		i += 2;
	}

	LocalFree ( argv );

	state.frame = captureFrame;
	state.path = capturePath;
	reshade::runtime::ultra_after_effects = afterEffects;
}
//-----------------------------------------------------------------------------

reshade::ultra::TestCheck::~TestCheck ()
{
	reshade::runtime::ultra_after_effects = nullptr;
}
//-----------------------------------------------------------------------------

void reshade::ultra::TestCheck::watch ( const TestFrames& frames )
{
	state.frames = &frames;
}
//-----------------------------------------------------------------------------

bool reshade::ultra::isReShadeLoaded ()
{
	return state.isLoaded;
}
//-----------------------------------------------------------------------------

bool reshade::ultra::TestCheck::needsNumberedFrames () const
{
	return captureFrame.has_value () || dump.frame.has_value () || ! ids.path.empty () || ! changes.toggles.empty () || changes.reloadEvery != 0 || files.unwatchAt.has_value ()
		|| exclusive.frame.has_value () || order.frame.has_value () || hostApi.isRequested;
}
//-----------------------------------------------------------------------------

bool reshade::ultra::TestCheck::isValid () const
{
	return isCommandLineValid;
}
//-----------------------------------------------------------------------------

int reshade::ultra::TestCheck::exitCode ( int loopResult ) const
{
	if ( loopResult != EXIT_SUCCESS )
		return loopResult;

	if ( state.presents == 0 )
		return 2;

	if ( captureFrame.has_value () && ! state.isWritten )
		return 3;

	if ( exclusive.isQueued && ! exclusive.hasRun )
		reshade::log::message ( reshade::log::level::error, "Test application: the exclusive call queued in frame %zu never ran.", *exclusive.frame );

	reshade::ultra::finishJsonTests ();
	return logHasErrors () ? 4 : EXIT_SUCCESS;
}
//-----------------------------------------------------------------------------

#endif
