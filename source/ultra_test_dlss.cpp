/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if defined ( RESHADE_TEST_APPLICATION ) && ULTRA_TEST_DLSS

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>

#if ULTRA_TEST_STREAMLINE
#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#endif

#include <atomic>

#include <reshade.hpp>

#include "dll_log.hpp"
#include "ultra_addon_reload.hpp"
#include "ultra_test_dlss.hpp"

//-----------------------------------------------------------------------------

namespace
{
	template <typename T>
	using ComPtr = Microsoft::WRL::ComPtr<T>;

	constexpr char	shaderSource[] = R"(
cbuffer Frame : register ( b0 )
{
	row_major float4x4	viewProj;
	row_major float4x4	prevViewProj;
	row_major float4x4	world[ 2 ];
	row_major float4x4	prevWorld[ 2 ];
	float2				jitterNdc;
	float2				renderSize;
};

cbuffer Object : register ( b1 )
{
	uint	objectKind;		// 0 the wall around the camera, 1 the floor, 2 the cube
	uint	worldIndex;
};

static const float	pi = 3.14159265;
static const uint	wallSegments = 128;
static const float	wallRadius = 20;
static const float	wallBottom = -2;
static const float	wallTop = 12;

static const float2	quadCorners[ 6 ] = { float2 ( 0, 0 ), float2 ( 1, 0 ), float2 ( 0, 1 ), float2 ( 1, 0 ), float2 ( 1, 1 ), float2 ( 0, 1 ) };
static const float3	cubeCorners[ 8 ] = { float3 ( -0.5, -0.5, -0.5 ), float3 ( 0.5, -0.5, -0.5 ), float3 ( -0.5, 0.5, -0.5 ), float3 ( 0.5, 0.5, -0.5 ),
										 float3 ( -0.5, -0.5, 0.5 ), float3 ( 0.5, -0.5, 0.5 ), float3 ( -0.5, 0.5, 0.5 ), float3 ( 0.5, 0.5, 0.5 ) };
static const uint	cubeIndices[ 36 ] = { 0, 1, 2, 1, 3, 2, 4, 6, 5, 5, 6, 7, 0, 2, 4, 4, 2, 6, 1, 5, 3, 5, 7, 3, 0, 4, 1, 1, 4, 5, 2, 3, 6, 3, 7, 6 };

uint hash ( uint x )
{
	x ^= x >> 16;
	x *= 0x7feb352d;
	x ^= x >> 15;
	x *= 0x846ca68b;
	x ^= x >> 16;
	return x;
}

float unit ( uint h )
{
	return float ( h & 0xffff ) / 65535.0;
}

struct SceneVsOut
{
	float4	position : SV_Position;
	float3	worldPosition : POSITION;
	float3	localPosition : TEXCOORD0;
	float4	currentClip : TEXCOORD1;
	float4	previousClip : TEXCOORD2;
};

struct ScenePsOut
{
	float4	colour : SV_Target0;
	float2	motion : SV_Target1;
};

float3 localPositionOf ( uint id )
{
	const float2	c = quadCorners[ id % 6 ];

	if ( objectKind == 0 )
	{
		const float	a = ( ( id / 6 ) + c.x ) / wallSegments * 2 * pi;
		return float3 ( wallRadius * sin ( a ), lerp ( wallBottom, wallTop, c.y ), wallRadius * cos ( a ) );
	}

	if ( objectKind == 1 )
		return float3 ( lerp ( -40, 40, c.x ), wallBottom, lerp ( -40, 40, c.y ) );

	return cubeCorners[ cubeIndices[ id % 36 ] ];
}

SceneVsOut sceneVs ( uint id : SV_VertexID )
{
	const float3	l = localPositionOf ( id );
	const float4	w = mul ( float4 ( l, 1 ), world[ worldIndex ] );
	const float4	pw = mul ( float4 ( l, 1 ), prevWorld[ worldIndex ] );

	SceneVsOut	o;
	o.currentClip = mul ( w, viewProj );
	o.previousClip = mul ( pw, prevViewProj );
	o.position = o.currentClip;
	o.position.xy += jitterNdc * o.position.w;
	o.worldPosition = w.xyz;
	o.localPosition = l;
	return o;
}

// Linear scene light: stripes of random brightness around the camera (a few above 1), a checkered floor, a cube with a colour per side
float3 sceneColour ( float3 p, float3 l )
{
	if ( objectKind == 0 )
	{
		const float	around = ( atan2 ( p.x, p.z ) + pi ) * 50;
		const uint	stripe = uint ( around );
		const uint	band = uint ( ( p.y - wallBottom ) * 1.5 );
		const uint	h = hash ( stripe * 977 + band * 131 );
		const float	brightness = ( h % 17 ) == 0 ? 3.0 : 0.15 + 0.75 * unit ( h );
		const float3	tint = lerp ( float3 ( 1, 0.85, 0.7 ), float3 ( 0.7, 0.85, 1 ), unit ( hash ( stripe + 7 ) ) );
		return brightness * tint * ( 0.9 + 0.1 * frac ( around ) );
	}

	if ( objectKind == 1 )
	{
		const int	checker = ( int ( floor ( p.x ) ) + int ( floor ( p.z ) ) ) & 1;
		return checker != 0 ? float3 ( 0.6, 0.55, 0.45 ) : float3 ( 0.2, 0.22, 0.25 );
	}

	const float3	a = abs ( l );
	if ( a.x >= a.y && a.x >= a.z )
		return l.x > 0 ? float3 ( 0.9, 0.2, 0.15 ) : float3 ( 0.2, 0.8, 0.3 );
	if ( a.y >= a.z )
		return l.y > 0 ? float3 ( 0.2, 0.3, 0.9 ) : float3 ( 0.9, 0.8, 0.2 );
	return l.z > 0 ? float3 ( 0.8, 0.3, 0.8 ) : float3 ( 0.3, 0.8, 0.9 );
}

// Tone-mapped colour (DLSS without IsHDR); motion as Ghost of Tsushima stores it: this frame's position minus the last one's in
// texture units, both without jitter (DLSS's MV scale is minus the render size)
ScenePsOut scenePs ( SceneVsOut i )
{
	const float2	current = i.currentClip.xy / i.currentClip.w;
	const float2	previous = i.previousClip.xy / i.previousClip.w;

	const float3	light = sceneColour ( i.worldPosition, i.localPosition );

	ScenePsOut	o;
	o.colour = float4 ( light / ( 1 + light ), 1 );
	o.motion = ( current - previous ) * float2 ( 0.5, -0.5 );
	return o;
}

struct FullscreenOut
{
	float4	position : SV_Position;
	float2	uv : TEXCOORD;
};

FullscreenOut fullscreenVs ( uint id : SV_VertexID )
{
	FullscreenOut	o;
	o.uv = float2 ( ( id << 1 ) & 2, id & 2 );
	o.position = float4 ( o.uv * float2 ( 2, -2 ) + float2 ( -1, 1 ), 0, 1 );
	return o;
}

// The pass after DLSS: the frame's vectors, last frame's (the history) and a reprojection from its own record of this and last frame's camera
cbuffer Water : register ( b0 )
{
	float2	waterSize;
	float	waterScale;
	float	waterPad;
};

struct WaterData
{
	row_major float4x4	viewProj;
	row_major float4x4	prevViewProj;
	float4				offset;
};

Texture2D<float2>				waterMotion : register ( t0 );
Texture2D<float2>				waterHistory : register ( t1 );
StructuredBuffer<WaterData>	waterData : register ( t2 );

float2 waterPs ( FullscreenOut i ) : SV_Target
{
	const int3		p = int3 ( i.position.xy, 0 );
	const WaterData	d = waterData[ 0 ];
	const float2	ndc = i.position.xy / waterSize * float2 ( 2, -2 ) + float2 ( -1, 1 );
	const float4	a = mul ( float4 ( ndc, 0.5, 1 ), d.viewProj );
	const float4	b = mul ( float4 ( ndc, 0.5, 1 ), d.prevViewProj );
	return waterMotion.Load ( p ) + 0.5 * waterHistory.Load ( p ) + ( a.xy / a.w - b.xy / b.w ) * waterScale + d.offset.xy;
}

Texture2D<float4>	composeShown : register ( t0 );
SamplerState		linearClamp : register ( s0 );

float4 composePs ( FullscreenOut i ) : SV_Target
{
	const float3	t = saturate ( composeShown.SampleLevel ( linearClamp, i.uv, 0 ).rgb );
	return float4 ( t <= 0.0031308 ? t * 12.92 : 1.055 * pow ( t, 1 / 2.4 ) - 0.055, 1 );
}

cbuffer PassArguments : register ( b0 )
{
	uint	what;			// csSum: which image; csAnalyse: unused
	uint	slot;			// csSum: the result cell it adds to
	uint2	passSize;
	uint2	displaySize;
	uint2	inputSize;
	uint4	rowY;			// csAnalyse: the rows whose brightness goes to rows
};

Texture2D<float4>			cColour : register ( t0 );
Texture2D<float>			cDepth : register ( t1 );
Texture2D<float2>			cMotion : register ( t2 );
Texture2D<float2>			cHistory : register ( t3 );
Texture2D<float2>			cWaterPre : register ( t4 );
Texture2D<float2>			cWaterPost : register ( t5 );
Texture2D<float4>			cShown : register ( t6 );
Texture2D<float>			cDepthHistory : register ( t7 );
RWByteAddressBuffer			results : register ( u0 );
RWStructuredBuffer<float>	rows : register ( u1 );
RWTexture2D<float4>			shownOut : register ( u2 );
RWTexture2D<float2>			motionOut : register ( u3 );

struct Camera
{
	row_major float4x4	transform;
};

StructuredBuffer<Camera>	currentInverse : register ( t8 );
StructuredBuffer<Camera>	previousCamera : register ( t9 );

groupshared uint	groupSums[ 4 ];

void addToCell ( uint index, uint value, uint thread )
{
	InterlockedAdd ( groupSums[ index ], value );
	GroupMemoryBarrierWithGroupSync ();
	if ( thread == 0 )
		results.InterlockedAdd ( ( slot + index ) * 4, groupSums[ index ] );
}

[ numthreads ( 64, 1, 1 ) ]
void csClear ( uint3 id : SV_DispatchThreadID )
{
	results.Store ( id.x * 4, 0 );
}

// An order-free checksum of an image's bits: the sum over its pixels of their bits times a weight per position
[ numthreads ( 8, 8, 1 ) ]
void csSum ( uint3 id : SV_DispatchThreadID, uint thread : SV_GroupIndex )
{
	if ( thread == 0 )
		groupSums[ 0 ] = 0;
	GroupMemoryBarrierWithGroupSync ();

	uint	value = 0;
	if ( all ( id.xy < passSize ) )
	{
		const int3	p = int3 ( id.xy, 0 );
		uint4		bits = 0;
		switch ( what )
		{
			case 0:		bits = asuint ( cColour.Load ( p ) ); break;
			case 1:		bits.x = asuint ( cDepth.Load ( p ) ); break;
			case 2:		bits.xy = asuint ( cMotion.Load ( p ) ); break;
			case 3:		bits.xy = asuint ( cHistory.Load ( p ) ); break;
			case 4:		bits.xy = asuint ( cWaterPre.Load ( p ) ); break;
			case 5:		bits.xy = asuint ( cWaterPost.Load ( p ) ); break;
			default:	bits.x = asuint ( cDepthHistory.Load ( p ) ); break;
		}
		const uint	mixed = bits.x * 0x9E3779B1 + bits.y * 0x85EBCA77 + bits.z * 0xC2B2AE3D + bits.w * 0x27D4EB2F + 0x165667B1;
		value = mixed * ( hash ( id.y * passSize.x + id.x ) | 1 );
	}

	addToCell ( 0, value, thread );
}

// The floor's vectors once more, from depth and the pass's own copy of last frame's camera, written over the scene's as a game's water
// pass writes its surface's: counts the floor's pixels up to 15 m away and those where the two differ by more than half a pixel
[ numthreads ( 8, 8, 1 ) ]
void csWater ( uint3 id : SV_DispatchThreadID, uint thread : SV_GroupIndex )
{
	if ( thread < 2 )
		groupSums[ thread ] = 0;
	GroupMemoryBarrierWithGroupSync ();

	uint2	counts = 0;
	if ( all ( id.xy < passSize ) )
	{
		const float		d = cDepth.Load ( int3 ( id.xy, 0 ) );
		const float2	ndc = ( float2 ( id.xy ) + 0.5 ) / float2 ( passSize ) * float2 ( 2, -2 ) + float2 ( -1, 1 );
		const float4	w = mul ( float4 ( ndc, d, 1 ), currentInverse[ 0 ].transform );
		const float3	p = w.xyz / w.w;
		if ( d > 0 && abs ( p.y - wallBottom ) < 0.02 && length ( p.xz ) < 15 )
		{
			const float4	before = mul ( float4 ( p, 1 ), previousCamera[ 0 ].transform );
			const float2	water = ( ndc - before.xy / before.w ) * float2 ( 0.5, -0.5 );
			const float2	scene = motionOut[ id.xy ];
			counts.x = 1;
			counts.y = any ( abs ( water - scene ) * float2 ( passSize ) > 0.5 ) ? 1 : 0;
			motionOut[ id.xy ] = water;
		}
	}

	InterlockedAdd ( groupSums[ 0 ], counts.x );
	InterlockedAdd ( groupSums[ 1 ], counts.y );
	GroupMemoryBarrierWithGroupSync ();

	if ( thread < 2 )
		results.InterlockedAdd ( ( slot + thread ) * 4, groupSums[ thread ] );
}

[ numthreads ( 8, 8, 1 ) ]
void csFill ( uint3 id : SV_DispatchThreadID )
{
	if ( all ( id.xy < displaySize ) )
		shownOut[ id.xy ] = float4 ( -7, -7, -7, 1 );
}

// DLSS's picture: pixels it left unwritten, its brightness, pixels that differ from the input, and the brightness of the checked rows
[ numthreads ( 8, 8, 1 ) ]
void csAnalyse ( uint3 id : SV_DispatchThreadID, uint thread : SV_GroupIndex )
{
	if ( thread < 4 )
		groupSums[ thread ] = 0;
	GroupMemoryBarrierWithGroupSync ();

	uint4	counts = 0;
	if ( all ( id.xy < displaySize ) )
	{
		const float4	s = cShown.Load ( int3 ( id.xy, 0 ) );
		const uint2		at = min ( uint2 ( ( float2 ( id.xy ) + 0.5 ) * float2 ( inputSize ) / float2 ( displaySize ) ), inputSize - 1 );
		const float4	input = cColour.Load ( int3 ( at, 0 ) );
		const float		brightness = dot ( max ( s.rgb, 0 ), float3 ( 0.2126, 0.7152, 0.0722 ) );
		const float3	difference = abs ( s.rgb - input.rgb );

		counts.x = s.r == -7 && s.g == -7 && s.b == -7 ? 1 : 0;
		counts.y = uint ( min ( brightness, 4.0 ) * 64 );
		counts.z = max ( difference.r, max ( difference.g, difference.b ) ) > 0.002 ? 1 : 0;
		counts.w = 1;

		for ( uint r = 0; r < 4; ++r )
			if ( id.y == rowY[ r ] )
				rows[ r * displaySize.x + id.x ] = brightness;
	}

	InterlockedAdd ( groupSums[ 0 ], counts.x );
	InterlockedAdd ( groupSums[ 1 ], counts.y );
	InterlockedAdd ( groupSums[ 2 ], counts.z );
	InterlockedAdd ( groupSums[ 3 ], counts.w );
	GroupMemoryBarrierWithGroupSync ();

	if ( thread < 4 )
		results.InterlockedAdd ( ( slot + thread ) * 4, groupSums[ thread ] );
}
)";

	using Matrix = std::array<float, 16>;

	[[ nodiscard ]] Matrix multiply ( const Matrix& a, const Matrix& b )
	{
		Matrix	out {};

		for ( auto row = 0; row < 4; ++row )
			for ( auto col = 0; col < 4; ++col )
				for ( auto k = 0; k < 4; ++k )
					out[ row * 4 + col ] += a[ row * 4 + k ] * b[ k * 4 + col ];

		return out;
	}

	[[ nodiscard ]] Matrix identity ()
	{
		return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	}

	[[ nodiscard ]] Matrix translation ( float x, float y, float z )
	{
		return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1 };
	}

	[[ nodiscard ]] Matrix scaling ( float s )
	{
		return { s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1 };
	}

	[[ nodiscard ]] Matrix rotationX ( float angle )
	{
		return { 1, 0, 0, 0, 0, std::cos ( angle ), std::sin ( angle ), 0, 0, -std::sin ( angle ), std::cos ( angle ), 0, 0, 0, 0, 1 };
	}

	[[ nodiscard ]] Matrix rotationY ( float angle )
	{
		return { std::cos ( angle ), 0, -std::sin ( angle ), 0, 0, 1, 0, 0, std::sin ( angle ), 0, std::cos ( angle ), 0, 0, 0, 0, 1 };
	}

	constexpr float		fieldOfView = 60.0f * 3.14159265f / 180.0f;
	constexpr float		zNear = 0.1f;
	constexpr float		zFar = 200.0f;
	constexpr float		framesPerSecond = 60.0f;
	constexpr float		yawPerSecond = 0.6f;

	[[ nodiscard ]] float xScaleOf ( UINT width, UINT height )
	{
		return 1.0f / std::tan ( 0.5f * fieldOfView ) * static_cast<float> ( height ) / static_cast<float> ( std::max ( width, 1u ) );
	}

	// Left-handed, reversed depth: 1 at the near plane, 0 at the far one
	[[ nodiscard ]] Matrix projection ( UINT width, UINT height )
	{
		const auto	a = zNear / ( zNear - zFar );
		return { xScaleOf ( width, height ), 0, 0, 0, 0, 1.0f / std::tan ( 0.5f * fieldOfView ), 0, 0, 0, 0, a, 1, 0, 0, -zFar * a, 0 };
	}

	// The camera stands at the origin and turns around its vertical axis
	[[ nodiscard ]] float yawAt ( size_t frame )
	{
		return yawPerSecond * static_cast<float> ( frame ) / framesPerSecond;
	}

	[[ nodiscard ]] Matrix viewAt ( size_t frame )
	{
		return rotationY ( -yawAt ( frame ) );
	}

	// Ahead of the camera, swinging, bobbing and tumbling on its own, clear of the floor and below the checked rows
	[[ nodiscard ]] Matrix cubeAt ( size_t frame )
	{
		const auto	t = static_cast<float> ( frame ) / framesPerSecond;
		const auto	a = yawAt ( frame ) + 0.25f * std::sin ( t * 1.7f );
		return multiply ( multiply ( multiply ( scaling ( 0.8f ), rotationX ( t * 1.1f ) ), rotationY ( t * 1.9f ) ),
						  translation ( 6.0f * std::sin ( a ), -0.9f + 0.2f * std::sin ( t * 2.3f ), 6.0f * std::cos ( a ) ) );
	}

	[[ nodiscard ]] float halton ( size_t index, size_t base )
	{
		auto	result = 0.0f;
		auto	fraction = 1.0f;

		for ( ; index > 0; index /= base )
		{
			fraction /= static_cast<float> ( base );
			result += fraction * static_cast<float> ( index % base );
		}

		return result;
	}

	// Where display pixel x of the middle row was in the frame before: the camera only turns, so depth plays no part
	[[ nodiscard ]] float previousX ( float x, UINT width, UINT height, size_t frame, size_t before )
	{
		const auto		xScale = xScaleOf ( width, height );
		const auto		viewX = ( 2.0f * ( x + 0.5f ) / static_cast<float> ( width ) - 1.0f ) / xScale;
		const Matrix	direction { viewX, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		const auto		toBefore = multiply ( multiply ( direction, rotationY ( yawAt ( frame ) ) ), viewAt ( before ) );
		const auto		ndc = xScale * toBefore[ 0 ] / toBefore[ 2 ];
		return ( ndc * 0.5f + 0.5f ) * static_cast<float> ( width ) - 0.5f;
	}

	constexpr UINT			slotCount = 2;
	constexpr DXGI_FORMAT	colourFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	constexpr DXGI_FORMAT	motionFormat = DXGI_FORMAT_R16G16_FLOAT;
	constexpr DWORD			gpuWaitMs = 10000;
	constexpr int			maxShift = 32;
	constexpr float			shiftTolerance = 1.0f;
	constexpr size_t		rowCount = 4;
	constexpr UINT			wallVertices = 128 * 6;

	// The cells of the results buffer
	enum Cell : UINT
	{
		colourBefore, depthBefore, motionBefore, historyAtStart, depthHistoryAtStart, waterBefore,
		colourAfter, depthAfter, motionAfter, historyAfter, depthHistoryAfter, waterAfter,
		unwritten, brightness, differing, samples, waterFloor, waterMismatch, keptFloor, keptMismatch, cellCount
	};

	// What csSum reads, as its switch numbers them
	enum class Image : UINT { colour, depth, motion, history, waterBefore, waterAfter, depthHistory };

	// The shader-visible heap: the compute table is all of it, the water table starts at motion, the compose table at shown
	enum Descriptor : UINT { colourSrv, depthSrv, motionSrv, historySrv, waterBeforeSrv, waterAfterSrv, shownSrv, depthHistorySrv, resultsUav, rowsUav, shownUav, motionUav,
							 descriptorCount };

	enum TargetView : UINT { colourRtv = 3, motionRtv, historyRtv, waterBeforeRtv, waterAfterRtv, targetViewCount };

	struct FrameConstants
	{
		Matrix	viewProj;
		Matrix	prevViewProj;
		Matrix	world[ 2 ];
		Matrix	prevWorld[ 2 ];
		float	jitterNdc[ 2 ];
		float	renderSize[ 2 ];
	};

	struct WaterData
	{
		Matrix	viewProj;
		Matrix	prevViewProj;
		float	offset[ 4 ];
	};

	struct PassConstants
	{
		UINT	what = 0;
		UINT	slot = 0;
		UINT	passSize[ 2 ] {};
		UINT	displaySize[ 2 ] {};
		UINT	inputSize[ 2 ] {};
		UINT	rowY[ 4 ] {};
	};

	constexpr UINT	frameConstantsOffset = 0;
	constexpr UINT	waterDataOffset = 512;
	constexpr UINT	inverseOffset = 768;
	constexpr UINT	uploadSlotSize = 1024;
	constexpr UINT	resultsSize = 256;

	static_assert ( sizeof ( FrameConstants ) <= waterDataOffset && sizeof ( WaterData ) + waterDataOffset <= inverseOffset && inverseOffset + sizeof ( Matrix ) <= uploadSlotSize
					&& cellCount * 4 <= resultsSize );

	// As Ghost of Tsushima creates its DLSS: auto-exposure, sharpening, inverted depth, low-resolution vectors, not HDR
	constexpr int			dlssCreateFlags = 0x6A;
	constexpr DXGI_FORMAT	depthFormat = DXGI_FORMAT_R32G8X24_TYPELESS;

	enum class Quality { performance, balanced, quality, ultraPerformance, dlaa };

	struct Mode
	{
		Quality						quality;
		const char*					name;
		UINT						width = 0;
		UINT						height = 0;
		std::string					how;		// the switch that led here: up, down, back
	};

	enum class CheckId : size_t
	{
		ngxStarts, featureCreated, evaluated, deviceKept, presented, sequenceDone,
		colourKept, depthKept, motionKept, historyKept, depthHistoryKept, historyAfterFrame, depthHistoryAfterFrame,
		drawAfterEvaluate, computeAfterEvaluate, waterVectors, keptCompute, outputWritten, notBlack, followsCamera, count
	};

	constexpr const char*	checkNames[] = {
		"DLSS starts on the device the app was given and offers Super Resolution",
		"every DLSS mode starts",
		"every DLSS evaluate succeeds",
		"the device was never removed",
		"every frame presented",
		"the whole mode sequence ran",
		"the app's colour input holds what the app wrote, after the evaluate",
		"the app's depth holds what the app wrote, after the evaluate",
		"the app's motion vectors hold what the app wrote, after the evaluate",
		"the app's history (last frame's vectors) holds what it held, after the evaluate",
		"the app's depth history holds what it held, after the evaluate",
		"the history holds last frame's vectors at the next frame's start",
		"the depth history holds last frame's depth at the next frame's start",
		"the draw after the evaluate (bindings set before it) gives what it gives before it",
		"the compute pass after the evaluate reads what the app wrote",
		"the water pass's vectors (its own copy of last frame's camera) match the scene's on the floor",
		"the compute pass after the evaluate that counts on its bindings from before (-dlss-keep-compute)",
		"DLSS wrote every pixel of its output",
		"the picture is not black",
		"the picture follows the camera (nothing stands still)" };

	static_assert ( std::size ( checkNames ) == size_t ( CheckId::count ) );

	struct Check
	{
		size_t				checked = 0;
		size_t				failed = 0;
		std::string			first;
		std::vector<size_t>	failedPerMode;
	};

	// One frame's numbers, read back once the GPU finished it
	struct Record
	{
		size_t				frame = 0;
		size_t				mode = 0;
		size_t				frameInMode = 0;
		UINT				slot = 0;
		UINT64				fenceValue = 0;
		std::array<uint32_t, cellCount>	cells {};
		std::vector<float>	rows;
		bool				isFresh = false;	// its images new: no history from the frame before
	};

	using Clock = std::chrono::steady_clock;

	// Every present that reaches the swap chain under Streamline's, generated frames included
	std::atomic<size_t>	swapchainPresents { 0 };

	void onSwapchainPresent ( reshade::api::command_queue*, reshade::api::swapchain*, const reshade::api::rect*, const reshade::api::rect*, uint32_t, const reshade::api::rect* )
	{
		++swapchainPresents;
	}

	[[ nodiscard ]] double msSince ( Clock::time_point start )
	{
		return std::chrono::duration<double, std::milli> ( Clock::now () - start ).count ();
	}

	// Since the process was made, as the OS counts it
	[[ nodiscard ]] double msSinceProcessStart ()
	{
		FILETIME	created {};
		FILETIME	unused[ 3 ] {};
		FILETIME	now {};
		GetProcessTimes ( GetCurrentProcess (), &created, &unused[ 0 ], &unused[ 1 ], &unused[ 2 ] );
		GetSystemTimePreciseAsFileTime ( &now );
		const auto	ticks = [] ( const FILETIME& f ) { return ( uint64_t ( f.dwHighDateTime ) << 32 ) | f.dwLowDateTime; };
		return double ( ticks ( now ) - ticks ( created ) ) / 10000.0;
	}

	[[ nodiscard ]] std::string format ( const char* text, ... )
	{
		char	buffer[ 1024 ];
		va_list	args;
		va_start ( args, text );
		std::vsnprintf ( buffer, sizeof buffer, text, args );
		va_end ( args );
		return buffer;
	}

	[[ nodiscard ]] ComPtr<ID3DBlob> compile ( const char* entryPoint, const char* profile )
	{
		static const auto	compilerModule = LoadLibraryW ( L"d3dcompiler_47.dll" );

		if ( compilerModule == nullptr )
			return {};

		const auto	compileFunc = reinterpret_cast<pD3DCompile> ( GetProcAddress ( compilerModule, "D3DCompile" ) );

		if ( compileFunc == nullptr )
			return {};

		ComPtr<ID3DBlob>	code;
		ComPtr<ID3DBlob>	errors;

		if ( FAILED ( compileFunc ( shaderSource, sizeof ( shaderSource ) - 1, "ultra_test_dlss", nullptr, nullptr, entryPoint, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors ) ) )
		{
			reshade::log::message ( reshade::log::level::warning, "dlss bench: shader %s does not compile: %s", entryPoint, errors != nullptr ? static_cast<const char*> ( errors->GetBufferPointer () ) : "" );
			return {};
		}

		return code;
	}

	[[ nodiscard ]] D3D12_RESOURCE_BARRIER transition ( ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after )
	{
		D3D12_RESOURCE_BARRIER	barrier { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
		barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
		return barrier;
	}

	[[ nodiscard ]] D3D12_RESOURCE_DESC textureDesc ( UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags )
	{
		D3D12_RESOURCE_DESC	desc { D3D12_RESOURCE_DIMENSION_TEXTURE2D };
		desc.Width = width;
		desc.Height = height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = format;
		desc.SampleDesc = { 1, 0 };
		desc.Flags = flags;
		return desc;
	}

	[[ nodiscard ]] D3D12_RESOURCE_DESC bufferDesc ( UINT64 size, D3D12_RESOURCE_FLAGS flags )
	{
		D3D12_RESOURCE_DESC	desc { D3D12_RESOURCE_DIMENSION_BUFFER };
		desc.Width = size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc = { 1, 0 };
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = flags;
		return desc;
	}

	const D3D12_RESOURCE_STATES	readable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	// The images of one render size, placed in one heap made for the largest: a new size takes the memory of the one before, as engines
	// alias their render targets
	struct SizeImages
	{
		ComPtr<ID3D12Resource>	colour;
		ComPtr<ID3D12Resource>	depth;
		ComPtr<ID3D12Resource>	motion;
		ComPtr<ID3D12Resource>	history;
		ComPtr<ID3D12Resource>	depthHistory;
		ComPtr<ID3D12Resource>	waterBefore;
		ComPtr<ID3D12Resource>	waterAfter;
	};

	[[ nodiscard ]] Matrix inverse ( const Matrix& m )
	{
		Matrix	inv {};
		inv[ 0 ] = m[ 5 ] * m[ 10 ] * m[ 15 ] - m[ 5 ] * m[ 11 ] * m[ 14 ] - m[ 9 ] * m[ 6 ] * m[ 15 ] + m[ 9 ] * m[ 7 ] * m[ 14 ] + m[ 13 ] * m[ 6 ] * m[ 11 ] - m[ 13 ] * m[ 7 ] * m[ 10 ];
		inv[ 4 ] = -m[ 4 ] * m[ 10 ] * m[ 15 ] + m[ 4 ] * m[ 11 ] * m[ 14 ] + m[ 8 ] * m[ 6 ] * m[ 15 ] - m[ 8 ] * m[ 7 ] * m[ 14 ] - m[ 12 ] * m[ 6 ] * m[ 11 ] + m[ 12 ] * m[ 7 ] * m[ 10 ];
		inv[ 8 ] = m[ 4 ] * m[ 9 ] * m[ 15 ] - m[ 4 ] * m[ 11 ] * m[ 13 ] - m[ 8 ] * m[ 5 ] * m[ 15 ] + m[ 8 ] * m[ 7 ] * m[ 13 ] + m[ 12 ] * m[ 5 ] * m[ 11 ] - m[ 12 ] * m[ 7 ] * m[ 9 ];
		inv[ 12 ] = -m[ 4 ] * m[ 9 ] * m[ 14 ] + m[ 4 ] * m[ 10 ] * m[ 13 ] + m[ 8 ] * m[ 5 ] * m[ 14 ] - m[ 8 ] * m[ 6 ] * m[ 13 ] - m[ 12 ] * m[ 5 ] * m[ 10 ] + m[ 12 ] * m[ 6 ] * m[ 9 ];
		inv[ 1 ] = -m[ 1 ] * m[ 10 ] * m[ 15 ] + m[ 1 ] * m[ 11 ] * m[ 14 ] + m[ 9 ] * m[ 2 ] * m[ 15 ] - m[ 9 ] * m[ 3 ] * m[ 14 ] - m[ 13 ] * m[ 2 ] * m[ 11 ] + m[ 13 ] * m[ 3 ] * m[ 10 ];
		inv[ 5 ] = m[ 0 ] * m[ 10 ] * m[ 15 ] - m[ 0 ] * m[ 11 ] * m[ 14 ] - m[ 8 ] * m[ 2 ] * m[ 15 ] + m[ 8 ] * m[ 3 ] * m[ 14 ] + m[ 12 ] * m[ 2 ] * m[ 11 ] - m[ 12 ] * m[ 3 ] * m[ 10 ];
		inv[ 9 ] = -m[ 0 ] * m[ 9 ] * m[ 15 ] + m[ 0 ] * m[ 11 ] * m[ 13 ] + m[ 8 ] * m[ 1 ] * m[ 15 ] - m[ 8 ] * m[ 3 ] * m[ 13 ] - m[ 12 ] * m[ 1 ] * m[ 11 ] + m[ 12 ] * m[ 3 ] * m[ 9 ];
		inv[ 13 ] = m[ 0 ] * m[ 9 ] * m[ 14 ] - m[ 0 ] * m[ 10 ] * m[ 13 ] - m[ 8 ] * m[ 1 ] * m[ 14 ] + m[ 8 ] * m[ 2 ] * m[ 13 ] + m[ 12 ] * m[ 1 ] * m[ 10 ] - m[ 12 ] * m[ 2 ] * m[ 9 ];
		inv[ 2 ] = m[ 1 ] * m[ 6 ] * m[ 15 ] - m[ 1 ] * m[ 7 ] * m[ 14 ] - m[ 5 ] * m[ 2 ] * m[ 15 ] + m[ 5 ] * m[ 3 ] * m[ 14 ] + m[ 13 ] * m[ 2 ] * m[ 7 ] - m[ 13 ] * m[ 3 ] * m[ 6 ];
		inv[ 6 ] = -m[ 0 ] * m[ 6 ] * m[ 15 ] + m[ 0 ] * m[ 7 ] * m[ 14 ] + m[ 4 ] * m[ 2 ] * m[ 15 ] - m[ 4 ] * m[ 3 ] * m[ 14 ] - m[ 12 ] * m[ 2 ] * m[ 7 ] + m[ 12 ] * m[ 3 ] * m[ 6 ];
		inv[ 10 ] = m[ 0 ] * m[ 5 ] * m[ 15 ] - m[ 0 ] * m[ 7 ] * m[ 13 ] - m[ 4 ] * m[ 1 ] * m[ 15 ] + m[ 4 ] * m[ 3 ] * m[ 13 ] + m[ 12 ] * m[ 1 ] * m[ 7 ] - m[ 12 ] * m[ 3 ] * m[ 5 ];
		inv[ 14 ] = -m[ 0 ] * m[ 5 ] * m[ 14 ] + m[ 0 ] * m[ 6 ] * m[ 13 ] + m[ 4 ] * m[ 1 ] * m[ 14 ] - m[ 4 ] * m[ 2 ] * m[ 13 ] - m[ 12 ] * m[ 1 ] * m[ 6 ] + m[ 12 ] * m[ 2 ] * m[ 5 ];
		inv[ 3 ] = -m[ 1 ] * m[ 6 ] * m[ 11 ] + m[ 1 ] * m[ 7 ] * m[ 10 ] + m[ 5 ] * m[ 2 ] * m[ 11 ] - m[ 5 ] * m[ 3 ] * m[ 10 ] - m[ 9 ] * m[ 2 ] * m[ 7 ] + m[ 9 ] * m[ 3 ] * m[ 6 ];
		inv[ 7 ] = m[ 0 ] * m[ 6 ] * m[ 11 ] - m[ 0 ] * m[ 7 ] * m[ 10 ] - m[ 4 ] * m[ 2 ] * m[ 11 ] + m[ 4 ] * m[ 3 ] * m[ 10 ] + m[ 8 ] * m[ 2 ] * m[ 7 ] - m[ 8 ] * m[ 3 ] * m[ 6 ];
		inv[ 11 ] = -m[ 0 ] * m[ 5 ] * m[ 11 ] + m[ 0 ] * m[ 7 ] * m[ 9 ] + m[ 4 ] * m[ 1 ] * m[ 11 ] - m[ 4 ] * m[ 3 ] * m[ 9 ] - m[ 8 ] * m[ 1 ] * m[ 7 ] + m[ 8 ] * m[ 3 ] * m[ 5 ];
		inv[ 15 ] = m[ 0 ] * m[ 5 ] * m[ 10 ] - m[ 0 ] * m[ 6 ] * m[ 9 ] - m[ 4 ] * m[ 1 ] * m[ 10 ] + m[ 4 ] * m[ 2 ] * m[ 9 ] + m[ 8 ] * m[ 1 ] * m[ 6 ] - m[ 8 ] * m[ 2 ] * m[ 5 ];

		const auto	determinant = m[ 0 ] * inv[ 0 ] + m[ 1 ] * inv[ 4 ] + m[ 2 ] * inv[ 8 ] + m[ 3 ] * inv[ 12 ];
		for ( auto& v : inv )
			v /= determinant;
		return inv;
	}

	// One evaluate: the frame's images and facts, recorded in the frame's own list
	struct Evaluation
	{
		ID3D12GraphicsCommandList*	list = nullptr;
		ID3D12Resource*				colour = nullptr;
		ID3D12Resource*				depth = nullptr;
		ID3D12Resource*				motion = nullptr;
		ID3D12Resource*				output = nullptr;
		UINT						renderWidth = 0;
		UINT						renderHeight = 0;
		UINT						displayWidth = 0;
		UINT						displayHeight = 0;
		float						jitterX = 0.0f;
		float						jitterY = 0.0f;
		bool						isReset = false;
		size_t						frame = 0;
		Matrix						viewToClip {};
		Matrix						view {};
		Matrix						previousView {};
		float						yaw = 0.0f;
		D3D12_RESOURCE_STATES		inputState = D3D12_RESOURCE_STATE_COMMON;
	};

	// How the app reaches DLSS: NGX directly, or Streamline in front of it as Ghost of Tsushima does. Each call says why it failed
	class DlssPath
	{
	public:
		virtual ~DlssPath () = default;

		// On the app's device: the modes' render sizes filled in
		[[ nodiscard ]] virtual bool start ( ID3D12Device* device, UINT displayWidth, UINT displayHeight, std::vector<Mode>& modes, std::string& why ) = 0;
		// At a mode switch, the GPU idle
		[[ nodiscard ]] virtual bool beginMode ( const Mode& mode, UINT displayWidth, UINT displayHeight, std::string& why ) = 0;
		// The mode's first frame, in its list before the evaluate
		[[ nodiscard ]] virtual bool prepare ( const Evaluation& e, const Mode& mode, std::string& why ) = 0;
		[[ nodiscard ]] virtual bool evaluate ( const Evaluation& e, std::string& why ) = 0;
		// The GPU idle: the mode's feature goes
		virtual void endMode () = 0;
		// gameExit: as Ghost of Tsushima ends, the feature and NGX left as they are
		virtual void stop ( bool gameExit ) = 0;

		// The frame's start and its present, for Frame Generation's and Reflex's markers
		virtual void beginFrame ( size_t ) {}
		virtual void beforePresent () {}
		virtual void afterPresent () {}
		// -dlss-fg-remake: Frame Generation off and its resources freed, on again at the next frame
		virtual void remakeFrameGeneration () {}
		// -dlss-fg: whether it generated frames, and the counts that say so
		[[ nodiscard ]] virtual bool hasFrameGenerationRun ( std::string& what ) const { what = "only through Streamline (-dlss-streamline)"; return false; }
	};

	// NGX as a game calls it: started on the device it was given, the feature created in the frame's list
	class NgxPath final : public DlssPath
	{
	public:
		explicit NgxPath ( const std::filesystem::path& dataPath ) : dataPath ( dataPath ) {}

		bool start ( ID3D12Device* d, UINT displayWidth, UINT displayHeight, std::vector<Mode>& modes, std::string& why ) override
		{
			device = d;
			NVSDK_NGX_FeatureCommonInfo	info {};
			auto	r = NVSDK_NGX_D3D12_Init_with_ProjectID ( "5a3f1d0e-7c2b-4e8a-9f61-0d4b2c8e7a13", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dataPath.c_str (), device, &info );
			if ( NVSDK_NGX_FAILED ( r ) )
			{
				why = format ( "NVSDK_NGX_D3D12_Init_with_ProjectID returned 0x%08X", unsigned ( r ) );
				return false;
			}
			isStarted = true;

			if ( NVSDK_NGX_FAILED ( r = NVSDK_NGX_D3D12_GetCapabilityParameters ( &capabilities ) ) || NVSDK_NGX_FAILED ( r = NVSDK_NGX_D3D12_AllocateParameters ( &parameters ) ) )
			{
				why = format ( "NGX's parameters: 0x%08X", unsigned ( r ) );
				return false;
			}

			int	available = 0;
			if ( NVSDK_NGX_FAILED ( capabilities->Get ( NVSDK_NGX_Parameter_SuperSampling_Available, &available ) ) || available == 0 )
			{
				int	needsDriver = 0;
				capabilities->Get ( NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver );
				why = format ( "Super Resolution is not available (needs a newer driver: %d; is nvngx_dlss.dll beside the exe?)", needsDriver );
				return false;
			}

			void*	callback = nullptr;
			capabilities->Get ( NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback );
			if ( callback == nullptr )
			{
				why = "NGX gives no optimal settings callback";
				return false;
			}

			for ( auto& m : modes )
			{
				capabilities->Set ( NVSDK_NGX_Parameter_Width, displayWidth );
				capabilities->Set ( NVSDK_NGX_Parameter_Height, displayHeight );
				capabilities->Set ( NVSDK_NGX_Parameter_PerfQualityValue, int ( qualityOf ( m.quality ) ) );
				capabilities->Set ( NVSDK_NGX_Parameter_RTXValue, 0 );
				r = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback> ( callback ) ( capabilities );
				capabilities->Get ( NVSDK_NGX_Parameter_OutWidth, &m.width );
				capabilities->Get ( NVSDK_NGX_Parameter_OutHeight, &m.height );
				if ( NVSDK_NGX_FAILED ( r ) )
				{
					why = format ( "the optimal settings for %s: 0x%08X", m.name, unsigned ( r ) );
					return false;
				}
			}
			return true;
		}

		bool beginMode ( const Mode&, UINT, UINT, std::string& ) override
		{
			return true;
		}

		bool prepare ( const Evaluation& e, const Mode& m, std::string& why ) override
		{
			parameters->Set ( NVSDK_NGX_Parameter_CreationNodeMask, 1u );
			parameters->Set ( NVSDK_NGX_Parameter_VisibilityNodeMask, 1u );
			parameters->Set ( NVSDK_NGX_Parameter_Width, e.renderWidth );
			parameters->Set ( NVSDK_NGX_Parameter_Height, e.renderHeight );
			parameters->Set ( NVSDK_NGX_Parameter_OutWidth, e.displayWidth );
			parameters->Set ( NVSDK_NGX_Parameter_OutHeight, e.displayHeight );
			parameters->Set ( NVSDK_NGX_Parameter_PerfQualityValue, int ( qualityOf ( m.quality ) ) );
			parameters->Set ( NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, dlssCreateFlags );
			parameters->Set ( NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0 );
			const auto	r = NVSDK_NGX_D3D12_CreateFeature ( e.list, NVSDK_NGX_Feature_SuperSampling, parameters, &feature );
			if ( NVSDK_NGX_SUCCEED ( r ) && feature != nullptr )
				return true;

			why = format ( "NVSDK_NGX_D3D12_CreateFeature returned 0x%08X", unsigned ( r ) );
			feature = nullptr;
			return false;
		}

		bool evaluate ( const Evaluation& e, std::string& why ) override
		{
			parameters->Set ( NVSDK_NGX_Parameter_Color, e.colour );
			parameters->Set ( NVSDK_NGX_Parameter_Output, e.output );
			parameters->Set ( NVSDK_NGX_Parameter_Depth, e.depth );
			parameters->Set ( NVSDK_NGX_Parameter_MotionVectors, e.motion );
			parameters->Set ( NVSDK_NGX_Parameter_Jitter_Offset_X, e.jitterX );
			parameters->Set ( NVSDK_NGX_Parameter_Jitter_Offset_Y, e.jitterY );
			parameters->Set ( NVSDK_NGX_Parameter_Sharpness, 0.0f );
			parameters->Set ( NVSDK_NGX_Parameter_Reset, e.isReset ? 1 : 0 );
			parameters->Set ( NVSDK_NGX_Parameter_MV_Scale_X, -float ( e.renderWidth ) );
			parameters->Set ( NVSDK_NGX_Parameter_MV_Scale_Y, -float ( e.renderHeight ) );
			parameters->Set ( NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, e.renderWidth );
			parameters->Set ( NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, e.renderHeight );
			parameters->Set ( NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f );
			parameters->Set ( NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f );
			parameters->Set ( NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 1000.0f / framesPerSecond );

			const auto	r = NVSDK_NGX_D3D12_EvaluateFeature ( e.list, feature, parameters );
			if ( NVSDK_NGX_SUCCEED ( r ) )
				return true;

			why = format ( "NVSDK_NGX_D3D12_EvaluateFeature returned 0x%08X", unsigned ( r ) );
			return false;
		}

		void endMode () override
		{
			if ( feature == nullptr )
				return;

			const auto	r = NVSDK_NGX_D3D12_ReleaseFeature ( feature );
			if ( NVSDK_NGX_FAILED ( r ) )
				reshade::log::message ( reshade::log::level::warning, "dlss bench: NVSDK_NGX_D3D12_ReleaseFeature returned 0x%08X", unsigned ( r ) );
			feature = nullptr;
		}

		// Features gone, then NGX for the device, before the device goes: NVIDIA's order
		void stop ( bool gameExit ) override
		{
			if ( gameExit )
				return;

			endMode ();
			if ( parameters != nullptr )
				NVSDK_NGX_D3D12_DestroyParameters ( parameters );
			parameters = nullptr;
			if ( isStarted )
				NVSDK_NGX_D3D12_Shutdown1 ( device );
			isStarted = false;
		}

	private:
		[[ nodiscard ]] static NVSDK_NGX_PerfQuality_Value qualityOf ( Quality q )
		{
			switch ( q )
			{
				case Quality::performance:		return NVSDK_NGX_PerfQuality_Value_MaxPerf;
				case Quality::balanced:			return NVSDK_NGX_PerfQuality_Value_Balanced;
				case Quality::quality:			return NVSDK_NGX_PerfQuality_Value_MaxQuality;
				case Quality::ultraPerformance:	return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
				case Quality::dlaa:				return NVSDK_NGX_PerfQuality_Value_DLAA;
			}
			return NVSDK_NGX_PerfQuality_Value_MaxPerf;
		}

		std::filesystem::path	dataPath;
		ID3D12Device*			device = nullptr;
		NVSDK_NGX_Parameter*	capabilities = nullptr;
		NVSDK_NGX_Parameter*	parameters = nullptr;
		NVSDK_NGX_Handle*		feature = nullptr;
		bool					isStarted = false;
	};

#if ULTRA_TEST_STREAMLINE
	// sl.interposer's entry points by name (Streamline ships no import library for the game's own DLLs); slInit runs before the app
	// makes its device, as Streamline requires
	struct StreamlineRuntime
	{
		HMODULE							module = nullptr;
		std::wstring					folder;
		std::string						why = "-dlss-streamline names no folder";
		PFun_slInit*					init = nullptr;
		PFun_slShutdown*				shutdown = nullptr;
		PFun_slSetD3DDevice*			setDevice = nullptr;
		PFun_slIsFeatureSupported*		isSupported = nullptr;
		PFun_slGetFeatureFunction*		featureFunction = nullptr;
		PFun_slSetConstants*			setConstants = nullptr;
		PFun_slEvaluateFeature*			evaluate = nullptr;
		PFun_slGetNewFrameToken*		newFrameToken = nullptr;
		PFun_slFreeResources*			freeResources = nullptr;
		void*							setTag = nullptr;		// slSetTag, the one Ghost of Tsushima calls
		reshade::ultra::TestDlssFactoryCreator	createFactory = nullptr;	// -dlss-fg: the interposer's CreateDXGIFactory2 and D3D12CreateDevice
		reshade::ultra::TestDlssDeviceCreator	createDevice = nullptr;
		std::string						deviceProblem;		// -dlss-fg: what slSetD3DDevice said when the app attached its device
		bool							isDeviceAttached = false;
		bool							isInitialized = false;
	};

	StreamlineRuntime	streamline;

	void onStreamlineLog ( sl::LogType type, const char* message )
	{
		std::string	text = message != nullptr ? message : "";
		while ( ! text.empty () && ( text.back () == '\n' || text.back () == '\r' ) )
			text.pop_back ();
		reshade::log::message ( type == sl::LogType::eInfo ? reshade::log::level::info : reshade::log::level::warning, "streamline: %s", text.c_str () );
	}

	[[ nodiscard ]] const char* streamlineResult ( sl::Result r )
	{
		static constexpr const char*	names[] = { "eOk", "eErrorIO", "eErrorDriverOutOfDate", "eErrorOSOutOfDate", "eErrorOSDisabledHWS", "eErrorDeviceNotCreated",
			"eErrorNoSupportedAdapterFound", "eErrorAdapterNotSupported", "eErrorNoPlugins", "eErrorVulkanAPI", "eErrorDXGIAPI", "eErrorD3DAPI", "eErrorNRDAPI",
			"eErrorNVAPI", "eErrorReflexAPI", "eErrorNGXFailed", "eErrorJSONParsing", "eErrorMissingProxy", "eErrorMissingResourceState", "eErrorInvalidIntegration",
			"eErrorMissingInputParameter", "eErrorNotInitialized", "eErrorComputeFailed", "eErrorInitNotCalled", "eErrorExceptionHandler", "eErrorInvalidParameter",
			"eErrorMissingConstants", "eErrorDuplicatedConstants", "eErrorMissingOrInvalidAPI", "eErrorCommonConstantsMissing", "eErrorUnsupportedInterface",
			"eErrorFeatureMissing", "eErrorFeatureNotSupported", "eErrorFeatureMissingHooks", "eErrorFeatureFailedToLoad", "eErrorFeatureWrongPriority",
			"eErrorFeatureMissingDependency", "eErrorFeatureManagerInvalidState", "eErrorInvalidState", "eWarnOutOfVRAM" };
		const auto	i = size_t ( r );
		return i < std::size ( names ) ? names[ i ] : "unknown";
	}

	[[ nodiscard ]] sl::float4x4 slMatrix ( const Matrix& m )
	{
		sl::float4x4	out;
		for ( uint32_t r = 0; r < 4; ++r )
			out.setRow ( r, sl::float4 ( m[ r * 4 ], m[ r * 4 + 1 ], m[ r * 4 + 2 ], m[ r * 4 + 3 ] ) );
		return out;
	}

	// DLSS through Streamline as Ghost of Tsushima calls it: the app's own device handed over (slSetD3DDevice), the mode through
	// slDLSSSetOptions, the images through slSetTag, slEvaluateFeature in the frame's list, slFreeResources at a mode switch
	class StreamlinePath final : public DlssPath
	{
	public:
		explicit StreamlinePath ( const reshade::ultra::TestDlssSettings& settings ) : settings ( settings ) {}

		bool start ( ID3D12Device* device, UINT displayWidth, UINT displayHeight, std::vector<Mode>& modes, std::string& why ) override
		{
			if ( ! streamline.isInitialized )
			{
				why = streamline.why;
				return false;
			}

			if ( ! streamline.deviceProblem.empty () )
			{
				why = streamline.deviceProblem;
				return false;
			}
			auto	r = streamline.isDeviceAttached ? sl::Result::eOk : streamline.setDevice ( device );
			if ( r != sl::Result::eOk )
			{
				why = format ( "slSetD3DDevice returned %s", streamlineResult ( r ) );
				return false;
			}

			auto				luid = device->GetAdapterLuid ();
			sl::AdapterInfo		adapter;
			adapter.deviceLUID = reinterpret_cast<uint8_t*> ( &luid );
			adapter.deviceLUIDSizeInBytes = sizeof ( luid );
			if ( ( r = streamline.isSupported ( sl::kFeatureDLSS, adapter ) ) != sl::Result::eOk )
			{
				why = format ( "slIsFeatureSupported ( DLSS ) returned %s", streamlineResult ( r ) );
				return false;
			}

			if ( ( r = streamline.featureFunction ( sl::kFeatureDLSS, "slDLSSGetOptimalSettings", reinterpret_cast<void*&> ( getOptimalSettings ) ) ) != sl::Result::eOk
				 || ( r = streamline.featureFunction ( sl::kFeatureDLSS, "slDLSSSetOptions", reinterpret_cast<void*&> ( setOptions ) ) ) != sl::Result::eOk )
			{
				why = format ( "slGetFeatureFunction for DLSS returned %s", streamlineResult ( r ) );
				return false;
			}

			if ( settings.isFrameGenerated && ! startFrameGeneration ( device, why ) )
				return false;

			for ( auto& m : modes )
			{
				sl::DLSSOptimalSettings	optimal;
				if ( ( r = getOptimalSettings ( optionsOf ( m, displayWidth, displayHeight ), optimal ) ) != sl::Result::eOk )
				{
					why = format ( "slDLSSGetOptimalSettings for %s returned %s", m.name, streamlineResult ( r ) );
					return false;
				}
				m.width = optimal.optimalRenderWidth;
				m.height = optimal.optimalRenderHeight;
			}
			return true;
		}

		bool beginMode ( const Mode& m, UINT displayWidth, UINT displayHeight, std::string& why ) override
		{
			const auto	r = setOptions ( viewport, optionsOf ( m, displayWidth, displayHeight ) );
			if ( r == sl::Result::eOk )
				return true;

			why = format ( "slDLSSSetOptions returned %s", streamlineResult ( r ) );
			return false;
		}

		bool prepare ( const Evaluation&, const Mode&, std::string& ) override
		{
			return true;
		}

		void beginFrame ( size_t frame ) override
		{
			const auto	index = uint32_t ( frame );
			token = nullptr;
			if ( const auto r = streamline.newFrameToken ( token, &index ); r != sl::Result::eOk )
				reshade::log::message ( reshade::log::level::warning, "dlss bench: slGetNewFrameToken returned %s at frame %zu", streamlineResult ( r ), frame );
			if ( ! settings.isFrameGenerated || token == nullptr )
				return;

			if ( std::exchange ( wantsFrameGenerationOn, false ) )
				setFrameGeneration ( sl::DLSSGMode::eOn );
			reflexSleep ( *token );
			pclSetMarker ( sl::PCLMarker::eSimulationStart, *token );
			pclSetMarker ( sl::PCLMarker::eSimulationEnd, *token );
			pclSetMarker ( sl::PCLMarker::eRenderSubmitStart, *token );
		}

		void beforePresent () override
		{
			if ( ! settings.isFrameGenerated || token == nullptr )
				return;
			pclSetMarker ( sl::PCLMarker::eRenderSubmitEnd, *token );
			pclSetMarker ( sl::PCLMarker::ePresentStart, *token );
		}

		void afterPresent () override
		{
			if ( ! settings.isFrameGenerated || token == nullptr )
				return;
			pclSetMarker ( sl::PCLMarker::ePresentEnd, *token );

			sl::DLSSGState	state;
			if ( dlssgGetState ( viewport, state, nullptr ) == sl::Result::eOk )
			{
				if ( frames == 0 || frames % 120 == 0 )
					reshade::log::message ( reshade::log::level::info, "dlss bench: Frame Generation: status 0x%X, %u presented, up to %u generated, minimum size %u", unsigned ( state.status ),
											state.numFramesActuallyPresented, state.numFramesToGenerateMax, state.minWidthOrHeight );
				generatedPresents += state.numFramesActuallyPresented;
				statusSeen |= uint32_t ( state.status );
				++frames;
			}
		}

		void remakeFrameGeneration () override
		{
			if ( ! settings.isFrameGenerated )
				return;
			setFrameGeneration ( sl::DLSSGMode::eOff );
			if ( const auto r = streamline.freeResources ( sl::kFeatureDLSS_G, viewport ); r != sl::Result::eOk )
				reshade::log::message ( reshade::log::level::warning, "dlss bench: slFreeResources ( Frame Generation ) returned %s", streamlineResult ( r ) );
			wantsFrameGenerationOn = true;
			reshade::log::message ( reshade::log::level::info, "dlss bench: Frame Generation off and freed, on again at the next frame" );
		}

		bool hasFrameGenerationRun ( std::string& why ) const override
		{
			const auto	presents = swapchainPresents.load ();
			why = format ( "%zu presents at the swap chain and %llu reported by Streamline in %zu frames, status bits 0x%X", presents, (unsigned long long) generatedPresents,
						   frames, statusSeen );
			return frames > 0 && statusSeen == 0 && presents >= frames * 3 / 2;
		}

		bool evaluate ( const Evaluation& e, std::string& why ) override
		{
			if ( token == nullptr )
			{
				why = "no frame token";
				return false;
			}
			sl::Result	r;

			const auto	clipToView = inverse ( e.viewToClip );
			const auto	clipToPrevClip = multiply ( multiply ( multiply ( clipToView, inverse ( e.view ) ), e.previousView ), e.viewToClip );
			sl::Constants	c;
			c.cameraViewToClip = slMatrix ( e.viewToClip );
			c.clipToCameraView = slMatrix ( clipToView );
			c.clipToLensClip = slMatrix ( identity () );
			c.clipToPrevClip = slMatrix ( clipToPrevClip );
			c.prevClipToClip = slMatrix ( inverse ( clipToPrevClip ) );
			c.jitterOffset = { e.jitterX, e.jitterY };
			c.mvecScale = { -1.0f, -1.0f };
			c.cameraPinholeOffset = { 0.0f, 0.0f };
			c.cameraPos = { 0.0f, 0.0f, 0.0f };
			c.cameraUp = { 0.0f, 1.0f, 0.0f };
			c.cameraRight = { std::cos ( e.yaw ), 0.0f, -std::sin ( e.yaw ) };
			c.cameraFwd = { std::sin ( e.yaw ), 0.0f, std::cos ( e.yaw ) };
			c.cameraNear = zNear;
			c.cameraFar = zFar;
			c.cameraFOV = fieldOfView;
			c.cameraAspectRatio = float ( e.renderWidth ) / float ( e.renderHeight );
			c.depthInverted = sl::Boolean::eTrue;
			c.cameraMotionIncluded = sl::Boolean::eTrue;
			c.motionVectors3D = sl::Boolean::eFalse;
			c.reset = e.isReset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
			if ( ( r = streamline.setConstants ( c, *token, viewport ) ) != sl::Result::eOk )
			{
				why = format ( "slSetConstants returned %s", streamlineResult ( r ) );
				return false;
			}

			// Frame Generation reads them at the present, when the app's images are elsewhere: copies
			const auto		lifecycle = settings.isFrameGenerated ? sl::eOnlyValidNow : lifecycleOf ( settings.tags );
			sl::Resource	colour ( sl::ResourceType::eTex2d, e.colour, nullptr, nullptr, uint32_t ( e.inputState ) );
			sl::Resource	depth ( sl::ResourceType::eTex2d, e.depth, nullptr, nullptr, uint32_t ( e.inputState ) );
			sl::Resource	motion ( sl::ResourceType::eTex2d, e.motion, nullptr, nullptr, uint32_t ( e.inputState ) );
			sl::Resource	output ( sl::ResourceType::eTex2d, e.output, nullptr, nullptr, uint32_t ( D3D12_RESOURCE_STATE_UNORDERED_ACCESS ) );
			const sl::Extent	render { 0, 0, e.renderWidth, e.renderHeight };
			const sl::Extent	display { 0, 0, e.displayWidth, e.displayHeight };
			const sl::ResourceTag	tags[] = { sl::ResourceTag ( &colour, sl::kBufferTypeScalingInputColor, lifecycle, &render ),
											   sl::ResourceTag ( &depth, sl::kBufferTypeDepth, lifecycle, &render ),
											   sl::ResourceTag ( &motion, sl::kBufferTypeMotionVectors, lifecycle, &render ),
											   sl::ResourceTag ( &output, sl::kBufferTypeScalingOutputColor, sl::eValidUntilPresent, &display ) };
			using SetTag = sl::Result ( const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer* );
			if ( ( r = reinterpret_cast<SetTag*> ( streamline.setTag ) ( viewport, tags, uint32_t ( std::size ( tags ) ), e.list ) ) != sl::Result::eOk )
			{
				why = format ( "slSetTag returned %s", streamlineResult ( r ) );
				return false;
			}

			hasResources = true;
			const sl::BaseStructure*	inputs[] = { &viewport };
			if ( ( r = streamline.evaluate ( sl::kFeatureDLSS, *token, inputs, uint32_t ( std::size ( inputs ) ), e.list ) ) != sl::Result::eOk )
			{
				why = format ( "slEvaluateFeature returned %s", streamlineResult ( r ) );
				return false;
			}

			// The picture without its HUD, which is DLSS's output here, copied for Frame Generation once DLSS wrote it
			if ( settings.isFrameGenerated )
			{
				const sl::ResourceTag	hudless ( &output, sl::kBufferTypeHUDLessColor, sl::eOnlyValidNow, &display );
				if ( ( r = reinterpret_cast<SetTag*> ( streamline.setTag ) ( viewport, &hudless, 1, e.list ) ) != sl::Result::eOk )
				{
					why = format ( "slSetTag ( HUD-less colour ) returned %s", streamlineResult ( r ) );
					return false;
				}
			}
			return true;
		}

		void endMode () override
		{
			if ( ! std::exchange ( hasResources, false ) )
				return;

			const auto	r = streamline.freeResources ( sl::kFeatureDLSS, viewport );
			if ( r != sl::Result::eOk )
				reshade::log::message ( reshade::log::level::warning, "dlss bench: slFreeResources returned %s", streamlineResult ( r ) );
		}

		// gameExit: the tags cleared, as the game calls slSetTag while it ends, then no free and no shutdown
		void stop ( bool gameExit ) override
		{
			if ( gameExit )
			{
				const sl::ResourceTag	tags[] = { sl::ResourceTag ( nullptr, sl::kBufferTypeScalingInputColor, sl::eValidUntilPresent ),
												   sl::ResourceTag ( nullptr, sl::kBufferTypeDepth, sl::eValidUntilPresent ),
												   sl::ResourceTag ( nullptr, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent ),
												   sl::ResourceTag ( nullptr, sl::kBufferTypeScalingOutputColor, sl::eValidUntilPresent ) };
				using SetTag = sl::Result ( const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer* );
				const auto	r = reinterpret_cast<SetTag*> ( streamline.setTag ) ( viewport, tags, uint32_t ( std::size ( tags ) ), nullptr );
				reshade::log::message ( reshade::log::level::info, "dlss bench: the end as the game's: slSetTag clearing the tags returned %s, no slFreeResources, no slShutdown",
										streamlineResult ( r ) );
				return;
			}

			endMode ();
			const auto	r = streamline.shutdown ();
			if ( r != sl::Result::eOk )
				reshade::log::message ( reshade::log::level::warning, "dlss bench: slShutdown returned %s", streamlineResult ( r ) );
			streamline.isInitialized = false;
		}

	private:
		[[ nodiscard ]] static sl::DLSSOptions optionsOf ( const Mode& m, UINT displayWidth, UINT displayHeight )
		{
			sl::DLSSOptions	o;
			switch ( m.quality )
			{
				case Quality::performance:		o.mode = sl::DLSSMode::eMaxPerformance; break;
				case Quality::balanced:			o.mode = sl::DLSSMode::eBalanced; break;
				case Quality::quality:			o.mode = sl::DLSSMode::eMaxQuality; break;
				case Quality::ultraPerformance:	o.mode = sl::DLSSMode::eUltraPerformance; break;
				case Quality::dlaa:				o.mode = sl::DLSSMode::eDLAA; break;
			}
			o.outputWidth = displayWidth;
			o.outputHeight = displayHeight;
			o.colorBuffersHDR = sl::Boolean::eFalse;
			o.useAutoExposure = sl::Boolean::eTrue;
			return o;
		}

		[[ nodiscard ]] static sl::ResourceLifecycle lifecycleOf ( reshade::ultra::TestDlssTags tags )
		{
			switch ( tags )
			{
				case reshade::ultra::TestDlssTags::onlyNow:			return sl::eOnlyValidNow;
				case reshade::ultra::TestDlssTags::untilEvaluate:	return sl::eValidUntilEvaluate;
				case reshade::ultra::TestDlssTags::untilPresent:	return sl::eValidUntilPresent;
			}
			return sl::eValidUntilPresent;
		}

		[[ nodiscard ]] bool startFrameGeneration ( ID3D12Device* device, std::string& why )
		{
			auto	r = streamline.featureFunction ( sl::kFeatureDLSS_G, "slDLSSGSetOptions", reinterpret_cast<void*&> ( dlssgSetOptions ) );
			if ( r == sl::Result::eOk )
				r = streamline.featureFunction ( sl::kFeatureDLSS_G, "slDLSSGGetState", reinterpret_cast<void*&> ( dlssgGetState ) );
			if ( r == sl::Result::eOk )
				r = streamline.featureFunction ( sl::kFeatureReflex, "slReflexSetOptions", reinterpret_cast<void*&> ( reflexSetOptions ) );
			if ( r == sl::Result::eOk )
				r = streamline.featureFunction ( sl::kFeatureReflex, "slReflexSleep", reinterpret_cast<void*&> ( reflexSleep ) );
			if ( r == sl::Result::eOk )
				r = streamline.featureFunction ( sl::kFeaturePCL, "slPCLSetMarker", reinterpret_cast<void*&> ( pclSetMarker ) );
			if ( r != sl::Result::eOk )
			{
				why = format ( "slGetFeatureFunction for Frame Generation, Reflex or PCL returned %s", streamlineResult ( r ) );
				return false;
			}

			auto				luid = device->GetAdapterLuid ();
			sl::AdapterInfo		adapter;
			adapter.deviceLUID = reinterpret_cast<uint8_t*> ( &luid );
			adapter.deviceLUIDSizeInBytes = sizeof ( luid );
			if ( ( r = streamline.isSupported ( sl::kFeatureDLSS_G, adapter ) ) != sl::Result::eOk )
			{
				why = format ( "slIsFeatureSupported ( Frame Generation ) returned %s", streamlineResult ( r ) );
				return false;
			}

			sl::ReflexOptions	reflex;
			reflex.mode = sl::ReflexMode::eLowLatency;
			if ( ( r = reflexSetOptions ( reflex ) ) != sl::Result::eOk )
			{
				why = format ( "slReflexSetOptions returned %s", streamlineResult ( r ) );
				return false;
			}
			wantsFrameGenerationOn = true;
			return true;
		}

		void setFrameGeneration ( sl::DLSSGMode mode )
		{
			sl::DLSSGOptions	o;
			o.mode = mode;
			o.numFramesToGenerate = 1;
			if ( const auto r = dlssgSetOptions ( viewport, o ); r != sl::Result::eOk )
				reshade::log::message ( reshade::log::level::warning, "dlss bench: slDLSSGSetOptions returned %s", streamlineResult ( r ) );
		}

		reshade::ultra::TestDlssSettings	settings;
		sl::ViewportHandle					viewport { 0 };
		sl::FrameToken*						token = nullptr;
		PFun_slDLSSGSetOptions*				dlssgSetOptions = nullptr;
		PFun_slDLSSGGetState*				dlssgGetState = nullptr;
		PFun_slReflexSetOptions*			reflexSetOptions = nullptr;
		PFun_slReflexSleep*					reflexSleep = nullptr;
		PFun_slPCLSetMarker*				pclSetMarker = nullptr;
		bool								wantsFrameGenerationOn = false;
		uint64_t							generatedPresents = 0;
		size_t								frames = 0;
		uint32_t							statusSeen = 0;
		PFun_slDLSSGetOptimalSettings*		getOptimalSettings = nullptr;
		PFun_slDLSSSetOptions*				setOptions = nullptr;
		bool								hasResources = false;
	};
#endif

	class Bench
	{
	public:
		Bench ( ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain3* swapchain, const reshade::ultra::TestDlssSettings& settings )
			: device ( device ), queue ( queue ), swapchain ( swapchain ), settings ( settings ), checks ( size_t ( CheckId::count ) )
		{
		}

		~Bench ()
		{
			if ( fenceEvent != nullptr )
				CloseHandle ( fenceEvent );
		}

		[[ nodiscard ]] int run ( const std::filesystem::path& dataPath, const std::function<bool ()>& pump );

	private:
		[[ nodiscard ]] bool setUp ();
		[[ nodiscard ]] bool startDlss ( const std::filesystem::path& dataPath );
		[[ nodiscard ]] bool makeSizeImages ( const Mode& mode );
		[[ nodiscard ]] bool recordFrame ();
		[[ nodiscard ]] bool waitFor ( UINT64 value, const char* what );
		[[ nodiscard ]] bool finishFrames ( bool all );
		void judge ( const Record& r );
		void note ( CheckId id, const Record* r, bool ok, const std::string& detail = {} );
		void noteOnce ( CheckId id, bool ok, const std::string& detail = {} );
		void sum ( Image image, Cell cell, UINT width, UINT height );
		void dispatchPass ( ID3D12PipelineState* pipeline, const PassConstants& pass, UINT width, UINT height );
		[[ nodiscard ]] int report ();
		[[ nodiscard ]] D3D12_CPU_DESCRIPTOR_HANDLE rtv ( UINT index ) const;
		[[ nodiscard ]] D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor ( UINT index ) const;
		[[ nodiscard ]] D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptor ( UINT index ) const;
		[[ nodiscard ]] bool isDeviceKept ( const char* when );
		[[ nodiscard ]] bool resizeSwapchain ();
		[[ nodiscard ]] bool makePipelines ();
		void reportTimes ();

		ID3D12Device*								device;
		ID3D12CommandQueue*							queue;
		IDXGISwapChain3*							swapchain;
		reshade::ultra::TestDlssSettings			settings;
		UINT										displayWidth = 0;
		UINT										displayHeight = 0;
		UINT										backBufferCount = 0;
		ComPtr<ID3D12Resource>						backBuffers[ 3 ];
		ComPtr<ID3D12RootSignature>					sceneSignature;
		ComPtr<ID3D12RootSignature>					waterSignature;
		ComPtr<ID3D12RootSignature>					composeSignature;
		ComPtr<ID3D12RootSignature>					computeSignature;
		ComPtr<ID3D12PipelineState>					scenePipeline;
		ComPtr<ID3D12PipelineState>					waterPipeline;
		ComPtr<ID3D12PipelineState>					composePipeline;
		ComPtr<ID3D12PipelineState>					clearPipeline;
		ComPtr<ID3D12PipelineState>					sumPipeline;
		ComPtr<ID3D12PipelineState>					fillPipeline;
		ComPtr<ID3D12PipelineState>					analysePipeline;
		ComPtr<ID3D12PipelineState>					waterComputePipeline;
		ComPtr<ID3DBlob>							sumCode;
		std::vector<ComPtr<ID3D12PipelineState>>	switchPipelines;
		ComPtr<ID3D12DescriptorHeap>				viewHeap;
		ComPtr<ID3D12DescriptorHeap>				rtvHeap;
		ComPtr<ID3D12DescriptorHeap>				dsvHeap;
		UINT										viewSize = 0;
		UINT										rtvSize = 0;
		ComPtr<ID3D12CommandAllocator>				allocators[ slotCount ];
		ComPtr<ID3D12GraphicsCommandList>			list;
		ComPtr<ID3D12Fence>							fence;
		HANDLE										fenceEvent = nullptr;
		UINT64										nextFenceValue = 1;
		UINT64										slotFence[ slotCount ] {};
		ComPtr<ID3D12Resource>						upload;
		uint8_t*									uploadMapped = nullptr;
		ComPtr<ID3D12Resource>						shown;
		ComPtr<ID3D12Resource>						results;
		ComPtr<ID3D12Resource>						rowsBuffer;
		ComPtr<ID3D12Resource>						readback[ slotCount ];
		ComPtr<ID3D12Heap>							sizeHeap;
		ComPtr<ID3D12Resource>						waterPrevious;		// last frame's camera, the water pass's own, kept across mode switches
		bool										isWaterPreviousReadable = false;
		UINT64										sizeOffsets[ 7 ] {};
		SizeImages									images;
		std::array<UINT, rowCount>					rowY {};

		std::unique_ptr<DlssPath>					dlss;

		std::vector<Mode>							modes;
		size_t										mode = 0;
		size_t										frame = 0;
		size_t										frameInMode = 0;
		bool										wantsFeature = false;
		bool										areImagesFresh = false;
		size_t										reloadedMode = SIZE_MAX;
		size_t										remadeMode = SIZE_MAX;
		Matrix										lastViewProj {};		// the frame before's camera with its own render size
		DXGI_FORMAT									backBufferFormat = DXGI_FORMAT_UNKNOWN;

		// Wall-clock times of the app's render thread, in ms
		struct FrameTime
		{
			size_t		frame = 0;
			size_t		mode = 0;
			size_t		frameInMode = 0;
			double		total = 0.0;
			double		present = 0.0;
			double		dlss = 0.0;
			const char*	dlssCall = "";
		};

		struct SwitchTime
		{
			size_t	mode = 0;
			double	idle = 0.0;
			double	release = 0.0;
			double	resize = 0.0;
			double	images = 0.0;
			double	begin = 0.0;
			double	pipelines = 0.0;
		};

		std::vector<FrameTime>						frameTimes;
		std::vector<SwitchTime>						switchTimes;
		double										startToFirstFrame = -1.0;
		double										dlssStart = 0.0;
		std::deque<Record>							pending;
		std::optional<Record>						previous;
		std::vector<Check>							checks;
		size_t										unprocessedFrames = 0;
		std::string									firstUnprocessed;
		bool										isBroken = false;
	};

	//-------------------------------------------------------------------------

	D3D12_CPU_DESCRIPTOR_HANDLE Bench::rtv ( UINT index ) const
	{
		auto	handle = rtvHeap->GetCPUDescriptorHandleForHeapStart ();
		handle.ptr += SIZE_T ( index ) * rtvSize;
		return handle;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE Bench::cpuDescriptor ( UINT index ) const
	{
		auto	handle = viewHeap->GetCPUDescriptorHandleForHeapStart ();
		handle.ptr += SIZE_T ( index ) * viewSize;
		return handle;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE Bench::gpuDescriptor ( UINT index ) const
	{
		auto	handle = viewHeap->GetGPUDescriptorHandleForHeapStart ();
		handle.ptr += UINT64 ( index ) * viewSize;
		return handle;
	}

	void Bench::note ( CheckId id, const Record* r, bool ok, const std::string& detail )
	{
		auto&	c = checks[ size_t ( id ) ];
		++c.checked;
		if ( ok )
			return;

		if ( c.failedPerMode.size () < modes.size () )
			c.failedPerMode.resize ( modes.size () );
		if ( r != nullptr )
			++c.failedPerMode[ r->mode ];

		if ( c.failed++ == 0 )
		{
			if ( r == nullptr )
				c.first = detail;
			else
			{
				const auto&	m = modes[ r->mode ];
				c.first = format ( "%s at frame %zu (%s %ux%u, its frame %zu, %s)", detail.c_str (), r->frame, m.name, m.width, m.height, r->frameInMode, m.how.c_str () );
			}
		}
	}

	void Bench::noteOnce ( CheckId id, bool ok, const std::string& detail )
	{
		note ( id, nullptr, ok, detail );
	}

	bool Bench::isDeviceKept ( const char* when )
	{
		const auto	reason = device->GetDeviceRemovedReason ();
		if ( reason == S_OK )
			return true;

		noteOnce ( CheckId::deviceKept, false, format ( "expected S_OK, got 0x%08X %s (frame %zu)", unsigned ( reason ), when, frame ) );
		isBroken = true;
		return false;
	}

	bool Bench::waitFor ( UINT64 value, const char* what )
	{
		if ( fence->GetCompletedValue () >= value )
			return true;

		if ( FAILED ( fence->SetEventOnCompletion ( value, fenceEvent ) ) || WaitForSingleObject ( fenceEvent, gpuWaitMs ) != WAIT_OBJECT_0 )
		{
			if ( isDeviceKept ( what ) )
			{
				noteOnce ( CheckId::deviceKept, false, format ( "the GPU did not finish %s within %u ms (frame %zu)", what, unsigned ( gpuWaitMs ), frame ) );
				isBroken = true;
			}
			return false;
		}

		return isDeviceKept ( what );
	}

	//-------------------------------------------------------------------------

	bool Bench::setUp ()
	{
		DXGI_SWAP_CHAIN_DESC1	chainDesc {};
		if ( FAILED ( swapchain->GetDesc1 ( &chainDesc ) ) || chainDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || chainDesc.BufferCount > std::size ( backBuffers ) )
		{
			reshade::log::message ( reshade::log::level::warning, "dlss bench: the swap chain is not the 8-bit SDR one the bench presents to (-hdr off)." );
			return false;
		}

		displayWidth = chainDesc.Width;
		displayHeight = chainDesc.Height;
		backBufferFormat = chainDesc.Format;
		backBufferCount = chainDesc.BufferCount;
		for ( UINT i = 0; i < backBufferCount; ++i )
			if ( FAILED ( swapchain->GetBuffer ( i, IID_PPV_ARGS ( &backBuffers[ i ] ) ) ) )
				return false;

		for ( size_t r = 0; r < rowCount; ++r )
			rowY[ r ] = UINT ( float ( displayHeight ) * ( 0.15f + 0.075f * float ( r ) ) );

		const auto	serialize = [ this ] ( const D3D12_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>& out ) {
			ComPtr<ID3DBlob>	blob;
			ComPtr<ID3DBlob>	errors;
			if ( FAILED ( D3D12SerializeRootSignature ( &desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors ) ) )
			{
				reshade::log::message ( reshade::log::level::warning, "dlss bench: a root signature does not serialize: %s", errors != nullptr ? static_cast<const char*> ( errors->GetBufferPointer () ) : "" );
				return false;
			}
			return SUCCEEDED ( device->CreateRootSignature ( 0, blob->GetBufferPointer (), blob->GetBufferSize (), IID_PPV_ARGS ( &out ) ) );
		};

		// Scene: the frame's constants (b0) and the object's (b1)
		{
			D3D12_ROOT_PARAMETER	params[ 2 ] {};
			params[ 0 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[ 0 ].Descriptor = { 0, 0 };
			params[ 1 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[ 1 ].Constants = { 1, 0, 2 };
			const D3D12_ROOT_SIGNATURE_DESC	desc { UINT ( std::size ( params ) ), params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
			if ( ! serialize ( desc, sceneSignature ) )
				return false;
		}

		// Water: root constants (b0), its record of the cameras as a root view (t2), the vectors and the history in a table (t0, t1)
		{
			D3D12_DESCRIPTOR_RANGE	range { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
			D3D12_ROOT_PARAMETER	params[ 3 ] {};
			params[ 0 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[ 0 ].Constants = { 0, 0, 4 };
			params[ 1 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[ 1 ].Descriptor = { 2, 0 };
			params[ 2 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[ 2 ].DescriptorTable = { 1, &range };
			for ( auto& p : params )
				p.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			const D3D12_ROOT_SIGNATURE_DESC	desc { UINT ( std::size ( params ) ), params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
			if ( ! serialize ( desc, waterSignature ) )
				return false;
		}

		{
			D3D12_DESCRIPTOR_RANGE		range { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
			D3D12_ROOT_PARAMETER		param {};
			param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			param.DescriptorTable = { 1, &range };
			param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			D3D12_STATIC_SAMPLER_DESC	sampler {};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			const D3D12_ROOT_SIGNATURE_DESC	desc { 1, &param, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
			if ( ! serialize ( desc, composeSignature ) )
				return false;
		}

		{
			D3D12_DESCRIPTOR_RANGE	ranges[ 2 ] { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0 }, { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, resultsUav } };
			D3D12_ROOT_PARAMETER	params[ 4 ] {};
			params[ 0 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[ 0 ].Constants = { 0, 0, sizeof ( PassConstants ) / 4 };
			params[ 1 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[ 1 ].DescriptorTable = { UINT ( std::size ( ranges ) ), ranges };
			params[ 2 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[ 2 ].Descriptor = { 8, 0 };
			params[ 3 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[ 3 ].Descriptor = { 9, 0 };
			const D3D12_ROOT_SIGNATURE_DESC	desc { UINT ( std::size ( params ) ), params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
			if ( ! serialize ( desc, computeSignature ) )
				return false;
		}

		const auto	sceneVs = compile ( "sceneVs", "vs_5_0" );
		const auto	scenePs = compile ( "scenePs", "ps_5_0" );
		const auto	fullscreenVs = compile ( "fullscreenVs", "vs_5_0" );
		const auto	waterPs = compile ( "waterPs", "ps_5_0" );
		const auto	composePs = compile ( "composePs", "ps_5_0" );
		const auto	csClear = compile ( "csClear", "cs_5_0" );
		const auto	csSum = compile ( "csSum", "cs_5_0" );
		const auto	csFill = compile ( "csFill", "cs_5_0" );
		const auto	csAnalyse = compile ( "csAnalyse", "cs_5_0" );
		const auto	csWater = compile ( "csWater", "cs_5_0" );
		if ( ! sceneVs || ! scenePs || ! fullscreenVs || ! waterPs || ! composePs || ! csClear || ! csSum || ! csFill || ! csAnalyse || ! csWater )
			return false;
		sumCode = csSum;

		const auto	graphics = [ this ] ( ID3D12RootSignature* signature, ID3DBlob* vs, ID3DBlob* ps, std::initializer_list<DXGI_FORMAT> targets, bool depth, ComPtr<ID3D12PipelineState>& out ) {
			D3D12_GRAPHICS_PIPELINE_STATE_DESC	desc {};
			desc.pRootSignature = signature;
			desc.VS = { vs->GetBufferPointer (), vs->GetBufferSize () };
			desc.PS = { ps->GetBufferPointer (), ps->GetBufferSize () };
			for ( UINT i = 0; i < 8; ++i )
				desc.BlendState.RenderTarget[ i ].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			desc.SampleMask = UINT_MAX;
			desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			desc.RasterizerState.DepthClipEnable = TRUE;
			desc.DepthStencilState.DepthEnable = depth;
			desc.DepthStencilState.DepthWriteMask = depth ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
			desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
			desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			desc.NumRenderTargets = UINT ( targets.size () );
			auto	i = 0;
			for ( const auto f : targets )
				desc.RTVFormats[ i++ ] = f;
			desc.DSVFormat = depth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_UNKNOWN;
			desc.SampleDesc = { 1, 0 };
			return SUCCEEDED ( device->CreateGraphicsPipelineState ( &desc, IID_PPV_ARGS ( &out ) ) );
		};

		const auto	computePipeline = [ this ] ( ID3DBlob* cs, ComPtr<ID3D12PipelineState>& out ) {
			D3D12_COMPUTE_PIPELINE_STATE_DESC	desc {};
			desc.pRootSignature = computeSignature.Get ();
			desc.CS = { cs->GetBufferPointer (), cs->GetBufferSize () };
			return SUCCEEDED ( device->CreateComputePipelineState ( &desc, IID_PPV_ARGS ( &out ) ) );
		};

		if ( ! graphics ( sceneSignature.Get (), sceneVs.Get (), scenePs.Get (), { colourFormat, motionFormat }, true, scenePipeline )
			 || ! graphics ( waterSignature.Get (), fullscreenVs.Get (), waterPs.Get (), { motionFormat }, false, waterPipeline )
			 || ! graphics ( composeSignature.Get (), fullscreenVs.Get (), composePs.Get (), { chainDesc.Format }, false, composePipeline )
			 || ! computePipeline ( csClear.Get (), clearPipeline ) || ! computePipeline ( csSum.Get (), sumPipeline )
			 || ! computePipeline ( csFill.Get (), fillPipeline ) || ! computePipeline ( csAnalyse.Get (), analysePipeline )
			 || ! computePipeline ( csWater.Get (), waterComputePipeline ) )
			return false;

		const D3D12_DESCRIPTOR_HEAP_DESC	viewHeapDesc { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, descriptorCount, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
		const D3D12_DESCRIPTOR_HEAP_DESC	rtvHeapDesc { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, targetViewCount };
		const D3D12_DESCRIPTOR_HEAP_DESC	dsvHeapDesc { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2 };
		if ( FAILED ( device->CreateDescriptorHeap ( &viewHeapDesc, IID_PPV_ARGS ( &viewHeap ) ) ) || FAILED ( device->CreateDescriptorHeap ( &rtvHeapDesc, IID_PPV_ARGS ( &rtvHeap ) ) )
			 || FAILED ( device->CreateDescriptorHeap ( &dsvHeapDesc, IID_PPV_ARGS ( &dsvHeap ) ) ) )
			return false;
		viewSize = device->GetDescriptorHandleIncrementSize ( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );
		rtvSize = device->GetDescriptorHandleIncrementSize ( D3D12_DESCRIPTOR_HEAP_TYPE_RTV );

		for ( UINT i = 0; i < backBufferCount; ++i )
			device->CreateRenderTargetView ( backBuffers[ i ].Get (), nullptr, rtv ( i ) );

		for ( auto& a : allocators )
			if ( FAILED ( device->CreateCommandAllocator ( D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS ( &a ) ) ) )
				return false;
		if ( FAILED ( device->CreateCommandList ( 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[ 0 ].Get (), nullptr, IID_PPV_ARGS ( &list ) ) ) || FAILED ( list->Close () ) )
			return false;
		list->SetName ( L"dlss bench frame" );
		if ( FAILED ( device->CreateFence ( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS ( &fence ) ) ) || ( fenceEvent = CreateEventW ( nullptr, FALSE, FALSE, nullptr ) ) == nullptr )
			return false;

		const D3D12_HEAP_PROPERTIES	defaultHeap { D3D12_HEAP_TYPE_DEFAULT };
		const D3D12_HEAP_PROPERTIES	uploadHeap { D3D12_HEAP_TYPE_UPLOAD };
		const D3D12_HEAP_PROPERTIES	readbackHeap { D3D12_HEAP_TYPE_READBACK };
		const auto					rowsBytes = UINT64 ( rowCount ) * displayWidth * sizeof ( float );

		const auto	uploadDesc = bufferDesc ( uploadSlotSize * slotCount, D3D12_RESOURCE_FLAG_NONE );
		const auto	shownDesc = textureDesc ( displayWidth, displayHeight, colourFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
		const auto	resultsDesc = bufferDesc ( resultsSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
		const auto	rowsDesc = bufferDesc ( rowsBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
		const auto	readbackDesc = bufferDesc ( resultsSize + rowsBytes, D3D12_RESOURCE_FLAG_NONE );
		if ( FAILED ( device->CreateCommittedResource ( &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS ( &upload ) ) )
			 || FAILED ( upload->Map ( 0, nullptr, reinterpret_cast<void**> ( &uploadMapped ) ) )
			 || FAILED ( device->CreateCommittedResource ( &defaultHeap, D3D12_HEAP_FLAG_NONE, &shownDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS ( &shown ) ) )
			 || FAILED ( device->CreateCommittedResource ( &defaultHeap, D3D12_HEAP_FLAG_NONE, &resultsDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS ( &results ) ) )
			 || FAILED ( device->CreateCommittedResource ( &defaultHeap, D3D12_HEAP_FLAG_NONE, &rowsDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS ( &rowsBuffer ) ) ) )
			return false;
		shown->SetName ( L"dlss bench output" );
		const auto	cameraDesc = bufferDesc ( 256, D3D12_RESOURCE_FLAG_NONE );
		if ( FAILED ( device->CreateCommittedResource ( &defaultHeap, D3D12_HEAP_FLAG_NONE, &cameraDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS ( &waterPrevious ) ) ) )
			return false;
		for ( auto& r : readback )
			if ( FAILED ( device->CreateCommittedResource ( &readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS ( &r ) ) ) )
				return false;

		// The views that never change: DLSS's output, the results and the rows
		device->CreateShaderResourceView ( shown.Get (), nullptr, cpuDescriptor ( shownSrv ) );
		device->CreateUnorderedAccessView ( shown.Get (), nullptr, nullptr, cpuDescriptor ( shownUav ) );
		D3D12_UNORDERED_ACCESS_VIEW_DESC	resultsView { DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER };
		resultsView.Buffer = { 0, resultsSize / 4, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW };
		device->CreateUnorderedAccessView ( results.Get (), nullptr, &resultsView, cpuDescriptor ( resultsUav ) );
		D3D12_UNORDERED_ACCESS_VIEW_DESC	rowsView { DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER };
		rowsView.Buffer = { 0, UINT ( rowCount * displayWidth ), sizeof ( float ), 0, D3D12_BUFFER_UAV_FLAG_NONE };
		device->CreateUnorderedAccessView ( rowsBuffer.Get (), nullptr, &rowsView, cpuDescriptor ( rowsUav ) );

		// One heap for the images of the largest size, each at its own place
		const D3D12_RESOURCE_DESC	largest[] = {
			textureDesc ( displayWidth, displayHeight, colourFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET ),
			textureDesc ( displayWidth, displayHeight, depthFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ),
			textureDesc ( displayWidth, displayHeight, motionFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS ),
			textureDesc ( displayWidth, displayHeight, motionFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET ),
			textureDesc ( displayWidth, displayHeight, depthFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ),
			textureDesc ( displayWidth, displayHeight, motionFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET ),
			textureDesc ( displayWidth, displayHeight, motionFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET ) };
		static_assert ( std::size ( largest ) == std::size ( decltype ( sizeOffsets ) {} ) );
		UINT64	heapSize = 0;
		for ( size_t i = 0; i < std::size ( largest ); ++i )
		{
			const auto	info = device->GetResourceAllocationInfo ( 0, 1, &largest[ i ] );
			heapSize = ( heapSize + info.Alignment - 1 ) / info.Alignment * info.Alignment;
			sizeOffsets[ i ] = heapSize;
			heapSize += info.SizeInBytes;
		}
		D3D12_HEAP_DESC	heapDesc {};
		heapDesc.SizeInBytes = heapSize;
		heapDesc.Properties = defaultHeap;
		heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
		return SUCCEEDED ( device->CreateHeap ( &heapDesc, IID_PPV_ARGS ( &sizeHeap ) ) );
	}

	bool Bench::startDlss ( const std::filesystem::path& dataPath )
	{
		// Up to a size never had, back down to one the run had, up again to another new one, down to the smallest, up to the largest
		modes = { { Quality::performance, "Performance" }, { Quality::quality, "Quality" }, { Quality::performance, "Performance" },
				  { Quality::balanced, "Balanced" }, { Quality::ultraPerformance, "Ultra Performance" }, { Quality::dlaa, "DLAA" } };

#if ULTRA_TEST_STREAMLINE
		if ( ! settings.streamlineFolder.empty () )
			dlss = std::make_unique<StreamlinePath> ( settings );
		else
#endif
			dlss = std::make_unique<NgxPath> ( dataPath );

		std::string	why;
		if ( ! dlss->start ( device, displayWidth, displayHeight, modes, why ) )
		{
			noteOnce ( CheckId::ngxStarts, false, why );
			return false;
		}

		for ( size_t i = 0; i < modes.size (); ++i )
		{
			auto&	m = modes[ i ];
			if ( m.width == 0 || m.height == 0 || m.width > displayWidth || m.height > displayHeight )
			{
				noteOnce ( CheckId::ngxStarts, false, format ( "expected a render size within %ux%u for %s, got %ux%u", displayWidth, displayHeight, m.name, m.width, m.height ) );
				return false;
			}

			const auto	seen = std::ranges::any_of ( modes.begin (), modes.begin () + i, [ & ] ( const Mode& o ) { return o.width == m.width && o.height == m.height; } );
			if ( i == 0 )
				m.how = "the run's first size";
			else
			{
				const auto&	b = modes[ i - 1 ];
				const auto	up = UINT64 ( m.width ) * m.height > UINT64 ( b.width ) * b.height;
				m.how = format ( "the switch %s from %ux%u to %s", up ? "up" : "down", b.width, b.height, seen ? "a size the run had" : "a size new to the run" );
			}
		}

		noteOnce ( CheckId::ngxStarts, true );
		return true;
	}

	bool Bench::makeSizeImages ( const Mode& m )
	{
		images = {};
		const auto	place = [ this ] ( size_t index, const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out, const wchar_t* name ) {
			if ( FAILED ( device->CreatePlacedResource ( sizeHeap.Get (), sizeOffsets[ index ], &desc, state, nullptr, IID_PPV_ARGS ( &out ) ) ) )
				return false;
			out->SetName ( name );
			return true;
		};

		const auto	w = m.width;
		const auto	h = m.height;
		if ( ! place ( 0, textureDesc ( w, h, colourFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET ), D3D12_RESOURCE_STATE_RENDER_TARGET, images.colour, L"dlss bench colour" )
			 || ! place ( 1, textureDesc ( w, h, depthFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ), D3D12_RESOURCE_STATE_DEPTH_WRITE, images.depth, L"dlss bench depth" )
			 || ! place ( 2, textureDesc ( w, h, motionFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS ), D3D12_RESOURCE_STATE_RENDER_TARGET,
						   images.motion, L"dlss bench motion" )
			 || ! place ( 3, textureDesc ( w, h, motionFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET ), D3D12_RESOURCE_STATE_RENDER_TARGET, images.history, L"dlss bench history" )
			 || ! place ( 4, textureDesc ( w, h, depthFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ), D3D12_RESOURCE_STATE_DEPTH_WRITE, images.depthHistory, L"dlss bench depth history" )
			 || ! place ( 5, textureDesc ( w, h, motionFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET ), D3D12_RESOURCE_STATE_RENDER_TARGET, images.waterBefore, L"dlss bench water before" )
			 || ! place ( 6, textureDesc ( w, h, motionFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET ), D3D12_RESOURCE_STATE_RENDER_TARGET, images.waterAfter, L"dlss bench water after" ) )
			return false;

		D3D12_SHADER_RESOURCE_VIEW_DESC	r32 { DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
		r32.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView ( images.colour.Get (), nullptr, cpuDescriptor ( colourSrv ) );
		device->CreateShaderResourceView ( images.depth.Get (), &r32, cpuDescriptor ( depthSrv ) );
		device->CreateShaderResourceView ( images.motion.Get (), nullptr, cpuDescriptor ( motionSrv ) );
		device->CreateShaderResourceView ( images.history.Get (), nullptr, cpuDescriptor ( historySrv ) );
		device->CreateShaderResourceView ( images.waterBefore.Get (), nullptr, cpuDescriptor ( waterBeforeSrv ) );
		device->CreateShaderResourceView ( images.waterAfter.Get (), nullptr, cpuDescriptor ( waterAfterSrv ) );
		device->CreateShaderResourceView ( images.depthHistory.Get (), &r32, cpuDescriptor ( depthHistorySrv ) );

		device->CreateRenderTargetView ( images.colour.Get (), nullptr, rtv ( colourRtv ) );
		device->CreateRenderTargetView ( images.motion.Get (), nullptr, rtv ( motionRtv ) );
		device->CreateRenderTargetView ( images.history.Get (), nullptr, rtv ( historyRtv ) );
		device->CreateRenderTargetView ( images.waterBefore.Get (), nullptr, rtv ( waterBeforeRtv ) );
		device->CreateRenderTargetView ( images.waterAfter.Get (), nullptr, rtv ( waterAfterRtv ) );
		device->CreateUnorderedAccessView ( images.motion.Get (), nullptr, nullptr, cpuDescriptor ( motionUav ) );
		D3D12_DEPTH_STENCIL_VIEW_DESC	dsv {};
		dsv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		auto	dsvHandle = dsvHeap->GetCPUDescriptorHandleForHeapStart ();
		device->CreateDepthStencilView ( images.depth.Get (), &dsv, dsvHandle );
		dsvHandle.ptr += device->GetDescriptorHandleIncrementSize ( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );
		device->CreateDepthStencilView ( images.depthHistory.Get (), &dsv, dsvHandle );
		return true;
	}

	//-------------------------------------------------------------------------

	void Bench::dispatchPass ( ID3D12PipelineState* pipeline, const PassConstants& pass, UINT width, UINT height )
	{
		list->SetPipelineState ( pipeline );
		list->SetComputeRoot32BitConstants ( 0, sizeof ( pass ) / 4, &pass, 0 );
		list->Dispatch ( ( width + 7 ) / 8, ( height + 7 ) / 8, 1 );
	}

	void Bench::sum ( Image image, Cell cell, UINT width, UINT height )
	{
		PassConstants	pass;
		pass.what = UINT ( image );
		pass.slot = cell;
		pass.passSize[ 0 ] = width;
		pass.passSize[ 1 ] = height;
		dispatchPass ( sumPipeline.Get (), pass, width, height );
	}

	// The game's frame: scene, its passes, DLSS between them, its passes after, the picture to the swap chain
	bool Bench::recordFrame ()
	{
		const auto	frameStart = Clock::now ();
		const auto	slot = UINT ( frame % slotCount );
		if ( ! waitFor ( slotFence[ slot ], "the frame two before" ) || ! finishFrames ( false ) )
			return false;

		const auto&	m = modes[ mode ];
		const auto	w = m.width;
		const auto	h = m.height;
		const auto	first = frameInMode == 0;
		const auto	fresh = first || std::exchange ( areImagesFresh, false );
		dlss->beginFrame ( frame );
		FrameTime	time { frame, mode, frameInMode };

		if ( FAILED ( allocators[ slot ]->Reset () ) || FAILED ( list->Reset ( allocators[ slot ].Get (), nullptr ) ) )
		{
			noteOnce ( CheckId::presented, false, format ( "the frame's command list did not reset (frame %zu)", frame ) );
			return false;
		}

		// The frame's constants, the water draw's record of the cameras, and the inverse camera the water vectors start from
		const auto	previousFrame = frame > 0 ? frame - 1 : 0;
		const auto	proj = projection ( w, h );
		const auto	jitterIndex = frame % 16 + 1;
		const auto	jitterX = halton ( jitterIndex, 2 ) - 0.5f;
		const auto	jitterY = halton ( jitterIndex, 3 ) - 0.5f;

		FrameConstants	constants {};
		constants.viewProj = multiply ( viewAt ( frame ), proj );
		constants.prevViewProj = frame > 0 ? lastViewProj : constants.viewProj;
		lastViewProj = constants.viewProj;
		constants.world[ 0 ] = constants.prevWorld[ 0 ] = identity ();
		constants.world[ 1 ] = cubeAt ( frame );
		constants.prevWorld[ 1 ] = cubeAt ( previousFrame );
		constants.jitterNdc[ 0 ] = 2.0f * jitterX / float ( w );
		constants.jitterNdc[ 1 ] = -2.0f * jitterY / float ( h );
		constants.renderSize[ 0 ] = float ( w );
		constants.renderSize[ 1 ] = float ( h );
		const WaterData	water { constants.viewProj, constants.prevViewProj, { 0.25f, -0.125f, 0.0f, 0.0f } };
		const auto		inverseCamera = inverse ( constants.viewProj );
		auto* const		slotMemory = uploadMapped + slot * uploadSlotSize;
		std::memcpy ( slotMemory + frameConstantsOffset, &constants, sizeof constants );
		std::memcpy ( slotMemory + waterDataOffset, &water, sizeof water );
		std::memcpy ( slotMemory + inverseOffset, inverseCamera.data (), sizeof inverseCamera );
		const auto	uploadAddress = upload->GetGPUVirtualAddress () + slot * uploadSlotSize;

		ID3D12DescriptorHeap* const	heaps[] { viewHeap.Get () };
		list->SetDescriptorHeaps ( 1, heaps );

		const D3D12_VIEWPORT	renderViewport { 0.0f, 0.0f, float ( w ), float ( h ), 0.0f, 1.0f };
		const D3D12_RECT		renderScissor { 0, 0, LONG ( w ), LONG ( h ) };
		const auto				dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart ();
		auto					historyDsv = dsv;
		historyDsv.ptr += device->GetDescriptorHandleIncrementSize ( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );
		constexpr float			zero[ 4 ] {};

		// New images took the memory of the old ones: they start cleared
		if ( fresh )
		{
			D3D12_RESOURCE_BARRIER	aliasing { D3D12_RESOURCE_BARRIER_TYPE_ALIASING };
			list->ResourceBarrier ( 1, &aliasing );
			for ( const auto target : { historyRtv, waterBeforeRtv, waterAfterRtv } )
				list->ClearRenderTargetView ( rtv ( target ), zero, 0, nullptr );
			list->ClearDepthStencilView ( historyDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr );
		}

		if ( ! isWaterPreviousReadable )
		{
			const auto	b = transition ( waterPrevious.Get (), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
			list->ResourceBarrier ( 1, &b );
			isWaterPreviousReadable = true;
		}

		list->SetComputeRootSignature ( computeSignature.Get () );
		list->SetComputeRootDescriptorTable ( 1, gpuDescriptor ( 0 ) );
		list->SetComputeRootShaderResourceView ( 2, uploadAddress + inverseOffset );
		list->SetComputeRootShaderResourceView ( 3, waterPrevious->GetGPUVirtualAddress () );
		{
			PassConstants	pass;
			list->SetPipelineState ( clearPipeline.Get () );
			list->SetComputeRoot32BitConstants ( 0, sizeof ( pass ) / 4, &pass, 0 );
			list->Dispatch ( 1, 1, 1 );
			D3D12_RESOURCE_BARRIER	uav { D3D12_RESOURCE_BARRIER_TYPE_UAV };
			uav.UAV.pResource = results.Get ();
			list->ResourceBarrier ( 1, &uav );
		}

		// What the last frame left in the history
		{
			const D3D12_RESOURCE_BARRIER	b[] { transition ( images.history.Get (), D3D12_RESOURCE_STATE_RENDER_TARGET, readable ),
											  transition ( images.depthHistory.Get (), D3D12_RESOURCE_STATE_DEPTH_WRITE, readable ) };
			list->ResourceBarrier ( UINT ( std::size ( b ) ), b );
		}
		sum ( Image::history, historyAtStart, w, h );
		sum ( Image::depthHistory, depthHistoryAtStart, w, h );

		// The scene
		{
			const D3D12_CPU_DESCRIPTOR_HANDLE	targets[] { rtv ( colourRtv ), rtv ( motionRtv ) };
			constexpr float						background[ 4 ] { 0.02f, 0.02f, 0.025f, 1.0f };
			list->ClearRenderTargetView ( targets[ 0 ], background, 0, nullptr );
			list->ClearRenderTargetView ( targets[ 1 ], zero, 0, nullptr );
			list->ClearDepthStencilView ( dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr );
			list->OMSetRenderTargets ( 2, targets, FALSE, &dsv );
			list->RSSetViewports ( 1, &renderViewport );
			list->RSSetScissorRects ( 1, &renderScissor );
			list->SetGraphicsRootSignature ( sceneSignature.Get () );
			list->SetPipelineState ( scenePipeline.Get () );
			list->SetGraphicsRootConstantBufferView ( 0, uploadAddress + frameConstantsOffset );
			list->IASetPrimitiveTopology ( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

			const UINT	objects[][ 3 ] { { 0, 0, wallVertices }, { 1, 0, 6 }, { 2, 1, 36 } };
			for ( const auto& o : objects )
			{
				list->SetGraphicsRoot32BitConstants ( 1, 2, o, 0 );
				list->DrawInstanced ( o[ 2 ], 1, 0, 0 );
			}

			const D3D12_RESOURCE_BARRIER	b[] { transition ( images.colour.Get (), D3D12_RESOURCE_STATE_RENDER_TARGET, readable ),
											  transition ( images.motion.Get (), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
											  transition ( images.depth.Get (), D3D12_RESOURCE_STATE_DEPTH_WRITE, readable ) };
			list->ResourceBarrier ( UINT ( std::size ( b ) ), b );
		}

		// The water pass writes the floor's vectors from its own copy of last frame's camera
		PassConstants	waterPass;
		waterPass.slot = waterFloor;
		waterPass.passSize[ 0 ] = w;
		waterPass.passSize[ 1 ] = h;
		dispatchPass ( waterComputePipeline.Get (), waterPass, w, h );
		{
			const auto	b = transition ( images.motion.Get (), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readable );
			list->ResourceBarrier ( 1, &b );
		}

		sum ( Image::colour, colourBefore, w, h );
		sum ( Image::depth, depthBefore, w, h );
		sum ( Image::motion, motionBefore, w, h );

		// The water draw once before DLSS for what it gives; its root signature, root arguments, viewport, scissor and topology stay
		// for its second draw after DLSS, which only names its target here
		const float		waterConstants[ 4 ] { float ( w ), float ( h ), 3.0f, 0.0f };
		{
			const auto	target = rtv ( waterBeforeRtv );
			list->OMSetRenderTargets ( 1, &target, FALSE, nullptr );
			list->SetGraphicsRootSignature ( waterSignature.Get () );
			list->SetPipelineState ( waterPipeline.Get () );
			list->SetGraphicsRoot32BitConstants ( 0, 4, waterConstants, 0 );
			list->SetGraphicsRootShaderResourceView ( 1, uploadAddress + waterDataOffset );
			list->SetGraphicsRootDescriptorTable ( 2, gpuDescriptor ( motionSrv ) );
			list->DrawInstanced ( 3, 1, 0, 0 );
			const auto	b = transition ( images.waterBefore.Get (), D3D12_RESOURCE_STATE_RENDER_TARGET, readable );
			list->ResourceBarrier ( 1, &b );
		}
		sum ( Image::waterBefore, waterBefore, w, h );
		{
			const auto	target = rtv ( waterAfterRtv );
			list->OMSetRenderTargets ( 1, &target, FALSE, nullptr );
		}

		// DLSS's output starts as a value it never writes, so a pixel it left shows
		{
			PassConstants	pass;
			pass.displaySize[ 0 ] = displayWidth;
			pass.displaySize[ 1 ] = displayHeight;
			dispatchPass ( fillPipeline.Get (), pass, displayWidth, displayHeight );
			D3D12_RESOURCE_BARRIER	uav { D3D12_RESOURCE_BARRIER_TYPE_UAV };
			uav.UAV.pResource = shown.Get ();
			list->ResourceBarrier ( 1, &uav );
		}

		// -dlss-keep-compute: the water pass's pipeline and arguments stay bound across DLSS for its second run after it
		if ( settings.reliesOnCompute )
		{
			waterPass.slot = keptFloor;
			list->SetPipelineState ( waterComputePipeline.Get () );
			list->SetComputeRoot32BitConstants ( 0, sizeof ( waterPass ) / 4, &waterPass, 0 );
		}

		// A new mode's first frame evaluates with Reset (unless -dlss-no-reset); the direct NGX path creates its feature in this list first
		Evaluation	e;
		e.list = list.Get ();
		e.colour = images.colour.Get ();
		e.depth = images.depth.Get ();
		e.motion = images.motion.Get ();
		e.output = shown.Get ();
		e.renderWidth = w;
		e.renderHeight = h;
		e.displayWidth = displayWidth;
		e.displayHeight = displayHeight;
		e.jitterX = jitterX;
		e.jitterY = jitterY;
		e.isReset = first && ! settings.isResetSkipped;
		e.frame = frame;
		e.viewToClip = proj;
		e.view = viewAt ( frame );
		e.previousView = viewAt ( previousFrame );
		e.yaw = yawAt ( frame );
		e.inputState = readable;
		std::string	why;
		if ( wantsFeature )
		{
			wantsFeature = false;
			const auto	before = Clock::now ();
			const auto	prepared = dlss->prepare ( e, m, why );
			time.dlss += msSince ( before );
			time.dlssCall = "feature creation";
			noteOnce ( CheckId::featureCreated, prepared, format ( "%s for %s %ux%u (frame %zu)", why.c_str (), m.name, w, h, frame ) );
			if ( ! prepared )
			{
				list->Close ();
				isBroken = true;
				return false;
			}
		}

		{
			const auto	before = Clock::now ();
			const auto	evaluated = dlss->evaluate ( e, why );
			const auto	took = msSince ( before );
			if ( took > time.dlss )
				time.dlssCall = "evaluate";
			time.dlss += took;
			noteOnce ( CheckId::evaluated, evaluated, format ( "%s at frame %zu (%s %ux%u, its frame %zu, %s)", why.c_str (), frame, m.name, w, h, frameInMode, m.how.c_str () ) );
		}

		if ( settings.reliesOnCompute )
		{
			const auto	b = transition ( images.motion.Get (), readable, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
			list->ResourceBarrier ( 1, &b );
			list->Dispatch ( ( w + 7 ) / 8, ( h + 7 ) / 8, 1 );
			const auto	back = transition ( images.motion.Get (), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readable );
			list->ResourceBarrier ( 1, &back );
		}

		// DLSS binds heaps and a pipeline of its own: those come back, the rest of the water draw's state was set before DLSS
		list->SetDescriptorHeaps ( 1, heaps );
		list->SetPipelineState ( waterPipeline.Get () );
		list->SetGraphicsRootDescriptorTable ( 2, gpuDescriptor ( motionSrv ) );
		list->DrawInstanced ( 3, 1, 0, 0 );

		{
			const D3D12_RESOURCE_BARRIER	b[] { transition ( images.waterAfter.Get (), D3D12_RESOURCE_STATE_RENDER_TARGET, readable ),
											  transition ( shown.Get (), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readable ) };
			list->ResourceBarrier ( UINT ( std::size ( b ) ), b );
		}

		// The compute passes after DLSS: everything bound again, as NGX leaves nothing of the compute state a game may count on
		list->SetComputeRootSignature ( computeSignature.Get () );
		list->SetComputeRootDescriptorTable ( 1, gpuDescriptor ( 0 ) );
		list->SetComputeRootShaderResourceView ( 2, uploadAddress + inverseOffset );
		list->SetComputeRootShaderResourceView ( 3, waterPrevious->GetGPUVirtualAddress () );
		sum ( Image::colour, colourAfter, w, h );
		sum ( Image::depth, depthAfter, w, h );
		sum ( Image::motion, motionAfter, w, h );
		sum ( Image::history, historyAfter, w, h );
		sum ( Image::depthHistory, depthHistoryAfter, w, h );
		sum ( Image::waterAfter, waterAfter, w, h );
		{
			PassConstants	pass;
			pass.slot = unwritten;
			pass.displaySize[ 0 ] = displayWidth;
			pass.displaySize[ 1 ] = displayHeight;
			pass.inputSize[ 0 ] = w;
			pass.inputSize[ 1 ] = h;
			std::ranges::copy ( rowY, pass.rowY );
			dispatchPass ( analysePipeline.Get (), pass, displayWidth, displayHeight );
		}

		// This frame's vectors, depth and camera become the next frame's history
		{
			const D3D12_RESOURCE_BARRIER	b[] { transition ( images.motion.Get (), readable, D3D12_RESOURCE_STATE_COPY_SOURCE ),
											  transition ( images.history.Get (), readable, D3D12_RESOURCE_STATE_COPY_DEST ),
											  transition ( images.depth.Get (), readable, D3D12_RESOURCE_STATE_COPY_SOURCE ),
											  transition ( images.depthHistory.Get (), readable, D3D12_RESOURCE_STATE_COPY_DEST ),
											  transition ( waterPrevious.Get (), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST ) };
			list->ResourceBarrier ( UINT ( std::size ( b ) ), b );
			list->CopyResource ( images.history.Get (), images.motion.Get () );
			list->CopyResource ( images.depthHistory.Get (), images.depth.Get () );
			list->CopyBufferRegion ( waterPrevious.Get (), 0, upload.Get (), UINT64 ( slot ) * uploadSlotSize + frameConstantsOffset, sizeof ( Matrix ) );
		}
		{
			const D3D12_RESOURCE_BARRIER	b[] { transition ( images.motion.Get (), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET ),
											  transition ( images.history.Get (), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET ),
											  transition ( images.depth.Get (), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE ),
											  transition ( images.depthHistory.Get (), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE ),
											  transition ( waterPrevious.Get (), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE ),
											  transition ( images.colour.Get (), readable, D3D12_RESOURCE_STATE_RENDER_TARGET ),
											  transition ( images.waterBefore.Get (), readable, D3D12_RESOURCE_STATE_RENDER_TARGET ),
											  transition ( images.waterAfter.Get (), readable, D3D12_RESOURCE_STATE_RENDER_TARGET ) };
			list->ResourceBarrier ( UINT ( std::size ( b ) ), b );
		}
		// DLSS's picture to the swap chain
		const auto	backIndex = swapchain->GetCurrentBackBufferIndex ();
		{
			auto	b = transition ( backBuffers[ backIndex ].Get (), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET );
			list->ResourceBarrier ( 1, &b );
			const auto				target = rtv ( backIndex );
			const D3D12_VIEWPORT	viewport { 0.0f, 0.0f, float ( displayWidth ), float ( displayHeight ), 0.0f, 1.0f };
			const D3D12_RECT		scissor { 0, 0, LONG ( displayWidth ), LONG ( displayHeight ) };
			list->OMSetRenderTargets ( 1, &target, FALSE, nullptr );
			list->RSSetViewports ( 1, &viewport );
			list->RSSetScissorRects ( 1, &scissor );
			list->SetGraphicsRootSignature ( composeSignature.Get () );
			list->SetPipelineState ( composePipeline.Get () );
			list->SetGraphicsRootDescriptorTable ( 0, gpuDescriptor ( shownSrv ) );
			list->DrawInstanced ( 3, 1, 0, 0 );
			const D3D12_RESOURCE_BARRIER	after[] { transition ( backBuffers[ backIndex ].Get (), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT ),
												  transition ( shown.Get (), readable, D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
												  transition ( results.Get (), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE ),
												  transition ( rowsBuffer.Get (), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE ) };
			list->ResourceBarrier ( UINT ( std::size ( after ) ), after );
		}

		const auto	rowsBytes = UINT64 ( rowCount ) * displayWidth * sizeof ( float );
		list->CopyBufferRegion ( readback[ slot ].Get (), 0, results.Get (), 0, resultsSize );
		list->CopyBufferRegion ( readback[ slot ].Get (), resultsSize, rowsBuffer.Get (), 0, rowsBytes );
		{
			const D3D12_RESOURCE_BARRIER	b[] { transition ( results.Get (), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
											  transition ( rowsBuffer.Get (), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS ) };
			list->ResourceBarrier ( UINT ( std::size ( b ) ), b );
		}

		if ( FAILED ( list->Close () ) )
		{
			noteOnce ( CheckId::presented, false, format ( "the frame's command list did not close (frame %zu)", frame ) );
			return false;
		}

		ID3D12CommandList* const	lists[] { list.Get () };
		queue->ExecuteCommandLists ( 1, lists );
		dlss->beforePresent ();
		const auto	presentStart = Clock::now ();
		const auto	presented = swapchain->Present ( 0, DXGI_PRESENT_ALLOW_TEARING );
		time.present = msSince ( presentStart );
		dlss->afterPresent ();
		if ( startToFirstFrame < 0.0 )
			startToFirstFrame = msSinceProcessStart ();
		noteOnce ( CheckId::presented, SUCCEEDED ( presented ), format ( "expected S_OK, got 0x%08X at frame %zu", unsigned ( presented ), frame ) );
		slotFence[ slot ] = nextFenceValue++;
		if ( FAILED ( queue->Signal ( fence.Get (), slotFence[ slot ] ) ) || ! isDeviceKept ( "at present" ) || FAILED ( presented ) )
			return false;

		time.total = msSince ( frameStart );
		frameTimes.push_back ( time );

		Record	r;
		r.frame = frame;
		r.mode = mode;
		r.frameInMode = frameInMode;
		r.isFresh = fresh;
		r.slot = slot;
		r.fenceValue = slotFence[ slot ];
		pending.push_back ( std::move ( r ) );
		return true;
	}

	// Reads back the frames the GPU finished, oldest first; all: waits for every one
	bool Bench::finishFrames ( bool all )
	{
		while ( ! pending.empty () )
		{
			auto&	r = pending.front ();
			if ( all && ! waitFor ( r.fenceValue, "the last frames" ) )
				return false;
			if ( fence->GetCompletedValue () < r.fenceValue )
				return true;

			const auto	rowsBytes = size_t ( rowCount ) * displayWidth * sizeof ( float );
			uint8_t*	mapped = nullptr;
			const D3D12_RANGE	range { 0, resultsSize + rowsBytes };
			if ( FAILED ( readback[ r.slot ]->Map ( 0, &range, reinterpret_cast<void**> ( &mapped ) ) ) )
			{
				noteOnce ( CheckId::presented, false, format ( "frame %zu's results did not map", r.frame ) );
				return false;
			}
			std::memcpy ( r.cells.data (), mapped, sizeof ( r.cells ) );
			r.rows.resize ( rowCount * displayWidth );
			std::memcpy ( r.rows.data (), mapped + resultsSize, rowsBytes );
			const D3D12_RANGE	nothing { 0, 0 };
			readback[ r.slot ]->Unmap ( 0, &nothing );

			judge ( r );
			previous = std::move ( r );
			pending.pop_front ();
		}
		return true;
	}

	void Bench::judge ( const Record& r )
	{
		const auto&	c = r.cells;
		const auto	same = [ & ] ( CheckId id, Cell expected, Cell got ) {
			note ( id, &r, c[ expected ] == c[ got ], format ( "expected checksum %08X, got %08X", c[ expected ], c[ got ] ) );
		};

		same ( CheckId::colourKept, colourBefore, colourAfter );
		same ( CheckId::depthKept, depthBefore, depthAfter );
		same ( CheckId::motionKept, motionBefore, motionAfter );
		same ( CheckId::historyKept, historyAtStart, historyAfter );
		same ( CheckId::depthHistoryKept, depthHistoryAtStart, depthHistoryAfter );
		same ( CheckId::drawAfterEvaluate, waterBefore, waterAfter );
		note ( CheckId::computeAfterEvaluate, &r, c[ colourBefore ] == c[ colourAfter ] && c[ depthBefore ] == c[ depthAfter ] && c[ motionBefore ] == c[ motionAfter ],
			   "its checksums of colour, depth and vectors differ from the same pass's before the evaluate" );

		// New images start their history cleared
		if ( previous.has_value () && ! r.isFresh && previous->frame + 1 == r.frame )
		{
			note ( CheckId::historyAfterFrame, &r, c[ historyAtStart ] == previous->cells[ motionBefore ],
				   format ( "expected checksum %08X (last frame's vectors), got %08X", previous->cells[ motionBefore ], c[ historyAtStart ] ) );
			note ( CheckId::depthHistoryAfterFrame, &r, c[ depthHistoryAtStart ] == previous->cells[ depthBefore ],
				   format ( "expected checksum %08X (last frame's depth), got %08X", previous->cells[ depthBefore ], c[ depthHistoryAtStart ] ) );
		}

		const auto	samplesSeen = std::max<uint32_t> ( c[ samples ], 1 );
		const auto	meanBrightness = double ( c[ brightness ] ) / 64.0 / samplesSeen;
		const auto	differingShare = double ( c[ differing ] ) / samplesSeen;
		if ( differingShare <= 0.001 )
		{
			if ( unprocessedFrames++ == 0 )
				firstUnprocessed = format ( "frame %zu (%s, its frame %zu)", r.frame, modes[ r.mode ].name, r.frameInMode );
		}
		note ( CheckId::outputWritten, &r, c[ unwritten ] == 0 && c[ samples ] == displayWidth * displayHeight,
			   format ( "expected 0 unwritten pixels of %u, got %u of %u", displayWidth * displayHeight, c[ unwritten ], c[ samples ] ) );
		note ( CheckId::notBlack, &r, meanBrightness > 0.02, format ( "expected a mean brightness above 0.02, got %.4f", meanBrightness ) );

		// The water pass's camera of the frame before is a run's first frame's own
		if ( r.frame > 0 )
			note ( CheckId::waterVectors, &r, c[ waterFloor ] > 0 && c[ waterMismatch ] == 0,
				   format ( "expected all floor pixels to match, got %u of %u apart", c[ waterMismatch ], c[ waterFloor ] ) );
		if ( settings.reliesOnCompute && r.frame > 0 )
			note ( CheckId::keptCompute, &r, c[ keptFloor ] > 0 && c[ keptMismatch ] == 0,
				   format ( "expected the floor's vectors again, got %u of %u floor pixels apart", c[ keptMismatch ], c[ keptFloor ] ) );

		if ( ! previous.has_value () || previous->frame + 1 != r.frame )
			return;

		// Each checked row's best match in the frame before, against where the turning camera put it
		const auto	from = int ( displayWidth / 2 - displayWidth / 8 );
		const auto	to = int ( displayWidth / 2 + displayWidth / 8 );
		auto		expected = 0.0;
		for ( auto x = from; x < to; ++x )
			expected += double ( previousX ( float ( x ), displayWidth, displayHeight, r.frame, previous->frame ) - float ( x ) );
		expected /= double ( to - from );

		std::string	wrong;
		std::string	moved;
		for ( size_t row = 0; row < rowCount; ++row )
		{
			// Compared as their deviation from the row's mean in units of its spread: a frame of the add-on without NR beside one with it
			// differs in contrast, not in place
			const auto	normalized = [ this, row ] ( const std::vector<float>& rows ) {
				const auto*	values = rows.data () + row * displayWidth;
				auto		mean = 0.0;
				auto		square = 0.0;
				for ( UINT x = 0; x < displayWidth; ++x )
				{
					mean += values[ x ];
					square += double ( values[ x ] ) * values[ x ];
				}
				mean /= displayWidth;
				const auto			spread = std::sqrt ( std::max ( square / displayWidth - mean * mean, 1e-12 ) );
				std::vector<double>	out ( displayWidth );
				for ( UINT x = 0; x < displayWidth; ++x )
					out[ x ] = ( values[ x ] - mean ) / spread;
				return out;
			};
			const auto	now = normalized ( r.rows );
			const auto	before = normalized ( previous->rows );
			std::array<double, 2 * maxShift + 1>	cost {};
			for ( auto s = -maxShift; s <= maxShift; ++s )
			{
				auto	total = 0.0;
				for ( auto x = from; x < to; ++x )
					total += std::abs ( now[ size_t ( x ) ] - before[ size_t ( x + s ) ] );
				cost[ size_t ( s + maxShift ) ] = total / double ( to - from );
			}

			const auto	best = size_t ( std::ranges::min_element ( cost ) - cost.begin () );
			auto		shift = double ( int ( best ) - maxShift );
			if ( best > 0 && best + 1 < cost.size () )
			{
				const auto	curve = cost[ best - 1 ] - 2.0 * cost[ best ] + cost[ best + 1 ];
				if ( curve > 0.0 )
					shift += 0.5 * ( cost[ best - 1 ] - cost[ best + 1 ] ) / curve;
			}

			if ( std::abs ( shift - expected ) > shiftTolerance )
				wrong += format ( "%srow %u moved %.2f px", wrong.empty () ? "" : ", ", rowY[ row ], shift );
			moved += format ( "%s%.2f", moved.empty () ? "" : ", ", shift );
		}

		// What the check sees on a mode's first frames and once after
		if ( r.frameInMode < 4 || r.frameInMode == 10 )
			reshade::log::message ( reshade::log::level::info, "dlss bench: mode %zu, frame %zu: the rows moved %s px, the camera %.2f px", r.mode + 1, r.frame, moved.c_str (), expected );
		// A mode's first two frames hold DLSS's picture without history: logged, not judged
		if ( r.frameInMode > 1 )
			note ( CheckId::followsCamera, &r, wrong.empty (), format ( "expected the picture to move %.2f px, got %s", expected, wrong.c_str () ) );
	}

	//-------------------------------------------------------------------------

	// Where the render thread's time went at the start and around each switch, as lines the script compares between runs
	void Bench::reportTimes ()
	{
		const auto	describe = [] ( const FrameTime& t ) {
			return format ( "%.1f ms (frame %zu: %.1f ms in Present, %.1f ms in DLSS, most in its %s)", t.total, t.frame, t.present, t.dlss, t.dlssCall[ 0 ] != 0 ? t.dlssCall : "nothing" );
		};
		const auto	span = [ & ] ( const char* key, size_t m, size_t count ) {
			const FrameTime*	longest = nullptr;
			double				total = 0.0;
			double				present = 0.0;
			double				dlssTime = 0.0;
			for ( const auto& t : frameTimes )
				if ( t.mode == m && t.frameInMode < count )
				{
					total += t.total;
					present += t.present;
					dlssTime += t.dlss;
					if ( longest == nullptr || t.total > longest->total )
						longest = &t;
				}
			if ( longest != nullptr )
				reshade::log::message ( reshade::log::level::info, "dlss bench: time %s: longest frame %s; its first %zu frames %.1f ms (%.1f ms in Present, %.1f ms in DLSS)", key,
										describe ( *longest ).c_str (), count, total, present, dlssTime );
		};

		reshade::log::message ( reshade::log::level::info, "dlss bench: time start to first frame: %.1f ms (DLSS started in %.1f ms)", startToFirstFrame, dlssStart );
		span ( "at start", 0, 30 );
		for ( const auto& s : switchTimes )
		{
			const auto	key = format ( "at the switch to mode %zu", s.mode + 1 );
			reshade::log::message ( reshade::log::level::info, "dlss bench: time %s: the switch itself %.1f ms (GPU idle %.1f, feature released %.1f, swap chain resized %.1f, images %.1f, "
									"mode started %.1f, pipelines %.1f)", key.c_str (), s.idle + s.release + s.resize + s.images + s.begin + s.pipelines, s.idle, s.release, s.resize,
									s.images, s.begin, s.pipelines );
			span ( key.c_str (), s.mode, 10 );
		}
	}

	// -dlss-resize: the swap chain resized to the size it has, as Ghost of Tsushima does at a mode switch
	bool Bench::resizeSwapchain ()
	{
		for ( auto& b : backBuffers )
			b.Reset ();
		const auto	hr = swapchain->ResizeBuffers ( backBufferCount, displayWidth, displayHeight, backBufferFormat, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING );
		if ( FAILED ( hr ) )
		{
			noteOnce ( CheckId::presented, false, format ( "the swap chain did not resize (0x%08X) at frame %zu", unsigned ( hr ), frame ) );
			return false;
		}
		for ( UINT i = 0; i < backBufferCount; ++i )
		{
			if ( FAILED ( swapchain->GetBuffer ( i, IID_PPV_ARGS ( &backBuffers[ i ] ) ) ) )
				return false;
			device->CreateRenderTargetView ( backBuffers[ i ].Get (), nullptr, rtv ( i ) );
		}
		return true;
	}

	// -dlss-pipelines: new pipelines after a switch, as the game makes about 240 there
	bool Bench::makePipelines ()
	{
		switchPipelines.clear ();
		for ( size_t i = 0; i < settings.pipelinesAtSwitch; ++i )
		{
			D3D12_COMPUTE_PIPELINE_STATE_DESC	desc {};
			desc.pRootSignature = computeSignature.Get ();
			desc.CS = { sumCode->GetBufferPointer (), sumCode->GetBufferSize () };
			ComPtr<ID3D12PipelineState>			pipeline;
			if ( FAILED ( device->CreateComputePipelineState ( &desc, IID_PPV_ARGS ( &pipeline ) ) ) )
				return false;
			switchPipelines.push_back ( pipeline );
		}
		return true;
	}

	int Bench::report ()
	{
		size_t	failures = 0;
		size_t	count = 0;

		reportTimes ();
		reshade::log::message ( reshade::log::level::info, "dlss bench: frames shown as the unprocessed input (by design while DLSS or NR get ready): %zu%s%s", unprocessedFrames,
								unprocessedFrames > 0 ? ", the first at " : "", firstUnprocessed.c_str () );

		for ( size_t i = 0; i < checks.size (); ++i )
		{
			const auto&	c = checks[ i ];
			if ( CheckId ( i ) == CheckId::keptCompute && ! settings.reliesOnCompute )
				continue;
			++count;
			if ( c.failed == 0 && c.checked > 0 )
			{
				reshade::log::message ( reshade::log::level::info, "dlss bench: %s: ok", checkNames[ i ] );
				continue;
			}

			++failures;
			if ( c.checked == 0 )
			{
				reshade::log::message ( reshade::log::level::warning, "dlss bench: %s: FAILED: expected it checked, got no frame to check", checkNames[ i ] );
				continue;
			}

			std::string	perMode;
			for ( size_t m = 0; m < c.failedPerMode.size (); ++m )
				if ( c.failedPerMode[ m ] > 0 )
					perMode += format ( "%s%s %ux%u (mode %zu): %zu", perMode.empty () ? "" : ", ", modes[ m ].name, modes[ m ].width, modes[ m ].height, m + 1, c.failedPerMode[ m ] );
			reshade::log::message ( reshade::log::level::warning, "dlss bench: %s: FAILED: %s; %zu of %zu failed%s%s", checkNames[ i ], c.first.c_str (), c.failed, c.checked,
									perMode.empty () ? "" : ", per mode: ", perMode.c_str () );
		}

		if ( failures == 0 )
			reshade::log::message ( reshade::log::level::info, "dlss bench: %zu checks, all ok", count );
		else
			reshade::log::message ( reshade::log::level::warning, "dlss bench: %zu checks, %zu FAILED", count, failures );
		return failures == 0 ? EXIT_SUCCESS : 5;
	}

	int Bench::run ( const std::filesystem::path& dataPath, const std::function<bool ()>& pump )
	{
		if ( ! setUp () )
		{
			noteOnce ( CheckId::ngxStarts, false, "the bench's own D3D12 set-up failed (see the lines before)" );
			return report ();
		}

		reshade::register_event<reshade::addon_event::present> ( onSwapchainPresent );
		const auto	dlssStarting = Clock::now ();
		const auto	isStarted = startDlss ( dataPath );
		dlssStart = msSince ( dlssStarting );
		if ( isStarted )
		{
			const auto	start = GetTickCount64 ();
			auto		isDone = false;

			std::string	why;
			if ( ! dlss->beginMode ( modes[ 0 ], displayWidth, displayHeight, why ) )
			{
				noteOnce ( CheckId::featureCreated, false, format ( "%s for %s %ux%u", why.c_str (), modes[ 0 ].name, modes[ 0 ].width, modes[ 0 ].height ) );
				isBroken = true;
			}
			else if ( makeSizeImages ( modes[ 0 ] ) )
			{
				wantsFeature = true;
				reshade::log::message ( reshade::log::level::info, "dlss bench: mode 1, %s %ux%u, from frame 0", modes[ 0 ].name, modes[ 0 ].width, modes[ 0 ].height );
			}
			else
				isBroken = true;

			while ( ! isBroken && pump () )
			{
				const auto	isWarm = mode > 0 || GetTickCount64 () - start >= settings.warmupMs;
				if ( frameInMode >= settings.framesPerMode && isWarm )
				{
					// The mode switch: the GPU idle, the feature and the old size's images released, the new ones made
					SwitchTime	t;
					t.mode = mode + 1;
					auto		step = Clock::now ();
					const auto	lap = [ &step ] { const auto took = msSince ( step ); step = Clock::now (); return took; };
					if ( ! finishFrames ( true ) )
						break;
					t.idle = lap ();
					if ( mode + 1 == modes.size () )
					{
						dlss->endMode ();
						isDone = true;
						break;
					}
					// The game's order: the swap chain resized between the last evaluate at the old size and the feature's release
					if ( settings.isResizedAtSwitch && ! resizeSwapchain () )
						break;
					t.resize = lap ();
					dlss->endMode ();
					t.release = lap ();
					++mode;
					const auto&	m = modes[ mode ];
					if ( ! makeSizeImages ( m ) )
					{
						noteOnce ( CheckId::featureCreated, false, format ( "the images for %s %ux%u could not be made", m.name, m.width, m.height ) );
						break;
					}
					t.images = lap ();
					if ( ! dlss->beginMode ( m, displayWidth, displayHeight, why ) )
					{
						noteOnce ( CheckId::featureCreated, false, format ( "%s for %s %ux%u", why.c_str (), m.name, m.width, m.height ) );
						break;
					}
					t.begin = lap ();
					if ( ! makePipelines () )
					{
						noteOnce ( CheckId::featureCreated, false, format ( "the pipelines after the switch to %s could not be made", m.name ) );
						break;
					}
					t.pipelines = lap ();
					switchTimes.push_back ( t );
					reshade::log::message ( reshade::log::level::info, "dlss bench: mode %zu, %s %ux%u, from frame %zu: %s", mode + 1, m.name, m.width, m.height, frame, m.how.c_str () );
					frameInMode = 0;
					wantsFeature = true;
				}

				// -dlss-reload: the add-ons reload a few frames before the switch, as a publish to the live folder does in the game
				if ( settings.reloadBeforeSwitch > 0 && reloadedMode != mode && ( mode > 0 || isWarm )
					 && frameInMode + settings.reloadBeforeSwitch >= settings.framesPerMode )
				{
					reloadedMode = mode;
					reshade::ultra::requestAddonReload ();
					reshade::log::message ( reshade::log::level::info, "dlss bench: the add-ons asked to reload at frame %zu, %zu frames before the switch", frame, settings.reloadBeforeSwitch );
				}

				// -dlss-fg-remake: Frame Generation's resources made anew before the switch, as the game's are at times
				if ( settings.fgRemakeBeforeSwitch > 0 && remadeMode != mode && ( mode > 0 || isWarm )
					 && frameInMode + settings.fgRemakeBeforeSwitch >= settings.framesPerMode )
				{
					remadeMode = mode;
					dlss->remakeFrameGeneration ();
				}

				// -dlss-new-images: new images of the same size within a mode
				else if ( settings.newImagesEvery > 0 && frameInMode > 0 && frameInMode % settings.newImagesEvery == 0 )
				{
					if ( ! finishFrames ( true ) || ! makeSizeImages ( modes[ mode ] ) )
						break;
					areImagesFresh = true;
				}

				if ( ! recordFrame () )
					break;
				++frame;
				++frameInMode;
			}

			(void) finishFrames ( true );
			// A measurement: Frame Generation may pause while the window is not in front, which a test run's never is
			if ( settings.isFrameGenerated )
			{
				std::string	what;
				const auto	ran = dlss->hasFrameGenerationRun ( what );
				reshade::log::message ( reshade::log::level::info, "dlss bench: Frame Generation %s: %s", ran ? "generated frames" : "generated no frames", what.c_str () );
			}
			noteOnce ( CheckId::sequenceDone, isDone, format ( "expected %zu modes of %zu frames, it ended in mode %zu at frame %zu", modes.size (), settings.framesPerMode, mode + 1, frame ) );
			noteOnce ( CheckId::deviceKept, device->GetDeviceRemovedReason () == S_OK, format ( "expected S_OK at the end, got 0x%08X", unsigned ( device->GetDeviceRemovedReason () ) ) );
		}

		if ( dlss != nullptr )
			dlss->stop ( settings.isGameExit );
		return report ();
	}
}
//-----------------------------------------------------------------------------

void reshade::ultra::prepareTestDlssBench ( const TestDlssSettings& settings, const std::filesystem::path& dataPath )
{
#if ULTRA_TEST_STREAMLINE
	if ( settings.streamlineFolder.empty () )
		return;

	auto&		s = streamline;
	s.folder = settings.streamlineFolder.wstring ();
	const auto	path = settings.streamlineFolder / L"sl.interposer.dll";
	s.module = LoadLibraryW ( path.c_str () );
	if ( s.module == nullptr )
	{
		s.why = format ( "sl.interposer.dll does not load from '%s' (error %lu)", path.u8string ().c_str (), GetLastError () );
		return;
	}

	const auto	take = [] ( const char* name, auto& slot ) {
		slot = reinterpret_cast<std::remove_reference_t<decltype ( slot )>> ( GetProcAddress ( streamline.module, name ) );
		if ( slot == nullptr )
			streamline.why = format ( "sl.interposer.dll has no %s", name );
		return slot != nullptr;
	};
	if ( ! take ( "slInit", s.init ) || ! take ( "slShutdown", s.shutdown ) || ! take ( "slSetD3DDevice", s.setDevice ) || ! take ( "slIsFeatureSupported", s.isSupported )
		 || ! take ( "slGetFeatureFunction", s.featureFunction ) || ! take ( "slSetConstants", s.setConstants ) || ! take ( "slEvaluateFeature", s.evaluate )
		 || ! take ( "slGetNewFrameToken", s.newFrameToken ) || ! take ( "slFreeResources", s.freeResources ) || ! take ( "slSetTag", s.setTag ) )
		return;

	// No downloads: neither Streamline nor NGX updates itself during a run
	const wchar_t*			paths[] = { s.folder.c_str () };
	static const sl::Feature	features[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
	static const std::wstring	logs = dataPath.wstring ();
	sl::Preferences			preferences;
	preferences.logLevel = sl::LogLevel::eDefault;
	preferences.pathsToPlugins = paths;
	preferences.numPathsToPlugins = 1;
	preferences.pathToLogsAndData = logs.c_str ();
	preferences.logMessageCallback = onStreamlineLog;
	preferences.flags = sl::PreferenceFlags::eDisableDebugText | ( settings.isListStateTracked ? sl::PreferenceFlags {} : sl::PreferenceFlags::eDisableCLStateTracking );
	preferences.featuresToLoad = features;
	preferences.numFeaturesToLoad = settings.isFrameGenerated ? uint32_t ( std::size ( features ) ) : 1;
	preferences.engine = sl::EngineType::eCustom;
	preferences.engineVersion = "1.0";
	preferences.projectId = "5a3f1d0e-7c2b-4e8a-9f61-0d4b2c8e7a13";
	preferences.renderAPI = sl::RenderAPI::eD3D12;
	const auto	r = s.init ( preferences, sl::kSDKVersion );
	if ( r != sl::Result::eOk )
	{
		s.why = format ( "slInit returned %s (headers of SDK %d.%d.%d)", streamlineResult ( r ), SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH );
		return;
	}
	if ( settings.isFrameGenerated && ( ! take ( "CreateDXGIFactory2", s.createFactory ) || ! take ( "D3D12CreateDevice", s.createDevice ) ) )
		return;
	s.isInitialized = true;
	reshade::log::message ( reshade::log::level::info, "dlss bench: Streamline started from '%s'", settings.streamlineFolder.u8string ().c_str () );
#else
	(void) settings;
	(void) dataPath;
#endif
}

reshade::ultra::TestDlssFactoryCreator reshade::ultra::testDlssFactoryCreator ()
{
#if ULTRA_TEST_STREAMLINE
	return streamline.isInitialized ? streamline.createFactory : nullptr;
#else
	return nullptr;
#endif
}

reshade::ultra::TestDlssDeviceCreator reshade::ultra::testDlssDeviceCreator ()
{
#if ULTRA_TEST_STREAMLINE
	return streamline.isInitialized ? streamline.createDevice : nullptr;
#else
	return nullptr;
#endif
}

void reshade::ultra::attachTestDlssDevice ( ID3D12Device* device )
{
#if ULTRA_TEST_STREAMLINE
	streamline.isDeviceAttached = true;
	if ( const auto r = streamline.setDevice ( device ); r != sl::Result::eOk )
		streamline.deviceProblem = format ( "slSetD3DDevice returned %s", streamlineResult ( r ) );
#else
	(void) device;
#endif
}

int reshade::ultra::runTestDlssBench ( ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain3* swapchain, const TestDlssSettings& settings,
									   const std::filesystem::path& dataPath, const std::function<bool ()>& pump )
{
	Bench	bench ( device, queue, swapchain, settings );
	return bench.run ( dataPath, pump );
}

#endif
