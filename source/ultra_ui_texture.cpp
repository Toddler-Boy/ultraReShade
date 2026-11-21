/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <mutex>
#include <vector>

#include "ultra_ui_texture.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	namespace
	{
		constexpr uint64_t	keepFrames = 4;

		struct Retired
		{
			Texture		texture;
			uint64_t	frame;
		};

		std::mutex				retiredLock;
		std::vector<Retired>	retired;
		uint64_t				frame = 0;

		void destroy ( const Texture& t )
		{
			if ( t.view.handle != 0 )
				t.device->destroy_resource_view ( t.view );
			if ( t.resource.handle != 0 )
				t.device->destroy_resource ( t.resource );
		}
	}

	namespace
	{
		[[ nodiscard ]] Texture create ( api::device* device, int width, int height, const void* pixels, api::format format, uint32_t pixelBytes )
		{
			Texture		t;
			const auto	desc = api::resource_desc ( uint32_t ( width ), uint32_t ( height ), 1, 1, format, 1, api::memory_heap::default_, api::resource_usage::shader_resource );
			const api::subresource_data	data { const_cast<void*> ( pixels ), uint32_t ( width ) * pixelBytes, uint32_t ( width * height ) * pixelBytes };

			if ( ! device->create_resource ( desc, &data, api::resource_usage::shader_resource, &t.resource ) )
				return {};

			if ( ! device->create_resource_view ( t.resource, api::resource_usage::shader_resource, api::resource_view_desc ( format ), &t.view ) )
			{
				device->destroy_resource ( t.resource );
				return {};
			}

			t.device = device;
			t.width = width;
			t.height = height;
			return t;
		}
	}

	Texture createTexture ( api::device* device, int width, int height, const uint32_t* rgba )
	{
		return create ( device, width, height, rgba, api::format::r8g8b8a8_unorm, 4 );
	}

	Texture createHalfTexture ( api::device* device, int width, int height, const uint16_t* rgba )
	{
		return create ( device, width, height, rgba, api::format::r16g16b16a16_float, 8 );
	}

	void releaseTexture ( Texture& texture )
	{
		if ( texture.device != nullptr )
		{
			std::scoped_lock	lock ( retiredLock );
			retired.push_back ( { texture, frame } );
		}

		texture = {};
	}

	void destroyTextureNow ( Texture& texture )
	{
		if ( texture.device != nullptr )
			destroy ( texture );

		texture = {};
	}

	void forgetDevice ( api::device* device )
	{
		std::scoped_lock	lock ( retiredLock );
		std::erase_if ( retired, [ device ] ( const Retired& r )
		{
			if ( r.texture.device != device )
				return false;

			destroy ( r.texture );
			return true;
		} );
	}

	void tickTextures ()
	{
		std::scoped_lock	lock ( retiredLock );
		++frame;
		std::erase_if ( retired, [] ( const Retired& r )
		{
			if ( frame - r.frame < keepFrames )
				return false;

			destroy ( r.texture );
			return true;
		} );
	}
}
