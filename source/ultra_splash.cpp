/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <atomic>

#include <reshade_ultra.hpp>

//-----------------------------------------------------------------------------

namespace
{
	// An add-on's alert (reshade::set_logo_alert): the logo shows red and blinking while it is on, and never plays after it
	std::atomic<bool>	logoAlert { false };
	std::atomic<bool>	logoAlertEnded { false };
}

void ReShadeSetLogoAlert ( bool on )
{
	if ( logoAlert.exchange ( on ) && ! on )
		logoAlertEnded = true;
}
//-----------------------------------------------------------------------------

#if RESHADE_GUI

#include <algorithm>
#include <chrono>
#include <cmath>

#include <imgui.h>
#include <stb_image.h>

#include "dll_log.hpp"
#include "dll_resources.hpp"
#include "reshade_api_device.hpp"
#include "ultra_splash.hpp"

//-----------------------------------------------------------------------------

namespace
{
	// Start logo: total time in seconds, then shares of that time, of the logo width and of the screen height
	constexpr float				logoDuration = 1.5f;
	constexpr float				logoHold = 0.3f;
	constexpr float				logoEdge = 0.25f;
	constexpr float				logoHeight = 0.03f;
	constexpr unsigned char		logoTint[ 3 ] = { 0xff, 0x14, 0x93 };
	constexpr int				logoStrips = 48;

	// The alert: one blink per period, shown for its first share, each edge fading over alertFade seconds
	constexpr unsigned char		alertTint[ 3 ] = { 0xff, 0x26, 0x26 };
	constexpr float				alertPeriod = 1.0f;
	constexpr float				alertShown = 0.6f;
	constexpr float				alertFade = 0.08f;

	// Top-left corner of the ring, as shares of the screen width and height; the logo sits to its right, both centred
	// on one line, so the ring can run while the logo plays
	constexpr float	anchorX = 0.03f;
	constexpr float	anchorY = 0.04f;

	// Compile ring: sizes as shares of the screen height (24 and 2 pixels at 1080p), fades in seconds
	constexpr float	ringDiameter = 24.0f / 1080.0f;
	constexpr float	ringThickness = 2.0f / 1080.0f;
	constexpr float	ringGap = 10.0f / 1080.0f;
	constexpr float	ringShift = -10.0f / 1080.0f;	// the ring sits this much left of the corner; the logo stays
	constexpr float	ringTrackAlpha = 0.15f;
	constexpr float	ringArcAlpha = 0.7f;
	constexpr float	ringTurnsPerSecond = 1.0f;
	constexpr float	ringMinArc = 0.25f;		// share of the circle at no progress; the gap stays visible until the end
	constexpr float	ringGlide = 0.25f;		// seconds the arc takes to reach a new length
	constexpr float	ringFadeIn = 0.2f;
	constexpr float	ringFadeOut = 0.3f;

	// A loading hitch at game start would otherwise play the whole logo within one frame
	constexpr float	maxFrameStep = 1.0f / 20.0f;
	// A swap chain that stops presenting while showing the logo must not hold back every other one's ring
	constexpr std::chrono::milliseconds	logoOwnerTimeout { 500 };

	constexpr float	pi = 3.14159265f;

	// The logo plays once per process; a runtime destroyed or gone quiet while showing it hands the rest to the next one
	std::atomic<bool>								logoDone { false };
	std::atomic<bool>								logoBroken { false };	// its texture could not be made
	std::atomic<const reshade::ultra::SplashState*>	logoOwner { nullptr };
	std::atomic<std::chrono::steady_clock::rep>		logoOwnerSeen { 0 };
	std::atomic<float>								logoElapsed { 0.0f };
	std::atomic<float>								alertElapsed { 0.0f };
	std::atomic<bool>								alertLogged { false };

	[[ nodiscard ]] float smoothstep ( float edge0, float edge1, float x )
	{
		const auto	t = std::clamp ( ( x - edge0 ) / ( edge1 - edge0 ), 0.0f, 1.0f );
		return t * t * ( 3.0f - 2.0f * t );
	}

	[[ nodiscard ]] float sweepFront ( float phase )
	{
		return -logoEdge + std::clamp ( phase, 0.0f, 1.0f ) * ( 1.0f + 2.0f * logoEdge );
	}

	[[ nodiscard ]] float frontBand ( float front, float x )
	{
		const auto	band = smoothstep ( front - logoEdge, front - logoEdge * 0.5f, x ) * ( 1.0f - smoothstep ( front - logoEdge * 0.5f, front, x ) ) * 4.0f;
		return std::min ( band, 1.0f );
	}

