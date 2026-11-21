/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <bit>
#include <cmath>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Windows.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include <stb_truetype.h>
#pragma clang diagnostic pop

#include "ultra_ui_font.hpp"
#include "ultra_ui_theme.hpp"

extern HMODULE g_module_handle;

//-----------------------------------------------------------------------------

namespace ultra::ui::font
{
	namespace
	{
		constexpr int	faceCount = int ( Face::count );

		// RCDATA resources of the DLL, one TTF each
		constexpr const wchar_t*	resourceNames[ faceCount ] = { L"ULTRA_FONT_REGULAR", L"ULTRA_FONT_MEDIUM", L"ULTRA_FONT_SEMIBOLD", L"ULTRA_FONT_BOLD" };

		// The glyph cache's pixels: one atlas of all glyphs drawn so far, at the size and face each was drawn in, made the first time it is
		// drawn. Full, it starts over at the next frame
		constexpr int	atlasWidth = 2048;
		constexpr int	atlasHeight = 1024;

		struct Entry
		{
			int		x = 0;
			int		y = 0;
			int		width = 0;
			int		height = 0;
			float	xoff = 0.0f;
			float	yoff = 0.0f;
		};

		struct Metrics
		{
			float	emScale = 0.0f;
			float	ascent = 0.0f;
			float	descent = 0.0f;
			float	capHeight = 0.0f;
			float	digitAdvance = 0.0f;
		};

		// What one device holds of the atlas: the rows from dirtyFrom up to dirtyTo are newer than its texture
		struct DeviceAtlas
		{
			api::resource		texture {};
			api::resource_view	view {};
			int					dirtyFrom = atlasHeight;
			int					dirtyTo = 0;
		};

		struct Atlas
		{
			std::once_flag								loaded;
			stbtt_fontinfo								info[ faceCount ] {};
			const unsigned char*						data[ faceCount ] {};
			size_t										sizes[ faceCount ] {};
			std::unordered_map<uint64_t, Entry>			entries;
			std::unordered_map<uint64_t, Metrics>		metrics;
			std::vector<uint32_t>						rgba = std::vector<uint32_t> ( size_t ( atlasWidth ) * size_t ( atlasHeight ), 0x00FFFFFFu );
			int											cursorX = 0;
			int											cursorY = 0;
			int											rowHeight = 0;
			bool										isFull = false;
			std::unordered_map<api::device*, DeviceAtlas>	devices;
			const DeviceAtlas*							current = nullptr;
		} atlas;

		void loadFaces ()
		{
			const auto	module = g_module_handle;

			for ( int i = 0; i < faceCount; ++i )
			{
				const auto	res = FindResourceW ( module, resourceNames[ i ], MAKEINTRESOURCEW ( 10 ) );		// RT_RCDATA
				const auto	handle = res != nullptr ? LoadResource ( module, res ) : nullptr;
				const auto*	bytes = handle != nullptr ? static_cast<const unsigned char*> ( LockResource ( handle ) ) : nullptr;

				if ( bytes != nullptr && stbtt_InitFont ( &atlas.info[ i ], bytes, stbtt_GetFontOffsetForIndex ( bytes, 0 ) ) )
				{
					atlas.data[ i ] = bytes;
					atlas.sizes[ i ] = SizeofResource ( module, res );
				}
			}
		}

		// A missing weight takes the regular face
		[[ nodiscard ]] int faceIndex ( Face face )
		{
			const auto	i = int ( face );
			return atlas.data[ i ] != nullptr ? i : atlas.data[ 0 ] != nullptr ? 0 : -1;
		}

		// UTF-8 as code points; a broken sequence counts its first byte as one unknown character
		[[ nodiscard ]] std::vector<int> codePoints ( std::string_view text )
		{
			std::vector<int>	out;
			for ( size_t i = 0; i < text.size (); )
			{
				const auto	b = uint8_t ( text[ i ] );
				const auto	length = b < 0x80 ? 1 : ( b >> 5 ) == 0x6 ? 2 : ( b >> 4 ) == 0xE ? 3 : ( b >> 3 ) == 0x1E ? 4 : 0;
				auto		cp = length == 1 ? int ( b ) : length == 2 ? int ( b & 0x1F ) : length == 3 ? int ( b & 0x0F ) : int ( b & 0x07 );
				auto		whole = length > 0 && i + size_t ( length ) <= text.size ();
				for ( int k = 1; whole && k < length; ++k )
				{
					const auto	c = uint8_t ( text[ i + size_t ( k ) ] );
					whole = ( c & 0xC0 ) == 0x80;
					cp = ( cp << 6 ) | int ( c & 0x3F );
				}
				out.push_back ( whole ? cp : 0xFFFD );
				i += whole ? size_t ( length ) : 1;
			}
			return out;
		}

