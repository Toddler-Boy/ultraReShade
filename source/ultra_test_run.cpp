/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RESHADE_TEST_APPLICATION

#include <algorithm>
#include <cwchar>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <Windows.h>
#include <shellapi.h>

#include <reshade.hpp>

#include "addon_manager.hpp"
#include "dll_log.hpp"
#include "ultra_addon_reload.hpp"
#include "ultra_log.hpp"
#include "ultra_test_check.hpp"
#include "ultra_test_run.hpp"

extern HMODULE g_module_handle;

//-----------------------------------------------------------------------------

namespace
{
	constexpr float					fixedStep = 1.0f / 60.0f;
	constexpr std::chrono::seconds	loadTimeout { 120 };

	[[ nodiscard ]] std::string narrow ( const wchar_t* text )
	{
		std::string	out;

		for ( ; *text != L'\0'; ++text )
			out += *text < 128 ? static_cast<char> ( *text ) : '?';

		return out;
	}

	// A whole decimal number, nothing after it
	[[ nodiscard ]] std::optional<size_t> parseCount ( const wchar_t* text )
	{
		wchar_t*	end = nullptr;
		const auto	value = std::wcstoull ( text, &end, 10 );

		if ( end == text || *end != L'\0' || *text == L'-' )
			return std::nullopt;

		return static_cast<size_t> ( value );
	}

	// A decimal number, nothing after it
	[[ nodiscard ]] std::optional<float> parseNumber ( const wchar_t* text )
	{
		wchar_t*	end = nullptr;
		const auto	value = std::wcstof ( text, &end );

		if ( end == text || *end != L'\0' )
			return std::nullopt;

		return value;
	}

	// <w>x<h>, both above zero
	[[ nodiscard ]] bool parseSize ( const wchar_t* text, LONG& width, LONG& height )
	{
		auto	w = 0L;
		auto	h = 0L;
		wchar_t	rest = L'\0';

		if ( std::swscanf ( text, L"%ldx%ld%lc", &w, &h, &rest ) != 2 || w <= 0 || h <= 0 )
			return false;

		width = w;
		height = h;
		return true;
	}

	// A few buffer size switches at random frames, the same for one seed; with fullscreen also exclusive fullscreen and back
	void pickSwitches ( reshade::ultra::TestOptions& options, size_t seed, bool withFullscreen )
	{
		using reshade::ultra::TestSwitchKind;

		std::mt19937	random ( static_cast<std::mt19937::result_type> ( seed ) );
		const auto		total = options.frames != 0 ? options.frames : 600;
		const auto		count = 3 + random () % 3;
		auto			frame = size_t ( 10 + random () % 10 );
		auto			isFullscreen = false;

		for ( size_t i = 0; i < count && frame + 5 < total; ++i, frame += 8 + random () % 15 )
		{
			reshade::ultra::TestSwitch	change { frame };
			const auto					isResize = random () % 5 < 2 || ! withFullscreen;

			if ( isResize )
			{
				change.width = std::max ( 64L, LONG ( GetSystemMetrics ( SM_CXSCREEN ) * int ( 50 + random () % 51 ) / 100 ) );
				change.height = std::max ( 64L, LONG ( GetSystemMetrics ( SM_CYSCREEN ) * int ( 50 + random () % 51 ) / 100 ) );
			}
			else
				isFullscreen = ! isFullscreen;

			change.kind = isResize ? TestSwitchKind::bufferSize : isFullscreen ? TestSwitchKind::fullscreen : TestSwitchKind::windowed;
			options.switches.push_back ( change );

			reshade::log::message ( reshade::log::level::info, "Test application: switch at frame %zu: %s %ldx%ld.", change.frame,
									isResize ? "buffer size" : isFullscreen ? "fullscreen" : "windowed", change.width, change.height );
		}
	}

	void reportBadValue ( const wchar_t* option, const wchar_t* value )
	{
		reshade::log::message ( reshade::log::level::error, "Test application: '%s' is not a valid value for %s.", narrow ( value ).c_str (), narrow ( option ).c_str () );
	}

	void reportMissingValue ( const wchar_t* option )
	{
		reshade::log::message ( reshade::log::level::error, "Test application: %s needs a value.", narrow ( option ).c_str () );
	}

	[[ nodiscard ]] bool isOption ( const wchar_t* arg, const wchar_t* name )
	{
		return _wcsicmp ( arg, name ) == 0;
	}

