/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "ultra_test_model.hpp"

//-----------------------------------------------------------------------------

// The test application's picture: one textured mesh with a depth buffer on a near black background, plus its motion
// vectors. Lit after Unreal: GGX, two specular lobes and scattering on skin, a panorama's light and one soft key
// light, ACES tone mapping. The cube tumbles, the head turns left and right
namespace reshade::ultra
{
	// head: the built-in scan
	enum class TestSceneKind { cube, head };

	// The swap chain's picture: 8-bit SDR, scRGB (16-bit float, linear, 1.0 = 80 nits) or HDR10 (Rec. 2020 with PQ)
	enum class TestOutput { sdr, scRgb, pq };

	[[ nodiscard ]] DXGI_FORMAT backBufferFormat ( TestOutput output );
	[[ nodiscard ]] DXGI_COLOR_SPACE_TYPE colourSpace ( TestOutput output );

	// How D3D12 hands the frame's constants to the shaders: root constants, a root constant buffer view, or a constant
	// buffer view in a descriptor table
	enum class TestConstantsPath { root, cbv, table };

	struct TestSceneSettings
	{
		TestSceneKind			kind = TestSceneKind::cube;
		bool					isJittered = false;		// sub-pixel camera jitter, Halton (2, 3) over 8 frames
		std::filesystem::path	environment;			// -env: a Radiance .hdr panorama; empty: a studio gradient
		bool					isScattering = true;	// -sss: skin scatters light under its surface; off: plain diffuse
		float					fieldOfView = 0.0f;		// -fov: vertical, degrees; 0: 60 for the cube, 22 for the head
		TestOutput				output = TestOutput::sdr;	// -hdr off|scrgb|pq
		float					paperWhite = 203.0f;	// -paper-white: nits of a white surface in HDR
		float					peak = 1000.0f;			// -peak: nits the HDR highlights roll off towards
		TestConstantsPath		constantsPath = TestConstantsPath::root;	// -constants root|cbv|table (D3D12)
	};

	// The shaders' constants, 52 floats: they fit D3D12's root constants next to the texture table
	struct TestSceneConstants
	{
		std::array<float, 16>	worldViewProj {};			// without jitter
		std::array<float, 16>	world {};
		std::array<float, 16>	previousWorldViewProj {};	// the frame before, without jitter
		float					jitterNdc[ 2 ] {};
		float					viewSize[ 2 ] {};
	};

	// How the model's surface is shaded, the pixel shader's second constant buffer
	struct TestMaterialConstants
	{
		uint32_t	shading = 0;				// the shader's shading values: standard, skin
		float		roughness = -1.0f;			// below 0: the specular map's green
	};

	struct TestTextureSource;

	[[ nodiscard ]] TestModel makeTestCube ();

	// Where the model and the camera are in each frame, and the frame before for the motion vectors
	class TestSceneView
	{
	public:
		TestSceneView ( const TestModel& model, const TestSceneSettings& settings );

		[[ nodiscard ]] TestSceneConstants nextFrame ( UINT width, UINT height, float seconds );

	private:
		TestSceneSettings		settings;
		float					center[ 3 ] {};
		float					radius = 1.0f;
		size_t					frameNumber = 0;
		std::array<float, 16>	previous {};
	};

	class TestSceneD3d11
	{
	public:
		TestSceneD3d11 ( ID3D11Device* device, const TestModel& model, const TestSceneSettings& settings );

		void resize ( ID3D11Texture2D* backBuffer );
		void render ( ID3D11DeviceContext* context, ID3D11RenderTargetView* backBufferRtv, float seconds );

	private:
		[[ nodiscard ]] bool isReady () const;