		// What is drawn for a code point: itself, else the face's replacement character, else a question mark; 0 for a control character,
		// which has no glyph
		[[ nodiscard ]] int resolve ( const stbtt_fontinfo& info, int cp )
		{
			if ( cp < 0x20 )
				return 0;
			if ( stbtt_FindGlyphIndex ( &info, cp ) != 0 )
				return cp;
			return stbtt_FindGlyphIndex ( &info, 0xFFFD ) != 0 ? 0xFFFD : '?';
		}

		// A size to the sixteenth of a pixel
		[[ nodiscard ]] uint64_t sizeKey ( float size )
		{
			return uint64_t ( std::lround ( size * 16.0f ) );
		}

		[[ nodiscard ]] const Metrics& metricsOf ( int fi, float size )
		{
			const auto	key = ( uint64_t ( fi ) << 60 ) | sizeKey ( size );
			auto [ it, fresh ] = atlas.metrics.try_emplace ( key );
			if ( ! fresh )
				return it->second;

			auto&		info = atlas.info[ fi ];
			auto&		m = it->second;
			int			ascent, descent, lineGap, x0, y0, x1, y1;
			m.emScale = stbtt_ScaleForMappingEmToPixels ( &info, size );
			stbtt_GetFontVMetrics ( &info, &ascent, &descent, &lineGap );
			stbtt_GetCodepointBox ( &info, 'H', &x0, &y0, &x1, &y1 );
			m.ascent = float ( ascent ) * m.emScale;
			m.descent = float ( descent ) * m.emScale;
			m.capHeight = float ( y1 ) * m.emScale;
			for ( int d = '0'; d <= '9'; ++d )
			{
				int	advance, bearing;
				stbtt_GetCodepointHMetrics ( &info, d, &advance, &bearing );
				m.digitAdvance = std::max ( m.digitAdvance, float ( advance ) * m.emScale );
			}
			return m;
		}

		[[ nodiscard ]] bool isDigit ( int c )	{ return c >= '0' && c <= '9'; }

		// Walks the text glyph by glyph: each one's code point as drawn and the pen's x before it; the width of it all
		template <typename Visit>
		float layout ( int fi, const Metrics& m, float x, std::string_view text, bool tabular, Visit&& visit )
		{
			const auto&	info = atlas.info[ fi ];
			const auto	start = x;
			auto		prev = 0;
			auto		isPrevDigit = false;

			for ( const auto original : codePoints ( text ) )
			{
				const auto	cp = resolve ( info, original );
				if ( cp == 0 )
				{
					prev = 0;
					continue;
				}

				// Tabular: no kerning on either side of a digit, else the text after a changing number moves with its digits
				const auto	glyph = stbtt_FindGlyphIndex ( &info, cp );
				const auto	isCurrentDigit = isDigit ( cp );
				if ( prev != 0 && ! ( tabular && ( isCurrentDigit || isPrevDigit ) ) )
					x += float ( stbtt_GetGlyphKernAdvance ( &info, prev, glyph ) ) * m.emScale;

				int	advanceUnits, bearing;
				stbtt_GetGlyphHMetrics ( &info, glyph, &advanceUnits, &bearing );
				const auto	own = float ( advanceUnits ) * m.emScale;
				const auto	cell = tabular && isCurrentDigit;
				const auto	advance = cell ? m.digitAdvance : own;
				const auto	shift = cell ? ( advance - own ) * 0.5f : 0.0f;

				visit ( cp, x + shift );
				x += advance;
				prev = glyph;
				isPrevDigit = isCurrentDigit;
			}

			return x - start;
		}