	// Each call timed on the thread that logs: what a line costs a game thread
	void logStress ( size_t lines, size_t threads )
	{
		struct Timing
		{
			std::chrono::duration<double, std::micro>	total {};
			std::chrono::duration<double, std::micro>	worst {};
		};

		std::vector<Timing>			timings ( threads );
		std::vector<std::thread>	workers;
		const auto					start = std::chrono::steady_clock::now ();

		for ( size_t t = 0; t < threads; ++t )
			workers.emplace_back ( [ t, lines, threads, &timings ]
			{
				for ( auto i = t; i < lines; i += threads )
				{
					const auto	before = std::chrono::steady_clock::now ();
					reshade::log::message ( reshade::log::level::info, "Test application: stress line %zu from thread %zu, about as long as a usual line of the log.", i, t );
					const std::chrono::duration<double, std::micro>	took = std::chrono::steady_clock::now () - before;

					timings[ t ].total += took;
					timings[ t ].worst = std::max ( timings[ t ].worst, took );
				}
			} );

		for ( auto& worker : workers )
			worker.join ();

		const std::chrono::duration<double, std::milli>	wall = std::chrono::steady_clock::now () - start;
		Timing	all;

		for ( const auto& timing : timings )
		{
			all.total += timing.total;
			all.worst = std::max ( all.worst, timing.worst );
		}

		reshade::log::message ( reshade::log::level::info, "Test application: %zu threads logged %zu lines in %.1f ms; a call took %.2f us on average, %.1f us at most.", threads,
								lines, wall.count (), lines != 0 ? all.total.count () / double ( lines ) : 0.0, all.worst.count () );
	}

	void logThroughAddons ()
	{
		for ( const auto& info : reshade::addon_loaded_info )
			if ( info.handle != nullptr && info.handle != g_module_handle )
				ReShadeLogMessage ( info.handle, static_cast<int> ( reshade::log::level::info ), "Test application: a line through the add-on API." );

		ReShadeLogMessage ( GetModuleHandleW ( L"dxgi.dll" ), static_cast<int> ( reshade::log::level::info ), "Test application: a line through the add-on API from a module that is no add-on." );
	}

	// As the Log tab copies: the new lines only, every kept line once some went past unseen
	void checkLogLines ()
	{
		const auto	isCheckLine = [] ( const std::string& line ) { return line.find ( "Test application: log line check " ) != std::string::npos; };
		auto		seen = reshade::ultra::logLineCount ();
		std::vector<std::string>	lines;

		for ( auto i = 0; i < 3; ++i )
			reshade::log::message ( reshade::log::level::info, "Test application: log line check %d.", i );

		const auto	isNewOnly = reshade::ultra::copyLogLines ( seen, lines ) && std::ranges::count_if ( lines, isCheckLine ) == 3 && lines.size () < 10;
		constexpr size_t	unseen = 12000;
		lines.clear ();

		for ( size_t i = 0; i < unseen; ++i )
			reshade::log::message ( reshade::log::level::info, "Test application: log line check past the kept lines, %zu.", i );

		const auto	isRebuilt = ! reshade::ultra::copyLogLines ( seen, lines ) && lines.size () < unseen && std::ranges::any_of ( lines, [] ( const std::string& line )
		{
			return line.find ( "kept lines, 11999." ) != std::string::npos;
		} );

		reshade::log::message ( isNewOnly && isRebuilt ? reshade::log::level::info : reshade::log::level::error,
								"Test application: the Log tab's copy %s: new lines only %s, %zu kept lines after %zu unseen ones %s.", isNewOnly && isRebuilt ? "works" : "failed",
								isNewOnly ? "yes" : "no", lines.size (), unseen, isRebuilt ? "rebuilt the view" : "did not rebuild it" );
	}

	// Read through a volatile pointer, so the compiler can't see the null
	volatile int* volatile	crashTarget = nullptr;

	__declspec ( noinline ) void writeToNull ()
	{
		*crashTarget = 0;
	}

	// The fault inside a call: clang catches synchronous faults only (no /EHa)
	[[ nodiscard ]] size_t causeFaults ( size_t count )
	{
		size_t	caught = 0;

		for ( size_t i = 0; i < count; ++i )
		{
			__try
			{
				writeToNull ();
			}
			__except ( GetExceptionCode () == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH )
			{
				++caught;
			}
		}

		return caught;
	}

	void accessConfig ( const reshade::ultra::TestConfigAccess& access )
	{
		if ( access.isWrite )
			ReShadeSetConfigValue ( g_module_handle, nullptr, access.section.c_str (), access.key.c_str (), access.value.has_value () ? access.value->c_str () : nullptr );

		char		value[ 1024 ] {};
		auto		size = sizeof ( value );
		const auto	isFound = ReShadeGetConfigValue ( g_module_handle, nullptr, access.section.c_str (), access.key.c_str (), value, &size );
		const auto	what = ! access.isWrite ? "read" : access.value.has_value () ? "set to '" + *access.value + "'" : std::string ( "removed" );

		reshade::log::message ( reshade::log::level::info, "Test application: config [%s] %s %s through the add-on API, reads back %s%s%s.", access.section.c_str (), access.key.c_str (),
								what.c_str (), isFound ? "'" : "nothing", isFound ? value : "", isFound ? "'" : "" );
	}

	// -exit-handler <ms>: the exit event shows another thread still running, then holds the exit for ms
	DWORD	exitHandlerSleep = 0;
	HANDLE	lineWanted = nullptr;
	HANDLE	lineWritten = nullptr;

