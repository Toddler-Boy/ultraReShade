/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "reshade_api_resource.hpp"

struct ImDrawList;

namespace reshade::api
{
	struct device;
}

//-----------------------------------------------------------------------------

// Top-left corner: a progress ring while effects compile and the overlay is closed, and to its right the start logo
// once per game start, or red and blinking while an add-on's alert is on (ReShadeSetLogoAlert).
namespace reshade::ultra
{
	struct SplashState
	{
		api::resource		logoTexture {};
		api::resource_view	logoView {};
		float				logoWidth = 0.0f;
		float				logoHeight = 0.0f;
		float				ringAlpha = 0.0f;
		float				ringProgress = 0.0f;
		float				ringTurn = 0.0f;
	};

	struct SplashFrame
	{
		api::device*	device;
		ImDrawList*		drawList;
		float			width;
		float			height;
		float			deltaTime;
		size_t			effectCount;
		size_t			remainingEffects;
		bool			isLoading;
		bool			isOverlayOpen;
	};

	[[ nodiscard ]] bool wantsSplashFrame ( const SplashState& state, bool isLoading );
	void drawSplash ( SplashState& state, const SplashFrame& frame );
	void destroySplash ( SplashState& state, api::device* device );

	[[ nodiscard ]] float compileProgress ( size_t effectCount, size_t remainingEffects );
	void drawWindowProgressRing ( float progress );
}
