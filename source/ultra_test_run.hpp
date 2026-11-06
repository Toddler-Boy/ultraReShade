/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <optional>

#include <Windows.h>

#include "reshade_api_device.hpp"
#include "ultra_test_scene.hpp"

//-----------------------------------------------------------------------------

// The test application's command line beyond upstream's flags, and its frame clock
namespace reshade::ultra
{
	struct TestOptions
	{
		std::optional<api::device_api>	api;				// -api dx11|dx12
		TestSceneSettings				scene;				// -scene cube|head, -jitter on|off, -sss on|off, -env <file.hdr>, -fov <degrees>; -model makes it a head
		LONG							width = 0;			// -size <w>x<h>, client area; 0: two thirds of the screen
		LONG							height = 0;
		size_t							frames = 0;			// -frames <n>: quit after n frames, each 1/60 s apart; 0: run until closed
		std::optional<size_t>			resizeAt;			// -resize-at <frame>: the window shrinks to three quarters before that frame
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

	private:
		[[ nodiscard ]] bool isFixedStep () const;
		void shrinkWindow ();

		size_t									frames;
		std::optional<size_t>					resizeAt;
		HWND									window;
		size_t									current = 0;
		size_t									upcoming = 0;
		bool									hasStarted = false;
		std::chrono::steady_clock::time_point	start = std::chrono::steady_clock::now ();
	};
}