	DWORD WINAPI writeLineAtExit ( void* )
	{
		WaitForSingleObject ( lineWanted, INFINITE );
		reshade::log::message ( reshade::log::level::info, "Test application: thread %lu still runs and writes this line while the exit event runs.", GetCurrentThreadId () );
		SetEvent ( lineWritten );
		return 0;
	}

	void onProcessExit ( reshade::api::process_exit reason )
	{
		reshade::log::message ( reshade::log::level::info, "Test application: exit event (%s) on thread %lu.", reason == reshade::api::process_exit::exit ? "exit" : "self-terminate", GetCurrentThreadId () );
		SetEvent ( lineWanted );

		if ( WaitForSingleObject ( lineWritten, 1000 ) != WAIT_OBJECT_0 )
			reshade::log::message ( reshade::log::level::error, "Test application: no other thread wrote its line within 1 s of the exit event." );

		if ( exitHandlerSleep != 0 )
		{
			reshade::log::message ( reshade::log::level::info, "Test application: the exit handler sleeps %lu ms.", exitHandlerSleep );
			Sleep ( exitHandlerSleep );
			reshade::log::message ( reshade::log::level::info, "Test application: the exit handler woke up after %lu ms.", exitHandlerSleep );
		}
	}

	void followExit ( size_t sleepMs )
	{
		exitHandlerSleep = static_cast<DWORD> ( sleepMs );
		lineWanted = CreateEventW ( nullptr, TRUE, FALSE, nullptr );
		lineWritten = CreateEventW ( nullptr, TRUE, FALSE, nullptr );
		CloseHandle ( CreateThread ( nullptr, 0, writeLineAtExit, nullptr, 0, nullptr ) );
		reshade::register_event<reshade::addon_event::ultra_process_exit> ( onProcessExit );
	}

	void crash ( size_t frame )
	{
		const auto	filter = SetUnhandledExceptionFilter ( nullptr );
		SetUnhandledExceptionFilter ( filter );
		const auto	owner = reshade::ultra::logModuleName ( reinterpret_cast<const void*> ( filter ) );
		reshade::log::message ( reshade::log::level::info, "Test application: an access violation in frame %zu, on purpose (the unhandled exception filter on top is %s).", frame,
								filter == nullptr ? "none" : owner.empty () ? "the test application's" : owner.c_str () );
		SetErrorMode ( GetErrorMode () | SEM_NOGPFAULTERRORBOX );
		*crashTarget = 0;
	}
}
//-----------------------------------------------------------------------------

