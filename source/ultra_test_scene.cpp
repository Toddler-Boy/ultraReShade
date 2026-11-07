/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RESHADE_TEST_APPLICATION

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <d3dcompiler.h>

#include "dll_log.hpp"
#include "ultra_test_environment.hpp"
#include "ultra_test_scene.hpp"

//-----------------------------------------------------------------------------

// A texture as the APIs take it: levels of rows in one format
struct reshade::ultra::TestTextureSource
{
	struct Level
	{
		uint32_t	width = 0;
		uint32_t	height = 0;
		const void*	data = nullptr;
		size_t		rowBytes = 0;
	};

	DXGI_FORMAT			format = DXGI_FORMAT_UNKNOWN;
	std::vector<Level>	levels;
};
//-----------------------------------------------------------------------------

namespace
{
	constexpr char	shaderSource[] = R"(
cbuffer Scene : register ( b0 )
{
	row_major float4x4	worldViewProj;
	row_major float4x4	world;
	row_major float4x4	previousWorldViewProj;
	float2				jitterNdc;
	float2				viewSize;
};

cbuffer Material : register ( b1 )
{
	uint	shading;
	float	roughnessOverride;	// below 0: the specular map's green
};

cbuffer Lighting : register ( b2 )
{
	float4	irradiance[ 9 ];	// the panorama's irradiance, spherical harmonics
	float4	keyDirection;		// towards the key light, view space; w: its angular radius
	float4	keyIrradiance;
	float	environmentMips;	// the panorama's last mip, roughness 1
	float	unitLength;			// metres per view unit
	float	exposure;
	float	scattering;			// 1: skin scatters light under its surface; 0: plain diffuse
	uint	outputMode;			// the swap chain's picture: SDR, scRGB or PQ
	float	paperWhite;			// nits of scene light 1 in HDR
	float	peakNits;			// what HDR highlights roll off towards
	float	outputPad;
};

Texture2D		colourMap : register ( t0 );
Texture2D		normalMap : register ( t1 );
Texture2D		specularMap : register ( t2 );
Texture2D		environmentMap : register ( t3 );
SamplerState	linearWrap : register ( s0 );

static const float	pi = 3.14159265;
static const uint	standardShading = 0;
static const uint	skinShading = 1;

struct VsIn
{
	float3	position : POSITION;
	float3	normal : NORMAL;
	float2	uv : TEXCOORD;
};

struct VsOut
{
	float4	position : SV_Position;
	float3	viewPosition : POSITION;
	float3	normal : NORMAL;
	float2	uv : TEXCOORD;
	float4	currentClip : TEXCOORD1;
	float4	previousClip : TEXCOORD2;
};

struct PsOut
{
	float4	colour : SV_Target0;
	float2	motion : SV_Target1;
};

VsOut vsMain ( VsIn i )
{
	VsOut	o;
	o.currentClip = mul ( float4 ( i.position, 1 ), worldViewProj );
	o.previousClip = mul ( float4 ( i.position, 1 ), previousWorldViewProj );
	o.position = o.currentClip;
	o.position.xy += jitterNdc * o.position.w;
	o.viewPosition = mul ( float4 ( i.position, 1 ), world ).xyz;
	o.normal = mul ( i.normal, ( float3x3 ) world );
	o.uv = i.uv;
	return o;
}

float3 irradianceAt ( float3 n )
{
	const float3	e = irradiance[ 0 ].rgb * 0.282095
					  + ( irradiance[ 1 ].rgb * n.y + irradiance[ 2 ].rgb * n.z + irradiance[ 3 ].rgb * n.x ) * 0.488603
					  + ( irradiance[ 4 ].rgb * n.x * n.y + irradiance[ 5 ].rgb * n.y * n.z + irradiance[ 7 ].rgb * n.x * n.z ) * 1.092548
					  + irradiance[ 6 ].rgb * 0.315392 * ( 3 * n.z * n.z - 1 ) + irradiance[ 8 ].rgb * 0.546274 * ( n.x * n.x - n.y * n.y );
	return max ( e, 0 );
}

// The panorama's light along d through a GGX lobe of that roughness (u = 0.5 looks at the camera, v from up to down)
float3 environmentAt ( float3 d, float roughness )
{
	const float2	uv = float2 ( atan2 ( -d.x, -d.z ) / ( 2 * pi ) + 0.5, acos ( clamp ( d.y, -1, 1 ) ) / pi );
	return environmentMap.SampleLevel ( linearWrap, uv, roughness * environmentMips ).rgb;
}

// GGX with height-correlated Smith visibility and Schlick's Fresnel; the light's disc widens the lobe and keeps its
// energy (Karis 2013)
float3 keySpecular ( float3 n, float3 v, float3 f0, float roughness )
{
	const float3	l = keyDirection.xyz;
	const float3	h = normalize ( v + l );
	const float		nol = saturate ( dot ( n, l ) );
	const float		nov = saturate ( abs ( dot ( n, v ) ) + 1e-4 );
	const float		noh = saturate ( dot ( n, h ) );
	const float		voh = saturate ( dot ( v, h ) );
	const float		alpha = max ( roughness * roughness, 0.002 );
	const float		widened = saturate ( alpha + keyDirection.w * 0.5 );
	const float		a2 = widened * widened;
	const float		d = noh * noh * ( a2 - 1 ) + 1;
	const float		distribution = a2 / ( pi * d * d ) * ( alpha / widened ) * ( alpha / widened );
	const float		visibility = 0.5 / max ( nol * sqrt ( nov * nov * ( 1 - a2 ) + a2 ) + nov * sqrt ( nol * nol * ( 1 - a2 ) + a2 ), 1e-5 );
	const float3	fresnel = f0 + ( 1 - f0 ) * pow ( 1 - voh, 5 );
	return distribution * visibility * fresnel * nol * keyIrradiance.rgb;
}

// The split sum's second factor, Karis' analytic fit
float3 environmentBrdf ( float3 f0, float roughness, float nov )
{
	const float4	r = roughness * float4 ( -1, -0.0275, -0.572, 0.022 ) + float4 ( 1, 0.0425, 1.04, -0.04 );
	const float		a004 = min ( r.x * r.x, exp2 ( -9.28 * nov ) ) * r.x + r.y;
	const float2	ab = float2 ( -1.04, 1.04 ) * a004 + r.zw;
	return f0 * ab.x + ab.y;
}

float3 specular ( float3 n, float3 v, float3 f0, float roughness )
{
	const float	nov = saturate ( dot ( n, v ) );
	return keySpecular ( n, v, f0, roughness ) + environmentAt ( reflect ( -v, n ), roughness ) * environmentBrdf ( f0, roughness, nov );
}

// The mesh has no tangents: the frame comes from the screen-space derivatives of position and uv
struct SurfaceFrame
{
	float3	t;
	float3	b;
	float3	n;
	float	scale;
};

SurfaceFrame surfaceFrame ( float3 viewPosition, float2 uv, float3 n )
{
	const float3	dp1 = ddx ( viewPosition );
	const float3	dp2 = ddy ( viewPosition );
	const float2	duv1 = ddx ( uv );
	const float2	duv2 = ddy ( uv );
	const float3	dp2perp = cross ( dp2, n );
	const float3	dp1perp = cross ( n, dp1 );

	SurfaceFrame	frame;
	frame.t = dp2perp * duv1.x + dp1perp * duv2.x;
	frame.b = dp2perp * duv1.y + dp1perp * duv2.y;
	frame.n = n;
	const float	frameLength = max ( dot ( frame.t, frame.t ), dot ( frame.b, frame.b ) );
	frame.scale = frameLength > 1e-20 ? rsqrt ( frameLength ) : 0;
	return frame;
}

float3 detailNormal ( SurfaceFrame frame, float4 encoded )
{
	const float3	detail = encoded.xyz * 2 - 1;
	return frame.scale > 0 ? normalize ( ( frame.t * detail.x + frame.b * detail.y ) * frame.scale + frame.n * detail.z ) : frame.n;
}

// Skin scatters light under the surface: red travels furthest. The diffuse light wraps past the terminator by how much
// the surface curves (thin, curved parts glow) and sees a smoother normal per channel, after Penner's pre-integrated
// skin and Jimenez' separable scattering
float3 skinDiffuse ( float3 albedo, float3 n, float3 smooth, float3 geometric, float3 viewPosition )
{
	const float3	nR = normalize ( lerp ( n, smooth, 0.9 ) );
	const float3	nG = normalize ( lerp ( n, smooth, 0.5 ) );
	const float3	nB = normalize ( lerp ( n, smooth, 0.3 ) );
	const float		curvature = length ( fwidth ( geometric ) ) / max ( length ( fwidth ( viewPosition ) ) * unitLength, 1e-6 );
	const float		scatter = saturate ( curvature * 0.015 );
	const float3	wrap = scatter * float3 ( 0.8, 0.35, 0.2 ) + float3 ( 0.15, 0.05, 0.03 );
	const float3	l = keyDirection.xyz;
	const float3	key = saturate ( ( float3 ( dot ( nR, l ), dot ( nG, l ), dot ( nB, l ) ) + wrap ) / ( 1 + wrap ) );
	const float3	ambient = float3 ( irradianceAt ( nR ).r, irradianceAt ( nG ).g, irradianceAt ( nB ).b );
	return albedo / pi * ( key * keyIrradiance.rgb + ambient );
}

float3 lambert ( float3 albedo, float3 n )
{
	return albedo / pi * ( saturate ( dot ( n, keyDirection.xyz ) ) * keyIrradiance.rgb + irradianceAt ( n ) );
}

// ACES filmic curve, Narkowicz' fit
float3 toneMap ( float3 x )
{
	return saturate ( x * ( 2.51 * x + 0.03 ) / ( x * ( 2.43 * x + 0.59 ) + 0.14 ) );
}

float3 encodeSrgb ( float3 c )
{
	return c <= 0.0031308 ? c * 12.92 : 1.055 * pow ( c, 1 / 2.4 ) - 0.055;
}

static const uint	sdrOutput = 0;
static const uint	scRgbOutput = 1;
static const uint	pqOutput = 2;

static const float3x3	rec709ToRec2020 = { 0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956 };

// Scene light in nits, 1 at paper white; above half the peak the highlights roll off towards it
float3 toNits ( float3 light )
{
	const float3	nits = max ( light, 0 ) * paperWhite;
	const float		knee = 0.5 * peakNits;
	return nits < knee ? nits : knee + ( peakNits - knee ) * ( 1 - exp ( -( nits - knee ) / ( peakNits - knee ) ) );
}

// SMPTE ST 2084
float3 encodePq ( float3 nits )
{
	const float3	y = pow ( saturate ( nits / 10000 ), 0.1593017578125 );
	return pow ( ( 0.8359375 + 18.8515625 * y ) / ( 1 + 18.6875 * y ), 78.84375 );
}

// Scene light as the swap chain takes it: ACES and sRGB in SDR, nits / 80 in scRGB, Rec. 2020 with PQ in HDR10
float3 encodeOutput ( float3 light )
{
	if ( outputMode == scRgbOutput )
		return toNits ( light ) / 80;

	if ( outputMode == pqOutput )
		return encodePq ( mul ( rec709ToRec2020, toNits ( light ) ) );

	return encodeSrgb ( toneMap ( light ) );
}

PsOut psMain ( VsOut i )
{
	const float4		colour = colourMap.Sample ( linearWrap, i.uv );
	const float3		v = normalize ( -i.viewPosition );
	const float3		geometric = normalize ( i.normal );
	const float4		material = specularMap.Sample ( linearWrap, i.uv );
	const float			roughness = clamp ( roughnessOverride >= 0 ? roughnessOverride : material.g, 0.03, 1 );
	const float3		f0 = 0.08 * material.r;
	const SurfaceFrame	frame = surfaceFrame ( i.viewPosition, i.uv, geometric );
	const float3		n = detailNormal ( frame, normalMap.Sample ( linearWrap, i.uv ) );
	float3				lit;

	// Unreal's skin: two specular lobes, the sharper one carrying most of the light
	if ( shading == skinShading )
	{
		const float3	smooth = detailNormal ( frame, normalMap.SampleBias ( linearWrap, i.uv, 3 ) );
		const float3	diffuse = scattering > 0 ? skinDiffuse ( colour.rgb, n, smooth, geometric, i.viewPosition ) : lambert ( colour.rgb, n );
		lit = diffuse + 0.85 * specular ( n, v, f0, roughness * 0.75 )
			  + 0.15 * specular ( n, v, f0, min ( roughness * 1.3, 1 ) );
	}
	else
		lit = lambert ( colour.rgb, n ) + specular ( n, v, f0, roughness );

	const float3	output = encodeOutput ( lit * exposure );

	// In pixels, this frame's position minus the last one's, both without jitter (x right, y down)
	const float2	currentNdc = i.currentClip.xy / i.currentClip.w;
	const float2	previousNdc = i.previousClip.xy / i.previousClip.w;

	PsOut	o;
	o.colour = float4 ( output, 1 );
	o.motion = ( currentNdc - previousNdc ) * float2 ( 0.5, -0.5 ) * viewSize;
	return o;
}
)";

	// The shader's shading values
	enum class Shading : uint32_t { standard, skin };

	// The shader's third constant buffer
	struct LightingConstants
	{
		std::array<std::array<float, 4>, 9>	irradiance {};
		float								keyDirection[ 4 ] {};
		float								keyIrradiance[ 4 ] {};
		float								environmentMips = 0.0f;
		float								unitLength = 1.0f;
		float								exposure = 1.0f;
		float								scattering = 1.0f;
		uint32_t							outputMode = 0;
		float								paperWhite = 203.0f;
		float								peakNits = 1000.0f;
		float								outputPad = 0.0f;
	};

	// Near black: about 0.4 nits, a little blue
	[[ nodiscard ]] std::array<float, 4> backgroundColour ( reshade::ultra::TestOutput output )
	{
		constexpr float	nits[ 3 ] = { 0.4f, 0.4f, 0.5f };
		const auto		pq = [] ( float value ) {
			const auto	y = std::pow ( value / 10000.0f, 0.1593017578125f );
			return std::pow ( ( 0.8359375f + 18.8515625f * y ) / ( 1.0f + 18.6875f * y ), 78.84375f );
		};

		switch ( output )
		{
			case reshade::ultra::TestOutput::scRgb:
				return { nits[ 0 ] / 80.0f, nits[ 1 ] / 80.0f, nits[ 2 ] / 80.0f, 1.0f };
			case reshade::ultra::TestOutput::pq:
				return { pq ( nits[ 0 ] ), pq ( nits[ 1 ] ), pq ( nits[ 2 ] ), 1.0f };
			case reshade::ultra::TestOutput::sdr:
				break;
		}

		return { 0.02f, 0.02f, 0.025f, 1.0f };
	}
	constexpr UINT		textureCount = 3;
	constexpr UINT		tableSize = textureCount + 1;		// the model's maps, then the panorama
	constexpr UINT		maxAnisotropy = 8;
	constexpr float		scanRoughness = 0.45f;				// the scan's specular map has no roughness
	constexpr float		pi = 3.14159265f;

	// Above and left of the camera, a little in front of the face; a disc of 0.1 radians
	constexpr float		keyDirection[ 3 ] = { -0.4f, 0.7f, -0.6f };
	constexpr float		keyAngularRadius = 0.1f;
	constexpr float		keyColour[ 3 ] = { 1.0f, 0.97f, 0.92f };

	constexpr DXGI_FORMAT	motionFormat = DXGI_FORMAT_R16G16_FLOAT;
	constexpr DXGI_FORMAT	environmentFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
	constexpr float			noMotion[ 4 ] {};
	constexpr size_t		jitterPhases = 8;
	constexpr size_t		constantsAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
	constexpr char			depthName[] = "Depth buffer";
	constexpr char			motionName[] = "Motion vectors";

	// WKPDID_D3DDebugObjectName and its wide twin: what tools and add-ons read back, D3D12's SetName sets the wide one
	constexpr GUID	debugObjectName { 0x429b8c22, 0x9188, 0x4b0c, { 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 } };
	constexpr GUID	debugObjectNameW { 0x4cca5fd8, 0x921f, 0x42c8, { 0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41 } };

	[[ nodiscard ]] std::wstring widen ( const char* text )
	{
		return std::wstring ( text, text + std::strlen ( text ) );
	}

	void nameD3d11 ( ID3D11DeviceChild* object, const char* name )
	{
		const auto	wide = widen ( name );
		object->SetPrivateData ( debugObjectName, static_cast<UINT> ( std::strlen ( name ) ), name );
		object->SetPrivateData ( debugObjectNameW, static_cast<UINT> ( wide.size () * sizeof ( wchar_t ) ), wide.c_str () );
	}

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

	[[ nodiscard ]] Matrix translation ( float x, float y, float z )
	{
		return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1 };
	}

	[[ nodiscard ]] Matrix rotationX ( float angle )
	{
		return { 1, 0, 0, 0, 0, std::cos ( angle ), std::sin ( angle ), 0, 0, -std::sin ( angle ), std::cos ( angle ), 0, 0, 0, 0, 1 };
	}

	[[ nodiscard ]] Matrix rotationY ( float angle )
	{
		return { std::cos ( angle ), 0, -std::sin ( angle ), 0, 0, 1, 0, 0, std::sin ( angle ), 0, std::cos ( angle ), 0, 0, 0, 0, 1 };
	}

	[[ nodiscard ]] Matrix pose ( reshade::ultra::TestSceneKind kind, float seconds )
	{
		switch ( kind )
		{
			case reshade::ultra::TestSceneKind::cube:
				return multiply ( rotationX ( seconds * 0.5f ), rotationY ( seconds * 0.8f ) );
			case reshade::ultra::TestSceneKind::head:
				return rotationY ( std::sin ( seconds * 0.6f ) * 0.9f );
		}

		return rotationY ( 0.0f );
	}

	// Where the camera looks, in units of the model's bounding sphere: the cube whole, the scan's head without the
	// shoulders
	struct Framing
	{
		float	lift = 0.0f;		// the point turned around and looked at, above the sphere's center
		float	height = 3.46f;		// how much is seen top to bottom, at front
		float	front = 0.0f;		// that plane, this far in front of the point
	};

	[[ nodiscard ]] Framing framing ( reshade::ultra::TestSceneKind kind )
	{
		switch ( kind )
		{
			case reshade::ultra::TestSceneKind::cube:
				return {};
			case reshade::ultra::TestSceneKind::head:
				return { 0.3f, 1.39f, 0.0f };
		}

		return {};
	}

	// Vertical field of view in radians: -fov, or wide for the cube and a portrait lens for the head
	[[ nodiscard ]] float fieldOfView ( const reshade::ultra::TestSceneSettings& settings )
	{
		constexpr float	degrees = 3.14159265f / 180.0f;

		if ( settings.fieldOfView > 0.0f )
			return settings.fieldOfView * degrees;

		return ( settings.kind == reshade::ultra::TestSceneKind::cube ? 60.0f : 22.0f ) * degrees;
	}

	// Camera to the point looked at, so that the framing's height fills the view
	[[ nodiscard ]] float cameraDistance ( const Framing& view, float fov )
	{
		return view.front + view.height * 0.5f / std::tan ( 0.5f * fov );
	}

	// Left-handed, camera at the origin looking down +z
	[[ nodiscard ]] Matrix projection ( UINT width, UINT height, float fov )
	{
		const auto	zNear = 0.1f;
		const auto	zFar = 100.0f;
		const auto	zScale = zFar / ( zFar - zNear );
		const auto	yScale = 1.0f / std::tan ( 0.5f * fov );
		const auto	xScale = yScale * static_cast<float> ( height ) / static_cast<float> ( std::max ( width, 1u ) );

		return { xScale, 0, 0, 0, 0, yScale, 0, 0, 0, 0, zScale, 1, 0, 0, -zNear * zScale, 0 };
	}

	// Element index (from 1) of the radical inverse sequence in base: evenly spread values in [0, 1)
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

	[[ nodiscard ]] reshade::ultra::TestModelImage solidImage ( uint8_t r, uint8_t g, uint8_t b )
	{
		return { 1, 1, { r, g, b, 255 } };
	}

	// What a slot shows when the model brings no image for it: white, flat, and Unreal's default specular and roughness
	[[ nodiscard ]] const reshade::ultra::TestModelImage& imageOrDefault ( const reshade::ultra::TestModelImage& image, size_t slot )
	{
		if ( image.width != 0 && image.height != 0 && image.rgba.size () == size_t ( image.width ) * image.height * 4 )
			return image;

		static const reshade::ultra::TestModelImage	defaults[ textureCount ] = { solidImage ( 255, 255, 255 ), solidImage ( 128, 128, 255 ), solidImage ( 128, 128, 0 ) };
		return defaults[ slot ];
	}

	// Each level averages 2x2 texels of the one above
	[[ nodiscard ]] std::vector<reshade::ultra::TestModelImage> mipChain ( const reshade::ultra::TestModelImage& base )
	{
		std::vector<reshade::ultra::TestModelImage>	levels { base };

		while ( levels.back ().width > 1 || levels.back ().height > 1 )
		{
			const auto&						above = levels.back ();
			reshade::ultra::TestModelImage	level { std::max ( above.width / 2, 1u ), std::max ( above.height / 2, 1u ), {} };
			level.rgba.resize ( size_t ( level.width ) * level.height * 4 );

			for ( uint32_t y = 0; y < level.height; ++y )
				for ( uint32_t x = 0; x < level.width; ++x )
					for ( uint32_t c = 0; c < 4; ++c )
					{
						const auto	x0 = std::min ( x * 2, above.width - 1 );
						const auto	x1 = std::min ( x * 2 + 1, above.width - 1 );
						const auto	y0 = std::min ( y * 2, above.height - 1 );
						const auto	y1 = std::min ( y * 2 + 1, above.height - 1 );
						const auto	texel = [ & ] ( uint32_t tx, uint32_t ty ) { return uint32_t ( above.rgba[ ( size_t ( ty ) * above.width + tx ) * 4 + c ] ); };

						level.rgba[ ( size_t ( y ) * level.width + x ) * 4 + c ] = uint8_t ( ( texel ( x0, y0 ) + texel ( x1, y0 ) + texel ( x0, y1 ) + texel ( x1, y1 ) + 2 ) / 4 );
					}

			levels.push_back ( std::move ( level ) );
		}

		return levels;
	}

	[[ nodiscard ]] reshade::ultra::TestTextureSource imageTexture ( const std::vector<reshade::ultra::TestModelImage>& levels, DXGI_FORMAT format )
	{
		reshade::ultra::TestTextureSource	source { format, {} };

		for ( const auto& level : levels )
			source.levels.push_back ( { level.width, level.height, level.rgba.data (), size_t ( level.width ) * 4 } );

		return source;
	}

	[[ nodiscard ]] reshade::ultra::TestTextureSource environmentTexture ( const reshade::ultra::TestEnvironment& environment )
	{
		reshade::ultra::TestTextureSource	source { environmentFormat, {} };

		for ( const auto& level : environment.levels )
			source.levels.push_back ( { level.width, level.height, level.rgba.data (), size_t ( level.width ) * 16 } );

		return source;
	}

	[[ nodiscard ]] LightingConstants lightingConstants ( const reshade::ultra::TestEnvironment& environment, const reshade::ultra::TestModel& model,
														  const reshade::ultra::TestSceneSettings& settings )
	{
		LightingConstants	constants;
		constants.scattering = settings.isScattering ? 1.0f : 0.0f;
		constants.outputMode = uint32_t ( settings.output );
		constants.paperWhite = settings.paperWhite;
		constants.peakNits = std::max ( settings.peak, settings.paperWhite );
		const auto			length = std::sqrt ( keyDirection[ 0 ] * keyDirection[ 0 ] + keyDirection[ 1 ] * keyDirection[ 1 ] + keyDirection[ 2 ] * keyDirection[ 2 ] );

		constants.irradiance = environment.irradiance;
		constants.environmentMips = static_cast<float> ( environment.levels.size () - 1 );
		constants.unitLength = model.radius;

		for ( auto c = 0; c < 3; ++c )
		{
			constants.keyDirection[ c ] = keyDirection[ c ] / length;
			constants.keyIrradiance[ c ] = keyColour[ c ] * pi;
		}

		constants.keyDirection[ 3 ] = keyAngularRadius;
		return constants;
	}

	// The model's maps with their mips and how its surface is shaded: the scan is skin, the cube a plain surface
	struct MaterialSource
	{
		reshade::ultra::TestMaterialConstants										constants;
		std::array<std::vector<reshade::ultra::TestModelImage>, textureCount>	chains;
	};

	[[ nodiscard ]] MaterialSource materialSource ( const reshade::ultra::TestModel& model, const reshade::ultra::TestSceneSettings& settings )
	{
		MaterialSource	source { {}, { mipChain ( imageOrDefault ( model.colour, 0 ) ), mipChain ( imageOrDefault ( model.normalMap, 1 ) ), mipChain ( imageOrDefault ( model.specular, 2 ) ) } };

		if ( settings.kind == reshade::ultra::TestSceneKind::head )
		{
			source.constants.shading = uint32_t ( Shading::skin );
			source.constants.roughness = scanRoughness;
		}

		return source;
	}

	[[ nodiscard ]] DXGI_FORMAT textureFormat ( size_t slot )
	{
		return slot == 0 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
	}

	constexpr D3D11_INPUT_ELEMENT_DESC	inputElementsD3d11[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof ( reshade::ultra::TestModelVertex, position ), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof ( reshade::ultra::TestModelVertex, normal ), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof ( reshade::ultra::TestModelVertex, uv ), D3D11_INPUT_PER_VERTEX_DATA, 0 } };

	constexpr D3D12_INPUT_ELEMENT_DESC	inputElementsD3d12[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof ( reshade::ultra::TestModelVertex, position ), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof ( reshade::ultra::TestModelVertex, normal ), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof ( reshade::ultra::TestModelVertex, uv ), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };

	[[ nodiscard ]] Microsoft::WRL::ComPtr<ID3DBlob> compile ( const char* entryPoint, const char* profile )
	{
		static const auto	compilerModule = LoadLibraryW ( L"d3dcompiler_47.dll" );

		if ( compilerModule == nullptr )
			return {};

		const auto	compileFunc = reinterpret_cast<pD3DCompile> ( GetProcAddress ( compilerModule, "D3DCompile" ) );

		if ( compileFunc == nullptr )
			return {};

		Microsoft::WRL::ComPtr<ID3DBlob>	code;
		Microsoft::WRL::ComPtr<ID3DBlob>	errors;

		if ( FAILED ( compileFunc ( shaderSource, sizeof ( shaderSource ) - 1, "ultra_test_scene", nullptr, nullptr, entryPoint, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
									&errors ) ) )
		{
			if ( errors != nullptr )
				reshade::log::message ( reshade::log::level::error, "Test application: the scene's shader doesn't compile: %s", static_cast<const char*> ( errors->GetBufferPointer () ) );

			return {};
		}

		return code;
	}

	[[ nodiscard ]] Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> createTextureD3d11 ( ID3D11Device* device, const reshade::ultra::TestTextureSource& source )
	{
		std::vector<D3D11_SUBRESOURCE_DATA>	data;

		for ( const auto& level : source.levels )
			data.push_back ( { level.data, static_cast<UINT> ( level.rowBytes ), 0 } );

		D3D11_TEXTURE2D_DESC	desc {};
		desc.Width = source.levels[ 0 ].width;
		desc.Height = source.levels[ 0 ].height;
		desc.MipLevels = static_cast<UINT> ( source.levels.size () );
		desc.ArraySize = 1;
		desc.Format = source.format;
		desc.SampleDesc = { 1, 0 };
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		Microsoft::WRL::ComPtr<ID3D11Texture2D>				texture;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>	view;

		if ( SUCCEEDED ( device->CreateTexture2D ( &desc, data.data (), &texture ) ) )
			device->CreateShaderResourceView ( texture.Get (), nullptr, &view );

		return view;
	}

	[[ nodiscard ]] Microsoft::WRL::ComPtr<ID3D11Buffer> createBufferD3d11 ( ID3D11Device* device, const void* data, size_t size, UINT bindFlags )
	{
		D3D11_BUFFER_DESC		desc {};
		D3D11_SUBRESOURCE_DATA	initial { data };
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.ByteWidth = static_cast<UINT> ( bindFlags == D3D11_BIND_CONSTANT_BUFFER ? ( size + 15 ) / 16 * 16 : size );
		desc.BindFlags = bindFlags;

		Microsoft::WRL::ComPtr<ID3D11Buffer>	buffer;
		device->CreateBuffer ( &desc, &initial, &buffer );
		return buffer;
	}

	void addCubeFace ( reshade::ultra::TestModel& cube, const float ( &n )[ 3 ], const float ( &u )[ 3 ], const float ( &v )[ 3 ], float faceU )
	{
		static constexpr float	corners[ 4 ][ 2 ] = { { -1, -1 }, { -1, 1 }, { 1, 1 }, { 1, -1 } };
		const auto				first = static_cast<uint32_t> ( cube.vertices.size () );

		for ( const auto& c : corners )
		{
			reshade::ultra::TestModelVertex	vertex {};

			for ( auto axis = 0; axis < 3; ++axis )
			{
				vertex.position[ axis ] = n[ axis ] + c[ 0 ] * u[ axis ] + c[ 1 ] * v[ axis ];
				vertex.normal[ axis ] = n[ axis ];
			}

			vertex.uv[ 0 ] = faceU;
			vertex.uv[ 1 ] = 0.5f;
			cube.vertices.push_back ( vertex );
		}

		for ( const auto index : { 0u, 1u, 2u, 0u, 2u, 3u } )
			cube.indices.push_back ( first + index );
	}
}
//-----------------------------------------------------------------------------

