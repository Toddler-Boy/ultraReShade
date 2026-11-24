/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <reshade_api_format.hpp>
#include <reshade_ultra_host_names.h>

#include "com_ptr.hpp"
#include "com_utils.hpp"
#include "d3d11_impl_state_block.hpp"
#include "dll_log.hpp"
#include "dll_resources.hpp"
#include "ultra_d3d11_float_clone.hpp"
#include "ultra_float_clone.hpp"

//-----------------------------------------------------------------------------

namespace reshade::ultra::d3d11::floatClone
{
	namespace
	{
		namespace host = reshade::host;

		// A tagged texture, the game's own and untouched, with its clone once it has one
		struct Tagged
		{
			ID3D11Device*				device = nullptr;
			ID3D11Texture2D*			game = nullptr;
			D3D11_TEXTURE2D_DESC		desc {};
			com_ptr<ID3D11Texture2D>	clone;
			bool						isFailed = false;
		};

		// A view the game made of a tagged texture, with the same view of the clone once the texture has one
		struct Viewed
		{
			ID3D11View*			game = nullptr;
			Tagged*				texture = nullptr;
			ViewKind			kind = ViewKind::shaderResource;
			com_ptr<ID3D11View>	clone;
			bool				isFailed = false;
		};

		// Lives as long as the process: a destruction callback can come after everything else is gone
		struct Tables
		{
			std::shared_mutex											lock;
			std::unordered_map<const void*, std::unique_ptr<Tagged>>	textures;
			std::unordered_map<const void*, Viewed>						views;
			std::unordered_map<const void*, ID3D11View*>				cloneViews;		// a clone's view to the game's
			std::unordered_map<const void*, Tagged*>					clones;			// a clone to its tagged texture
		};

		[[ nodiscard ]] Tables& tables ()
		{
			static auto*	t = new Tables;
			return *t;
		}

		// Any change of what a bind finds: the contexts' memory of their binds is over when it moves
		std::atomic<uint32_t>	tableGeneration { 1 };

		//---------------------------------------------------------------------
		// The pixel shaders of the rule: the pointers bound are found without a lock

		constexpr size_t	triggerSlots = 256;

		std::array<std::atomic<uintptr_t>, triggerSlots>	triggerPointers {};		// 0 free, 1 removed
		std::array<std::atomic<uint32_t>, triggerSlots>		triggerHashes {};
		std::atomic<size_t>									triggerCount { 0 };
		std::mutex											triggerLock;
		std::unordered_map<const void*, uint32_t>			pixelHashes;			// every pixel shader made while the rule had formats

		[[ nodiscard ]] size_t slotOf ( uintptr_t pointer )
		{
			return size_t ( ( pointer >> 4 ) * 0x9E3779B97F4A7C15ull >> 32 ) & ( triggerSlots - 1 );
		}

		// Under triggerLock
		void addTrigger ( const void* shader, uint32_t hash )
		{
			const auto	key = uintptr_t ( shader );
			auto		free = triggerSlots;
			for ( size_t i = 0, at = slotOf ( key ); i < triggerSlots; ++i, at = ( at + 1 ) & ( triggerSlots - 1 ) )
			{
				const auto	p = triggerPointers[ at ].load ( std::memory_order_relaxed );
				if ( p == key )
					return;
				if ( p <= 1 && free == triggerSlots )
					free = at;
				if ( p == 0 )
					break;
			}
			if ( free == triggerSlots )
			{
				reshade::log::message ( reshade::log::level::warning, "Float targets: more than %zu trigger pixel shaders, the rest does not make clones", triggerSlots );
				return;
			}
			triggerHashes[ free ].store ( hash, std::memory_order_relaxed );
			triggerPointers[ free ].store ( key, std::memory_order_release );
			triggerCount.fetch_add ( 1, std::memory_order_relaxed );
		}

		void removeTrigger ( const void* shader )
		{
			const auto	key = uintptr_t ( shader );
			for ( size_t i = 0, at = slotOf ( key ); i < triggerSlots; ++i, at = ( at + 1 ) & ( triggerSlots - 1 ) )
			{
				const auto	p = triggerPointers[ at ].load ( std::memory_order_relaxed );
				if ( p == key )
				{
					triggerPointers[ at ].store ( 1, std::memory_order_release );
					triggerCount.fetch_sub ( 1, std::memory_order_relaxed );
					return;
				}
				if ( p == 0 )
					return;
			}
		}

		void forgetPixelShader ( const void* shader )
		{
			std::scoped_lock	lock ( triggerLock );
			pixelHashes.erase ( shader );
			removeTrigger ( shader );
		}

		[[ nodiscard ]] const void* triggerOf ( const void* shader )
		{
			if ( shader == nullptr || triggerCount.load ( std::memory_order_relaxed ) == 0 )
				return nullptr;

			const auto	key = uintptr_t ( shader );
			for ( size_t i = 0, at = slotOf ( key ); i < triggerSlots; ++i, at = ( at + 1 ) & ( triggerSlots - 1 ) )
			{
				const auto	p = triggerPointers[ at ].load ( std::memory_order_acquire );
				if ( p == key )
					return shader;
				if ( p == 0 )
					return nullptr;
			}
			return nullptr;
		}