	[[ nodiscard ]] ImU32 white ( float alpha )
	{
		return IM_COL32 ( 255, 255, 255, static_cast<int> ( alpha * 255.0f + 0.5f ) );
	}

	[[ nodiscard ]] ImU32 logoColour ( float band, float alpha )
	{
		const auto	channel = [ band ] ( unsigned char tint ) { return static_cast<int> ( 255.0f + ( tint - 255.0f ) * band + 0.5f ); };
		return IM_COL32 ( channel ( logoTint[ 0 ] ), channel ( logoTint[ 1 ] ), channel ( logoTint[ 2 ] ), static_cast<int> ( alpha * 255.0f + 0.5f ) );
	}

	[[ nodiscard ]] std::chrono::steady_clock::rep nowTicks ()
	{
		return std::chrono::steady_clock::now ().time_since_epoch ().count ();
	}

	[[ nodiscard ]] bool isLogoOwnerGone ( std::chrono::steady_clock::rep now )
	{
		return now - logoOwnerSeen.load () > std::chrono::duration_cast<std::chrono::steady_clock::duration> ( logoOwnerTimeout ).count ();
	}

	[[ nodiscard ]] bool isLogoWanted ()
	{
		return ! logoBroken && ( logoAlert || ( ! logoDone && ! logoAlertEnded ) );
	}

	[[ nodiscard ]] bool claimLogo ( const reshade::ultra::SplashState& state )
	{
		if ( ! isLogoWanted () )
			return false;

		const auto							now = nowTicks ();
		const reshade::ultra::SplashState*	expected = nullptr;
		const auto							isOwner = logoOwner.compare_exchange_strong ( expected, &state ) || expected == &state
													  || ( isLogoOwnerGone ( now ) && logoOwner.compare_exchange_strong ( expected, &state ) );

		if ( isOwner )
			logoOwnerSeen = now;

		return isOwner;
	}

	[[ nodiscard ]] bool isLogoPendingFor ( const reshade::ultra::SplashState& state )
	{
		if ( ! isLogoWanted () )
			return false;

		const auto	owner = logoOwner.load ();
		return owner == nullptr || owner == &state || isLogoOwnerGone ( nowTicks () );
	}

	[[ nodiscard ]] bool loadLogo ( reshade::ultra::SplashState& state, reshade::api::device* device )
	{
		if ( state.logoView != 0 )
			return true;

		const auto	png = reshade::resources::load_data_resource ( IDB_ULTRA_LOGO );

		if ( png.data == nullptr || png.data_size == 0 )
		{
			reshade::log::message ( reshade::log::level::error, "Failed to find the ultraReShade logo in the DLL resources." );
			return false;
		}

		auto		width = 0;
		auto		height = 0;
		auto		channels = 0;
		const auto	pixels = stbi_load_from_memory ( static_cast<const stbi_uc*> ( png.data ), static_cast<int> ( png.data_size ), &width, &height, &channels, STBI_rgb_alpha );

		if ( pixels == nullptr )
		{
			reshade::log::message ( reshade::log::level::error, "Failed to decode the ultraReShade logo (%s).", stbi_failure_reason () );
			return false;
		}

		const auto								rowPitch = static_cast<uint32_t> ( width ) * 4;
		const reshade::api::subresource_data	initialData { pixels, rowPitch, rowPitch * static_cast<uint32_t> ( height ) };
		const auto								isCreated = device->create_resource (
			reshade::api::resource_desc ( static_cast<uint32_t> ( width ), static_cast<uint32_t> ( height ), 1, 1, reshade::api::format::r8g8b8a8_unorm, 1, reshade::api::memory_heap::default_,
										  reshade::api::resource_usage::shader_resource | reshade::api::resource_usage::copy_dest ),
			&initialData, reshade::api::resource_usage::shader_resource, &state.logoTexture );

		stbi_image_free ( pixels );

		if ( ! isCreated )
		{
			reshade::log::message ( reshade::log::level::error, "Failed to create the ultraReShade logo texture." );
			state.logoTexture = {};
			return false;
		}

		if ( ! device->create_resource_view ( state.logoTexture, reshade::api::resource_usage::shader_resource, reshade::api::resource_view_desc ( reshade::api::format::r8g8b8a8_unorm ),
											  &state.logoView ) )
		{
			reshade::log::message ( reshade::log::level::error, "Failed to create the ultraReShade logo texture view." );
			device->destroy_resource ( state.logoTexture );
			state.logoTexture = {};
			state.logoView = {};
			return false;
		}

		state.logoWidth = static_cast<float> ( width );
		state.logoHeight = static_cast<float> ( height );
		return true;
	}