reshade::ultra::TestOptions reshade::ultra::parseTestOptions ()
{
	TestOptions				options;
	std::optional<size_t>	randomSeed;
	auto					isRandomFullscreen = false;
	auto					argc = 0;
	const auto	argv = CommandLineToArgvW ( GetCommandLineW (), &argc );

	if ( argv == nullptr )
		return options;

	for ( auto i = 1; i < argc; ++i )
	{
		const auto	arg = argv[ i ];

		// Runs stay in the background unless someone wants to watch one: an agent's run never steals the focus
		if ( isOption ( arg, L"-background" ) || isOption ( arg, L"-show" ) )
		{
			options.isBackground = isOption ( arg, L"-background" );
			continue;
		}

		if ( isOption ( arg, L"-dlss-sl-track" ) || isOption ( arg, L"-dlss-resize" ) || isOption ( arg, L"-dlss-no-reset" ) || isOption ( arg, L"-dlss-keep-compute" )
			 || isOption ( arg, L"-dlss-game-exit" ) || isOption ( arg, L"-dlss-fg" ) )
		{
			auto&	flag = isOption ( arg, L"-dlss-sl-track" ) ? options.dlss.isListStateTracked : isOption ( arg, L"-dlss-resize" ) ? options.dlss.isResizedAtSwitch
						   : isOption ( arg, L"-dlss-no-reset" ) ? options.dlss.isResetSkipped : isOption ( arg, L"-dlss-game-exit" ) ? options.dlss.isGameExit
						   : isOption ( arg, L"-dlss-fg" ) ? options.dlss.isFrameGenerated : options.dlss.reliesOnCompute;
			flag = true;
			continue;
		}

		if ( isOption ( arg, L"-log-frames" ) )
		{
			options.isLoggingFrames = true;
			continue;
		}

		if ( isOption ( arg, L"-log-addons" ) )
		{
			options.isLoggingThroughAddons = true;
			continue;
		}

		if ( isOption ( arg, L"-check-log-lines" ) )
		{
			options.isCheckingLogLines = true;
			continue;
		}

		if ( isOption ( arg, L"-log-stress" ) )
		{
			const auto	lines = i + 2 < argc ? parseCount ( argv[ i + 1 ] ) : std::nullopt;
			const auto	threads = i + 2 < argc ? parseCount ( argv[ i + 2 ] ) : std::nullopt;

			if ( ! lines.has_value () || ! threads.has_value () || *threads == 0 )
			{
				reshade::log::message ( reshade::log::level::error, "Test application: -log-stress needs a number of lines and of threads." );
				options.isValid = false;
				break;
			}

			options.stressLines = *lines;
			options.stressThreads = *threads;
			i += 2;
			continue;
		}

		if ( isOption ( arg, L"-draws" ) )
		{
			const auto	draws = i + 2 < argc ? parseCount ( argv[ i + 1 ] ) : std::nullopt;
			const auto	scale = i + 2 < argc ? parseNumber ( argv[ i + 2 ] ) : std::nullopt;

			if ( ! draws.has_value () || *draws > 1000 || ! scale.has_value () || *scale <= 0.0f || *scale > 10.0f )
			{
				reshade::log::message ( reshade::log::level::error, "Test application: -draws needs a number of draws (up to 1000) and a scale (above 0, up to 10)." );
				options.isValid = false;
				break;
			}

			options.scene.loadDraws = *draws;
			options.scene.loadScale = *scale;
			i += 2;
			continue;
		}

		if ( isOption ( arg, L"-random-switches" ) )
		{
			randomSeed = i + 1 < argc ? parseCount ( argv[ i + 1 ] ) : std::nullopt;

			if ( ! randomSeed.has_value () )
			{
				reshade::log::message ( reshade::log::level::error, "Test application: -random-switches needs a seed (and fullscreen to include exclusive fullscreen)." );
				options.isValid = false;
				break;
			}

			isRandomFullscreen = i + 2 < argc && isOption ( argv[ i + 2 ], L"fullscreen" );
			i += isRandomFullscreen ? 2 : 1;
			continue;
		}

		if ( isOption ( arg, L"-set-config" ) || isOption ( arg, L"-get-config" ) )
		{
			TestConfigAccess	access;
			access.isWrite = isOption ( arg, L"-set-config" );
			const auto			count = access.isWrite ? 4 : 3;
			const auto			frame = i + count < argc ? parseCount ( argv[ i + 1 ] ) : std::nullopt;

			if ( ! frame.has_value () )
			{
				reshade::log::message ( reshade::log::level::error, "Test application: %s needs a frame, a section and a key%s.", narrow ( arg ).c_str (), access.isWrite ? " and a value (- removes it)" : "" );
				options.isValid = false;
				break;
			}

			access.frame = *frame;
			access.section = narrow ( argv[ i + 2 ] );
			access.key = narrow ( argv[ i + 3 ] );
			if ( access.isWrite && ! isOption ( argv[ i + 4 ], L"-" ) )
				access.value = narrow ( argv[ i + 4 ] );
			options.configAccesses.push_back ( access );
			i += count;
			continue;
		}

		if ( isOption ( arg, L"-buffer-size-at" ) )
		{
			TestSwitch	change;
			const auto	frame = i + 2 < argc ? parseCount ( argv[ i + 1 ] ) : std::nullopt;

			if ( ! frame.has_value () || ! parseSize ( argv[ i + 2 ], change.width, change.height ) )
			{
				reshade::log::message ( reshade::log::level::error, "Test application: -buffer-size-at needs a frame and <w>x<h>." );
				options.isValid = false;
				break;
			}

			change.frame = *frame;
			options.switches.push_back ( change );
			i += 2;
			continue;
		}

		const auto	isKnown = isOption ( arg, L"-api" ) || isOption ( arg, L"-scene" ) || isOption ( arg, L"-jitter" ) || isOption ( arg, L"-sss" ) || isOption ( arg, L"-env" ) || isOption ( arg, L"-fov" ) || isOption ( arg, L"-hdr" )
							  || isOption ( arg, L"-paper-white" ) || isOption ( arg, L"-peak" ) || isOption ( arg, L"-reload-every" ) || isOption ( arg, L"-probe-devices" )
							  || isOption ( arg, L"-constants" ) || isOption ( arg, L"-owner" )
							  || isOption ( arg, L"-size" ) || isOption ( arg, L"-frames" ) || isOption ( arg, L"-resize-at" )
							  || isOption ( arg, L"-fullscreen-at" ) || isOption ( arg, L"-windowed-at" ) || isOption ( arg, L"-crash-at" ) || isOption ( arg, L"-handled-faults" )
							  || isOption ( arg, L"-exit-handler" ) || isOption ( arg, L"-terminate-at" ) || isOption ( arg, L"-dlss" ) || isOption ( arg, L"-dlss-warmup" ) || isOption ( arg, L"-dlss-streamline" ) || isOption ( arg, L"-dlss-tags" )
							  || isOption ( arg, L"-dlss-new-images" ) || isOption ( arg, L"-dlss-pipelines" ) || isOption ( arg, L"-dlss-reload" ) || isOption ( arg, L"-dlss-fg-remake" );

		if ( ! isKnown )
			continue;

		if ( i + 1 >= argc )
		{
			reportMissingValue ( arg );
			options.isValid = false;
			break;
		}

		const auto	value = argv[ ++i ];

		if ( isOption ( arg, L"-api" ) )
		{
			if ( isOption ( value, L"dx11" ) )
				options.api = api::device_api::d3d11;
			else if ( isOption ( value, L"dx12" ) )
				options.api = api::device_api::d3d12;
			else
				options.isValid = false;
		}
		else if ( isOption ( arg, L"-scene" ) )
		{
			if ( isOption ( value, L"cube" ) )
				options.scene.kind = TestSceneKind::cube;
			else if ( isOption ( value, L"head" ) )
				options.scene.kind = TestSceneKind::head;
			else
				options.isValid = false;
		}
		else if ( isOption ( arg, L"-jitter" ) || isOption ( arg, L"-sss" ) )
		{
			auto&	setting = isOption ( arg, L"-jitter" ) ? options.scene.isJittered : options.scene.isScattering;

			if ( isOption ( value, L"on" ) )
				setting = true;
			else if ( isOption ( value, L"off" ) )
				setting = false;
			else
				options.isValid = false;
		}
		else if ( isOption ( arg, L"-dlss-streamline" ) )
			options.dlss.streamlineFolder = value;
		else if ( isOption ( arg, L"-dlss-tags" ) )
		{
			if ( isOption ( value, L"now" ) )
				options.dlss.tags = TestDlssTags::onlyNow;
			else if ( isOption ( value, L"evaluate" ) )
				options.dlss.tags = TestDlssTags::untilEvaluate;
			else if ( isOption ( value, L"present" ) )
				options.dlss.tags = TestDlssTags::untilPresent;
			else
				options.isValid = false;
		}
		else if ( isOption ( arg, L"-env" ) )
			options.scene.environment = value;
		else if ( isOption ( arg, L"-owner" ) )
			options.owner = value;
		else if ( isOption ( arg, L"-fov" ) )
		{
			const auto	degrees = parseNumber ( value );

			if ( degrees.has_value () && *degrees > 1.0f && *degrees < 170.0f )
				options.scene.fieldOfView = *degrees;
			else
				options.isValid = false;
		}
		else if ( isOption ( arg, L"-constants" ) )
		{
			if ( isOption ( value, L"root" ) )
				options.scene.constantsPath = TestConstantsPath::root;
			else if ( isOption ( value, L"cbv" ) )
				options.scene.constantsPath = TestConstantsPath::cbv;
			else if ( isOption ( value, L"table" ) )
				options.scene.constantsPath = TestConstantsPath::table;
			else
				options.isValid = false;
		}
		else if ( isOption ( arg, L"-hdr" ) )
		{
			if ( isOption ( value, L"off" ) )
				options.scene.output = TestOutput::sdr;
			else if ( isOption ( value, L"scrgb" ) )
				options.scene.output = TestOutput::scRgb;
			else if ( isOption ( value, L"pq" ) )
				options.scene.output = TestOutput::pq;
			else
				options.isValid = false;
		}
		else if ( isOption ( arg, L"-paper-white" ) || isOption ( arg, L"-peak" ) )
		{
			const auto	nits = parseNumber ( value );

			if ( nits.has_value () && *nits >= 1.0f && *nits <= 10000.0f )
				( isOption ( arg, L"-peak" ) ? options.scene.peak : options.scene.paperWhite ) = *nits;
			else
				options.isValid = false;
		}
		else if ( isOption ( arg, L"-size" ) )
		{
			if ( ! parseSize ( value, options.width, options.height ) )
				options.isValid = false;
		}
		else if ( const auto count = parseCount ( value ); count.has_value () )
		{
			if ( isOption ( arg, L"-frames" ) )
				options.frames = *count;
			else if ( isOption ( arg, L"-reload-every" ) )
				options.reloadEvery = *count;
			else if ( isOption ( arg, L"-probe-devices" ) )
				options.probeDevices = *count;
			else if ( isOption ( arg, L"-crash-at" ) )
				options.crashAt = *count;
			else if ( isOption ( arg, L"-exit-handler" ) )
				options.exitHandlerMs = *count;
			else if ( isOption ( arg, L"-terminate-at" ) )
				options.terminateAt = *count;
			else if ( isOption ( arg, L"-handled-faults" ) )
				options.handledFaults = *count;
			else if ( isOption ( arg, L"-dlss" ) )
				options.dlss.framesPerMode = *count;
			else if ( isOption ( arg, L"-dlss-warmup" ) )
				options.dlss.warmupMs = *count;
			else if ( isOption ( arg, L"-dlss-new-images" ) )
				options.dlss.newImagesEvery = *count;
			else if ( isOption ( arg, L"-dlss-pipelines" ) )
				options.dlss.pipelinesAtSwitch = *count;
			else if ( isOption ( arg, L"-dlss-reload" ) )
				options.dlss.reloadBeforeSwitch = *count;
			else if ( isOption ( arg, L"-dlss-fg-remake" ) )
				options.dlss.fgRemakeBeforeSwitch = *count;
			else if ( isOption ( arg, L"-fullscreen-at" ) || isOption ( arg, L"-windowed-at" ) )
				options.switches.push_back ( { *count, isOption ( arg, L"-fullscreen-at" ) ? TestSwitchKind::fullscreen : TestSwitchKind::windowed } );
			else
				options.resizeAt = *count;
		}
		else
			options.isValid = false;

		if ( ! options.isValid )
		{
			reportBadValue ( arg, value );
			break;
		}
	}

	LocalFree ( argv );

	if ( randomSeed.has_value () && options.isValid )
		pickSwitches ( options, *randomSeed, isRandomFullscreen );

	std::ranges::stable_sort ( options.switches, {}, &TestSwitch::frame );

	// The DLSS bench is a D3D12 game with NGX's 64-bit loader linked in
	if ( options.dlss.framesPerMode > 0 && options.isValid )
	{
#if ULTRA_TEST_DLSS
		if ( options.api.has_value () && *options.api != api::device_api::d3d12 )
		{
			reshade::log::message ( reshade::log::level::error, "Test application: -dlss runs on D3D12 only (-api dx12)." );
			options.isValid = false;
		}
		options.api = api::device_api::d3d12;
#else
		reshade::log::message ( reshade::log::level::error, "Test application: -dlss needs the 64-bit test application." );
		options.isValid = false;
#endif
	}

	// Fullscreen takes the foreground, which a background window never has
	if ( options.isBackground && std::ranges::any_of ( options.switches, [] ( const TestSwitch& change ) { return change.kind == TestSwitchKind::fullscreen; } ) )
	{
		reshade::log::message ( reshade::log::level::error, "Test application: fullscreen needs -show (background runs never take the foreground)." );
		options.isValid = false;
	}

	return options;
}
//-----------------------------------------------------------------------------

