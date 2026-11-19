/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>

#include <d3d12.h>
#include <dxgi1_4.h>

//-----------------------------------------------------------------------------

// -dlss: the test app as a game that ships DLSS. It starts DLSS on the device it was given (NGX directly, or Streamline in front of
// it), renders a moving scene with jitter, depth and motion vectors below the window's size, evaluates DLSS Super Resolution in its
// frame's list between passes of its own, switches the DLSS mode as a game does, and checks itself: each check a line
// "dlss bench: <what>: ok" or "... FAILED: ...", then "dlss bench: N checks, all ok" or the failures; timings as "dlss bench: time ..."
namespace reshade::ultra
{
	// How long Streamline may count on the tagged images: -dlss-tags now|evaluate|present
	enum class TestDlssTags { onlyNow, untilEvaluate, untilPresent };

	struct TestDlssSettings
	{
		size_t					framesPerMode = 0;		// -dlss <frames>: frames per DLSS mode; 0: off
		size_t					warmupMs = 3000;		// -dlss-warmup <ms>: the first mode lasts at least this long, as NR starts in the background
		std::filesystem::path	streamlineFolder;		// -dlss-streamline <folder>: DLSS through that folder's sl.interposer.dll and plugins
		TestDlssTags			tags = TestDlssTags::untilPresent;
		bool					isListStateTracked = false;	// -dlss-sl-track: Streamline restores the list's state after its evaluate
		bool					isResizedAtSwitch = false;	// -dlss-resize: each mode switch resizes the swap chain to its own size
		bool					isResetSkipped = false;		// -dlss-no-reset: a new mode's first evaluate without Reset
		bool					reliesOnCompute = false;	// -dlss-keep-compute: a compute pass after DLSS counts on its bindings from before
		size_t					newImagesEvery = 0;		// -dlss-new-images <n>: new colour, depth and vector images of the same size every n frames
		size_t					pipelinesAtSwitch = 0;	// -dlss-pipelines <n>: n new pipelines after each mode switch
		size_t					reloadBeforeSwitch = 0;	// -dlss-reload <n>: the add-ons reload n frames before each mode switch (development builds)
		bool					isGameExit = false;		// -dlss-game-exit: the end as Ghost of Tsushima's: tags cleared, no release, no NGX or Streamline shutdown
		bool					isFrameGenerated = false;	// -dlss-fg (with -dlss-streamline): Frame Generation alongside, with Reflex, through Streamline's swap chain
		size_t					fgRemakeBeforeSwitch = 0;	// -dlss-fg-remake <n>: Frame Generation turned off, freed and on again n frames before each switch
	};

	using TestDlssFactoryCreator = HRESULT ( WINAPI* ) ( UINT flags, REFIID riid, void** factory );
	using TestDlssDeviceCreator = HRESULT ( WINAPI* ) ( IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** device );

	// -dlss-fg: the DXGI factory and the D3D12 device come from Streamline's interposer, so swap chain, device, queue and lists are its
	// proxies; null otherwise
	[[ nodiscard ]] TestDlssFactoryCreator testDlssFactoryCreator ();
	[[ nodiscard ]] TestDlssDeviceCreator testDlssDeviceCreator ();

	// -dlss-fg: the device goes to Streamline (slSetD3DDevice) before its factory makes the swap chain
	void attachTestDlssDevice ( ID3D12Device* device );

	// Before the app makes its device: Streamline's slInit, which must come first
	void prepareTestDlssBench ( const TestDlssSettings& settings, const std::filesystem::path& dataPath );

	// Runs the whole sequence; pump handles the window's messages and is false once it closed. 0: every check passed, 5: one failed
	[[ nodiscard ]] int runTestDlssBench ( ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain3* swapchain, const TestDlssSettings& settings,
										   const std::filesystem::path& dataPath, const std::function<bool ()>& pump );
}
