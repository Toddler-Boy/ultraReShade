/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <Windows.h>

#include "reshade_api_device.hpp"
#include "ultra_test_scene.hpp"

//-----------------------------------------------------------------------------

// The test application's command line beyond upstream's flags, and its frame clock
namespace reshade::ultra
{
	enum class TestSwitchKind
	{
		fullscreen,		// -fullscreen-at <frame>: exclusive fullscreen at the desktop's current display mode
		windowed,		// -windowed-at <frame>: back to the window
		bufferSize		// -buffer-size-at <frame> <w>x<h>: the swap chain's buffers only, the display mode stays
	};

	struct TestSwitch
	{
		size_t			frame = 0;
		TestSwitchKind	kind = TestSwitchKind::bufferSize;
		LONG			width = 0;
		LONG			height = 0;
	};

	// Through the add-on API on the global config, then read back into the log
	struct TestConfigAccess
	{
		size_t						frame = 0;
		std::string					section;
		std::string					key;
		bool						isWrite = false;
		std::optional<std::string>	value;		// a write without one removes the key
	};

	struct TestOptions
	{
		std::optional<api::device_api>	api;				// -api dx11|dx12
		TestSceneSettings				scene;				// -scene cube|head, -jitter on|off, -sss on|off, -env <file.hdr>, -fov <degrees>,
															// -hdr off|scrgb|pq, -paper-white <nits>, -peak <nits>,
															// -constants root|cbv|table
		LONG							width = 0;			// -size <w>x<h>, client area; 0: two thirds of the screen
		LONG							height = 0;
		size_t							frames = 0;			// -frames <n>: quit after n frames, each 1/60 s apart; 0: run until closed
		std::optional<size_t>			resizeAt;			// -resize-at <frame>: the window shrinks to three quarters before that frame
		std::vector<TestSwitch>			switches;			// in frame order; -random-switches <seed> [fullscreen] picks a few buffer sizes (and fullscreen switches)
		size_t							reloadEvery = 0;	// -reload-every <n>: the add-ons reload every n frames ([ADDON] ReloadOnChange=1)
		bool							isBackground = true;	// never activated, always at the bottom of the z-order, no fullscreen; -show: a normal window
		std::wstring					owner;				// -owner <name>: who started the run, shown in the window title
		std::optional<size_t>			crashAt;			// -crash-at <frame>: an access violation in that frame, without the error dialog
		size_t							stressLines = 0;	// -log-stress <lines> <threads>: that many lines in frame 1 from that many threads
		size_t							stressThreads = 0;	//   at once, with the time each call took on its thread
		bool							isLoggingFrames = false;	// -log-frames: a line in every frame
		bool							isLoggingThroughAddons = false;	// -log-addons: in frame 1 a line through the add-on API for each loaded add-on, and one for dxgi.dll
		bool							isCheckingLogLines = false;	// -check-log-lines: in frame 2 the Log tab's copy of new lines, also past the lines kept
		size_t							handledFaults = 0;	// -handled-faults <n>: n access violations caught in every frame, the frame times every 120 frames
		std::vector<TestConfigAccess>	configAccesses;		// -set-config <frame> <section> <key> <value|->, -get-config <frame> <section> <key>
		bool							isValid = true;		// false: an option was malformed, the log says which
	};

	[[ nodiscard ]] TestOptions parseTestOptions ();

	// With no options, or -help: writes the options to the console the test app was started from and returns true
	[[ nodiscard ]] bool printTestUsage ();

	// The run's shader cache starts from the shared one beside the test app (<exe folder>\cache-shared) unless -cold-cache;
	// a run without errors adds its new files to it afterwards
	[[ nodiscard ]] bool isColdCache ();
	void seedEffectCache ( const std::filesystem::path& cache );
	void shareEffectCache ( const std::filesystem::path& cache );

	class TestFrames
	{
	public:
		TestFrames ( const TestOptions& options, HWND window );

		// Starts the next frame; false once the requested frames are done
		[[ nodiscard ]] bool next ();

		// Frame n is then the same picture in every run: the clock starts once ReShade has loaded its effects
		void holdClockUntilLoaded ();

		[[ nodiscard ]] size_t index () const;
		[[ nodiscard ]] float seconds () const;

		// The next switch due by this frame, once (several on one frame come on the frames after)
		[[ nodiscard ]] std::optional<TestSwitch> takeSwitch ();

	private:
		[[ nodiscard ]] bool isFixedStep () const;
		void shrinkWindow ();
		void checkBackground ( const char* when );
		void causeHandledFaults ();

		size_t									frames;
		std::optional<size_t>					resizeAt;
		size_t									reloadEvery;
		std::optional<size_t>					crashAt;
		size_t									stressLines;
		size_t									stressThreads;
		bool									isLoggingFrames;
		bool									isLoggingThroughAddons;
		bool									isCheckingLogLines;
		size_t									handledFaults;
		std::vector<TestConfigAccess>			configAccesses;
		std::chrono::steady_clock::time_point	frameStart;
		std::chrono::duration<double, std::milli>	framesTime {};
		std::chrono::duration<double, std::milli>	faultsTime {};
		std::vector<TestSwitch>					switches;
		size_t									nextSwitch = 0;
		bool									isBackground;
		bool									wasForeground = false;
		HWND									window;
		size_t									current = 0;
		size_t									upcoming = 0;
		bool									hasStarted = false;
		bool									holdsClock = false;
		std::chrono::steady_clock::time_point	start = std::chrono::steady_clock::now ();
	};
}