DXGI_FORMAT reshade::ultra::backBufferFormat ( TestOutput output )
{
	switch ( output )
	{
		case TestOutput::scRgb:
			return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case TestOutput::pq:
			return DXGI_FORMAT_R10G10B10A2_UNORM;
		case TestOutput::sdr:
			break;
	}

	return DXGI_FORMAT_R8G8B8A8_UNORM;
}
//-----------------------------------------------------------------------------

DXGI_COLOR_SPACE_TYPE reshade::ultra::colourSpace ( TestOutput output )
{
	switch ( output )
	{
		case TestOutput::scRgb:
			return DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
		case TestOutput::pq:
			return DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
		case TestOutput::sdr:
			break;
	}

	return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}
//-----------------------------------------------------------------------------

// Each face samples its own texel of a 6x1 colour map
reshade::ultra::TestModel reshade::ultra::makeTestCube ()
{
	static constexpr float	normals[ 6 ][ 3 ] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };

	TestModel	cube;
	cube.radius = std::sqrt ( 3.0f );
	cube.colour = { 6, 1, {} };

	for ( auto face = 0; face < 6; ++face )
	{
		const auto&	n = normals[ face ];
		const float	u[ 3 ] = { n[ 0 ] != 0 ? 0.0f : 1.0f, n[ 0 ] != 0 ? 1.0f : 0.0f, 0.0f };
		const float	v[ 3 ] = { n[ 1 ] * u[ 2 ] - n[ 2 ] * u[ 1 ], n[ 2 ] * u[ 0 ] - n[ 0 ] * u[ 2 ], n[ 0 ] * u[ 1 ] - n[ 1 ] * u[ 0 ] };
		addCubeFace ( cube, n, u, v, ( static_cast<float> ( face ) + 0.5f ) / 6.0f );

		for ( auto axis = 0; axis < 3; ++axis )
			cube.colour.rgba.push_back ( static_cast<uint8_t> ( ( std::abs ( n[ axis ] ) * 0.6f + 0.3f ) * 255.0f ) );

		cube.colour.rgba.push_back ( 255 );
	}

	return cube;
}
//-----------------------------------------------------------------------------