		// Rows of the atlas changed: every device's texture is behind by them
		void markDirty ( int from, int to )
		{
			for ( auto& [ device, d ] : atlas.devices )
			{
				d.dirtyFrom = std::min ( d.dirtyFrom, from );
				d.dirtyTo = std::max ( d.dirtyTo, to );
			}
		}

		void startOver ()
		{
			atlas.entries.clear ();
			std::ranges::fill ( atlas.rgba, 0x00FFFFFFu );
			atlas.cursorX = atlas.cursorY = atlas.rowHeight = 0;
			atlas.isFull = false;
			markDirty ( 0, atlasHeight );
		}

		// The glyph of that code point in that size, rasterized into the atlas the first time; null for one with no pixels and when the
		// atlas has no room (it starts over at the next frame)
		[[ nodiscard ]] const Entry* entryOf ( int fi, float size, int cp )
		{
			const auto	key = ( uint64_t ( fi ) << 60 ) | ( sizeKey ( size ) << 32 ) | uint64_t ( cp );
			const auto	found = atlas.entries.find ( key );
			if ( found != atlas.entries.end () )
				return found->second.width > 0 ? &found->second : nullptr;

			auto&		info = atlas.info[ fi ];
			const auto	scale = stbtt_ScaleForMappingEmToPixels ( &info, size );
			const auto	glyph = stbtt_FindGlyphIndex ( &info, cp );
			int			x0, y0, x1, y1;
			stbtt_GetGlyphBitmapBox ( &info, glyph, scale, scale, &x0, &y0, &x1, &y1 );

			Entry		entry;
			entry.width = x1 - x0;
			entry.height = y1 - y0;
			entry.xoff = float ( x0 );
			entry.yoff = float ( y0 );

			if ( entry.width > 0 && entry.height > 0 )
			{
				// A pixel round each glyph, so a texture sample never reaches its neighbour
				if ( atlas.cursorX + entry.width + 2 > atlasWidth )
				{
					atlas.cursorX = 0;
					atlas.cursorY += atlas.rowHeight;
					atlas.rowHeight = 0;
				}
				if ( entry.width + 2 > atlasWidth || atlas.cursorY + entry.height + 2 > atlasHeight )
				{
					atlas.isFull = true;
					return nullptr;
				}

				entry.x = atlas.cursorX + 1;
				entry.y = atlas.cursorY + 1;
				atlas.cursorX += entry.width + 2;
				atlas.rowHeight = std::max ( atlas.rowHeight, entry.height + 2 );

				std::vector<unsigned char>	pixels ( size_t ( entry.width ) * size_t ( entry.height ) );
				stbtt_MakeGlyphBitmap ( &info, pixels.data (), entry.width, entry.height, entry.width, scale, scale, glyph );
				for ( int row = 0; row < entry.height; ++row )
					for ( int column = 0; column < entry.width; ++column )
						atlas.rgba[ size_t ( entry.y + row ) * size_t ( atlasWidth ) + size_t ( entry.x + column ) ] =
							0x00FFFFFFu | ( uint32_t ( pixels[ size_t ( row ) * size_t ( entry.width ) + size_t ( column ) ] ) << 24 );
				markDirty ( entry.y, entry.y + entry.height );
			}

			const auto&	stored = atlas.entries.emplace ( key, entry ).first->second;
			return stored.width > 0 ? &stored : nullptr;
		}
	}

	void forgetDevice ( api::device* device )
	{
		if ( auto it = atlas.devices.find ( device ); it != atlas.devices.end () )
		{
			if ( atlas.current == &it->second )
				atlas.current = nullptr;

			device->destroy_resource_view ( it->second.view );
			device->destroy_resource ( it->second.texture );
			atlas.devices.erase ( it );
		}
	}

	std::span<const unsigned char> faceData ( Face face )
	{
		std::call_once ( atlas.loaded, loadFaces );
		const auto	i = faceIndex ( face );
		if ( i < 0 )
			return {};
		return { atlas.data[ i ], size_t ( atlas.sizes[ i ] ) };
	}