		[[ nodiscard ]] uint32_t triggerHashOf ( const void* shader )
		{
			const auto	key = uintptr_t ( shader );
			for ( size_t i = 0, at = slotOf ( key ); i < triggerSlots; ++i, at = ( at + 1 ) & ( triggerSlots - 1 ) )
			{
				const auto	p = triggerPointers[ at ].load ( std::memory_order_acquire );
				if ( p == key )
					return triggerHashes[ at ].load ( std::memory_order_relaxed );
				if ( p == 0 )
					break;
			}
			return 0;
		}

		//---------------------------------------------------------------------
		// Formats

		[[ nodiscard ]] uint32_t formatBitOf ( DXGI_FORMAT f )
		{
			switch ( f )
			{
				case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				case DXGI_FORMAT_R8G8B8A8_UNORM:
				case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:	return host::float_format::rgba8;
				case DXGI_FORMAT_R10G10B10A2_UNORM:		return host::float_format::rgb10a2;
				default:								return 0;
			}
		}

		// The clone's texture format: a typeless texture stays typeless
		[[ nodiscard ]] DXGI_FORMAT cloneFormatOf ( DXGI_FORMAT f )
		{
			return f == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R16G16B16A16_TYPELESS : DXGI_FORMAT_R16G16B16A16_FLOAT;
		}

		// A view of the clone for the format the game's view had: an sRGB view becomes a plain float one, the values stay as the game
		// writes and reads them. Unknown for a format the clone cannot be viewed as
		[[ nodiscard ]] DXGI_FORMAT cloneViewFormatOf ( DXGI_FORMAT f )
		{
			switch ( f )
			{
				case DXGI_FORMAT_R8G8B8A8_UNORM:
				case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				case DXGI_FORMAT_R10G10B10A2_UNORM:		return DXGI_FORMAT_R16G16B16A16_FLOAT;
				case DXGI_FORMAT_R8G8B8A8_UINT:
				case DXGI_FORMAT_R10G10B10A2_UINT:		return DXGI_FORMAT_R16G16B16A16_UINT;
				case DXGI_FORMAT_R8G8B8A8_SNORM:		return DXGI_FORMAT_R16G16B16A16_SNORM;
				case DXGI_FORMAT_R8G8B8A8_SINT:			return DXGI_FORMAT_R16G16B16A16_SINT;
				default:								return DXGI_FORMAT_UNKNOWN;
			}
		}

		[[ nodiscard ]] bool isFloatFamily ( DXGI_FORMAT f )
		{
			switch ( f )
			{
				case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				case DXGI_FORMAT_R16G16B16A16_FLOAT:
				case DXGI_FORMAT_R16G16B16A16_UNORM:
				case DXGI_FORMAT_R16G16B16A16_UINT:
				case DXGI_FORMAT_R16G16B16A16_SNORM:
				case DXGI_FORMAT_R16G16B16A16_SINT:		return true;
				default:								return false;
			}
		}

		[[ nodiscard ]] DXGI_FORMAT typedOf ( DXGI_FORMAT f )
		{
			return DXGI_FORMAT ( api::format_to_default_typed ( api::format ( f ) ) );
		}

		[[ nodiscard ]] const char* nameOf ( DXGI_FORMAT f )
		{
			switch ( f )
			{
				case DXGI_FORMAT_R8G8B8A8_TYPELESS:		return "RGBA8 typeless";
				case DXGI_FORMAT_R8G8B8A8_UNORM:		return "RGBA8";
				case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:	return "RGBA8 sRGB";
				case DXGI_FORMAT_R10G10B10A2_UNORM:		return "RGB10A2";
				case DXGI_FORMAT_R10G10B10A2_TYPELESS:	return "RGB10A2 typeless";
				case DXGI_FORMAT_B8G8R8A8_UNORM:		return "BGRA8";
				case DXGI_FORMAT_R11G11B10_FLOAT:		return "R11G11B10F";
				case DXGI_FORMAT_R16G16B16A16_TYPELESS:	return "RGBA16 typeless";
				case DXGI_FORMAT_R16G16B16A16_FLOAT:	return "RGBA16F";
				default:								return "other format";
			}
		}

		// One line per cause
		std::mutex				loggedLock;
		std::set<std::string>	logged;

		void tellOnce ( const std::string& cause, const char* format, ... )
		{
			{
				std::scoped_lock	lock ( loggedLock );
				if ( ! logged.insert ( cause ).second )
					return;
			}

			char		text[ 512 ];
			std::va_list	args;
			va_start ( args, format );
			std::vsnprintf ( text, sizeof text, format, args );
			va_end ( args );
			reshade::log::message ( reshade::log::level::info, "Float clone: %s", text );
		}

		[[ nodiscard ]] std::string causeOf ( const char* what, DXGI_FORMAT a, DXGI_FORMAT b = DXGI_FORMAT_UNKNOWN )
		{
			return std::string ( what ) + ' ' + std::to_string ( int ( a ) ) + ' ' + std::to_string ( int ( b ) );
		}

		//---------------------------------------------------------------------
		// The converting draw: one pixel shader reading the source as it is stored, writing the destination's format

		struct Objects
		{
			com_ptr<ID3D11VertexShader>	vs;
			com_ptr<ID3D11PixelShader>	ps;
			com_ptr<ID3D11Buffer>		offset;
			bool						isFailed = false;
		};

		std::mutex										objectsLock;
		std::unordered_map<ID3D11Device*, Objects>		objects;