namespace
{
	[[ nodiscard ]] std::filesystem::path sharedEffectCache ()
	{
		wchar_t	path[ MAX_PATH ] {};
		GetModuleFileNameW ( nullptr, path, MAX_PATH );
		return std::filesystem::path ( path ).parent_path () / L"cache-shared";
	}
}
//-----------------------------------------------------------------------------

bool reshade::ultra::isColdCache ()
{
	auto		argc = 0;
	const auto	argv = CommandLineToArgvW ( GetCommandLineW (), &argc );
	auto		isCold = false;

	for ( auto i = 1; argv != nullptr && i < argc; ++i )
		isCold = isCold || isOption ( argv[ i ], L"-cold-cache" );

	LocalFree ( argv );
	return isCold;
}
//-----------------------------------------------------------------------------

// Only into a cache that has nothing yet: a run folder used again keeps what it built
void reshade::ultra::seedEffectCache ( const std::filesystem::path& cache )
{
	std::error_code	ec;

	if ( isColdCache () || ! std::filesystem::is_empty ( cache, ec ) || ec )
		return;

	for ( const auto& entry : std::filesystem::directory_iterator ( sharedEffectCache (), ec ) )
		if ( entry.is_regular_file ( ec ) )
			std::filesystem::copy_file ( entry.path (), cache / entry.path ().filename (), std::filesystem::copy_options::skip_existing, ec );
}
//-----------------------------------------------------------------------------