	void prepare ( api::device* device )
	{
		std::call_once ( atlas.loaded, loadFaces );

		if ( atlas.isFull )
			startOver ();

		atlas.current = nullptr;
		if ( device == nullptr || faceIndex ( Face::regular ) < 0 )
			return;

		auto [ it, fresh ] = atlas.devices.try_emplace ( device );
		auto&	d = it->second;
		if ( fresh )
		{
			const auto	format = api::format::r8g8b8a8_unorm;
			const api::subresource_data	data { atlas.rgba.data (), uint32_t ( atlasWidth ) * 4, uint32_t ( atlasWidth ) * uint32_t ( atlasHeight ) * 4 };
			if ( ! device->create_resource ( api::resource_desc ( uint32_t ( atlasWidth ), uint32_t ( atlasHeight ), 1, 1, format, 1, api::memory_heap::default_,
																  api::resource_usage::shader_resource | api::resource_usage::copy_dest ),
											 &data, api::resource_usage::shader_resource, &d.texture )
				 || ! device->create_resource_view ( d.texture, api::resource_usage::shader_resource, api::resource_view_desc ( format ), &d.view ) )
			{
				if ( d.texture.handle != 0 )
					device->destroy_resource ( d.texture );
				atlas.devices.erase ( it );
				return;
			}
		}

		atlas.current = &d;
	}

	void flush ( api::command_list* cmdList, api::device* device )
	{
		const auto	it = atlas.devices.find ( device );
		if ( it == atlas.devices.end () || it->second.dirtyTo <= it->second.dirtyFrom )
			return;

		auto&	d = it->second;
		api::subresource_box	box;
		box.left = 0;
		box.top = d.dirtyFrom;
		box.front = 0;
		box.right = atlasWidth;
		box.bottom = d.dirtyTo;
		box.back = 1;

		const auto	rows = uint32_t ( d.dirtyTo - d.dirtyFrom );
		cmdList->barrier ( d.texture, api::resource_usage::shader_resource, api::resource_usage::copy_dest );
		cmdList->update_texture_region ( api::subresource_data { &atlas.rgba[ size_t ( d.dirtyFrom ) * size_t ( atlasWidth ) ], uint32_t ( atlasWidth ) * 4, uint32_t ( atlasWidth ) * 4 * rows },
										 d.texture, 0, &box );
		cmdList->barrier ( d.texture, api::resource_usage::copy_dest, api::resource_usage::shader_resource );

		d.dirtyFrom = atlasHeight;
		d.dirtyTo = 0;
	}

	float draw ( ImDrawList* drawList, ImVec2 pos, Face face, float size, ImU32 color, std::string_view text, bool tabular )
	{
		const auto	fi = faceIndex ( face );
		if ( fi < 0 || atlas.current == nullptr )
			return 0.0f;

		const auto&	m = metricsOf ( fi, size );
		const auto	baseline = std::round ( pos.y + m.ascent );
		const auto	w = float ( atlasWidth );
		const auto	h = float ( atlasHeight );

		struct Quad
		{
			const Entry*	entry;
			float			x;
		};
		static std::vector<Quad>	quads;
		quads.clear ();

		const auto	advance = layout ( fi, m, std::round ( pos.x ), text, tabular, [ & ] ( int cp, float x )
		{
			if ( const auto* entry = entryOf ( fi, size, cp ) )
				quads.push_back ( { entry, std::round ( x ) } );
		} );

		const auto	count = int ( quads.size () );
		drawList->PushTexture ( ImTextureRef ( ImTextureID ( atlas.current->view.handle ) ) );
		drawList->PrimReserve ( count * 6, count * 4 );
		for ( const auto& q : quads )
			drawList->PrimRectUV ( ImVec2 ( q.x + q.entry->xoff, baseline + q.entry->yoff ), ImVec2 ( q.x + q.entry->xoff + float ( q.entry->width ), baseline + q.entry->yoff + float ( q.entry->height ) ),
								   ImVec2 ( float ( q.entry->x ) / w, float ( q.entry->y ) / h ), ImVec2 ( float ( q.entry->x + q.entry->width ) / w, float ( q.entry->y + q.entry->height ) / h ), color );
		drawList->PopTexture ();
		return advance;
	}

