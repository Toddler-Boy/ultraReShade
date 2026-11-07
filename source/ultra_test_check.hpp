/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>

//-----------------------------------------------------------------------------

// What an agent reads after a test run: -capture <frame> <file.png> (the picture after the effects, without the
// overlay; an scRGB swap chain writes <file>.exr, HDR10 a 16-bit PQ PNG) and the exit code
//   0 fine, 1 bad command line, 2 ReShade never presented a frame, 3 capture not written, 4 the log has errors
namespace reshade::ultra
{
	class TestFrames;

	// True once ReShade presented with every effect loaded (stays true through later reloads)
	[[ nodiscard ]] bool isReShadeLoaded ();

	class TestCheck
	{
	public:
		TestCheck ();
		~TestCheck ();

		TestCheck ( const TestCheck& ) = delete;
		TestCheck& operator= ( const TestCheck& ) = delete;

		// Captures go by the scene's frame number, which presents don't follow (no present on a resize frame)
		void watch ( const TestFrames& frames );

		[[ nodiscard ]] bool isValid () const;
		[[ nodiscard ]] int exitCode ( int loopResult ) const;

	private:
		bool							isCommandLineValid = true;
		std::optional<size_t>			captureFrame;
		std::filesystem::path			capturePath;
	};
}
