// ReShadeDrawBench64.exe / ReShadeDrawBench32.exe: a draw-heavy D3D11 / D3D12 frame loop that links no ReShade code,
// so any ReShade DLL (official or fork) placed next to it as dxgi.dll is measured the way a game loads it (the present
// route's proxy). Without a dxgi.dll next to it, it measures the bare API.
// Every draw does what a game's draw stream does: a pipeline bind, a constant update, a descriptor bind and an indexed draw.
// Command line: -d3d11 (default) | -d3d12, -draws <n> (5000), -targets <k> render target switches per frame (0),
// -frames <n> measured frames (600), -warmup <n> (300), -width/-height, -out <file> (results line, appended),
// -label <text>, -background, -owner <name> (as the test app, for Tools/test-run.sh)
// Result: CPU ms per frame for recording the draws (submit), for Present (present: ReShade's runtime work sits here)
// and frame to frame (frame), each as average and 99th percentile.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
	template <typename T>
	struct Com
	{
		T *p = nullptr;
		Com() = default;
		Com(Com &&other) noexcept : p(other.p) { other.p = nullptr; }
		Com(const Com &) = delete;
		Com &operator=(const Com &) = delete;
		~Com() { if (p != nullptr) p->Release(); }
		T *operator->() const { return p; }
		T **operator&() { return &p; }
		T *get() const { return p; }
	};

	struct Options
	{
		bool d3d12 = false;
		int draws = 5000;
		int targets = 0;
		int frames = 600;
		int warmup = 300;
		int width = 1280;
		int height = 720;
		bool background = false;
		std::string owner;
		std::string label;
		std::string out;
	};

	struct Sample
	{
		double submit = 0.0;
		double present = 0.0;
		double frame = 0.0;
	};

	bool s_background = false;
	bool s_quit = false;

	const char s_shader[] = R"(