reshade::ultra::TestSceneView::TestSceneView ( const TestModel& model, const TestSceneSettings& settings ) : settings ( settings )
{
	std::copy ( std::begin ( model.center ), std::end ( model.center ), center );
	radius = model.radius;
}
//-----------------------------------------------------------------------------

// The model's bounding sphere becomes the unit sphere; z flips from glTF's right-handed space to the left-handed view
reshade::ultra::TestSceneConstants reshade::ultra::TestSceneView::nextFrame ( UINT width, UINT height, float seconds )
{
	const auto	scale = 1.0f / radius;
	const auto	view = framing ( settings.kind );
	const auto	fit = multiply ( multiply ( translation ( -center[ 0 ], -center[ 1 ], -center[ 2 ] ), Matrix { scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, -scale, 0, 0, 0, 0, 1 } ),
								 translation ( 0.0f, -view.lift, 0.0f ) );

	TestSceneConstants	constants;
	const auto	fov = fieldOfView ( settings );
	constants.world = multiply ( multiply ( fit, pose ( settings.kind, seconds ) ), translation ( 0.0f, 0.0f, cameraDistance ( view, fov ) ) );
	constants.worldViewProj = multiply ( constants.world, projection ( width, height, fov ) );
	constants.previousWorldViewProj = frameNumber == 0 ? constants.worldViewProj : previous;
	constants.viewSize[ 0 ] = static_cast<float> ( width );
	constants.viewSize[ 1 ] = static_cast<float> ( height );

	if ( settings.isJittered && width != 0 && height != 0 )
	{
		const auto	phase = frameNumber % jitterPhases + 1;
		constants.jitterNdc[ 0 ] = ( halton ( phase, 2 ) - 0.5f ) * 2.0f / static_cast<float> ( width );
		constants.jitterNdc[ 1 ] = -( halton ( phase, 3 ) - 0.5f ) * 2.0f / static_cast<float> ( height );
	}

	previous = constants.worldViewProj;
	++frameNumber;
	return constants;
}
//-----------------------------------------------------------------------------

