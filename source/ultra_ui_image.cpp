/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include <wincodec.h>

#include "ultra_ui_colourspace.hpp"
#include "ultra_ui_image.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	namespace
	{
		// Defined here, not taken from windowscodecs.lib, which neither add-on build links
		constexpr CLSID	wicFactory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
		constexpr GUID	wicRgba = { 0xf5c7ad2d, 0x6a8d, 0x43dd, { 0xa7, 0xa8, 0xa2, 0x99, 0x35, 0x26, 0x1a, 0xe9 } };

		using Key = std::pair<api::device*, std::wstring>;

		void releaseAll ( std::initializer_list<IUnknown*> objects )
		{
			for ( auto* object : objects )
				if ( object != nullptr )
					object->Release ();
		}

		// The decoded sRGB pixels stay, to make the texture again for another colour space. A failed load keeps an
		// entry without pixels, so it is not tried every frame
		struct Image
		{
			std::vector<uint32_t>	srgb;
			int						width = 0;
			int						height = 0;
			OverlayLook				look;
			Texture					texture;
		};

		// The file's alpha at full size stays, to shrink it again for another height
		struct Mask
		{
			std::vector<float>	alpha;
			int					width = 0;
			int					height = 0;
			int					shrunkTo = 0;
			Texture				texture;
		};

		std::map<Key, Image>	images;
		std::map<Key, Mask>		masks;

		// The texture for the overlay as it is now
		void make ( api::device* device, Image& img, const OverlayLook& look )
		{
			releaseTexture ( img.texture );
			img.look = look;

			if ( ! look.hdr () )
			{
				img.texture = createTexture ( device, img.width, img.height, img.srgb.data () );
				return;
			}

			std::vector<uint16_t>	encoded;
			encodeForOverlay ( img.srgb.data (), img.srgb.size (), look, encoded );
			img.texture = createHalfTexture ( device, img.width, img.height, encoded.data () );
		}

		[[ nodiscard ]] bool decode ( const std::wstring& path, std::vector<uint32_t>& rgba, UINT& width, UINT& height )
		{
			const auto	com = CoInitializeEx ( nullptr, COINIT_MULTITHREADED );

			IWICImagingFactory*		factory = nullptr;
			IWICBitmapDecoder*		decoder = nullptr;
			IWICBitmapFrameDecode*	frame = nullptr;
			IWICFormatConverter*	converter = nullptr;
			auto					ok = false;

			if ( SUCCEEDED ( CoCreateInstance ( wicFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof ( IWICImagingFactory ), reinterpret_cast<void**> ( &factory ) ) )
				 && SUCCEEDED ( factory->CreateDecoderFromFilename ( path.c_str (), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder ) )
				 && SUCCEEDED ( decoder->GetFrame ( 0, &frame ) )
				 && SUCCEEDED ( factory->CreateFormatConverter ( &converter ) )
				 && SUCCEEDED ( converter->Initialize ( frame, wicRgba, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom ) )
				 && SUCCEEDED ( converter->GetSize ( &width, &height ) ) && width > 0 && height > 0 )
			{
				rgba.resize ( size_t ( width ) * height );
				ok = SUCCEEDED ( converter->CopyPixels ( nullptr, width * 4, UINT ( rgba.size () * 4 ), reinterpret_cast<BYTE*> ( rgba.data () ) ) );
			}

			releaseAll ( { converter, frame, decoder, factory } );

			// Already initialised in another mode on this thread is fine; only our own initialisation is undone
			if ( SUCCEEDED ( com ) )
				CoUninitialize ();

			return ok;
		}

		// Area average of n samples, step apart, onto m <= n samples
		void shrink ( const float* src, int n, int srcStep, float* dst, int m, int dstStep )
		{
			const auto	ratio = float ( n ) / float ( m );

			for ( int i = 0; i < m; ++i )
			{
				const auto	a = float ( i ) * ratio;
				const auto	b = a + ratio;
				auto		sum = 0.0f;

				for ( auto j = int ( a ); j < n && float ( j ) < b; ++j )
					sum += src[ j * srcStep ] * ( std::min ( b, float ( j + 1 ) ) - std::max ( a, float ( j ) ) );

				dst[ i * dstStep ] = sum / ratio;
			}
		}

		void makeMask ( api::device* device, Mask& m, int height )
		{
			releaseTexture ( m.texture );
			m.shrunkTo = height;

			const auto	h = std::clamp ( height, 1, m.height );
			const auto	w = std::max ( 1, int ( std::lround ( float ( m.width ) * float ( h ) / float ( m.height ) ) ) );

			std::vector<float>	rows ( size_t ( w ) * size_t ( m.height ) );
			for ( int y = 0; y < m.height; ++y )
				shrink ( &m.alpha[ size_t ( y ) * size_t ( m.width ) ], m.width, 1, &rows[ size_t ( y ) * size_t ( w ) ], w, 1 );

			std::vector<float>	out ( size_t ( w ) * size_t ( h ) );
			for ( int x = 0; x < w; ++x )
				shrink ( &rows[ size_t ( x ) ], m.height, w, &out[ size_t ( x ) ], h, w );

			std::vector<uint32_t>	rgba ( out.size () );
			std::ranges::transform ( out, rgba.begin (), [] ( float a ) { return 0x00FFFFFFu | ( uint32_t ( std::lround ( std::clamp ( a, 0.0f, 1.0f ) * 255.0f ) ) << 24 ); } );

			m.texture = createTexture ( device, w, h, rgba.data () );
		}
	}

	void forgetImages ( api::device* device )
	{
		const auto	drop = [ device ] ( auto& entry )
		{
			if ( entry.first.first != device )
				return false;

			destroyTextureNow ( entry.second.texture );
			return true;
		};

		std::erase_if ( images, drop );
		std::erase_if ( masks, drop );
	}

	const Texture* image ( api::effect_runtime* runtime, const std::wstring& path )
	{
		auto* const	device = runtime != nullptr ? runtime->get_device () : nullptr;
		if ( device == nullptr || path.empty () )
			return nullptr;

		auto [ it, fresh ] = images.try_emplace ( Key { device, path } );
		auto&		img = it->second;
		const auto	look = overlayLook ( runtime );

		if ( fresh )
		{
			UINT	width = 0, height = 0;
			if ( ! decode ( path, img.srgb, width, height ) )
				return nullptr;

			img.width = int ( width );
			img.height = int ( height );
			make ( device, img, look );
		}
		else if ( ! img.srgb.empty () && look != img.look )
			make ( device, img, look );

		return img.texture.valid () ? &img.texture : nullptr;
	}

	bool decodeImage ( const std::wstring& path, std::vector<uint32_t>& rgba, int& width, int& height )
	{
		UINT	w = 0, h = 0;
		if ( ! decode ( path, rgba, w, h ) )
			return false;
		width = int ( w );
		height = int ( h );
		return true;
	}

	const Texture* mask ( api::effect_runtime* runtime, const std::wstring& path, int height )
	{
		auto* const	device = runtime != nullptr ? runtime->get_device () : nullptr;
		if ( device == nullptr || path.empty () || height <= 0 )
			return nullptr;

		auto [ it, fresh ] = masks.try_emplace ( Key { device, path } );
		auto&	m = it->second;

		if ( fresh )
		{
			std::vector<uint32_t>	rgba;
			UINT					fileWidth = 0, fileHeight = 0;
			if ( ! decode ( path, rgba, fileWidth, fileHeight ) )
				return nullptr;

			m.width = int ( fileWidth );
			m.height = int ( fileHeight );
			m.alpha.resize ( rgba.size () );
			std::ranges::transform ( rgba, m.alpha.begin (), [] ( uint32_t p ) { return float ( p >> 24 ) / 255.0f; } );
		}

		if ( m.alpha.empty () )
			return nullptr;

		if ( m.shrunkTo != height )
			makeMask ( device, m, height );

		return m.texture.valid () ? &m.texture : nullptr;
	}
}
