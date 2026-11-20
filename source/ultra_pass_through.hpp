/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <filesystem>

#include <Windows.h>

//-----------------------------------------------------------------------------

// ReShade as a proxy DLL (named like a system DLL) in an exe ultraGame did not install into: every function it exports
// that the system DLL exports too jumps to the system DLL's function of that name, which loads at the first call. Nothing else of ReShade runs
namespace reshade::ultra
{
	// The system DLL self stands in for, empty when it is no proxy (the Vulkan layer, an injected one)
	[[ nodiscard ]] std::filesystem::path proxiedSystemDll ( HMODULE self );

	// From DllMain, before anything else of ReShade; false when not every export could be forwarded (the debugger's output says why)
	[[ nodiscard ]] bool startPassThrough ( HMODULE self, const std::filesystem::path& systemDll );
}
