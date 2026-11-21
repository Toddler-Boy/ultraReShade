/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

//-----------------------------------------------------------------------------

// What ultraReShade answers the add-on that draws its overlay (export ReShadeHost; names and cells in include\reshade_ultra_host_names.h),
// in the form of include\reshade_ultra_api.h. A call names its effect runtime; the runtimes known are the ones constructed and not yet
// destroyed
namespace reshade
{
	class runtime;

	namespace ultra
	{
		// A runtime's construction and destruction
		void noteRuntime ( runtime* runtime );
		void forgetRuntime ( runtime* runtime );

		// Gives access to a runtime's overlay, screenshot and effect state to the answers in ultra_host_api.cpp
		struct HostAccess;
		struct ShadersAccess;
	}
}