cbuffer PerDraw : register(b0) { float4 offset_scale; float4 colour; };
StructuredBuffer<float4> tint : register(t0);
struct VsOut { float4 pos : SV_Position; float4 col : COLOR; };
VsOut vs_main(float3 pos : POSITION)
{
	VsOut o;
	o.pos = float4(pos.xy * offset_scale.z + offset_scale.xy, pos.z * 0.25 + 0.5, 1.0);
	o.col = colour;
	return o;
}
float4 ps_main(VsOut i) : SV_Target { return i.col * tint[0]; }
float4 ps_alt(VsOut i) : SV_Target { return i.col.bgra * tint[0]; }
)";

	const float s_cube_vertices[] = {
		-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1, -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1 };
	const unsigned short s_cube_indices[] = {
		0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1, 3, 2, 6, 3, 6, 7, 1, 5, 6, 1, 6, 2, 0, 3, 7, 0, 7, 4 };

	struct PerDraw
	{
		float offset_scale[4];
		float colour[4];
		float pad[56]; // 256 bytes, D3D12's constant buffer alignment
	};

	void fillPerDraw(PerDraw &d, int i, int n)
	{
		const int side = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(n))));
		const float cell = 2.0f / side;
		d.offset_scale[0] = -1.0f + cell * (static_cast<float>(i % side) + 0.5f);
		d.offset_scale[1] = -1.0f + cell * (static_cast<float>((i / side) % side) + 0.5f);
		d.offset_scale[2] = cell * 0.3f;
		d.offset_scale[3] = 0.0f;
		d.colour[0] = static_cast<float>(i & 255) / 255.0f;
		d.colour[1] = 0.5f;
		d.colour[2] = 1.0f - d.colour[0];
		d.colour[3] = 1.0f;
	}

	Options parseOptions()
	{
		Options o;
		int argc = 0;
		LPWSTR *const argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		const auto narrow = [](const wchar_t *w) {
			std::string s;
			for (; *w != 0; ++w)
				s += static_cast<char>(*w);
			return s;
		};
		for (int i = 1; i < argc; ++i)
		{
			const std::wstring a = argv[i];
			const bool has_value = i + 1 < argc;
			if (a == L"-d3d12")
				o.d3d12 = true;
			else if (a == L"-d3d11")
				o.d3d12 = false;
			else if (a == L"-background")
				o.background = true;
			else if (a == L"-draws" && has_value)
				o.draws = _wtoi(argv[++i]);
			else if (a == L"-targets" && has_value)
				o.targets = _wtoi(argv[++i]);
			else if (a == L"-frames" && has_value)
				o.frames = _wtoi(argv[++i]);
			else if (a == L"-warmup" && has_value)
				o.warmup = _wtoi(argv[++i]);
			else if (a == L"-width" && has_value)
				o.width = _wtoi(argv[++i]);
			else if (a == L"-height" && has_value)
				o.height = _wtoi(argv[++i]);
			else if (a == L"-owner" && has_value)
				o.owner = narrow(argv[++i]);
			else if (a == L"-label" && has_value)
				o.label = narrow(argv[++i]);
			else if (a == L"-out" && has_value)
				o.out = narrow(argv[++i]);
		}
		LocalFree(argv);
		o.draws = std::max(1, o.draws);
		return o;
	}

	LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		switch (msg)
		{
		case WM_WINDOWPOSCHANGING:
			if (s_background)
			{
				WINDOWPOS *const pos = reinterpret_cast<WINDOWPOS *>(lparam);
				pos->hwndInsertAfter = HWND_BOTTOM;
				pos->flags = (pos->flags & ~SWP_NOZORDER) | SWP_NOACTIVATE;
			}
			break;
		case WM_MOUSEACTIVATE:
			if (s_background)
				return MA_NOACTIVATE;
			break;
		case WM_DESTROY:
			s_quit = true;
			PostQuitMessage(0);
			break;
		}
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}

	bool pumpMessages()
	{
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
				return false;
			DispatchMessageW(&msg);
		}
		return !s_quit;
	}

	double nowMs()
	{
		static const double s_scale = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 / static_cast<double>(f.QuadPart); }();
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return static_cast<double>(t.QuadPart) * s_scale;
	}

	Com<ID3DBlob> compile(const char *entry, const char *target)
	{
		Com<ID3DBlob> code, errors;
		D3DCompile(s_shader, sizeof(s_shader) - 1, "bench", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (errors.get() != nullptr)
			OutputDebugStringA(static_cast<const char *>(errors->GetBufferPointer()));
		return code;
	}

	bool tearingSupported(IDXGIFactory2 *factory)
	{
		Com<IDXGIFactory5> f5;
		BOOL allow = FALSE;
		if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f5))))
			f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow));
		return allow != FALSE;
	}

	// Runs the frame loop, timing each frame's parts, until warmup + frames are done
	template <typename Record, typename Present>
	std::vector<Sample> runFrames(const Options &o, Record &&record, Present &&present)
	{
		std::vector<Sample> samples;
		samples.reserve(o.frames);
		double last = nowMs();
		for (int frame = 0; frame < o.warmup + o.frames; ++frame)
		{
			if (!pumpMessages())
				break;
			const double t0 = nowMs();
			record(frame);
			const double t1 = nowMs();
			present();
			const double t2 = nowMs();
			if (frame >= o.warmup)
				samples.push_back({ t1 - t0, t2 - t1, t2 - last });
			last = t2;
		}
		return samples;
	}

	int runD3D11(const Options &o, HWND hwnd, std::vector<Sample> &samples)
	{
		Com<ID3D11Device> device;
		Com<ID3D11DeviceContext> ctx;
		if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx)))
			return 2;

		Com<IDXGIDevice> dxgi_device;
		Com<IDXGIAdapter> adapter;
		Com<IDXGIFactory2> factory;
		device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
		dxgi_device->GetAdapter(&adapter);
		adapter->GetParent(IID_PPV_ARGS(&factory));
		const bool tearing = tearingSupported(factory.get());

		DXGI_SWAP_CHAIN_DESC1 scd = {};
		scd.Width = o.width;
		scd.Height = o.height;
		scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		scd.SampleDesc.Count = 1;
		scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		scd.BufferCount = 2;
		scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		scd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
		Com<IDXGISwapChain1> swapchain;
		if (FAILED(factory->CreateSwapChainForHwnd(device.get(), hwnd, &scd, nullptr, nullptr, &swapchain)))
			return 3;

		Com<ID3D11Texture2D> back_buffer;
		swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
		Com<ID3D11RenderTargetView> back_rtv;
		device->CreateRenderTargetView(back_buffer.get(), nullptr, &back_rtv);

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = o.width;
		td.Height = o.height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.SampleDesc.Count = 1;
		td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		Com<ID3D11Texture2D> offscreen;
		device->CreateTexture2D(&td, nullptr, &offscreen);
		Com<ID3D11RenderTargetView> offscreen_rtv;
		device->CreateRenderTargetView(offscreen.get(), nullptr, &offscreen_rtv);
		td.Format = DXGI_FORMAT_D32_FLOAT;
		td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		Com<ID3D11Texture2D> depth;
		device->CreateTexture2D(&td, nullptr, &depth);
		Com<ID3D11DepthStencilView> dsv;
		device->CreateDepthStencilView(depth.get(), nullptr, &dsv);

		const Com<ID3DBlob> vs_code = compile("vs_main", "vs_5_0");
		const Com<ID3DBlob> ps_code = compile("ps_main", "ps_5_0");
		const Com<ID3DBlob> ps_alt_code = compile("ps_alt", "ps_5_0");
		if (vs_code.get() == nullptr || ps_code.get() == nullptr || ps_alt_code.get() == nullptr)
			return 4;
		Com<ID3D11VertexShader> vs;
		Com<ID3D11PixelShader> ps[2];
		device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs);
		device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps[0]);
		device->CreatePixelShader(ps_alt_code->GetBufferPointer(), ps_alt_code->GetBufferSize(), nullptr, &ps[1]);
		const D3D11_INPUT_ELEMENT_DESC layout_desc = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
		Com<ID3D11InputLayout> layout;
		device->CreateInputLayout(&layout_desc, 1, vs_code->GetBufferPointer(), vs_code->GetBufferSize(), &layout);

		const auto make_buffer = [&](UINT bind, const void *data, UINT size, ID3D11Buffer **buffer) {
			D3D11_BUFFER_DESC bd = { size, D3D11_USAGE_IMMUTABLE, bind, 0, 0, 0 };
			const D3D11_SUBRESOURCE_DATA init = { data, 0, 0 };
			device->CreateBuffer(&bd, &init, buffer);
		};
		Com<ID3D11Buffer> vb, ib, tint;
		make_buffer(D3D11_BIND_VERTEX_BUFFER, s_cube_vertices, sizeof(s_cube_vertices), &vb);
		make_buffer(D3D11_BIND_INDEX_BUFFER, s_cube_indices, sizeof(s_cube_indices), &ib);
		{
			const float one[4] = { 1, 1, 1, 1 };
			D3D11_BUFFER_DESC bd = { sizeof(one), D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(one) };
			const D3D11_SUBRESOURCE_DATA init = { one, 0, 0 };
			device->CreateBuffer(&bd, &init, &tint);
		}
		Com<ID3D11ShaderResourceView> tint_srv;
		device->CreateShaderResourceView(tint.get(), nullptr, &tint_srv);
		Com<ID3D11Buffer> cb;
		{
			D3D11_BUFFER_DESC bd = { 32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			device->CreateBuffer(&bd, nullptr, &cb);
		}

		const D3D11_VIEWPORT viewport = { 0, 0, static_cast<float>(o.width), static_cast<float>(o.height), 0, 1 };
		const UINT stride = 12, offset = 0;
		const float clear[4] = { 0.2f, 0.2f, 0.25f, 1.0f };
		const int switch_every = o.targets > 0 ? std::max(1, o.draws / o.targets) : 0;

		samples = runFrames(o,
			[&](int) {
				ctx->ClearRenderTargetView(back_rtv.get(), clear);
				ctx->ClearRenderTargetView(offscreen_rtv.get(), clear);
				ctx->ClearDepthStencilView(dsv.get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
				ID3D11RenderTargetView *rtv = back_rtv.get();
				ctx->OMSetRenderTargets(1, &rtv, dsv.get());
				ctx->RSSetViewports(1, &viewport);
				ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				ctx->IASetInputLayout(layout.get());
				ID3D11Buffer *const vbs[] = { vb.get() };
				ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
				ctx->IASetIndexBuffer(ib.get(), DXGI_FORMAT_R16_UINT, 0);
				PerDraw d = {};
				int target = 0;
				for (int i = 0; i < o.draws; ++i)
				{
					if (switch_every != 0 && i != 0 && i % switch_every == 0)
					{
						target ^= 1;
						rtv = target != 0 ? offscreen_rtv.get() : back_rtv.get();
						ctx->OMSetRenderTargets(1, &rtv, dsv.get());
					}
					ctx->VSSetShader(vs.get(), nullptr, 0);
					ctx->PSSetShader(ps[i & 1].get(), nullptr, 0);
					fillPerDraw(d, i, o.draws);
					D3D11_MAPPED_SUBRESOURCE mapped;
					if (SUCCEEDED(ctx->Map(cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
					{
						std::memcpy(mapped.pData, &d, 32);
						ctx->Unmap(cb.get(), 0);
					}
					ID3D11Buffer *const cbs[] = { cb.get() };
					ctx->VSSetConstantBuffers(0, 1, cbs);
					ID3D11ShaderResourceView *const srvs[] = { tint_srv.get() };
					ctx->PSSetShaderResources(0, 1, srvs);
					ctx->DrawIndexed(36, 0, 0);
				}
			},
			[&] { swapchain->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0); });
		return 0;
	}

	int runD3D12(const Options &o, HWND hwnd, std::vector<Sample> &samples)
	{
		constexpr UINT frame_count = 2;

		Com<IDXGIFactory2> factory;
		if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
			return 2;
		Com<ID3D12Device> device;
		if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
			return 2;
		const bool tearing = tearingSupported(factory.get());

		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		Com<ID3D12CommandQueue> queue;
		device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

		DXGI_SWAP_CHAIN_DESC1 scd = {};
		scd.Width = o.width;
		scd.Height = o.height;
		scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		scd.SampleDesc.Count = 1;
		scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		scd.BufferCount = frame_count;
		scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		scd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
		Com<IDXGISwapChain1> swapchain1;
		if (FAILED(factory->CreateSwapChainForHwnd(queue.get(), hwnd, &scd, nullptr, nullptr, &swapchain1)))
			return 3;
		Com<IDXGISwapChain3> swapchain;
		swapchain1->QueryInterface(IID_PPV_ARGS(&swapchain));

		const auto heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool visible, ID3D12DescriptorHeap **out) {
			D3D12_DESCRIPTOR_HEAP_DESC hd = { type, count, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
			device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(out));
		};
		Com<ID3D12DescriptorHeap> rtv_heap, dsv_heap, srv_heap;
		heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, frame_count + 1, false, &rtv_heap);
		heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false, &dsv_heap);
		heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true, &srv_heap);
		const UINT rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		const auto rtv_handle = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * rtv_size; return h; };

		Com<ID3D12Resource> back_buffers[frame_count];
		for (UINT i = 0; i < frame_count; ++i)
		{
			swapchain->GetBuffer(i, IID_PPV_ARGS(&back_buffers[i]));
			device->CreateRenderTargetView(back_buffers[i].get(), nullptr, rtv_handle(i));
		}

		const auto texture = [&](DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, ID3D12Resource **out) {
			const D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
			D3D12_RESOURCE_DESC rd = {};
			rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			rd.Width = o.width;
			rd.Height = o.height;
			rd.DepthOrArraySize = 1;
			rd.MipLevels = 1;
			rd.Format = format;
			rd.SampleDesc.Count = 1;
			rd.Flags = flags;
			device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, clear, IID_PPV_ARGS(out));
		};
		const float clear[4] = { 0.2f, 0.2f, 0.25f, 1.0f };
		D3D12_CLEAR_VALUE offscreen_clear = { DXGI_FORMAT_R16G16B16A16_FLOAT, { { clear[0], clear[1], clear[2], clear[3] } } };
		Com<ID3D12Resource> offscreen;
		texture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &offscreen_clear, &offscreen);
		device->CreateRenderTargetView(offscreen.get(), nullptr, rtv_handle(frame_count));
		D3D12_CLEAR_VALUE depth_clear = {};
		depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
		depth_clear.DepthStencil.Depth = 1.0f;
		Com<ID3D12Resource> depth;
		texture(DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, &depth);
		const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
		device->CreateDepthStencilView(depth.get(), nullptr, dsv);

		const auto upload = [&](UINT64 size, ID3D12Resource **out) {
			const D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
			D3D12_RESOURCE_DESC rd = {};
			rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			rd.Width = size;
			rd.Height = 1;
			rd.DepthOrArraySize = 1;
			rd.MipLevels = 1;
			rd.SampleDesc.Count = 1;
			rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(out));
		};
		const auto fill = [](ID3D12Resource *r, const void *data, size_t size) {
			void *p = nullptr;
			r->Map(0, nullptr, &p);
			std::memcpy(p, data, size);
			r->Unmap(0, nullptr);
		};
		Com<ID3D12Resource> vb, ib, tint, cbs;
		upload(sizeof(s_cube_vertices), &vb);
		fill(vb.get(), s_cube_vertices, sizeof(s_cube_vertices));
		upload(sizeof(s_cube_indices), &ib);
		fill(ib.get(), s_cube_indices, sizeof(s_cube_indices));
		const float one[4] = { 1, 1, 1, 1 };
		upload(sizeof(one), &tint);
		fill(tint.get(), one, sizeof(one));
		const UINT64 cb_frame_size = static_cast<UINT64>(o.draws) * sizeof(PerDraw);
		upload(cb_frame_size * frame_count, &cbs);
		PerDraw *cb_data = nullptr;
		cbs->Map(0, nullptr, reinterpret_cast<void **>(&cb_data));

		{
			D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = DXGI_FORMAT_UNKNOWN;
			sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			sd.Buffer.NumElements = 1;
			sd.Buffer.StructureByteStride = sizeof(one);
			device->CreateShaderResourceView(tint.get(), &sd, srv_heap->GetCPUDescriptorHandleForHeapStart());
		}

		Com<ID3D12RootSignature> root_signature;
		{
			const D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
			D3D12_ROOT_PARAMETER params[2] = {};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[1].DescriptorTable = { 1, &range };
			params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			const D3D12_ROOT_SIGNATURE_DESC rsd = { 2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
			Com<ID3DBlob> blob, errors;
			D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
			device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_signature));
		}

		const Com<ID3DBlob> vs_code = compile("vs_main", "vs_5_1");
		const Com<ID3DBlob> ps_code = compile("ps_main", "ps_5_1");
		const Com<ID3DBlob> ps_alt_code = compile("ps_alt", "ps_5_1");
		if (vs_code.get() == nullptr || ps_code.get() == nullptr || ps_alt_code.get() == nullptr)
			return 4;
		const D3D12_INPUT_ELEMENT_DESC layout_desc = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
		Com<ID3D12PipelineState> psos[2][2]; // [pixel shader][target: back buffer, offscreen]
		for (int p = 0; p < 2; ++p)
			for (int t = 0; t < 2; ++t)
			{
				const ID3DBlob *const ps_blob = p == 0 ? ps_code.get() : ps_alt_code.get();
				D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
				pd.pRootSignature = root_signature.get();
				pd.VS = { vs_code->GetBufferPointer(), vs_code->GetBufferSize() };
				pd.PS = { const_cast<ID3DBlob *>(ps_blob)->GetBufferPointer(), const_cast<ID3DBlob *>(ps_blob)->GetBufferSize() };
				pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
				pd.SampleMask = UINT_MAX;
				pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
				pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
				pd.RasterizerState.DepthClipEnable = TRUE;
				pd.DepthStencilState.DepthEnable = TRUE;
				pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
				pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
				pd.InputLayout = { &layout_desc, 1 };
				pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
				pd.NumRenderTargets = 1;
				pd.RTVFormats[0] = t == 0 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
				pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
				pd.SampleDesc.Count = 1;
				if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psos[p][t]))))
					return 4;
			}

		Com<ID3D12CommandAllocator> allocators[frame_count];
		for (UINT i = 0; i < frame_count; ++i)
			device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i]));
		Com<ID3D12GraphicsCommandList> list;
		device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].get(), nullptr, IID_PPV_ARGS(&list));
		list->Close();
		Com<ID3D12Fence> fence;
		device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
		const HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		UINT64 fence_values[frame_count] = {};
		UINT64 next_fence = 1;

		const D3D12_VIEWPORT viewport = { 0, 0, static_cast<float>(o.width), static_cast<float>(o.height), 0, 1 };
		const D3D12_RECT scissor = { 0, 0, o.width, o.height };
		const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGPUVirtualAddress(), sizeof(s_cube_vertices), 12 };
		const D3D12_INDEX_BUFFER_VIEW ibv = { ib->GetGPUVirtualAddress(), sizeof(s_cube_indices), DXGI_FORMAT_R16_UINT };
		const int switch_every = o.targets > 0 ? std::max(1, o.draws / o.targets) : 0;
		const auto barrier = [&](ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
			list->ResourceBarrier(1, &b);
		};

		samples = runFrames(o,
			[&](int) {
				const UINT index = swapchain->GetCurrentBackBufferIndex();
				if (fence->GetCompletedValue() < fence_values[index])
				{
					fence->SetEventOnCompletion(fence_values[index], fence_event);
					WaitForSingleObject(fence_event, INFINITE);
				}
				allocators[index]->Reset();
				list->Reset(allocators[index].get(), nullptr);
				barrier(back_buffers[index].get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
				list->ClearRenderTargetView(rtv_handle(index), clear, 0, nullptr);
				list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
				D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(index);
				list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
				list->SetGraphicsRootSignature(root_signature.get());
				ID3D12DescriptorHeap *const heaps[] = { srv_heap.get() };
				list->SetDescriptorHeaps(1, heaps);
				list->RSSetViewports(1, &viewport);
				list->RSSetScissorRects(1, &scissor);
				list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				list->IASetVertexBuffers(0, 1, &vbv);
				list->IASetIndexBuffer(&ibv);
				PerDraw *const frame_cbs = cb_data + static_cast<size_t>(index) * o.draws;
				const D3D12_GPU_VIRTUAL_ADDRESS frame_cb_address = cbs->GetGPUVirtualAddress() + index * cb_frame_size;
				int target = 0;
				for (int i = 0; i < o.draws; ++i)
				{
					if (switch_every != 0 && i != 0 && i % switch_every == 0)
					{
						target ^= 1;
						if (target != 0)
						{
							barrier(offscreen.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
							rtv = rtv_handle(frame_count);
						}
						else
						{
							barrier(offscreen.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
							rtv = rtv_handle(index);
						}
						list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
					}
					list->SetPipelineState(psos[i & 1][target].get());
					fillPerDraw(frame_cbs[i], i, o.draws);
					list->SetGraphicsRootConstantBufferView(0, frame_cb_address + static_cast<UINT64>(i) * sizeof(PerDraw));
					list->SetGraphicsRootDescriptorTable(1, srv_heap->GetGPUDescriptorHandleForHeapStart());
					list->DrawIndexedInstanced(36, 1, 0, 0, 0);
				}
				if (target != 0)
					barrier(offscreen.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				barrier(back_buffers[index].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
				list->Close();
				ID3D12CommandList *const lists[] = { list.get() };
				queue->ExecuteCommandLists(1, lists);
			},
			[&] {
				const UINT index = swapchain->GetCurrentBackBufferIndex();
				swapchain->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
				queue->Signal(fence.get(), next_fence);
				fence_values[index] = next_fence++;
			});

		queue->Signal(fence.get(), next_fence);
		fence->SetEventOnCompletion(next_fence, fence_event);
		WaitForSingleObject(fence_event, INFINITE);
		CloseHandle(fence_event);
		return 0;
	}

	void stats(std::vector<double> values, double &average, double &p99)
	{
		average = 0.0;
		p99 = 0.0;
		if (values.empty())
			return;
		for (const double v : values)
			average += v;
		average /= static_cast<double>(values.size());
		std::ranges::sort(values);
		p99 = values[std::min(values.size() - 1, values.size() * 99 / 100)];
	}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
	const Options o = parseOptions();
	s_background = o.background;

	WNDCLASSW wc = {};
	wc.lpfnWndProc = windowProc;
	wc.hInstance = instance;
	wc.lpszClassName = L"ReShadeDrawBench";
	RegisterClassW(&wc);
	RECT rect = { 0, 0, o.width, o.height };
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	const std::wstring title = L"ReShade draw bench" + (o.owner.empty() ? std::wstring() : L": " + std::wstring(o.owner.begin(), o.owner.end()));
	const HWND hwnd = CreateWindowExW(o.background ? WS_EX_NOACTIVATE : 0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
	if (hwnd == nullptr)
		return 1;
	ShowWindow(hwnd, SW_SHOWNOACTIVATE);
	if (o.background)
		SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

	std::vector<Sample> samples;
	const int code = o.d3d12 ? runD3D12(o, hwnd, samples) : runD3D11(o, hwnd, samples);
	DestroyWindow(hwnd);
	if (code != 0)
		return code;

	std::vector<double> submit, present, frame;
	for (const Sample &s : samples)
	{
		submit.push_back(s.submit);
		present.push_back(s.present);
		frame.push_back(s.frame);
	}
	double submit_avg, submit_p99, present_avg, present_p99, frame_avg, frame_p99;
	stats(submit, submit_avg, submit_p99);
	stats(present, present_avg, present_p99);
	stats(frame, frame_avg, frame_p99);

	const bool reshade = GetModuleHandleW(L"dxgi.dll") != nullptr && [] {
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(GetModuleHandleW(L"dxgi.dll"), path, MAX_PATH);
		return std::wcsstr(path, L"ystem32") == nullptr && std::wcsstr(path, L"YSTEM32") == nullptr;
	}();
	char line[512];
	std::snprintf(line, sizeof(line), "%s %s draws %d targets %d frames %zu proxy %s | submit %.3f / %.3f | present %.3f / %.3f | frame %.3f / %.3f ms (avg / p99)\n",
		o.label.empty() ? "-" : o.label.c_str(), o.d3d12 ? "d3d12" : "d3d11", o.draws, o.targets, samples.size(), reshade ? "yes" : "no",
		submit_avg, submit_p99, present_avg, present_p99, frame_avg, frame_p99);
	if (!o.out.empty())
		if (FILE *f = nullptr; fopen_s(&f, o.out.c_str(), "a") == 0)
		{
			std::fputs(line, f);
			std::fclose(f);
		}
	std::fputs(line, stdout);
	return samples.size() == static_cast<size_t>(o.frames) ? 0 : 5;
}
