/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <optional>

#include <Windows.h>
#include <dxgi.h>

#include "ultra_test_run.hpp"

//-----------------------------------------------------------------------------

// The test application's switches between windowed, exclusive fullscreen and other buffer sizes (D3D11, D3D12).
// Fullscreen only ever uses the desktop's current display mode, and whatever ends the process (returning, exit, a
// crash, closing the window) leaves fullscreen first
namespace reshade::ultra
{
	class TestDisplay
	{
	public:
		TestDisplay ( IDXGISwapChain* swapchain, HWND window );
		~TestDisplay ();

		TestDisplay ( const TestDisplay& ) = delete;
		TestDisplay& operator= ( const TestDisplay& ) = delete;

		// The size the swap chain's buffers need now, when the switch changed it
		[[ nodiscard ]] std::optional<SIZE> apply ( const std::optional<TestSwitch>& change );

		[[ nodiscard ]] bool isFullscreen () const;

	private:
		[[ nodiscard ]] std::optional<SIZE> enterFullscreen ();
		[[ nodiscard ]] SIZE leaveFullscreen ();

		IDXGISwapChain*		swapchain;
		HWND				window;
		RECT				windowedRect {};
		HWND				previousForeground = nullptr;	// gets the foreground back after fullscreen
	};

	// For the window's close message
	void leaveTestFullscreen ();
}
