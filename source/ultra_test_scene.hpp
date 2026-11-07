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
// vectors. Lit the way Unreal lights a MetaHuman: GGX, two specular lobes and scattering on skin, a panorama's light
// and one soft key light, ACES tone mapping. The cube tumbles, the head turns left and right
namespace reshade::ultra
{
	// head: the built-in scan; model: a head from -model, framed by everything it draws (hair included)
	enum class TestSceneKind { cube, head, model };

	// The swap chain's picture: 8-bit SDR, scRGB (16-bit float, linear, 1.0 = 80 nits) or HDR10 (Rec. 2020 with PQ)
	enum class TestOutput { sdr, scRgb, pq };

	[[ nodiscard ]] DXGI_FORMAT backBufferFormat ( TestOutput output );
	[[ nodiscard ]] DXGI_COLOR_SPACE_TYPE colourSpace ( TestOutput output );

	struct TestSceneSettings
	{
		TestSceneKind			kind = TestSceneKind::cube;
		bool					isJittered = false;		// sub-pixel camera jitter, Halton (2, 3) over 8 frames
		std::filesystem::path	environment;			// -env: a Radiance .hdr panorama; empty: a studio gradient
		bool					isScattering = true;	// -sss: skin scatters light under its surface; off: plain diffuse
		float					fieldOfView = 0.0f;		// -fov: vertical, degrees; 0: 60 for the cube, 22 for the heads
		TestOutput				output = TestOutput::sdr;	// -hdr off|scrgb|pq
		float					paperWhite = 203.0f;	// -paper-white: nits of a white surface in HDR
		float					peak = 1000.0f;			// -peak: nits the HDR highlights roll off towards
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

	// How one part is shaded, the pixel shader's second constant buffer
	struct TestPartConstants
	{
		float		alphaCutoff = 0.0f;			// colour alpha below this is no surface; 0 draws every pixel
		uint32_t	shading = 0;				// the shader's shading values: standard, skin, eye occlusion, tear line, hair
		float		roughness = -1.0f;			// below 0: the specular map's green
		float		specular = -1.0f;			// Unreal's Specular, 0.5 = 4 % reflectance; below 0: the specular map's red
		float		hairRoot[ 4 ] {};			// rgb; a: where along the strand the ombre is complete
		float		hairOmbre[ 4 ] {};			// rgb; a: how far the tips go towards it
		float		hairHighlight[ 4 ] {};		// rgb; a: how much of it where the strips mask is set
	};

	struct TestTextureSource;

	// Where the part's colour goes: written with depth, or blended over what is drawn
	enum class TestBlend : uint32_t { opaque, multiply, add, count };

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
		static constexpr size_t	blendCount = size_t ( TestBlend::count );

		// One part of the model: its indices, colour, normal and specular maps, and how it is shaded
		struct Draw
		{
			UINT												firstIndex = 0;
			UINT												indexCount = 0;
			TestBlend											blend = TestBlend::opaque;
			Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>	textures[ 3 ];
			Microsoft::WRL::ComPtr<ID3D11Buffer>				partConstants;
		};

		[[ nodiscard ]] bool isReady () const;

		ID3D11Device*											device;
		TestSceneView											view;
		std::array<float, 4>									background;
		std::vector<Draw>										draws;
		const TestModel*										animated = nullptr;		// the rigged model, posed each frame; null: still
		std::vector<TestModelVertex>							posed;
		Microsoft::WRL::ComPtr<ID3D11Buffer>					poseBuffers[ 2 ];		// this frame's pose and the one before, alternating
		size_t													poseCount = 0;
		Microsoft::WRL::ComPtr<ID3D11VertexShader>				vs;
		Microsoft::WRL::ComPtr<ID3D11PixelShader>				ps;
		Microsoft::WRL::ComPtr<ID3D11InputLayout>				inputLayout;
		Microsoft::WRL::ComPtr<ID3D11Buffer>					vertexBuffer;
		Microsoft::WRL::ComPtr<ID3D11Buffer>					tangentBuffer;
		Microsoft::WRL::ComPtr<ID3D11Buffer>					indexBuffer;
		Microsoft::WRL::ComPtr<ID3D11Buffer>					constants;
		Microsoft::WRL::ComPtr<ID3D11Buffer>					lighting;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>		environment;
		Microsoft::WRL::ComPtr<ID3D11SamplerState>				sampler;
		Microsoft::WRL::ComPtr<ID3D11RasterizerState>			rasterizer;
		Microsoft::WRL::ComPtr<ID3D11DepthStencilState>			depthStates[ blendCount ];
		Microsoft::WRL::ComPtr<ID3D11BlendState>				blendStates[ blendCount ];
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
		static constexpr size_t	blendCount = size_t ( TestBlend::count );

		// One part of the model: its indices and shading; its table (three maps, then the panorama) sits at 4 * its
		// place in the heap
		struct Draw
		{
			UINT						firstIndex = 0;
			UINT						indexCount = 0;
			TestBlend					blend = TestBlend::opaque;
			D3D12_GPU_VIRTUAL_ADDRESS	partConstants = 0;
		};

		[[ nodiscard ]] bool isReady () const;
		[[ nodiscard ]] bool upload ( const TestModel& model, const TestSceneSettings& settings );
		[[ nodiscard ]] Microsoft::WRL::ComPtr<ID3D12Resource> uploadTexture ( const TestTextureSource& source );
		void waitFor ( UINT64 value );

		// One pose buffer more than frames in flight: the oldest is free once this frame's slot is, the newest but one is
		// the frame before
		static constexpr size_t	poseBufferCount = frameCount + 1;

		ID3D12Device*											device;
		ID3D12CommandQueue*										queue;
		TestSceneView											view;
		std::array<float, 4>									background;
		std::vector<Draw>										draws;
		const TestModel*										animated = nullptr;		// the rigged model, posed each frame; null: still
		std::vector<TestModelVertex>							posed;
		Microsoft::WRL::ComPtr<ID3D12Resource>					poseBuffers[ poseBufferCount ];
		void*													poseMapped[ poseBufferCount ] {};
		size_t													poseCount = 0;
		bool													isUploaded = false;
		Microsoft::WRL::ComPtr<ID3D12RootSignature>				rootSignature;
		Microsoft::WRL::ComPtr<ID3D12PipelineState>				pipelines[ blendCount ];
		Microsoft::WRL::ComPtr<ID3D12CommandAllocator>			allocators[ frameCount ];
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>		cmdList;
		Microsoft::WRL::ComPtr<ID3D12Fence>						fence;
		HANDLE													fenceEvent = nullptr;
		UINT64													fenceValues[ frameCount ] {};
		UINT64													nextFenceValue = 1;
		UINT													frameIndex = 0;
		Microsoft::WRL::ComPtr<ID3D12Resource>					vertexBuffer;
		Microsoft::WRL::ComPtr<ID3D12Resource>					tangentBuffer;
		Microsoft::WRL::ComPtr<ID3D12Resource>					indexBuffer;
		Microsoft::WRL::ComPtr<ID3D12Resource>					lighting;		// the lighting, then each part's constants, 256 bytes apart
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