	float width ( Face face, float size, std::string_view text, bool tabular )
	{
		const auto	fi = faceIndex ( face );
		if ( fi < 0 )
			return 0.0f;

		return layout ( fi, metricsOf ( fi, size ), 0.0f, text, tabular, [] ( int, float ) {} );
	}

	float lineHeight ( Face face, float size )
	{
		const auto	fi = faceIndex ( face );
		if ( fi < 0 )
			return size;

		const auto&	m = metricsOf ( fi, size );
		return m.ascent - m.descent;
	}

	float centredTop ( Face face, float size, float top, float height )
	{
		const auto	fi = faceIndex ( face );
		if ( fi < 0 )
			return top + ( height - size ) * 0.5f;

		const auto&	m = metricsOf ( fi, size );
		return top + ( height + m.capHeight ) * 0.5f - m.ascent;
	}

	namespace
	{
		// The face's ascent and descent (below 0) in font units: its typographic ones where it has them, as browsers take them
		void verticalMetrics ( const stbtt_fontinfo& info, int& ascent, int& descent )
		{
			int	gap = 0;
			if ( ! stbtt_GetFontVMetricsOS2 ( &info, &ascent, &descent, &gap ) )
				stbtt_GetFontVMetrics ( &info, &ascent, &descent, &gap );
		}

		// The glyphs the face has, with the pen's position before each, from 0; their whole advance
		[[ nodiscard ]] float placeGlyphs ( const stbtt_fontinfo& info, float scale, const std::vector<int>& cps, std::vector<std::pair<int, float>>& placed )
		{
			placed.clear ();
			auto	pen = 0.0f;
			auto	prev = 0;
			for ( const auto cp : cps )
			{
				const auto	glyph = stbtt_FindGlyphIndex ( &info, resolve ( info, cp ) );
				if ( glyph == 0 )
					continue;
				if ( prev != 0 )
					pen += float ( stbtt_GetGlyphKernAdvance ( &info, prev, glyph ) ) * scale;
				placed.emplace_back ( glyph, pen );
				int	advance = 0;
				int	bearing = 0;
				stbtt_GetGlyphHMetrics ( &info, glyph, &advance, &bearing );
				pen += float ( advance ) * scale;
				prev = glyph;
			}
			return pen;
		}

		// The glyphs placed as placeGlyphs places them, cut as CSS's text-overflow: ellipsis does where wider than maxWidth, with the
		// face's ellipsis where it has one; their whole advance
		[[ nodiscard ]] float placeCut ( const stbtt_fontinfo& info, float scale, std::string_view text, float maxWidth, std::vector<std::pair<int, float>>& placed )
		{
			auto	cps = codePoints ( text );
			auto	wide = placeGlyphs ( info, scale, cps, placed );
			if ( wide > maxWidth )
			{
				const std::vector<int>	ellipsis = stbtt_FindGlyphIndex ( &info, 0x2026 ) != 0 ? std::vector<int> { 0x2026 } : std::vector<int> { '.', '.', '.' };
				while ( ! cps.empty () )
				{
					cps.pop_back ();
					while ( ! cps.empty () && cps.back () == ' ' )
						cps.pop_back ();
					auto	cut = cps;
					cut.insert ( cut.end (), ellipsis.begin (), ellipsis.end () );
					wide = placeGlyphs ( info, scale, cut, placed );
					if ( wide <= maxWidth )
						break;
				}
			}
			return wide;
		}

		// A pixel to spare on either side for the glyphs' overhang
		constexpr int	margin = 1;

		[[ nodiscard ]] int lineWidth ( float wide )
		{
			return int ( std::ceil ( wide ) ) + 2 * margin;
		}
	}

	PlacedLine placeLine ( Face face, float size, std::string_view text, float maxWidth )
	{
		std::call_once ( atlas.loaded, loadFaces );
		const auto	fi = faceIndex ( face );
		if ( fi < 0 || size <= 0.0f )
			return {};

		const auto&	info = atlas.info[ fi ];
		PlacedLine	l;
		l.face = fi;
		l.scale = stbtt_ScaleForMappingEmToPixels ( &info, size );

		std::vector<std::pair<int, float>>	placed;
		l.width = lineWidth ( placeCut ( info, l.scale, text, maxWidth, placed ) );
		l.glyphs.reserve ( placed.size () );
		for ( const auto& [ glyph, pen ] : placed )
			l.glyphs.push_back ( { glyph, pen } );

		int	ascent = 0;
		int	descent = 0;
		verticalMetrics ( info, ascent, descent );
		l.ascent = int ( std::ceil ( float ( ascent ) * l.scale ) );
		l.height = l.ascent + int ( std::ceil ( float ( -descent ) * l.scale ) );
		return l;
	}