	// Vertical strips with per-vertex colour carry the sweeping edges; the tint is exact for white artwork
	void drawLogo ( const reshade::ultra::SplashState& state, const reshade::ultra::SplashFrame& frame, float time )
	{
		const auto	height = frame.height * logoHeight;
		const auto	width = height * state.logoWidth / state.logoHeight;
		const auto	left = frame.width * anchorX + frame.height * ( ringDiameter + ringGap );
		const auto	top = frame.height * anchorY;

		const auto	sweep = ( 1.0f - logoHold ) * 0.5f;
		const auto	frontIn = sweepFront ( time / sweep );
		const auto	frontOut = sweepFront ( ( time - sweep - logoHold ) / sweep );

		const auto	drawList = frame.drawList;
		drawList->PushTexture ( ImTextureRef ( state.logoView.handle ) );
		drawList->PrimReserve ( logoStrips * 6, ( logoStrips + 1 ) * 2 );

		const auto	first = static_cast<int> ( drawList->_VtxCurrentIdx );

		for ( auto i = 0; i <= logoStrips; ++i )
		{
			const auto	u = static_cast<float> ( i ) / logoStrips;
			const auto	alpha = ( 1.0f - smoothstep ( frontIn - logoEdge, frontIn, u ) ) * smoothstep ( frontOut - logoEdge, frontOut, u );
			const auto	colour = logoColour ( std::max ( frontBand ( frontIn, u ), frontBand ( frontOut, u ) ), alpha );
			const auto	x = left + u * width;

			drawList->PrimWriteVtx ( ImVec2 ( x, top ), ImVec2 ( u, 0.0f ), colour );
			drawList->PrimWriteVtx ( ImVec2 ( x, top + height ), ImVec2 ( u, 1.0f ), colour );
		}

		for ( auto i = 0; i < logoStrips; ++i )
		{
			const auto	v = first + i * 2;
			drawList->PrimWriteIdx ( static_cast<ImDrawIdx> ( v ) );
			drawList->PrimWriteIdx ( static_cast<ImDrawIdx> ( v + 2 ) );
			drawList->PrimWriteIdx ( static_cast<ImDrawIdx> ( v + 3 ) );
			drawList->PrimWriteIdx ( static_cast<ImDrawIdx> ( v ) );
			drawList->PrimWriteIdx ( static_cast<ImDrawIdx> ( v + 3 ) );
			drawList->PrimWriteIdx ( static_cast<ImDrawIdx> ( v + 1 ) );
		}

		drawList->PopTexture ();
	}

	void drawAlert ( const reshade::ultra::SplashState& state, const reshade::ultra::SplashFrame& frame, float time )
	{
		const auto	height = frame.height * logoHeight;
		const auto	width = height * state.logoWidth / state.logoHeight;
		const auto	left = frame.width * anchorX + frame.height * ( ringDiameter + ringGap );
		const auto	top = frame.height * anchorY;
		const auto	phase = std::fmod ( time, alertPeriod );
		const auto	shown = alertPeriod * alertShown;
		const auto	alpha = smoothstep ( 0.0f, alertFade, phase ) * ( 1.0f - smoothstep ( shown - alertFade, shown, phase ) );

		frame.drawList->AddImage ( ImTextureRef ( state.logoView.handle ), ImVec2 ( left, top ), ImVec2 ( left + width, top + height ), ImVec2 ( 0.0f, 0.0f ), ImVec2 ( 1.0f, 1.0f ),
								   IM_COL32 ( alertTint[ 0 ], alertTint[ 1 ], alertTint[ 2 ], static_cast<int> ( alpha * 255.0f + 0.5f ) ) );
	}

	// The arc turns all the time, so work shows even while one long effect compiles; its length shows the progress
	void drawRing ( ImDrawList* drawList, ImVec2 center, float screenHeight, float progress, float turn, float alpha )
	{
		const auto	thickness = std::max ( 1.0f, screenHeight * ringThickness );
		const auto	radius = ( screenHeight * ringDiameter - thickness ) * 0.5f;
		const auto	length = ringMinArc + ( 1.0f - ringMinArc ) * progress;
		const auto	start = 2.0f * pi * turn - 0.5f * pi;

		drawList->AddCircle ( center, radius, white ( ringTrackAlpha * alpha ), 0, thickness );
		drawList->PathArcTo ( center, radius, start, start + 2.0f * pi * length );
		drawList->PathStroke ( white ( ringArcAlpha * alpha ), ImDrawFlags_None, thickness );
	}

