/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <chrono>
#include <cstddef>
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

	struct TestOptions
	{
		std::optional<api::device_api>	api;				// -api dx11|dx12
		TestSceneSettings				scene;				// -scene cube|head, -jitter on|off, -sss on|off, -env <file.hdr>, -fov <degrees>,
															// -hdr off|scrgb|pq, -paper-white <nits>, -peak <nits>,
															// -constants root|cbv|table; -model makes it a head
		LONG							width = 0;			// -size <w>x<h>, client area; 0: two thirds of the screen
		LONG							height = 0;
		size_t							frames = 0;			// -frames <n>: quit after n frames, each 1/60 s apart; 0: run until closed
		std::optional<size_t>			resizeAt;			// -resize-at <frame>: the window shrinks to three quarters before that frame
		std::vector<TestSwitch>			switches;			// in frame order; -random-switches <seed> [fullscreen] picks a few buffer sizes (and fullscreen switches)
		size_t							reloadEvery = 0;	// -reload-every <n>: the add-ons reload every n frames ([ADDON] ReloadOnChange=1)
		bool							isBackground = false;	// -background: never activated, always at the bottom of the z-order; no fullscreen
		std::wstring					owner;				// -owner <name>: who started the run, shown in the window title
		bool							isValid = true;		// false: an option was malformed, the log says which
	};

	[[ nodiscard ]] TestOptions parseTestOptions ();

	class TestFrames
	{
	public:
		TestFrames ( const TestOptions& options, HWND window );

		// Starts the next frame; false once the requested frames are done
		[[ nodiscard ]] bool next ();

		[[ nodiscard ]] size_t index () const;
		[[ nodiscard ]] float seconds () const;

		// The next switch due by this frame, once (several on one frame come on the frames after)
		[[ nodiscard ]] std::optional<TestSwitch> takeSwitch ();

	private:
		[[ nodiscard ]] bool isFixedStep () const;
		void shrinkWindow ();
		void checkBackground ( const char* when );

		size_t									frames;
		std::optional<size_t>					resizeAt;
		size_t									reloadEvery;
		std::vector<TestSwitch>					switches;
		size_t									nextSwitch = 0;
		bool									isBackground;
		bool									wasForeground = false;
		HWND									window;
		size_t									current = 0;
		size_t									upcoming = 0;
		bool									hasStarted = false;
		std::chrono::steady_clock::time_point	start = std::chrono::steady_clock::now ();
	};
}