		[[ nodiscard ]] Objects* objectsOf ( ID3D11Device* device )
		{
			std::scoped_lock	lock ( objectsLock );
			auto&				o = objects[ device ];
			if ( o.vs != nullptr || o.isFailed )
				return o.isFailed ? nullptr : &o;

			const auto	vs = reshade::resources::load_data_resource ( IDR_FULLSCREEN_VS );
			const auto	ps = reshade::resources::load_data_resource ( IDR_FLOAT_COPY_PS );
			const D3D11_BUFFER_DESC	offset { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
			if ( FAILED ( device->CreateVertexShader ( vs.data, vs.data_size, nullptr, &o.vs ) ) || FAILED ( device->CreatePixelShader ( ps.data, ps.data_size, nullptr, &o.ps ) )
				 || FAILED ( device->CreateBuffer ( &offset, nullptr, &o.offset ) ) )
			{
				o.isFailed = true;
				tellOnce ( "objects", "the converting draw cannot be made, copies between a clone and another format are dropped" );
				return nullptr;
			}
			return &o;
		}

		// A texture as one end of a draw
		struct Surface
		{
			ID3D11Resource*			resource = nullptr;
			D3D11_TEXTURE2D_DESC	desc {};
			bool					isClone = false;
		};

		struct Part
		{
			UINT	sourceSubresource = 0;
			UINT	destSubresource = 0;
			UINT	sourceX = 0;
			UINT	sourceY = 0;
			UINT	destX = 0;
			UINT	destY = 0;
			UINT	width = 0;
			UINT	height = 0;
		};

		[[ nodiscard ]] const char* whyNotDrawn ( const Surface& source, const Surface& dest )
		{
			if ( source.desc.SampleDesc.Count > 1 )
				return "the source is multisampled";
			if ( ( source.desc.BindFlags & D3D11_BIND_SHADER_RESOURCE ) == 0 )
				return "the source is no shader resource";
			if ( ( dest.desc.BindFlags & D3D11_BIND_RENDER_TARGET ) == 0 || dest.desc.Usage != D3D11_USAGE_DEFAULT )
				return "the destination is no render target";
			return nullptr;
		}

		// All parts in one go: the context's state is kept and put back once
		[[ nodiscard ]] bool paint ( ID3D11DeviceContext* context, const Surface& source, const Surface& dest, std::span<const Part> parts )
		{
			com_ptr<ID3D11Device>	device;
			context->GetDevice ( &device );
			const auto*	o = objectsOf ( device.get () );
			if ( o == nullptr )
				return false;

			const auto	sourceFormat = source.isClone ? DXGI_FORMAT_R16G16B16A16_FLOAT : typedOf ( source.desc.Format );
			const auto	destFormat = dest.isClone ? DXGI_FORMAT_R16G16B16A16_FLOAT : typedOf ( dest.desc.Format );
			const auto	sourceLevels = std::max ( source.desc.MipLevels, 1u );
			const auto	destLevels = std::max ( dest.desc.MipLevels, 1u );
			const auto	isDestMultisampled = dest.desc.SampleDesc.Count > 1;

			auto	block = std::make_unique<reshade::d3d11::state_block> ( device );
			block->capture ( context );

			ID3D11Buffer* const	offsetBuffer = o->offset.get ();
			context->IASetInputLayout ( nullptr );
			context->IASetPrimitiveTopology ( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
			context->VSSetShader ( o->vs.get (), nullptr, 0 );
			context->HSSetShader ( nullptr, nullptr, 0 );
			context->DSSetShader ( nullptr, nullptr, 0 );
			context->GSSetShader ( nullptr, nullptr, 0 );
			context->PSSetShader ( o->ps.get (), nullptr, 0 );
			context->PSSetConstantBuffers ( 0, 1, &offsetBuffer );
			context->RSSetState ( nullptr );
			context->RSSetScissorRects ( 0, nullptr );
			context->OMSetBlendState ( nullptr, nullptr, 0xFFFFFFFF );
			context->OMSetDepthStencilState ( nullptr, 0 );

			auto	isDone = true;
			for ( const auto& p : parts )
			{
				D3D11_SHADER_RESOURCE_VIEW_DESC	read {};
				read.Format = sourceFormat;
				read.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
				read.Texture2DArray = { p.sourceSubresource % sourceLevels, 1, p.sourceSubresource / sourceLevels, 1 };
				D3D11_RENDER_TARGET_VIEW_DESC	write {};
				write.Format = destFormat;
				if ( isDestMultisampled )
				{
					write.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
					write.Texture2DMSArray = { p.destSubresource / destLevels, 1 };
				}
				else
				{
					write.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
					write.Texture2DArray = { p.destSubresource % destLevels, p.destSubresource / destLevels, 1 };
				}

				com_ptr<ID3D11ShaderResourceView>	readView;
				com_ptr<ID3D11RenderTargetView>		writeView;
				if ( FAILED ( device->CreateShaderResourceView ( source.resource, &read, &readView ) ) || FAILED ( device->CreateRenderTargetView ( dest.resource, &write, &writeView ) ) )
				{
					isDone = false;
					break;
				}

				const int32_t			offset[ 4 ] { int32_t ( p.sourceX ) - int32_t ( p.destX ), int32_t ( p.sourceY ) - int32_t ( p.destY ), 0, 0 };
				const D3D11_VIEWPORT	viewport { float ( p.destX ), float ( p.destY ), float ( p.width ), float ( p.height ), 0.0f, 1.0f };
				ID3D11ShaderResourceView* const	readViews[] { readView.get () };
				ID3D11RenderTargetView* const	writeViews[] { writeView.get () };
				context->UpdateSubresource ( offsetBuffer, 0, nullptr, offset, 0, 0 );
				context->PSSetShaderResources ( 0, 1, readViews );
				context->RSSetViewports ( 1, &viewport );
				context->OMSetRenderTargets ( 1, writeViews, nullptr );
				context->Draw ( 3, 0 );
			}

			block->apply_and_release ();
			return isDone;
		}

		// Every subresource of the source into the same one of the destination
		[[ nodiscard ]] std::vector<Part> allParts ( const Surface& source )
		{
			const auto			levels = std::max ( source.desc.MipLevels, 1u );
			const auto			layers = std::max ( source.desc.ArraySize, 1u );
			std::vector<Part>	parts;
			parts.reserve ( size_t ( levels ) * layers );
			for ( UINT sub = 0; sub < levels * layers; ++sub )
			{
				const auto	mip = sub % levels;
				parts.push_back ( { sub, sub, 0, 0, 0, 0, std::max ( source.desc.Width >> mip, 1u ), std::max ( source.desc.Height >> mip, 1u ) } );
			}
			return parts;
		}

		//---------------------------------------------------------------------
		// Clones

		[[ nodiscard ]] Tagged* taggedOf ( const void* resource )
		{
			auto&		t = tables ();
			const auto	it = t.textures.find ( resource );
			return it != t.textures.end () ? it->second.get () : nullptr;
		}

		[[ nodiscard ]] bool describe ( ID3D11Resource* resource, D3D11_TEXTURE2D_DESC& desc )
		{
			D3D11_RESOURCE_DIMENSION	type;
			resource->GetType ( &type );
			if ( type != D3D11_RESOURCE_DIMENSION_TEXTURE2D )
				return false;
			static_cast<ID3D11Texture2D*> ( resource )->GetDesc ( &desc );
			return true;
		}

		void forgetTexture ( const void* key )
		{
			auto&	t = tables ();
			std::unique_lock	lock ( t.lock );
			const auto	it = t.textures.find ( key );
			if ( it == t.textures.end () )
				return;
			if ( it->second->clone != nullptr )
				t.clones.erase ( it->second->clone.get () );
			t.textures.erase ( it );
			tableGeneration.fetch_add ( 1, std::memory_order_release );
		}

		void forgetView ( const void* key )
		{
			auto&	t = tables ();
			std::unique_lock	lock ( t.lock );
			const auto	it = t.views.find ( key );
			if ( it == t.views.end () )
				return;
			if ( it->second.clone != nullptr )
				t.cloneViews.erase ( it->second.clone.get () );
			t.views.erase ( it );
			tableGeneration.fetch_add ( 1, std::memory_order_release );
		}

		// Under the exclusive lock, for a texture that has its clone
		void makeCloneView ( Tables& t, Viewed& v )
		{
			com_ptr<ID3D11View>	made;
			auto				hr = E_FAIL;
			auto				format = DXGI_FORMAT_UNKNOWN;
			auto* const			clone = v.texture->clone.get ();
			auto* const			device = v.texture->device;

			switch ( v.kind )
			{
				case ViewKind::shaderResource:
				{
					D3D11_SHADER_RESOURCE_VIEW_DESC	d;
					static_cast<ID3D11ShaderResourceView*> ( v.game )->GetDesc ( &d );
					format = d.Format;
					d.Format = cloneViewFormatOf ( format );
					com_ptr<ID3D11ShaderResourceView>	view;
					if ( d.Format != DXGI_FORMAT_UNKNOWN && SUCCEEDED ( hr = device->CreateShaderResourceView ( clone, &d, &view ) ) )
						made = view.get ();
					break;
				}
				case ViewKind::renderTarget:
				{
					D3D11_RENDER_TARGET_VIEW_DESC	d;
					static_cast<ID3D11RenderTargetView*> ( v.game )->GetDesc ( &d );
					format = d.Format;
					d.Format = cloneViewFormatOf ( format );
					com_ptr<ID3D11RenderTargetView>	view;
					if ( d.Format != DXGI_FORMAT_UNKNOWN && SUCCEEDED ( hr = device->CreateRenderTargetView ( clone, &d, &view ) ) )
						made = view.get ();
					break;
				}
				case ViewKind::unorderedAccess:
				{
					D3D11_UNORDERED_ACCESS_VIEW_DESC	d;
					static_cast<ID3D11UnorderedAccessView*> ( v.game )->GetDesc ( &d );
					format = d.Format;
					d.Format = cloneViewFormatOf ( format );
					com_ptr<ID3D11UnorderedAccessView>	view;
					if ( d.Format != DXGI_FORMAT_UNKNOWN && SUCCEEDED ( hr = device->CreateUnorderedAccessView ( clone, &d, &view ) ) )
						made = view.get ();
					break;
				}
			}

			if ( made == nullptr )
			{
				v.isFailed = true;
				tellOnce ( causeOf ( "view", format, DXGI_FORMAT ( v.kind ) ), "a %s view of a clone cannot be made for a view of format %s (0x%08lX), the game's view stays",
						   v.kind == ViewKind::shaderResource ? "shader resource" : v.kind == ViewKind::renderTarget ? "render target" : "unordered access", nameOf ( format ),
						   static_cast<unsigned long> ( hr ) );
				return;
			}
			v.clone = made;
			t.cloneViews[ made.get () ] = v.game;
		}

		struct Resolved
		{
			ID3D11View*	replacement = nullptr;
			bool		isPending = false;
		};

		// The view the game named: the clone's view of it (made now when the texture got its clone since), and whether it is a render
		// target of a texture that has no clone yet
		[[ nodiscard ]] Resolved resolve ( const void* view )
		{
			auto&	t = tables ();
			{
				std::shared_lock	lock ( t.lock );
				const auto			it = t.views.find ( view );
				if ( it == t.views.end () )
					return {};

				const auto&	v = it->second;
				if ( v.texture->clone == nullptr )
					return { nullptr, v.kind == ViewKind::renderTarget && ! v.texture->isFailed };
				if ( v.clone != nullptr )
					return { v.clone.get (), false };
				if ( v.isFailed )
					return {};
			}

			std::unique_lock	lock ( t.lock );
			const auto			it = t.views.find ( view );
			if ( it == t.views.end () )
				return {};
			if ( it->second.clone == nullptr && ! it->second.isFailed && it->second.texture->clone != nullptr )
				makeCloneView ( t, it->second );
			return { it->second.clone.get (), false };
		}

		[[ nodiscard ]] const ContextState::Seen& seen ( ContextState& state, const void* view )
		{
			auto&		slot = state.seen[ ( uintptr_t ( view ) >> 4 ) % state.seen.size () ];
			const auto	generation = tableGeneration.load ( std::memory_order_acquire );
			if ( slot.view != view || slot.generation != generation )
			{
				const auto	r = resolve ( view );
				slot = { view, r.replacement, generation, r.isPending };
			}
			return slot;
		}

		void carryContents ( ID3D11DeviceContext* context, const Tagged& t )
		{
			Surface	source { t.game, t.desc, false };
			Surface	dest { t.clone.get (), {}, true };
			t.clone->GetDesc ( &dest.desc );

			if ( const auto* why = whyNotDrawn ( source, dest ); why != nullptr )
			{
				tellOnce ( causeOf ( "carry", t.desc.Format ), "the contents of a %s clone are not carried over, %s", nameOf ( t.desc.Format ), why );
				return;
			}
			const auto	parts = allParts ( source );
			if ( ! paint ( context, source, dest, parts ) )
				tellOnce ( causeOf ( "carry draw", t.desc.Format ), "the contents of a %s clone could not be drawn over", nameOf ( t.desc.Format ) );
		}

		// The clone of a tagged texture, made now when it has none: false when it cannot be
		[[ nodiscard ]] bool cloneFor ( Tagged* texture, ID3D11DeviceContext* context, const void* trigger, bool hasContents )
		{
			auto&	t = tables ();
			{
				std::unique_lock	lock ( t.lock );
				if ( texture->clone != nullptr )
					return true;
				if ( texture->isFailed )
					return false;

				auto	d = texture->desc;
				d.Format = cloneFormatOf ( d.Format );
				d.BindFlags |= D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
				const auto	hr = texture->device->CreateTexture2D ( &d, nullptr, &texture->clone );
				if ( FAILED ( hr ) )
				{
					texture->isFailed = true;
					tellOnce ( causeOf ( "clone", texture->desc.Format, DXGI_FORMAT ( texture->desc.SampleDesc.Count ) ),
							   "a %ux%u %s texture with %u samples cannot get a clone (0x%08lX), it stays as the game made it", texture->desc.Width, texture->desc.Height,
							   nameOf ( texture->desc.Format ), texture->desc.SampleDesc.Count, static_cast<unsigned long> ( hr ) );
					tableGeneration.fetch_add ( 1, std::memory_order_release );
					return false;
				}

				t.clones[ texture->clone.get () ] = texture;
				detail::hasClonedTextures.store ( true, std::memory_order_relaxed );
				tableGeneration.fetch_add ( 1, std::memory_order_release );
			}

			const auto&	d = texture->desc;
			const auto	hash = trigger != nullptr ? triggerHashOf ( trigger ) : 0u;
			char		at[ 48 ] = "a copy";
			if ( hash != 0 )
				std::snprintf ( at, sizeof at, "a draw of pixel shader %08X", hash );
			char		key[ 96 ];
			std::snprintf ( key, sizeof key, "made %u %u %u %u %u %u %u", d.Width, d.Height, unsigned ( d.Format ), d.ArraySize, d.MipLevels, d.SampleDesc.Count, d.BindFlags );
			tellOnce ( key, "%ux%u %s, %u layers, %u mips, %u samples, bind 0x%X: cloned as %s at %s", d.Width, d.Height, nameOf ( d.Format ), d.ArraySize, d.MipLevels,
					   d.SampleDesc.Count, d.BindFlags, nameOf ( cloneFormatOf ( d.Format ) ), at );

			if ( hasContents )
				carryContents ( context, *texture );
			return true;
		}
	}

	//-------------------------------------------------------------------------

	void ContextState::clear ()
	{
		trigger = nullptr;
		hasPendingTarget = false;
	}

	void noteTexture ( ID3D11Device* device, ID3D11Texture2D* texture, bool hasInitialData )
	{
		const auto	formats = reshade::ultra::floatClone::ruleFormats ();
		if ( formats == 0 )
			return;

		D3D11_TEXTURE2D_DESC	d;
		texture->GetDesc ( &d );
		if ( ( formats & formatBitOf ( d.Format ) ) == 0 || d.Usage != D3D11_USAGE_DEFAULT || d.CPUAccessFlags != 0 )
			return;
		if ( ( d.MiscFlags & ~UINT ( D3D11_RESOURCE_MISC_GENERATE_MIPS | D3D11_RESOURCE_MISC_TEXTURECUBE ) ) != 0 )
		{
			if ( ( d.MiscFlags & ( D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE ) ) != 0 )
				tellOnce ( causeOf ( "shared", d.Format ), "a shared %s texture is not tagged, it stays as the game made it", nameOf ( d.Format ) );
			return;
		}
		if ( ( d.BindFlags & ~UINT ( D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS ) ) != 0 )
			return;
		if ( hasInitialData && ( d.BindFlags & D3D11_BIND_RENDER_TARGET ) == 0 )
			return;

		auto	entry = std::make_unique<Tagged> ();
		entry->device = device;
		entry->game = texture;
		entry->desc = d;
		{
			std::unique_lock	lock ( tables ().lock );
			tables ().textures[ texture ] = std::move ( entry );
		}
		detail::hasTaggedTextures.store ( true, std::memory_order_relaxed );
		register_destruction_callback_d3dx ( texture, [ key = static_cast<const void*> ( texture ) ] { forgetTexture ( key ); } );
	}

	void noteView ( ID3D11Resource* resource, ID3D11View* view, ViewKind kind )
	{
		if ( ! hasTagged () )
			return;

		auto&	t = tables ();
		{
			std::shared_lock	lock ( t.lock );
			if ( taggedOf ( resource ) == nullptr )
				return;
		}
		{
			std::unique_lock	lock ( t.lock );
			auto* const			texture = taggedOf ( resource );
			if ( texture == nullptr )
				return;
			Viewed	v;
			v.game = view;
			v.texture = texture;
			v.kind = kind;
			t.views[ view ] = std::move ( v );
			tableGeneration.fetch_add ( 1, std::memory_order_release );
		}
		register_destruction_callback_d3dx ( view, [ key = static_cast<const void*> ( view ) ] { forgetView ( key ); } );
	}

	void notePixelShader ( ID3D11PixelShader* shader, const void* code, size_t size )
	{
		if ( reshade::ultra::floatClone::ruleFormats () == 0 )
			return;

		const auto	hash = reshade::ultra::floatClone::hashOfCode ( code, size );
		{
			std::scoped_lock	lock ( triggerLock );
			pixelHashes[ shader ] = hash;
			if ( reshade::ultra::floatClone::isTriggerHash ( hash ) )
				addTrigger ( shader, hash );
		}
		register_destruction_callback_d3dx ( shader, [ key = static_cast<const void*> ( shader ) ] { forgetPixelShader ( key ); } );
	}

	void rulesChanged ()
	{
		std::scoped_lock	lock ( triggerLock );
		for ( auto& p : triggerPointers )
			if ( p.load ( std::memory_order_relaxed ) > 1 )
				p.store ( 1, std::memory_order_release );
		triggerCount.store ( 0, std::memory_order_relaxed );
		for ( const auto& [ shader, hash ] : pixelHashes )
			if ( reshade::ultra::floatClone::isTriggerHash ( hash ) )
				addTrigger ( shader, hash );
	}

	void forgetDevice ( ID3D11Device* device )
	{
		{
			auto&	t = tables ();
			std::unique_lock	lock ( t.lock );
			std::erase_if ( t.views, [ & ] ( const auto& e ) { return e.second.texture->device == device; } );
			std::erase_if ( t.cloneViews, [ & ] ( const auto& e ) { return ! t.views.contains ( e.second ); } );
			std::erase_if ( t.clones, [ & ] ( const auto& e ) { return e.second->device == device; } );
			std::erase_if ( t.textures, [ & ] ( const auto& e ) { return e.second->device == device; } );
			tableGeneration.fetch_add ( 1, std::memory_order_release );
		}
		std::scoped_lock	lock ( objectsLock );
		objects.erase ( device );
	}

	//-------------------------------------------------------------------------

	template <typename View>
	View* const* swapViews ( ContextState& state, UINT count, View* const* views, View** storage )
	{
		constexpr auto	isTarget = std::is_same_v<View, ID3D11RenderTargetView>;
		if ( views == nullptr )
		{
			if constexpr ( isTarget )
				state.hasPendingTarget = false;
			return views;
		}

		auto	isChanged = false;
		auto	isPending = false;
		for ( UINT i = 0; i < count; ++i )
		{
			auto*	view = views[ i ];
			auto*	use = view;
			if ( view != nullptr )
			{
				const auto&	s = seen ( state, view );
				if ( s.replacement != nullptr )
					use = static_cast<View*> ( s.replacement );
				isPending |= s.isPending;
			}
			if ( use != view && ! isChanged )
			{
				std::copy ( views, views + i, storage );
				isChanged = true;
			}
			if ( isChanged )
				storage[ i ] = use;
		}

		if constexpr ( isTarget )
			state.hasPendingTarget = isPending;
		return isChanged ? storage : views;
	}

	template <typename View>
	View* swapView ( ContextState& state, View* view )
	{
		if ( view == nullptr )
			return view;
		const auto&	s = seen ( state, view );
		return s.replacement != nullptr ? static_cast<View*> ( s.replacement ) : view;
	}

	template <typename View>
	void unswapViews ( UINT count, View** views )
	{
		if ( ! hasClones () || views == nullptr )
			return;

		auto&	t = tables ();
		std::shared_lock	lock ( t.lock );
		for ( UINT i = 0; i < count; ++i )
		{
			if ( views[ i ] == nullptr )
				continue;
			const auto	it = t.cloneViews.find ( views[ i ] );
			if ( it == t.cloneViews.end () )
				continue;
			views[ i ]->Release ();
			it->second->AddRef ();
			views[ i ] = static_cast<View*> ( it->second );
		}
	}

	template ID3D11ShaderResourceView* const* swapViews ( ContextState&, UINT, ID3D11ShaderResourceView* const*, ID3D11ShaderResourceView** );
	template ID3D11RenderTargetView* const* swapViews ( ContextState&, UINT, ID3D11RenderTargetView* const*, ID3D11RenderTargetView** );
	template ID3D11UnorderedAccessView* const* swapViews ( ContextState&, UINT, ID3D11UnorderedAccessView* const*, ID3D11UnorderedAccessView** );
	template ID3D11ShaderResourceView* swapView ( ContextState&, ID3D11ShaderResourceView* );
	template ID3D11RenderTargetView* swapView ( ContextState&, ID3D11RenderTargetView* );
	template ID3D11UnorderedAccessView* swapView ( ContextState&, ID3D11UnorderedAccessView* );
	template ID3D11View* swapView ( ContextState&, ID3D11View* );
	template void unswapViews ( UINT, ID3D11ShaderResourceView** );
	template void unswapViews ( UINT, ID3D11RenderTargetView** );
	template void unswapViews ( UINT, ID3D11UnorderedAccessView** );

	void bindPixelShader ( ContextState& state, ID3D11PixelShader* shader )
	{
		state.trigger = triggerOf ( shader );
	}

	bool cloneBoundTargets ( ID3D11DeviceContext* context, ContextState& state )
	{
		ID3D11RenderTargetView*	targets[ D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT ] {};
		ID3D11DepthStencilView*	depth = nullptr;
		context->OMGetRenderTargets ( D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth );

		auto&	t = tables ();
		for ( auto* target : targets )
		{
			if ( target == nullptr )
				continue;

			Tagged*	pending = nullptr;
			{
				std::shared_lock	lock ( t.lock );
				const auto			it = t.views.find ( target );
				if ( it != t.views.end () && it->second.kind == ViewKind::renderTarget && it->second.texture->clone == nullptr && ! it->second.texture->isFailed )
					pending = it->second.texture;
			}
			if ( pending != nullptr )
				(void) cloneFor ( pending, context, state.trigger, true );
		}

		auto	isChanged = false;
		for ( auto& target : targets )
		{
			if ( target == nullptr )
				continue;
			if ( const auto r = resolve ( target ); r.replacement != nullptr )
			{
				target->Release ();
				target = static_cast<ID3D11RenderTargetView*> ( r.replacement );
				target->AddRef ();
				isChanged = true;
			}
		}
		if ( isChanged )
			context->OMSetRenderTargetsAndUnorderedAccessViews ( D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr );

		for ( auto* target : targets )
			if ( target != nullptr )
				target->Release ();
		if ( depth != nullptr )
			depth->Release ();
		state.hasPendingTarget = false;
		return isChanged;
	}

	//-------------------------------------------------------------------------

	CopyPlan planCopy ( ID3D11DeviceContext* context, ID3D11Resource* dest, ID3D11Resource* source, bool isOverwritten )
	{
		CopyPlan	plan { CopyKind::untouched, dest, source };
		if ( ! hasTagged () )
			return plan;

		Tagged*	d = nullptr;
		Tagged*	s = nullptr;
		{
			std::shared_lock	lock ( tables ().lock );
			d = taggedOf ( dest );
			s = taggedOf ( source );
		}
		if ( d == nullptr && s == nullptr )
			return plan;

		const auto	isCloned = [] ( const Tagged* x ) { return x != nullptr && x->clone != nullptr; };
		const auto	endOf = [] ( Tagged* x, ID3D11Resource* game ) -> ID3D11Resource* { return x != nullptr && x->clone != nullptr ? x->clone.get () : game; };

		if ( d != nullptr && s != nullptr )
		{
			if ( ! isCloned ( d ) && ! isCloned ( s ) )
				return plan;
			if ( ! cloneFor ( d, context, nullptr, ! isOverwritten ) || ! cloneFor ( s, context, nullptr, true ) )
			{
				tellOnce ( causeOf ( "copy partner", d->desc.Format, s->desc.Format ), "a copy between a clone and a texture that cannot get one is dropped" );
				plan.kind = CopyKind::dropped;
				return plan;
			}
			plan.kind = CopyKind::swapped;
			plan.dest = endOf ( d, dest );
			plan.source = endOf ( s, source );
			return plan;
		}

		// One end is no tagged texture: what it is decides
		auto* const				mine = d != nullptr ? d : s;
		auto* const				other = d != nullptr ? source : dest;
		D3D11_TEXTURE2D_DESC	otherDesc;
		if ( ! describe ( other, otherDesc ) )
			return plan;
		const auto	isFloat = isFloatFamily ( otherDesc.Format );
		if ( ! isCloned ( mine ) && ! isFloat )
			return plan;
		if ( ! cloneFor ( mine, context, nullptr, mine != d || ! isOverwritten ) )
		{
			tellOnce ( causeOf ( "copy end", mine->desc.Format, otherDesc.Format ), "a copy of a %s texture that cannot get a clone stays as the game made it", nameOf ( mine->desc.Format ) );
			return plan;
		}

		plan.kind = isFloat ? CopyKind::swapped : CopyKind::converted;
		plan.dest = endOf ( d, dest );
		plan.source = endOf ( s, source );
		return plan;
	}

	void convertCopy ( ID3D11DeviceContext* context, const CopyPlan& plan, const CopyRegion* region )
	{
		Surface	source { plan.source, {}, false };
		Surface	dest { plan.dest, {}, false };
		if ( ! describe ( plan.source, source.desc ) || ! describe ( plan.dest, dest.desc ) )
			return;
		{
			std::shared_lock	lock ( tables ().lock );
			source.isClone = tables ().clones.contains ( plan.source );
			dest.isClone = tables ().clones.contains ( plan.dest );
		}

		if ( const auto* why = whyNotDrawn ( source, dest ); why != nullptr )
		{
			tellOnce ( causeOf ( "convert", source.desc.Format, dest.desc.Format ), "a copy from %s into %s is dropped, %s", nameOf ( source.desc.Format ), nameOf ( dest.desc.Format ), why );
			return;
		}

		std::vector<Part>	parts;
		if ( region == nullptr )
			parts = allParts ( source );
		else
		{
			const auto	mip = region->sourceSubresource % std::max ( source.desc.MipLevels, 1u );
			const auto	box = region->sourceBox;
			const auto	left = box != nullptr ? box->left : 0;
			const auto	top = box != nullptr ? box->top : 0;
			parts.push_back ( { region->sourceSubresource, region->destSubresource, left, top, region->destX, region->destY,
								box != nullptr ? box->right - box->left : std::max ( source.desc.Width >> mip, 1u ), box != nullptr ? box->bottom - box->top : std::max ( source.desc.Height >> mip, 1u ) } );
		}
		if ( ! paint ( context, source, dest, parts ) )
			tellOnce ( causeOf ( "convert draw", source.desc.Format, dest.desc.Format ), "a copy from %s into %s could not be drawn, it is dropped", nameOf ( source.desc.Format ), nameOf ( dest.desc.Format ) );
	}

	ResolvePlan planResolve ( ID3D11DeviceContext* context, ID3D11Resource* dest, ID3D11Resource* source, DXGI_FORMAT format )
	{
		ResolvePlan	plan { false, format, dest, source };
		if ( ! hasTagged () )
			return plan;

		Tagged*	d = nullptr;
		Tagged*	s = nullptr;
		{
			std::shared_lock	lock ( tables ().lock );
			d = taggedOf ( dest );
			s = taggedOf ( source );
		}
		if ( d == nullptr && s == nullptr )
			return plan;

		const auto	isCloned = [] ( const Tagged* x ) { return x != nullptr && x->clone != nullptr; };
		D3D11_TEXTURE2D_DESC	otherDesc {};
		const auto				hasOther = d == nullptr || s == nullptr;
		const auto				isFloat = hasOther && describe ( d != nullptr ? source : dest, otherDesc ) && isFloatFamily ( otherDesc.Format );
		if ( ! isCloned ( d ) && ! isCloned ( s ) && ! isFloat )
			return plan;

		const auto	isMade = ( d == nullptr || cloneFor ( d, context, nullptr, false ) ) && ( s == nullptr || cloneFor ( s, context, nullptr, true ) );
		if ( ! isMade || ( hasOther && ! isFloat ) )
		{
			tellOnce ( causeOf ( "resolve", format ), "a resolve between a clone and a texture of another format is dropped" );
			plan.isDropped = true;
			return plan;
		}

		plan.dest = d != nullptr ? d->clone.get () : dest;
		plan.source = s != nullptr ? s->clone.get () : source;
		if ( const auto as = cloneViewFormatOf ( format ); as != DXGI_FORMAT_UNKNOWN )
			plan.format = as;
		return plan;
	}

	bool isUpdateDropped ( ID3D11Resource* dest )
	{
		if ( ! hasClones () )
			return false;

		D3D11_TEXTURE2D_DESC	d {};
		{
			std::shared_lock	lock ( tables ().lock );
			const auto*			t = taggedOf ( dest );
			if ( t == nullptr || t->clone == nullptr )
				return false;
			d = t->desc;
		}
		tellOnce ( causeOf ( "update", d.Format ), "an update of a %s texture from the game's data is dropped, its clone is %s", nameOf ( d.Format ), nameOf ( cloneFormatOf ( d.Format ) ) );
		return true;
	}

	ID3D11Resource* swapResource ( ID3D11Resource* resource )
	{
		if ( ! hasClones () || resource == nullptr )
			return resource;

		std::shared_lock	lock ( tables ().lock );
		const auto*			t = taggedOf ( resource );
		return t != nullptr && t->clone != nullptr ? t->clone.get () : resource;
	}

	uint64_t sourceOf ( uint64_t clone, uint32_t& format )
	{
		if ( ! hasClones () )
			return 0;

		std::shared_lock	lock ( tables ().lock );
		const auto			it = tables ().clones.find ( reinterpret_cast<const void*> ( uintptr_t ( clone ) ) );
		if ( it == tables ().clones.end () )
			return 0;
		format = uint32_t ( it->second->desc.Format );
		return uint64_t ( reinterpret_cast<uintptr_t> ( it->second->game ) );
	}
}