// Each file under a name of its own first, then renamed: a run starting meanwhile never copies half a file
void reshade::ultra::shareEffectCache ( const std::filesystem::path& cache )
{
	const auto		shared = sharedEffectCache ();
	std::error_code	ec;

	if ( isColdCache () || ( ! std::filesystem::create_directories ( shared, ec ) && ec ) )
		return;

	const auto	suffix = L".part" + std::to_wstring ( GetCurrentProcessId () );

	for ( const auto& entry : std::filesystem::directory_iterator ( cache, ec ) )
	{
		const auto	target = shared / entry.path ().filename ();

		if ( ! entry.is_regular_file ( ec ) || std::filesystem::exists ( target, ec ) )
			continue;

		auto	part = target;
		part += suffix;

		if ( std::filesystem::copy_file ( entry.path (), part, ec ) && ! MoveFileExW ( part.c_str (), target.c_str (), 0 ) )
			std::filesystem::remove ( part, ec );
	}
}
//-----------------------------------------------------------------------------

reshade::ultra::TestFrames::TestFrames ( const TestOptions& options, HWND window )
	: frames ( options.frames ), resizeAt ( options.resizeAt ), reloadEvery ( options.reloadEvery ), crashAt ( options.crashAt ), exitHandlerMs ( options.exitHandlerMs ),
	  terminateAt ( options.terminateAt ), stressLines ( options.stressLines ),
	  stressThreads ( options.stressThreads ), isLoggingFrames ( options.isLoggingFrames ), isLoggingThroughAddons ( options.isLoggingThroughAddons ),
	  isCheckingLogLines ( options.isCheckingLogLines ), handledFaults ( options.handledFaults ), configAccesses ( options.configAccesses ), switches ( options.switches ), isBackground ( options.isBackground ), window ( window ),
	  holdsClock ( options.frames != 0 || options.resizeAt.has_value () || options.reloadEvery != 0 || options.crashAt.has_value () || ! options.switches.empty () )
{
}
//-----------------------------------------------------------------------------

void reshade::ultra::TestFrames::holdClockUntilLoaded ()
{
	holdsClock = true;
}
//-----------------------------------------------------------------------------

bool reshade::ultra::TestFrames::isFixedStep () const
{
	return frames != 0;
}
//-----------------------------------------------------------------------------

