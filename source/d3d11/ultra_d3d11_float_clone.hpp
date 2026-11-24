/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <d3d11_4.h>

//-----------------------------------------------------------------------------

// D3D11: the game's float render targets by the rule the add-on states (source\ultra_float_clone.hpp).
// A 2D texture made in a format of the rule is tagged, nothing else changes. The first time a tagged texture is a render target of a draw
// whose pixel shader is one of the rule's, it gets a clone (same description, RGBA16F) with its contents, and every use of it from then on,
// by any context, goes to the clone: the views of it (made once per view the game made), copies, clears, resolves
namespace reshade::ultra::d3d11::floatClone
{
	namespace detail
	{
		inline std::atomic<bool>	hasTaggedTextures { false };
		inline std::atomic<bool>	hasClonedTextures { false };
	}

	// Binds look no further when no texture is tagged (render targets) or cloned (the other views)
	[[ nodiscard ]] inline bool hasTagged () { return detail::hasTaggedTextures.load ( std::memory_order_relaxed ); }
	[[ nodiscard ]] inline bool hasClones () { return detail::hasClonedTextures.load ( std::memory_order_relaxed ); }

	enum class ViewKind : uint8_t { shaderResource, renderTarget, unorderedAccess };

	// What a context keeps of its binds. One thread records on a context at a time, so none of it is locked
	struct ContextState
	{
		struct Seen
		{
			const void*	view = nullptr;
			ID3D11View*	replacement = nullptr;
			uint32_t	generation = 0;
			bool		isPending = false;
		};

		std::array<Seen, 128>	seen {};
		const void*				trigger = nullptr;				// the pixel shader bound, when it is one of the rule's
		bool					hasPendingTarget = false;		// a render target bound is a tagged texture without a clone

		[[ nodiscard ]] bool isDue () const { return trigger != nullptr && hasPendingTarget; }
		void clear ();
	};

	//-------------------------------------------------------------------------
	// Making objects

	void noteTexture ( ID3D11Device* device, ID3D11Texture2D* texture, bool hasInitialData );
	void noteView ( ID3D11Resource* resource, ID3D11View* view, ViewKind kind );
	void notePixelShader ( ID3D11PixelShader* shader, const void* code, size_t size );
	void forgetDevice ( ID3D11Device* device );

	// The rule's shaders changed: the pixel shaders made so far are told again, from the next bind on
	void rulesChanged ();

	//-------------------------------------------------------------------------
	// Binds, as a context is to take them: the views the game names, or where the texture has a clone, the views of that. The array is
	// storage for the views when any changed (then returned), else the game's own are returned

	template <typename View>
	[[ nodiscard ]] View* const* swapViews ( ContextState& state, UINT count, View* const* views, View** storage );

	template <typename View>
	[[ nodiscard ]] View* swapView ( ContextState& state, View* view );

	// The pixel shader a context binds: the rule's, or not
	void bindPixelShader ( ContextState& state, ID3D11PixelShader* shader );

	// What a context hands the game when it asks for a view it has bound: the game's view, not the clone's
	template <typename View>
	void unswapViews ( UINT count, View** views );

	// A draw is due to make clones of the tagged textures bound as render targets: made, bound in place of them. False when none was
	[[ nodiscard ]] bool cloneBoundTargets ( ID3D11DeviceContext* context, ContextState& state );

	//-------------------------------------------------------------------------
	// Copies and the other uses of a resource

	// untouched: as the game made it. swapped: the same call on the resources of the plan. converted: the plan's ends differ in format
	// family, convertCopy draws it. dropped: it cannot be done (logged)
	enum class CopyKind : uint8_t { untouched, swapped, converted, dropped };

	struct CopyPlan
	{
		CopyKind		kind = CopyKind::untouched;
		ID3D11Resource*	dest = nullptr;
		ID3D11Resource*	source = nullptr;
	};

	// isOverwritten: the copy writes all of dest, so a clone made for it needs no contents
	[[ nodiscard ]] CopyPlan planCopy ( ID3D11DeviceContext* context, ID3D11Resource* dest, ID3D11Resource* source, bool isOverwritten );

	// A converted copy: the given region of one subresource, or every subresource when region is null
	struct CopyRegion
	{
		UINT				destSubresource = 0;
		UINT				destX = 0;
		UINT				destY = 0;
		UINT				sourceSubresource = 0;
		const D3D11_BOX*	sourceBox = nullptr;
	};

	void convertCopy ( ID3D11DeviceContext* context, const CopyPlan& plan, const CopyRegion* region );

	struct ResolvePlan
	{
		bool			isDropped = false;
		DXGI_FORMAT		format = DXGI_FORMAT_UNKNOWN;
		ID3D11Resource*	dest = nullptr;
		ID3D11Resource*	source = nullptr;
	};

	[[ nodiscard ]] ResolvePlan planResolve ( ID3D11DeviceContext* context, ID3D11Resource* dest, ID3D11Resource* source, DXGI_FORMAT format );

	// The game's data cannot go into a clone: true when dest is one (logged, the call is left out)
	[[ nodiscard ]] bool isUpdateDropped ( ID3D11Resource* dest );

	// The clone of a resource, else the resource itself
	[[ nodiscard ]] ID3D11Resource* swapResource ( ID3D11Resource* resource );

	//-------------------------------------------------------------------------

	// The game's texture a clone stands for and the format it was made as (an api::format); 0 for a handle that is no clone
	[[ nodiscard ]] uint64_t sourceOf ( uint64_t clone, uint32_t& format );
}