reshade::ultra::TestSceneD3d11::TestSceneD3d11 ( ID3D11Device* device, const TestModel& model, const TestSceneSettings& settings )
	: device ( device ), view ( model, settings ), background ( backgroundColour ( settings.output ) )
{
	const auto	vsCode = compile ( "vsMain", "vs_5_0" );
	const auto	psCode = compile ( "psMain", "ps_5_0" );

	if ( vsCode == nullptr || psCode == nullptr )
		return;

	device->CreateVertexShader ( vsCode->GetBufferPointer (), vsCode->GetBufferSize (), nullptr, &vs );
	device->CreatePixelShader ( psCode->GetBufferPointer (), psCode->GetBufferSize (), nullptr, &ps );
	device->CreateInputLayout ( inputElementsD3d11, UINT ( std::size ( inputElementsD3d11 ) ), vsCode->GetBufferPointer (), vsCode->GetBufferSize (), &inputLayout );
	indexCount = static_cast<UINT> ( model.indices.size () );

	vertexBuffer = createBufferD3d11 ( device, model.vertices.data (), model.vertices.size () * sizeof ( TestModelVertex ), D3D11_BIND_VERTEX_BUFFER );
	indexBuffer = createBufferD3d11 ( device, model.indices.data (), model.indices.size () * sizeof ( uint32_t ), D3D11_BIND_INDEX_BUFFER );

	D3D11_BUFFER_DESC	constantsDesc {};
	constantsDesc.Usage = D3D11_USAGE_DEFAULT;
	constantsDesc.ByteWidth = ( sizeof ( TestSceneConstants ) + 15 ) / 16 * 16;
	constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	device->CreateBuffer ( &constantsDesc, nullptr, &constants );

	const auto	panorama = makeTestEnvironment ( settings.environment );
	const auto	lightingData = lightingConstants ( panorama, model, settings );
	lighting = createBufferD3d11 ( device, &lightingData, sizeof ( lightingData ), D3D11_BIND_CONSTANT_BUFFER );
	environment = createTextureD3d11 ( device, environmentTexture ( panorama ) );

	const auto	material = materialSource ( model, settings );
	materialConstants = createBufferD3d11 ( device, &material.constants, sizeof ( material.constants ), D3D11_BIND_CONSTANT_BUFFER );

	for ( size_t slot = 0; slot < textureCount; ++slot )
		textures[ slot ] = createTextureD3d11 ( device, imageTexture ( material.chains[ slot ], textureFormat ( slot ) ) );

	D3D11_SAMPLER_DESC	samplerDesc {};
	samplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
	samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.MaxAnisotropy = maxAnisotropy;
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	samplerDesc.MaxLOD = FLT_MAX;
	device->CreateSamplerState ( &samplerDesc, &sampler );

	D3D11_RASTERIZER_DESC	rasterizerDesc {};
	rasterizerDesc.FillMode = D3D11_FILL_SOLID;
	rasterizerDesc.CullMode = D3D11_CULL_NONE;
	rasterizerDesc.DepthClipEnable = TRUE;
	device->CreateRasterizerState ( &rasterizerDesc, &rasterizer );

	D3D11_DEPTH_STENCIL_DESC	depthDesc {};
	depthDesc.DepthEnable = TRUE;
	depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	device->CreateDepthStencilState ( &depthDesc, &depthState );
}
//-----------------------------------------------------------------------------

