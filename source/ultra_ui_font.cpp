/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <bit>
#include <cmath>
#include <mutex>
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
#include "ultra_ui_texture.hpp"
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

		// What the panel draws, baked at these sizes; any other size scales the nearest of its face
		struct Bake
		{
			Face	face;
			float	size;
		};

		constexpr Bake	bakes[] = {
			{ Face::regular, theme::sizes::body },		{ Face::regular, theme::sizes::tip },
			{ Face::medium, theme::sizes::heading },
			{ Face::semibold, theme::sizes::choice },	{ Face::semibold, theme::sizes::key },	{ Face::semibold, theme::sizes::title },
			{ Face::bold, theme::sizes::value },		{ Face::bold, theme::sizes::info },
		};

		// Printable ASCII: every string the panel draws is ours or a game name ultraGame made ASCII
		constexpr int	firstChar = 0x20;
		constexpr int	charCount = 0x7F - firstChar;

		struct Baked
		{
			Face	face;
			float	size;
			float	emScale;
			float	ascent;
			float	descent;
			float	capHeight;
			float	digitAdvance;
			std::vector<stbtt_packedchar>	chars;
		};

		struct Atlas
		{
			std::once_flag							loaded;
			stbtt_fontinfo							info[ faceCount ] {};
			const unsigned char*					data[ faceCount ] {};
			size_t									sizes[ faceCount ] {};
			float									scale = 0.0f;
			std::vector<Baked>						baked;
			int										width = 0;
			int										height = 0;
			std::vector<uint32_t>					rgba;
			std::unordered_map<api::device*, Texture>	textures;
			const Texture*							current = nullptr;
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

		[[ nodiscard ]] bool pack ( int dim, float scale, std::vector<unsigned char>& alpha )
		{
			alpha.assign ( size_t ( dim ) * size_t ( dim ), 0 );

			stbtt_pack_context	context;
			if ( ! stbtt_PackBegin ( &context, alpha.data (), dim, dim, 0, 1, nullptr ) )
				return false;

			stbtt_PackSetOversampling ( &context, 2, 1 );
			atlas.baked.clear ();
			atlas.baked.reserve ( std::size ( bakes ) );

			auto	fits = true;
			for ( const auto& bake : bakes )
			{
				const auto	fi = faceIndex ( bake.face );
				if ( fi < 0 )
					continue;

				auto&		info = atlas.info[ fi ];
				Baked		b { bake.face, bake.size * scale };
				b.emScale = stbtt_ScaleForMappingEmToPixels ( &info, b.size );

				b.chars.resize ( size_t ( charCount ) );

				if ( ! stbtt_PackFontRange ( &context, atlas.data[ fi ], 0, STBTT_POINT_SIZE ( b.size ), firstChar, charCount, b.chars.data () ) )
				{
					fits = false;
					break;
				}

				int	ascent, descent, lineGap, x0, y0, x1, y1;
				stbtt_GetFontVMetrics ( &info, &ascent, &descent, &lineGap );
				stbtt_GetCodepointBox ( &info, 'H', &x0, &y0, &x1, &y1 );
				b.ascent = float ( ascent ) * b.emScale;
				b.descent = float ( descent ) * b.emScale;
				b.capHeight = float ( y1 ) * b.emScale;
				b.digitAdvance = 0.0f;
				for ( int d = '0'; d <= '9'; ++d )
					b.digitAdvance = std::max ( b.digitAdvance, b.chars[ size_t ( d - firstChar ) ].xadvance );

				atlas.baked.push_back ( std::move ( b ) );
			}

			stbtt_PackEnd ( &context );
			return fits;
		}

		void bake ( float scale )
		{
			atlas.scale = scale;
			atlas.baked.clear ();
			atlas.rgba.clear ();

			for ( auto& [ device, texture ] : atlas.textures )
				releaseTexture ( texture );
			atlas.textures.clear ();
			atlas.current = nullptr;

			if ( faceIndex ( Face::regular ) < 0 )
				return;

			std::vector<unsigned char>	alpha;
			for ( int dim = 512; dim <= 4096; dim *= 2 )
			{
				if ( ! pack ( dim, scale, alpha ) )
					continue;

				atlas.width = dim;
				atlas.height = dim;
				atlas.rgba.resize ( alpha.size () );
				std::ranges::transform ( alpha, atlas.rgba.begin (), [] ( unsigned char a ) { return 0x00FFFFFFu | ( uint32_t ( a ) << 24 ); } );
				return;
			}

			atlas.baked.clear ();
		}

		[[ nodiscard ]] const Baked* nearest ( Face face, float size )
		{
			if ( atlas.current == nullptr )
				return nullptr;

			const Baked*	best = nullptr;
			for ( const auto& b : atlas.baked )
				if ( b.face == face && ( best == nullptr || std::abs ( b.size - size ) < std::abs ( best->size - size ) ) )
					best = &b;

			if ( best == nullptr && face != Face::regular )
				return nearest ( Face::regular, size );

			return best;
		}

		[[ nodiscard ]] const stbtt_packedchar* glyph ( const Baked& b, int c )
		{
			return c >= firstChar && c < firstChar + charCount ? &b.chars[ size_t ( c - firstChar ) ] : nullptr;
		}

		[[ nodiscard ]] bool isDigit ( int c )	{ return c >= '0' && c <= '9'; }

		// Walks the text glyph by glyph: the pen's x after each, and the glyph's quad when it has one
		template <typename Visit>
		float layout ( const Baked& b, float size, float x, std::string_view text, bool tabular, Visit&& visit )
		{
			const auto	k = size / b.size;
			const auto	fi = faceIndex ( b.face );
			const auto	start = x;
			auto		prev = 0;

			for ( const auto ch : text )
			{
				const auto	cp = int ( uint8_t ( ch ) );
				const auto*	pc = glyph ( b, cp );

				if ( pc == nullptr )
				{
					prev = 0;
					continue;
				}

				// Tabular: no kerning on either side of a digit, else the text after a changing number moves with its digits
				const auto	cell = tabular && isDigit ( cp );
				if ( prev != 0 && ! ( tabular && ( isDigit ( cp ) || isDigit ( prev ) ) ) )
					x += float ( stbtt_GetCodepointKernAdvance ( &atlas.info[ fi ], prev, cp ) ) * b.emScale * k;

				const auto	advance = cell ? b.digitAdvance * k : pc->xadvance * k;
				const auto	shift = cell ? ( advance - pc->xadvance * k ) * 0.5f : 0.0f;

				visit ( *pc, x + shift, k );
				x += advance;
				prev = cp;
			}

			return x - start;
		}
	}

	void forgetDevice ( api::device* device )
	{
		if ( auto it = atlas.textures.find ( device ); it != atlas.textures.end () )
		{
			if ( atlas.current == &it->second )
				atlas.current = nullptr;

			destroyTextureNow ( it->second );
			atlas.textures.erase ( it );
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

		if ( const auto scale = theme::scale (); scale != atlas.scale )
			bake ( scale );

		atlas.current = nullptr;
		if ( atlas.baked.empty () || device == nullptr )
			return;

		auto&	texture = atlas.textures[ device ];
		if ( ! texture.valid () )
			texture = createTexture ( device, atlas.width, atlas.height, atlas.rgba.data () );

		if ( texture.valid () )
			atlas.current = &texture;
	}

	float draw ( ImDrawList* drawList, ImVec2 pos, Face face, float size, ImU32 color, std::string_view text, bool tabular )
	{
		const auto*	b = nearest ( face, size );
		if ( b == nullptr )
			return 0.0f;

		const auto	glyphs = int ( text.size () );
		const auto	baseline = std::round ( pos.y + b->ascent * size / b->size );
		const auto	w = float ( atlas.width );
		const auto	h = float ( atlas.height );
		int			used = 0;

		drawList->PushTexture ( atlas.current->ref () );
		drawList->PrimReserve ( glyphs * 6, glyphs * 4 );

		const auto	advance = layout ( *b, size, std::round ( pos.x ), text, tabular, [ & ] ( const stbtt_packedchar& pc, float x, float k )
		{
			if ( pc.x1 == pc.x0 )
				return;

			drawList->PrimRectUV ( ImVec2 ( x + pc.xoff * k, baseline + pc.yoff * k ), ImVec2 ( x + pc.xoff2 * k, baseline + pc.yoff2 * k ),
								   ImVec2 ( float ( pc.x0 ) / w, float ( pc.y0 ) / h ), ImVec2 ( float ( pc.x1 ) / w, float ( pc.y1 ) / h ), color );
			++used;
		} );

		drawList->PrimUnreserve ( ( glyphs - used ) * 6, ( glyphs - used ) * 4 );
		drawList->PopTexture ();
		return advance;
	}

	float width ( Face face, float size, std::string_view text, bool tabular )
	{
		const auto*	b = nearest ( face, size );
		if ( b == nullptr )
			return 0.0f;

		return layout ( *b, size, 0.0f, text, tabular, [] ( const stbtt_packedchar&, float, float ) {} );
	}

	float lineHeight ( Face face, float size )
	{
		const auto*	b = nearest ( face, size );
		return b != nullptr ? ( b->ascent - b->descent ) * size / b->size : size;
	}

	float centredTop ( Face face, float size, float top, float height )
	{
		const auto*	b = nearest ( face, size );
		if ( b == nullptr )
			return top + ( height - size ) * 0.5f;

		const auto	k = size / b->size;
		return top + ( height + b->capHeight * k ) * 0.5f - b->ascent * k;
	}

	namespace
	{
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
				const auto	glyph = stbtt_FindGlyphIndex ( &info, cp );
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