	namespace
	{
		// rasterizeGlyph's glyphs as rasterized, for the add-on's lifetime: at most this many bytes of pixels, dropped a whole size
		// (face and scale) at a time, the least recently used first
		constexpr size_t	glyphCacheLimit = size_t ( 4 ) << 20;

		struct CachedGlyph
		{
			int						x0 = 0;
			int						y0 = 0;
			int						width = 0;
			int						height = 0;
			std::vector<uint8_t>	pixels;
		};

		struct CachedSize
		{
			std::unordered_map<uint32_t, CachedGlyph>	glyphs;	// by glyph index and quarter pixel shift
			size_t										bytes = 0;
			uint64_t									used = 0;
		};

		struct GlyphCache
		{
			std::mutex								lock;
			std::unordered_map<uint64_t, CachedSize>	sizes;
			size_t									bytes = 0;
			uint64_t								uses = 0;
		} glyphCache;

		// Under glyphCache.lock
		void makeRoom ( size_t bytes, uint64_t keep )
		{
			while ( glyphCache.bytes + bytes > glyphCacheLimit )
			{
				auto	oldest = glyphCache.sizes.end ();
				for ( auto it = glyphCache.sizes.begin (); it != glyphCache.sizes.end (); ++it )
					if ( it->first != keep && ( oldest == glyphCache.sizes.end () || it->second.used < oldest->second.used ) )
						oldest = it;
				if ( oldest == glyphCache.sizes.end () )
					return;
				glyphCache.bytes -= oldest->second.bytes;
				glyphCache.sizes.erase ( oldest );
			}
		}
	}

	bool rasterizeGlyph ( const PlacedLine& line, const Glyph& g, uint8_t* coverage, int stride, int rows, int left, int top )
	{
		const auto&	info = atlas.info[ line.face ];
		// The pen rounded to quarter pixels, so glyphs repeat in the cache: each sits at most 1/8 px from its exact place
		const auto	x = std::round ( ( float ( margin ) + g.pen ) * 4.0f ) * 0.25f;
		const auto	whole = std::floor ( x );
		const auto	quarter = int ( ( x - whole ) * 4.0f );
		const auto	shift = float ( quarter ) * 0.25f;

		std::scoped_lock	lock ( glyphCache.lock );
		const auto			sizeKey = ( uint64_t ( uint32_t ( line.face ) ) << 32 ) | std::bit_cast<uint32_t> ( line.scale );
		const auto			key = uint32_t ( g.index ) * 4u + uint32_t ( quarter );
		auto*				size = &glyphCache.sizes[ sizeKey ];
		size->used = ++glyphCache.uses;
		auto				found = size->glyphs.find ( key );
		const auto			isHit = found != size->glyphs.end ();
		if ( ! isHit )
		{
			CachedGlyph	c;
			int			x1 = 0, y1 = 0;
			stbtt_GetGlyphBitmapBoxSubpixel ( &info, g.index, line.scale, line.scale, shift, 0.0f, &c.x0, &c.y0, &x1, &y1 );
			c.width = std::max ( 0, x1 - c.x0 );
			c.height = std::max ( 0, y1 - c.y0 );
			if ( c.width > 0 && c.height > 0 )
			{
				c.pixels.assign ( size_t ( c.width ) * size_t ( c.height ), 0 );
				stbtt_MakeGlyphBitmapSubpixel ( &info, c.pixels.data (), c.width, c.height, c.width, line.scale, line.scale, shift, 0.0f, g.index );
			}
			const auto	bytes = c.pixels.size ();
			makeRoom ( bytes, sizeKey );
			size = &glyphCache.sizes[ sizeKey ];
			size->bytes += bytes;
			glyphCache.bytes += bytes;
			found = size->glyphs.emplace ( key, std::move ( c ) ).first;
		}

		const auto&	c = found->second;
		for ( int gy = 0; gy < c.height; ++gy )
		{
			const auto	ly = line.ascent + c.y0 + gy;
			const auto	ty = top + ly;
			if ( ly < 0 || ly >= line.height || ty < 0 || ty >= rows )
				continue;
			for ( int gx = 0; gx < c.width; ++gx )
			{
				const auto	lx = int ( whole ) + c.x0 + gx;
				const auto	tx = left + lx;
				if ( lx < 0 || lx >= line.width || tx < 0 || tx >= stride )
					continue;
				auto&	to = coverage[ size_t ( ty ) * size_t ( stride ) + size_t ( tx ) ];
				to = std::max ( to, c.pixels[ size_t ( gy ) * size_t ( c.width ) + size_t ( gx ) ] );
			}
		}
		return isHit;
	}