bool reshade::ultra::TestSceneD3d11::isReady () const
{
	const auto	hasTextures = std::ranges::all_of ( textures, [] ( const auto& t ) { return t != nullptr; } );

	return vs != nullptr && ps != nullptr && inputLayout != nullptr && vertexBuffer != nullptr && indexBuffer != nullptr && constants != nullptr && lighting != nullptr
		   && materialConstants != nullptr && environment != nullptr && hasTextures && depthState != nullptr && sampler != nullptr && rasterizer != nullptr && dsv != nullptr
		   && motionRtv != nullptr;
}
//-----------------------------------------------------------------------------

void reshade::ultra::TestSceneD3d11::resize ( ID3D11Texture2D* backBuffer )
{
	dsv.Reset ();
	depth.Reset ();

	D3D11_TEXTURE2D_DESC	desc {};
	backBuffer->GetDesc ( &desc );
	width = desc.Width;
	height = desc.Height;

	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32_TYPELESS;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;

	if ( FAILED ( device->CreateTexture2D ( &desc, nullptr, &depth ) ) )
		return;

	D3D11_DEPTH_STENCIL_VIEW_DESC	dsvDesc {};
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsvDesc.ViewDimension = desc.SampleDesc.Count > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
	device->CreateDepthStencilView ( depth.Get (), &dsvDesc, &dsv );
	nameD3d11 ( depth.Get (), depthName );

	motionRtv.Reset ();
	motion.Reset ();
	desc.Format = motionFormat;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	if ( SUCCEEDED ( device->CreateTexture2D ( &desc, nullptr, &motion ) ) )
	{
		device->CreateRenderTargetView ( motion.Get (), nullptr, &motionRtv );
		nameD3d11 ( motion.Get (), motionName );
	}
}
//-----------------------------------------------------------------------------