// A run with numbered frames shows time 0 until ReShade has loaded its effects, so the motion starts clean and, with a
// fixed step, frame n is the same picture however long compiling takes; other runs animate from the first frame
bool reshade::ultra::TestFrames::next ()
{
	if ( ! hasStarted )
	{
		const auto	isLoaded = ! holdsClock || isReShadeLoaded ();

		if ( ! isLoaded && std::chrono::steady_clock::now () - start < loadTimeout )
			return true;

		if ( ! isLoaded )
			reshade::log::message ( reshade::log::level::error, "Test application: ReShade had not loaded its effects after %d s, the frames start anyway.",
									static_cast<int> ( loadTimeout.count () ) );

		hasStarted = true;
		start = std::chrono::steady_clock::now ();
	}

	if ( isFixedStep () && upcoming >= frames )
	{
		checkBackground ( "at the end" );
		return false;
	}

	current = upcoming++;

	if ( current == 0 )
		checkBackground ( "at the start" );
	else if ( isBackground && ! wasForeground && GetForegroundWindow () == window )
	{
		wasForeground = true;
		reshade::log::message ( reshade::log::level::error, "Test application: the background window became the foreground window in frame %zu.", current );
	}

	if ( resizeAt == current )
		shrinkWindow ();

	if ( reloadEvery != 0 && current != 0 && current % reloadEvery == 0 )
		requestAddonReload ();

	if ( isLoggingFrames )
		reshade::log::message ( reshade::log::level::info, "Test application: frame %zu.", current );

	if ( current == 1 && stressLines != 0 )
		logStress ( stressLines, stressThreads );

	if ( current == 1 && isLoggingThroughAddons )
		logThroughAddons ();

	if ( current == 2 && isCheckingLogLines )
		checkLogLines ();

	for ( const auto& access : configAccesses )
		if ( access.frame == current )
			accessConfig ( access );

	if ( crashAt == current )
		crash ( current );

	if ( current == 1 && exitHandlerMs.has_value () )
		followExit ( *exitHandlerMs );

	if ( terminateAt == current )
	{
		reshade::log::message ( reshade::log::level::info, "Test application: TerminateProcess on its own process in frame %zu.", current );
		TerminateProcess ( GetCurrentProcess (), 0 );
	}

	if ( handledFaults != 0 )
		causeHandledFaults ();

	return true;
}
//-----------------------------------------------------------------------------

// The frame time includes the faults' time, every 120 frames into the log
void reshade::ultra::TestFrames::causeHandledFaults ()
{
	constexpr size_t	reportEvery = 120;
	const auto			now = std::chrono::steady_clock::now ();

	if ( current != 0 )
		framesTime += now - frameStart;

	frameStart = now;
	const auto	caught = causeFaults ( handledFaults );
	faultsTime += std::chrono::steady_clock::now () - now;

	if ( caught != handledFaults )
		reshade::log::message ( reshade::log::level::error, "Test application: %zu of %zu access violations were caught.", caught, handledFaults );

	if ( current == 0 || current % reportEvery != 0 )
		return;

	reshade::log::message ( reshade::log::level::info, "Test application: frames %zu to %zu took %.3f ms each, %.3f ms of it for %zu caught access violations (%.2f us each).",
							current - reportEvery, current, framesTime.count () / reportEvery, faultsTime.count () / reportEvery, handledFaults,
							faultsTime.count () * 1000.0 / double ( reportEvery * handledFaults ) );
	framesTime = {};
	faultsTime = {};
}
//-----------------------------------------------------------------------------

size_t reshade::ultra::TestFrames::index () const
{
	return current;
}
//-----------------------------------------------------------------------------

std::optional<reshade::ultra::TestSwitch> reshade::ultra::TestFrames::takeSwitch ()
{
	if ( ! hasStarted || nextSwitch >= switches.size () || switches[ nextSwitch ].frame > current )
		return std::nullopt;

	return switches[ nextSwitch++ ];
}
//-----------------------------------------------------------------------------

float reshade::ultra::TestFrames::seconds () const
{
	if ( isFixedStep () )
		return static_cast<float> ( current ) * fixedStep;

	if ( ! hasStarted )
		return 0.0f;

	return std::chrono::duration<float> ( std::chrono::steady_clock::now () - start ).count ();
}
//-----------------------------------------------------------------------------

// Logs the visible windows below the test window (the desktop's own are always there) and whether it has the foreground
void reshade::ultra::TestFrames::checkBackground ( const char* when )
{
	if ( ! isBackground )
		return;

	std::string	below;

	for ( auto next = GetWindow ( window, GW_HWNDNEXT ); next != nullptr; next = GetWindow ( next, GW_HWNDNEXT ) )
	{
		char	name[ 64 ] {};

		if ( IsWindowVisible ( next ) && GetClassNameA ( next, name, sizeof ( name ) ) > 0 )
			below += std::string ( below.empty () ? "" : ", " ) + name;
	}

	const auto	isForeground = GetForegroundWindow () == window;
	reshade::log::message ( isForeground ? reshade::log::level::error : reshade::log::level::info, "Test application: %s the window is %sthe foreground; visible windows below it: %s.",
							when, isForeground ? "" : "not ", below.empty () ? "none" : below.c_str () );
}
//-----------------------------------------------------------------------------

