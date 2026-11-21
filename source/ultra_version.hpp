/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <string>

//-----------------------------------------------------------------------------

// This build of ultraReShade: the number in the VERSION file and the time the DLL was built. Both are taken in by
// ultra_version.cpp, which the build compiles again with every build of the DLL and never otherwise
namespace reshade::ultra
{
	struct Version
	{
		std::string	number;		// 6.8.0
		std::string	built;		// as the compiler wrote it: Nov 21 2026 14:03:22
	};

	[[ nodiscard ]] const Version& version ();

	// "6.8.0 - Nov 21 2026 14:03:22"
	[[ nodiscard ]] std::string versionText ();
}