	size_t cachedGlyphBytes ()
	{
		std::scoped_lock	lock ( glyphCache.lock );
		return glyphCache.bytes;
	}

	Line rasterize ( Face face, float size, std::string_view text, float maxWidth )
	{
		std::call_once ( atlas.loaded, loadFaces );
		const auto	fi = faceIndex ( face );
		if ( fi < 0 || size <= 0.0f )
			return {};

		const auto&	info = atlas.info[ fi ];
		const auto	scale = stbtt_ScaleForMappingEmToPixels ( &info, size );

		std::vector<std::pair<int, float>>	placed;
		const auto							wide = placeCut ( info, scale, text, maxWidth, placed );

		int	ascent = 0;
		int	descent = 0;
		verticalMetrics ( info, ascent, descent );

		Line	l;
		l.ascent = int ( std::ceil ( float ( ascent ) * scale ) );
		l.height = l.ascent + int ( std::ceil ( float ( -descent ) * scale ) );
		l.width = lineWidth ( wide );
		l.coverage.assign ( size_t ( l.width ) * size_t ( l.height ), 0 );

		std::vector<uint8_t>	glyphPixels;
		for ( const auto& [ glyph, pen ] : placed )
		{
			const auto	x = float ( margin ) + pen;
			const auto	whole = std::floor ( x );
			const auto	shift = x - whole;
			int			x0 = 0, y0 = 0, x1 = 0, y1 = 0;
			stbtt_GetGlyphBitmapBoxSubpixel ( &info, glyph, scale, scale, shift, 0.0f, &x0, &y0, &x1, &y1 );
			const auto	w = x1 - x0;
			const auto	h = y1 - y0;
			if ( w <= 0 || h <= 0 )
				continue;

			glyphPixels.assign ( size_t ( w ) * size_t ( h ), 0 );
			stbtt_MakeGlyphBitmapSubpixel ( &info, glyphPixels.data (), w, h, w, scale, scale, shift, 0.0f, glyph );
			for ( int gy = 0; gy < h; ++gy )
			{
				const auto	ty = l.ascent + y0 + gy;
				if ( ty < 0 || ty >= l.height )
					continue;
				for ( int gx = 0; gx < w; ++gx )
				{
					const auto	tx = int ( whole ) + x0 + gx;
					if ( tx < 0 || tx >= l.width )
						continue;
					auto&	to = l.coverage[ size_t ( ty ) * size_t ( l.width ) + size_t ( tx ) ];
					to = std::max ( to, glyphPixels[ size_t ( gy ) * size_t ( w ) + size_t ( gx ) ] );
				}
			}
		}
		return l;
	}

	float baselineInLine ( Face face, float size, float lineHeight )
	{
		std::call_once ( atlas.loaded, loadFaces );
		const auto	fi = faceIndex ( face );
		if ( fi < 0 )
			return size * lineHeight * 0.8f;

		const auto&	info = atlas.info[ fi ];
		const auto	scale = stbtt_ScaleForMappingEmToPixels ( &info, size );
		int			ascent = 0;
		int			descent = 0;
		verticalMetrics ( info, ascent, descent );
		return ( lineHeight * size - float ( ascent - descent ) * scale ) * 0.5f + float ( ascent ) * scale;
	}
}