// The size message arrives before this returns, so the frame it starts already renders at the new size
void reshade::ultra::TestFrames::shrinkWindow ()
{
	RECT	client {};
	GetClientRect ( window, &client );

	RECT	frame { 0, 0, client.right * 3 / 4, client.bottom * 3 / 4 };
	AdjustWindowRectEx ( &frame, static_cast<DWORD> ( GetWindowLongW ( window, GWL_STYLE ) ), FALSE, static_cast<DWORD> ( GetWindowLongW ( window, GWL_EXSTYLE ) ) );
	SetWindowPos ( window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE );
}
//-----------------------------------------------------------------------------

bool reshade::ultra::printTestUsage ()
{
	auto		argc = 0;
	const auto	argv = CommandLineToArgvW ( GetCommandLineW (), &argc );
	const auto	wanted = argc <= 1 || ( argv != nullptr && ( isOption ( argv[ 1 ], L"-help" ) || isOption ( argv[ 1 ], L"--help" ) || isOption ( argv[ 1 ], L"/?" ) ) );
	LocalFree ( argv );

	if ( ! wanted )
		return false;

	static constexpr char	usage[] =
		"ReShade test application: a scene in a window, the whole ReShade runtime with its overlay and add-ons.\n"
		"Runs stay behind every window and never take the focus; -show for a normal window.\n"
		"Agents and scripts run it through C:/dev/ultraGame/Tools/test-run.sh <owner> [--limit s] <options>.\n"
		"\n"
		"Run       -work-dir <dir>  -owner <name>  -show  -frames <n>  -cold-cache  -config-layered\n"
		"Scene     -api dx11|dx12  -scene cube|head  -size <w>x<h>  -fov <degrees>  -env <file.hdr>  -sss on|off  -jitter on|off\n"
		"Load      -draws <n> <scale>  (D3D11: the model n more times a frame at scale times its size, no depth test)\n"
		"HDR       -hdr off|scrgb|pq  -paper-white <nits>  -peak <nits>\n"
		"Output    -capture <frame> <file>  -dump-constants <frame> <file>  -dump-resource-ids <file>  -dump-file-events <file>\n"
		"Switches  -resize-at <frame>  -buffer-size-at <frame> <w>x<h>  -fullscreen-at <frame>  -windowed-at <frame>\n"
		"          -random-switches <seed> [fullscreen]  (fullscreen needs -show)\n"
		"Devices   -probe-devices <n>  (D3D11: n devices, each presenting once and released, before the run's own)\n"
		"Add-ons   -reload-every <n>  -reload-effects-every <n>  -toggle-technique-at <frame> <file.fx> <technique>  -constants root|cbv|table\n"
		"          -run-exclusive-at <frame>\n"
		"Files     -watch <file>  -watch-folder <folder> <pattern>  -unwatch-at <frame>\n"
		"Config    -set-config <frame> <section> <key> <value|->  -get-config <frame> <section> <key>\n"
		"Log       -log-frames  -log-addons  -log-stress <lines> <threads>  -check-log-lines  -crash-at <frame>  -handled-faults <n>\n"
		"Exit      -exit-handler <ms>  (an exit event handler that shows another thread alive, then sleeps ms)  -terminate-at <frame>\n"
		"DLSS      -dlss <frames per mode>  -dlss-warmup <ms>  (D3D12, 64-bit: a game with DLSS Super Resolution and mode switches, checking itself)\n"
		"          -dlss-streamline <folder>  -dlss-tags now|evaluate|present  -dlss-sl-track  -dlss-resize  -dlss-no-reset  -dlss-keep-compute\n"
		"          -dlss-new-images <n>  -dlss-pipelines <n>  -dlss-reload <n>  -dlss-game-exit  -dlss-fg  -dlss-fg-remake <n>\n"
		"Json      -json-tests  (the add-on API's json functions; the run ends when they are done)\n"
		"\n"
		"Exit codes: 0 fine, 1 bad command line, 2 ReShade never presented, 3 capture missing, 4 the log has errors, 5 a -dlss check failed.\n";

	// A windows program has no console of its own: the one it was started from, else nothing to write to
	if ( AttachConsole ( ATTACH_PARENT_PROCESS ) || GetStdHandle ( STD_OUTPUT_HANDLE ) != nullptr )
	{
		auto	out = GetStdHandle ( STD_OUTPUT_HANDLE );
		if ( out == nullptr || out == INVALID_HANDLE_VALUE )
			out = CreateFileW ( L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr );
		DWORD	written = 0;
		if ( out != nullptr && out != INVALID_HANDLE_VALUE )
			WriteFile ( out, usage, DWORD ( sizeof usage - 1 ), &written, nullptr );
	}

	return true;
}
//-----------------------------------------------------------------------------

#endif