		ID3D11Device*											device;
		TestSceneView											view;
		std::array<float, 4>									background;
		UINT													indexCount = 0;
		Microsoft::WRL::ComPtr<ID3D11VertexShader>				vs;
		Microsoft::WRL::ComPtr<ID3D11PixelShader>				ps;
		Microsoft::WRL::ComPtr<ID3D11InputLayout>				inputLayout;
		Microsoft::WRL::ComPtr<ID3D11Buffer>					vertexBuffer;
		Microsoft::WRL::ComPtr<ID3D11Buffer>					indexBuffer;
		Microsoft::WRL::ComPtr<ID3D11Buffer>					constants;
		Microsoft::WRL::ComPtr<ID3D11Buffer>					lighting;
		Microsoft::WRL::ComPtr<ID3D11Buffer>					materialConstants;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>		textures[ 3 ];			// colour, normal and specular maps
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>		environment;
		Microsoft::WRL::ComPtr<ID3D11SamplerState>				sampler;
		Microsoft::WRL::ComPtr<ID3D11RasterizerState>			rasterizer;
		Microsoft::WRL::ComPtr<ID3D11DepthStencilState>			depthState;
		Microsoft::WRL::ComPtr<ID3D11Texture2D>					depth;
		Microsoft::WRL::ComPtr<ID3D11DepthStencilView>			dsv;
		Microsoft::WRL::ComPtr<ID3D11Texture2D>					motion;
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView>			motionRtv;
		UINT													width = 0;
		UINT													height = 0;
	};

	class TestSceneD3d12
	{
	public:
		TestSceneD3d12 ( ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT backBufferFormat, const TestModel& model, const TestSceneSettings& settings );
		~TestSceneD3d12 ();

		void waitIdle ();
		void resize ( UINT newWidth, UINT newHeight );

		// false: nothing went out, the caller presents its own frame
		[[ nodiscard ]] bool render ( ID3D12Resource* backBuffer, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv, float seconds );

	private:
		static constexpr UINT	frameCount = 3;

		[[ nodiscard ]] bool isReady () const;
		[[ nodiscard ]] bool upload ( const TestModel& model, const TestSceneSettings& settings );
		[[ nodiscard ]] Microsoft::WRL::ComPtr<ID3D12Resource> uploadTexture ( const TestTextureSource& source );
		void setFrameConstants ( const TestSceneConstants& data, UINT index );
		void waitFor ( UINT64 value );

		ID3D12Device*											device;
		ID3D12CommandQueue*										queue;
		TestSceneView											view;
		std::array<float, 4>									background;
		UINT													indexCount = 0;
		bool													isUploaded = false;
		Microsoft::WRL::ComPtr<ID3D12RootSignature>				rootSignature;
		Microsoft::WRL::ComPtr<ID3D12PipelineState>				pipeline;
		Microsoft::WRL::ComPtr<ID3D12CommandAllocator>			allocators[ frameCount ];
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>		cmdList;
		Microsoft::WRL::ComPtr<ID3D12Fence>						fence;
		HANDLE													fenceEvent = nullptr;
		UINT64													fenceValues[ frameCount ] {};
		UINT64													nextFenceValue = 1;
		UINT													frameIndex = 0;
		Microsoft::WRL::ComPtr<ID3D12Resource>					vertexBuffer;
		Microsoft::WRL::ComPtr<ID3D12Resource>					indexBuffer;
		Microsoft::WRL::ComPtr<ID3D12Resource>					lighting;		// the lighting, then the material's constants, 256 bytes apart
		TestConstantsPath										constantsPath;
		Microsoft::WRL::ComPtr<ID3D12Resource>					frameConstants;	// cbv and table: one 256-byte slot per frame in flight
		uint8_t*												frameConstantsMapped = nullptr;
		UINT													frameConstantsViews = 0;	// table: the heap index of the first slot's view
		std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>	textures;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>			srvHeap;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>			dsvHeap;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>			rtvHeap;
		Microsoft::WRL::ComPtr<ID3D12Resource>					depth;
		Microsoft::WRL::ComPtr<ID3D12Resource>					motion;
		UINT													width = 0;
		UINT													height = 0;
	};
}