void reshade::ultra::TestSceneD3d11::render ( ID3D11DeviceContext* context, ID3D11RenderTargetView* backBufferRtv, float seconds )
{
	context->ClearRenderTargetView ( backBufferRtv, background.data () );

	if ( ! isReady () )
		return;

	const auto	frameConstants = view.nextFrame ( width, height, seconds );
	context->UpdateSubresource ( constants.Get (), 0, nullptr, &frameConstants, 0, 0 );

	ID3D11RenderTargetView* const	targets[] { backBufferRtv, motionRtv.Get () };
	context->ClearRenderTargetView ( motionRtv.Get (), noMotion );
	context->ClearDepthStencilView ( dsv.Get (), D3D11_CLEAR_DEPTH, 1.0f, 0 );
	context->OMSetRenderTargets ( UINT ( std::size ( targets ) ), targets, dsv.Get () );

	const D3D11_VIEWPORT	viewport { 0.0f, 0.0f, static_cast<float> ( width ), static_cast<float> ( height ), 0.0f, 1.0f };
	context->RSSetViewports ( 1, &viewport );
	context->RSSetState ( rasterizer.Get () );

	const auto	stride = UINT ( sizeof ( TestModelVertex ) );
	const auto	offset = 0u;
	context->IASetInputLayout ( inputLayout.Get () );
	context->IASetPrimitiveTopology ( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	context->IASetVertexBuffers ( 0, 1, vertexBuffer.GetAddressOf (), &stride, &offset );
	context->IASetIndexBuffer ( indexBuffer.Get (), DXGI_FORMAT_R32_UINT, 0 );

	ID3D11Buffer* const				pixelConstants[] { constants.Get (), materialConstants.Get (), lighting.Get () };
	ID3D11ShaderResourceView* const	views[ tableSize ] { textures[ 0 ].Get (), textures[ 1 ].Get (), textures[ 2 ].Get (), environment.Get () };
	context->VSSetConstantBuffers ( 0, 1, constants.GetAddressOf () );
	context->VSSetShader ( vs.Get (), nullptr, 0 );
	context->PSSetShader ( ps.Get (), nullptr, 0 );
	context->PSSetConstantBuffers ( 0, UINT ( std::size ( pixelConstants ) ), pixelConstants );
	context->PSSetShaderResources ( 0, tableSize, views );
	context->PSSetSamplers ( 0, 1, sampler.GetAddressOf () );
	context->OMSetBlendState ( nullptr, nullptr, UINT_MAX );
	context->OMSetDepthStencilState ( depthState.Get (), 0 );
	context->DrawIndexed ( indexCount, 0, 0 );
}
//-----------------------------------------------------------------------------

reshade::ultra::TestSceneD3d12::TestSceneD3d12 ( ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT backBufferFormat, const TestModel& model,
												 const TestSceneSettings& settings )
	: device ( device ), queue ( queue ), view ( model, settings ), background ( backgroundColour ( settings.output ) ), constantsPath ( settings.constantsPath )
{
	const auto	vsCode = compile ( "vsMain", "vs_5_0" );
	const auto	psCode = compile ( "psMain", "ps_5_0" );

	if ( vsCode == nullptr || psCode == nullptr )
		return;

	D3D12_DESCRIPTOR_RANGE	range {};
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = tableSize;

	// The frame's constants (b0, as -constants says), the model's table, the material (b1), the lighting (b2)
	D3D12_DESCRIPTOR_RANGE	constantsRange {};
	constantsRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
	constantsRange.NumDescriptors = 1;

	D3D12_ROOT_PARAMETER	params[ 4 ] {};
	params[ 0 ].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	switch ( constantsPath )
	{
		case TestConstantsPath::root:
			params[ 0 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[ 0 ].Constants.Num32BitValues = sizeof ( TestSceneConstants ) / 4;
			break;
		case TestConstantsPath::cbv:
			params[ 0 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[ 0 ].Descriptor = { 0, 0 };
			break;
		case TestConstantsPath::table:
			params[ 0 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[ 0 ].DescriptorTable = { 1, &constantsRange };
			break;
	}

	params[ 1 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[ 1 ].DescriptorTable = { 1, &range };
	params[ 1 ].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[ 2 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[ 2 ].Descriptor = { 1, 0 };
	params[ 2 ].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[ 3 ].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[ 3 ].Descriptor = { 2, 0 };
	params[ 3 ].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC	samplerDesc {};
	samplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
	samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.MaxAnisotropy = maxAnisotropy;
	samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
	samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC	rootDesc {};
	rootDesc.NumParameters = UINT ( std::size ( params ) );
	rootDesc.pParameters = params;
	rootDesc.NumStaticSamplers = 1;
	rootDesc.pStaticSamplers = &samplerDesc;
	rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	Microsoft::WRL::ComPtr<ID3DBlob>	rootBlob;

	if ( FAILED ( D3D12SerializeRootSignature ( &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, nullptr ) )
		 || FAILED ( device->CreateRootSignature ( 0, rootBlob->GetBufferPointer (), rootBlob->GetBufferSize (), IID_PPV_ARGS ( &rootSignature ) ) ) )
		return;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC	pipelineDesc {};
	pipelineDesc.pRootSignature = rootSignature.Get ();
	pipelineDesc.VS = { vsCode->GetBufferPointer (), vsCode->GetBufferSize () };
	pipelineDesc.PS = { psCode->GetBufferPointer (), psCode->GetBufferSize () };
	pipelineDesc.BlendState.RenderTarget[ 0 ] = { FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
												   D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL };
	pipelineDesc.SampleMask = UINT_MAX;
	pipelineDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pipelineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pipelineDesc.RasterizerState.DepthClipEnable = TRUE;
	pipelineDesc.DepthStencilState.DepthEnable = TRUE;
	pipelineDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pipelineDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	pipelineDesc.InputLayout = { inputElementsD3d12, UINT ( std::size ( inputElementsD3d12 ) ) };
	pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pipelineDesc.NumRenderTargets = 2;
	pipelineDesc.RTVFormats[ 0 ] = backBufferFormat;
	pipelineDesc.RTVFormats[ 1 ] = motionFormat;
	pipelineDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	pipelineDesc.SampleDesc = { 1, 0 };
	device->CreateGraphicsPipelineState ( &pipelineDesc, IID_PPV_ARGS ( &pipeline ) );

	for ( auto& allocator : allocators )
		device->CreateCommandAllocator ( D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS ( &allocator ) );

	if ( allocators[ 0 ] != nullptr && SUCCEEDED ( device->CreateCommandList ( 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[ 0 ].Get (), nullptr, IID_PPV_ARGS ( &cmdList ) ) ) )
		cmdList->Close ();

	device->CreateFence ( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS ( &fence ) );
	fenceEvent = CreateEventW ( nullptr, FALSE, FALSE, nullptr );

	const D3D12_DESCRIPTOR_HEAP_DESC	dsvHeapDesc { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
	device->CreateDescriptorHeap ( &dsvHeapDesc, IID_PPV_ARGS ( &dsvHeap ) );

	const D3D12_DESCRIPTOR_HEAP_DESC	rtvHeapDesc { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
	device->CreateDescriptorHeap ( &rtvHeapDesc, IID_PPV_ARGS ( &rtvHeap ) );

	isUploaded = upload ( model, settings );
}
//-----------------------------------------------------------------------------

reshade::ultra::TestSceneD3d12::~TestSceneD3d12 ()
{
	waitIdle ();

	if ( fenceEvent != nullptr )
		CloseHandle ( fenceEvent );
}
//-----------------------------------------------------------------------------

// Copied to the GPU through a staging buffer; returns after the copy is done
Microsoft::WRL::ComPtr<ID3D12Resource> reshade::ultra::TestSceneD3d12::uploadTexture ( const TestTextureSource& source )
{
	const auto	levelCount = static_cast<UINT> ( source.levels.size () );

	D3D12_RESOURCE_DESC	desc { D3D12_RESOURCE_DIMENSION_TEXTURE2D };
	desc.Width = source.levels[ 0 ].width;
	desc.Height = source.levels[ 0 ].height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = static_cast<UINT16> ( levelCount );
	desc.Format = source.format;
	desc.SampleDesc = { 1, 0 };

	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>	layouts ( levelCount );
	UINT64											totalSize = 0;
	device->GetCopyableFootprints ( &desc, 0, levelCount, 0, layouts.data (), nullptr, nullptr, &totalSize );

	D3D12_RESOURCE_DESC	stagingDesc { D3D12_RESOURCE_DIMENSION_BUFFER };
	stagingDesc.Width = totalSize;
	stagingDesc.Height = 1;
	stagingDesc.DepthOrArraySize = 1;
	stagingDesc.MipLevels = 1;
	stagingDesc.SampleDesc = { 1, 0 };
	stagingDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	const D3D12_HEAP_PROPERTIES				defaultHeap { D3D12_HEAP_TYPE_DEFAULT };
	const D3D12_HEAP_PROPERTIES				uploadHeap { D3D12_HEAP_TYPE_UPLOAD };
	Microsoft::WRL::ComPtr<ID3D12Resource>	texture;
	Microsoft::WRL::ComPtr<ID3D12Resource>	staging;
	uint8_t*								mapped = nullptr;

	if ( FAILED ( device->CreateCommittedResource ( &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS ( &texture ) ) )
		 || FAILED ( device->CreateCommittedResource ( &uploadHeap, D3D12_HEAP_FLAG_NONE, &stagingDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS ( &staging ) ) )
		 || FAILED ( staging->Map ( 0, nullptr, reinterpret_cast<void**> ( &mapped ) ) ) )
		return {};

	if ( FAILED ( allocators[ 0 ]->Reset () ) || FAILED ( cmdList->Reset ( allocators[ 0 ].Get (), nullptr ) ) )
	{
		staging->Unmap ( 0, nullptr );
		return {};
	}

	for ( UINT level = 0; level < levelCount; ++level )
	{
		const auto&	image = source.levels[ level ];
		const auto&	layout = layouts[ level ];

		for ( uint32_t row = 0; row < image.height; ++row )
			std::memcpy ( mapped + layout.Offset + size_t ( row ) * layout.Footprint.RowPitch, static_cast<const uint8_t*> ( image.data ) + row * image.rowBytes, image.rowBytes );

		D3D12_TEXTURE_COPY_LOCATION	destination { texture.Get (), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
		D3D12_TEXTURE_COPY_LOCATION	from { staging.Get (), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
		destination.SubresourceIndex = level;
		from.PlacedFootprint = layout;
		cmdList->CopyTextureRegion ( &destination, 0, 0, 0, &from, nullptr );
	}

	staging->Unmap ( 0, nullptr );

	D3D12_RESOURCE_BARRIER	barrier { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
	barrier.Transition.pResource = texture.Get ();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmdList->ResourceBarrier ( 1, &barrier );

	if ( FAILED ( cmdList->Close () ) )
		return {};

	ID3D12CommandList* const	lists[] { cmdList.Get () };
	queue->ExecuteCommandLists ( 1, lists );
	waitIdle ();
	return texture;
}
//-----------------------------------------------------------------------------

// Vertices, indices and constants stay in upload memory, the textures are copied to the GPU one by one. The table
// holds the model's three maps, then the panorama
bool reshade::ultra::TestSceneD3d12::upload ( const TestModel& model, const TestSceneSettings& settings )
{
	const auto	hasAllocators = std::ranges::all_of ( allocators, [] ( const auto& a ) { return a != nullptr; } );

	if ( pipeline == nullptr || ! hasAllocators || cmdList == nullptr || fence == nullptr || fenceEvent == nullptr )
		return false;

	const D3D12_HEAP_PROPERTIES	uploadHeap { D3D12_HEAP_TYPE_UPLOAD };

	const auto	makeUploadBuffer = [ & ] ( const void* data, size_t size, Microsoft::WRL::ComPtr<ID3D12Resource>& buffer ) {
		D3D12_RESOURCE_DESC	desc { D3D12_RESOURCE_DIMENSION_BUFFER };
		desc.Width = std::max<size_t> ( size, 1 );
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc = { 1, 0 };
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		void*	mapped = nullptr;

		if ( FAILED ( device->CreateCommittedResource ( &uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS ( &buffer ) ) )
			 || FAILED ( buffer->Map ( 0, nullptr, &mapped ) ) )
			return false;

		std::memcpy ( mapped, data, size );
		buffer->Unmap ( 0, nullptr );
		return true;
	};

	// Constant buffer views start on 256 bytes: the lighting, then the material
	const auto				panorama = makeTestEnvironment ( settings.environment );
	const auto				lightingData = lightingConstants ( panorama, model, settings );
	const auto				material = materialSource ( model, settings );
	std::vector<uint8_t>	constantsData ( 2 * constantsAlignment );
	std::memcpy ( constantsData.data (), &lightingData, sizeof ( lightingData ) );
	std::memcpy ( constantsData.data () + constantsAlignment, &material.constants, sizeof ( material.constants ) );
	indexCount = static_cast<UINT> ( model.indices.size () );

	if ( ! makeUploadBuffer ( model.vertices.data (), model.vertices.size () * sizeof ( TestModelVertex ), vertexBuffer )
		 || ! makeUploadBuffer ( model.indices.data (), model.indices.size () * sizeof ( uint32_t ), indexBuffer )
		 || ! makeUploadBuffer ( constantsData.data (), constantsData.size (), lighting ) )
		return false;

	// The table, then (-constants table) one view per frame in flight
	frameConstantsViews = tableSize;
	const auto	constantsViewCount = constantsPath == TestConstantsPath::table ? frameCount : 0;

	const D3D12_DESCRIPTOR_HEAP_DESC	srvHeapDesc { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, frameConstantsViews + constantsViewCount, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };

	if ( FAILED ( device->CreateDescriptorHeap ( &srvHeapDesc, IID_PPV_ARGS ( &srvHeap ) ) ) )
		return false;

	if ( constantsPath != TestConstantsPath::root )
	{
		const std::vector<uint8_t>	empty ( frameCount * constantsAlignment );

		if ( ! makeUploadBuffer ( empty.data (), empty.size (), frameConstants ) || FAILED ( frameConstants->Map ( 0, nullptr, reinterpret_cast<void**> ( &frameConstantsMapped ) ) ) )
			return false;
	}

	const auto	environmentResource = uploadTexture ( environmentTexture ( panorama ) );

	if ( environmentResource == nullptr )
		return false;

	textures.push_back ( environmentResource );

	const auto	srvSize = device->GetDescriptorHandleIncrementSize ( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );
	auto		srvHandle = srvHeap->GetCPUDescriptorHandleForHeapStart ();

	for ( size_t slot = 0; slot < tableSize; ++slot, srvHandle.ptr += srvSize )
	{
		const auto	texture = slot < textureCount ? uploadTexture ( imageTexture ( material.chains[ slot ], textureFormat ( slot ) ) ) : environmentResource;

		if ( texture == nullptr )
			return false;

		if ( slot < textureCount )
			textures.push_back ( texture );

		device->CreateShaderResourceView ( texture.Get (), nullptr, srvHandle );
	}

	return true;
}
//-----------------------------------------------------------------------------

// The frame slot index is free: the frame that last used it has finished on the GPU
void reshade::ultra::TestSceneD3d12::setFrameConstants ( const TestSceneConstants& data, UINT index )
{
	switch ( constantsPath )
	{
		case TestConstantsPath::root:
			cmdList->SetGraphicsRoot32BitConstants ( 0, sizeof ( data ) / 4, &data, 0 );
			break;
		case TestConstantsPath::cbv:
			std::memcpy ( frameConstantsMapped + index * constantsAlignment, &data, sizeof ( data ) );
			cmdList->SetGraphicsRootConstantBufferView ( 0, frameConstants->GetGPUVirtualAddress () + index * constantsAlignment );
			break;
		// The slot's view is written every frame, as engines do with their per-frame descriptors
		case TestConstantsPath::table:
		{
			std::memcpy ( frameConstantsMapped + index * constantsAlignment, &data, sizeof ( data ) );

			const auto								descriptorSize = device->GetDescriptorHandleIncrementSize ( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );
			const D3D12_CONSTANT_BUFFER_VIEW_DESC	viewDesc { frameConstants->GetGPUVirtualAddress () + index * constantsAlignment, static_cast<UINT> ( constantsAlignment ) };
			auto									cpuHandle = srvHeap->GetCPUDescriptorHandleForHeapStart ();
			auto									gpuHandle = srvHeap->GetGPUDescriptorHandleForHeapStart ();
			cpuHandle.ptr += SIZE_T ( frameConstantsViews + index ) * descriptorSize;
			gpuHandle.ptr += UINT64 ( frameConstantsViews + index ) * descriptorSize;

			device->CreateConstantBufferView ( &viewDesc, cpuHandle );
			cmdList->SetGraphicsRootDescriptorTable ( 0, gpuHandle );
			break;
		}
	}
}
//-----------------------------------------------------------------------------

bool reshade::ultra::TestSceneD3d12::isReady () const
{
	return isUploaded && dsvHeap != nullptr && depth != nullptr && rtvHeap != nullptr && motion != nullptr;
}
//-----------------------------------------------------------------------------

void reshade::ultra::TestSceneD3d12::waitFor ( UINT64 value )
{
	if ( fence->GetCompletedValue () < value && SUCCEEDED ( fence->SetEventOnCompletion ( value, fenceEvent ) ) )
		WaitForSingleObject ( fenceEvent, INFINITE );
}
//-----------------------------------------------------------------------------

void reshade::ultra::TestSceneD3d12::waitIdle ()
{
	if ( fence == nullptr || fenceEvent == nullptr )
		return;

	const auto	value = nextFenceValue++;

	if ( SUCCEEDED ( queue->Signal ( fence.Get (), value ) ) )
		waitFor ( value );
}
//-----------------------------------------------------------------------------

void reshade::ultra::TestSceneD3d12::resize ( UINT newWidth, UINT newHeight )
{
	waitIdle ();
	depth.Reset ();
	motion.Reset ();
	width = newWidth;
	height = newHeight;

	if ( dsvHeap == nullptr || rtvHeap == nullptr || width == 0 || height == 0 )
		return;

	D3D12_RESOURCE_DESC	desc { D3D12_RESOURCE_DIMENSION_TEXTURE2D };
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R32_TYPELESS;
	desc.SampleDesc = { 1, 0 };
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE	clearValue {};
	clearValue.Format = DXGI_FORMAT_D32_FLOAT;
	clearValue.DepthStencil.Depth = 1.0f;

	const D3D12_HEAP_PROPERTIES	props { D3D12_HEAP_TYPE_DEFAULT };

	if ( FAILED ( device->CreateCommittedResource ( &props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS ( &depth ) ) ) )
		return;

	depth->SetName ( widen ( depthName ).c_str () );

	D3D12_DEPTH_STENCIL_VIEW_DESC	dsvDesc {};
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	device->CreateDepthStencilView ( depth.Get (), &dsvDesc, dsvHeap->GetCPUDescriptorHandleForHeapStart () );

	// Stays a render target between frames
	desc.Format = motionFormat;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	clearValue = { motionFormat, {} };

	if ( FAILED ( device->CreateCommittedResource ( &props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue, IID_PPV_ARGS ( &motion ) ) ) )
		return;

	motion->SetName ( widen ( motionName ).c_str () );
	device->CreateRenderTargetView ( motion.Get (), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart () );
}
//-----------------------------------------------------------------------------

bool reshade::ultra::TestSceneD3d12::render ( ID3D12Resource* backBuffer, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv, float seconds )
{
	if ( ! isReady () )
		return false;

	const auto	index = frameIndex;
	waitFor ( fenceValues[ index ] );

	if ( FAILED ( allocators[ index ]->Reset () ) || FAILED ( cmdList->Reset ( allocators[ index ].Get (), pipeline.Get () ) ) )
		return false;

	D3D12_RESOURCE_BARRIER	barrier { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
	barrier.Transition.pResource = backBuffer;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	cmdList->ResourceBarrier ( 1, &barrier );

	const auto							dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart ();
	const auto							motionRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart ();
	const D3D12_CPU_DESCRIPTOR_HANDLE	targets[] { backBufferRtv, motionRtv };
	cmdList->ClearRenderTargetView ( backBufferRtv, background.data (), 0, nullptr );
	cmdList->ClearRenderTargetView ( motionRtv, noMotion, 0, nullptr );
	cmdList->ClearDepthStencilView ( dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr );
	cmdList->OMSetRenderTargets ( UINT ( std::size ( targets ) ), targets, FALSE, &dsv );

	const auto					frameConstantsData = view.nextFrame ( width, height, seconds );
	ID3D12DescriptorHeap* const	heaps[] { srvHeap.Get () };
	cmdList->SetGraphicsRootSignature ( rootSignature.Get () );
	cmdList->SetDescriptorHeaps ( 1, heaps );
	setFrameConstants ( frameConstantsData, index );
	cmdList->SetGraphicsRootConstantBufferView ( 3, lighting->GetGPUVirtualAddress () );

	const D3D12_VIEWPORT	viewport { 0.0f, 0.0f, static_cast<float> ( width ), static_cast<float> ( height ), 0.0f, 1.0f };
	const D3D12_RECT		scissor { 0, 0, static_cast<LONG> ( width ), static_cast<LONG> ( height ) };
	cmdList->RSSetViewports ( 1, &viewport );
	cmdList->RSSetScissorRects ( 1, &scissor );

	const D3D12_VERTEX_BUFFER_VIEW	vertexView { vertexBuffer->GetGPUVirtualAddress (), static_cast<UINT> ( vertexBuffer->GetDesc ().Width ), sizeof ( TestModelVertex ) };
	const D3D12_INDEX_BUFFER_VIEW	indexView { indexBuffer->GetGPUVirtualAddress (), static_cast<UINT> ( indexBuffer->GetDesc ().Width ), DXGI_FORMAT_R32_UINT };
	cmdList->IASetPrimitiveTopology ( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	cmdList->IASetVertexBuffers ( 0, 1, &vertexView );
	cmdList->IASetIndexBuffer ( &indexView );
	cmdList->SetGraphicsRootDescriptorTable ( 1, srvHeap->GetGPUDescriptorHandleForHeapStart () );
	cmdList->SetGraphicsRootConstantBufferView ( 2, lighting->GetGPUVirtualAddress () + constantsAlignment );
	cmdList->DrawIndexedInstanced ( indexCount, 1, 0, 0, 0 );

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	cmdList->ResourceBarrier ( 1, &barrier );

	if ( FAILED ( cmdList->Close () ) )
		return false;

	ID3D12CommandList* const	lists[] { cmdList.Get () };
	queue->ExecuteCommandLists ( 1, lists );

	fenceValues[ index ] = nextFenceValue++;
	queue->Signal ( fence.Get (), fenceValues[ index ] );
	frameIndex = ( index + 1 ) % frameCount;
	return true;
}
//-----------------------------------------------------------------------------

#endif