	void advanceLogo ( reshade::ultra::SplashState& state, const reshade::ultra::SplashFrame& frame, float step )
	{
		if ( ! claimLogo ( state ) )
			return;

		if ( ! loadLogo ( state, frame.device ) )
		{
			logoBroken = true;
			return;
		}

		if ( logoAlert )
		{
			if ( ! alertLogged.exchange ( true ) )
				reshade::log::message ( reshade::log::level::info, "Start logo: an add-on's alert, red and blinking until the add-on ends it." );
			const auto	time = alertElapsed.load ();
			drawAlert ( state, frame, time );
			alertElapsed = time + step;
			return;
		}

		const auto	elapsed = logoElapsed.load ();

		if ( elapsed >= logoDuration )
		{
			logoDone = true;
			return;
		}

		drawLogo ( state, frame, elapsed / logoDuration );
		logoElapsed = elapsed + step;
	}

	[[ nodiscard ]] bool isRingWanted ( const reshade::ultra::SplashFrame& frame )
	{
		return frame.isLoading && ! frame.isOverlayOpen;
	}

	void advanceRing ( reshade::ultra::SplashState& state, const reshade::ultra::SplashFrame& frame, float step )
	{
		// A load starts from no progress; the length then glides to the share of effects done and never goes back, so a
		// finished effect never snaps it; at the end it closes while the ring fades out
		if ( state.ringAlpha <= 0.0f )
			state.ringProgress = 0.0f;

		state.ringAlpha = isRingWanted ( frame ) ? std::min ( 1.0f, state.ringAlpha + step / ringFadeIn ) : std::max ( 0.0f, state.ringAlpha - step / ringFadeOut );

		const auto	target = frame.isLoading ? reshade::ultra::compileProgress ( frame.effectCount, frame.remainingEffects ) : 1.0f;

		if ( target > state.ringProgress )
			state.ringProgress += ( target - state.ringProgress ) * std::min ( 1.0f, step / ringGlide );

		if ( state.ringAlpha <= 0.0f )
			return;

		const auto	radius = frame.height * ringDiameter * 0.5f;
		const auto	center = ImVec2 ( frame.width * anchorX + frame.height * ringShift + radius, frame.height * ( anchorY + logoHeight * 0.5f ) );
		state.ringTurn = std::fmod ( state.ringTurn + step * ringTurnsPerSecond, 1.0f );
		drawRing ( frame.drawList, center, frame.height, state.ringProgress, state.ringTurn, state.ringAlpha );
	}
}
//-----------------------------------------------------------------------------

bool reshade::ultra::wantsSplashFrame ( const SplashState& state, bool isLoading )
{
	return isLoading || state.ringAlpha > 0.0f || isLogoPendingFor ( state );
}
//-----------------------------------------------------------------------------

void reshade::ultra::drawSplash ( SplashState& state, const SplashFrame& frame )
{
	const auto	step = std::clamp ( frame.deltaTime, 0.0f, maxFrameStep );
	advanceLogo ( state, frame, step );
	advanceRing ( state, frame, step );
}
//-----------------------------------------------------------------------------

void reshade::ultra::destroySplash ( SplashState& state, api::device* device )
{
	const SplashState*	expected = &state;
	logoOwner.compare_exchange_strong ( expected, nullptr );

	device->destroy_resource_view ( state.logoView );
	device->destroy_resource ( state.logoTexture );
	state = SplashState ();
}
//-----------------------------------------------------------------------------

float reshade::ultra::compileProgress ( size_t effectCount, size_t remainingEffects )
{
	if ( effectCount == 0 || remainingEffects > effectCount )
		return 1.0f;

	return static_cast<float> ( effectCount - remainingEffects ) / static_cast<float> ( effectCount );
}
//-----------------------------------------------------------------------------

void reshade::ultra::drawWindowProgressRing ( float progress )
{
	const auto	pos = ImGui::GetWindowPos ();
	const auto	size = ImGui::GetWindowSize ();
	const auto	turn = static_cast<float> ( std::fmod ( ImGui::GetTime () * ringTurnsPerSecond, 1.0 ) );
	drawRing ( ImGui::GetWindowDrawList (), ImVec2 ( pos.x + size.x * 0.5f, pos.y + size.y * 0.5f ), ImGui::GetIO ().DisplaySize.y, progress, turn, 1.0f );
}
//-----------------------------------------------------------------------------

#endif
