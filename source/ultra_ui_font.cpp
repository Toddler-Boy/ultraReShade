/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <bit>
#include <cmath>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Windows.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include "ultra_ui_font.hpp"

extern HMODULE g_module_handle;

//-----------------------------------------------------------------------------

namespace ultra::ui::font
{
	using Microsoft::WRL::ComPtr;

	namespace
	{
		// The face's four weights are one variable font, a resource of the DLL, set to the weight axis' values
		constexpr int	weightCount = int ( Face::count );
		constexpr int	weights[ weightCount ] = { 400, 500, 600, 700 };

		// The atlas of the glyphs drawn so far, each in the size, quarter pixel and face it was drawn in, made the first time it is drawn.
		// Full, it starts over at the next frame
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

		// What one device holds of the atlas: the rows from dirtyFrom up to dirtyTo are newer than its texture
		struct DeviceAtlas
		{
			api::resource		texture {};
			api::resource_view	view {};
			int					dirtyFrom = atlasHeight;
			int					dirtyTo = 0;
		};

		// A face, Inter's weight or a system font that has what Inter lacks, with the vertical metrics of its OS/2 table in font units
		struct FaceEntry
		{
			ComPtr<IDWriteFontFace>		face;
			float						unitsPerEm = 1.0f;
			float						ascent = 0.0f;
			float						descent = 0.0f;
			float						capHeight = 0.0f;
		};

		// One glyph of shaped text: the face's slot in the high half of its index, the pen's x before it
		struct ShapedGlyph
		{
			uint32_t	index = 0;
			float		pen = 0.0f;
		};

		struct Shaped
		{
			std::vector<ShapedGlyph>	glyphs;
			float						width = 0.0f;
		};

		struct Bitmap
		{
			int						left = 0;
			int						top = 0;
			int						width = 0;
			int						height = 0;
			std::vector<uint8_t>	alpha;
		};

		struct Engine
		{
			std::once_flag									loaded;
			std::mutex										lock;
			ComPtr<IDWriteFactory3>							factory;
			ComPtr<IDWriteTextAnalyzer>						analyzer;
			ComPtr<IDWriteFontFallback>						fallback;
			const unsigned char*							data = nullptr;
			size_t											size = 0;
			std::vector<FaceEntry>							faces;			// the four weights, then the system fonts that came in
			std::unordered_map<std::wstring, int>			systemSlots;	// by family and weight
			std::unordered_map<std::string, Shaped>			shapes;

			std::unordered_map<uint64_t, Entry>				entries;
			std::vector<uint32_t>							rgba = std::vector<uint32_t> ( size_t ( atlasWidth ) * size_t ( atlasHeight ), 0x00FFFFFFu );
			int												cursorX = 0;
			int												cursorY = 0;
			int												rowHeight = 0;
			bool											isFull = false;
			std::unordered_map<api::device*, DeviceAtlas>	devices;
			const DeviceAtlas*								current = nullptr;
		} engine;

		//-------------------------------------------------------------------------
		// DirectWrite's text sources: the text as it asks for it, and the script ranges it finds

		class TextSource final : public IDWriteTextAnalysisSource
		{
		public:
			TextSource ( const wchar_t* text, UINT32 length ) : text ( text ), length ( length ) {}

			HRESULT STDMETHODCALLTYPE QueryInterface ( REFIID id, void** out ) override
			{
				if ( id == __uuidof ( IUnknown ) || id == __uuidof ( IDWriteTextAnalysisSource ) )
				{
					*out = static_cast<IDWriteTextAnalysisSource*> ( this );
					return S_OK;
				}
				*out = nullptr;
				return E_NOINTERFACE;
			}

			ULONG STDMETHODCALLTYPE AddRef () override	{ return 1; }
			ULONG STDMETHODCALLTYPE Release () override	{ return 1; }

			HRESULT STDMETHODCALLTYPE GetTextAtPosition ( UINT32 position, const WCHAR** out, UINT32* count ) override
			{
				*out = position < length ? text + position : nullptr;
				*count = position < length ? length - position : 0;
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE GetTextBeforePosition ( UINT32 position, const WCHAR** out, UINT32* count ) override
			{
				*out = position > 0 && position <= length ? text : nullptr;
				*count = position <= length ? position : 0;
				return S_OK;
			}

			DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection () override	{ return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT; }

			HRESULT STDMETHODCALLTYPE GetLocaleName ( UINT32 position, UINT32* count, const WCHAR** locale ) override
			{
				*count = position < length ? length - position : 0;
				*locale = L"";
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE GetNumberSubstitution ( UINT32 position, UINT32* count, IDWriteNumberSubstitution** substitution ) override
			{
				*count = position < length ? length - position : 0;
				*substitution = nullptr;
				return S_OK;
			}

		private:
			const wchar_t*	text;
			UINT32			length;
		};

		struct ScriptRange
		{
			UINT32					position;
			UINT32					length;
			DWRITE_SCRIPT_ANALYSIS	script;
		};

		class ScriptSink final : public IDWriteTextAnalysisSink
		{
		public:
			std::vector<ScriptRange>	ranges;

			HRESULT STDMETHODCALLTYPE QueryInterface ( REFIID id, void** out ) override
			{
				if ( id == __uuidof ( IUnknown ) || id == __uuidof ( IDWriteTextAnalysisSink ) )
				{
					*out = static_cast<IDWriteTextAnalysisSink*> ( this );
					return S_OK;
				}
				*out = nullptr;
				return E_NOINTERFACE;
			}

			ULONG STDMETHODCALLTYPE AddRef () override	{ return 1; }
			ULONG STDMETHODCALLTYPE Release () override	{ return 1; }

			HRESULT STDMETHODCALLTYPE SetScriptAnalysis ( UINT32 position, UINT32 length, const DWRITE_SCRIPT_ANALYSIS* script ) override
			{
				ranges.push_back ( { position, length, *script } );
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE SetLineBreakpoints ( UINT32, UINT32, const DWRITE_LINE_BREAKPOINT* ) override	{ return S_OK; }
			HRESULT STDMETHODCALLTYPE SetBidiLevel ( UINT32, UINT32, UINT8, UINT8 ) override							{ return S_OK; }
			HRESULT STDMETHODCALLTYPE SetNumberSubstitution ( UINT32, UINT32, IDWriteNumberSubstitution* ) override	{ return S_OK; }
		};

		//-------------------------------------------------------------------------
		// The faces

		[[ nodiscard ]] uint16_t be16 ( const uint8_t* p )	{ return uint16_t ( ( p[ 0 ] << 8 ) | p[ 1 ] ); }

		// Under no lock, at the first use
		void addFace ( ComPtr<IDWriteFontFace> face )
		{
			FaceEntry	e;
			e.face = face;

			ComPtr<IDWriteFontFace1>	face1;
			DWRITE_FONT_METRICS1		m {};
			if ( SUCCEEDED ( face.As ( &face1 ) ) )
			{
				face1->GetMetrics ( &m );
				e.unitsPerEm = float ( m.designUnitsPerEm );
				e.ascent = float ( m.ascent );
				e.descent = -float ( m.descent );
				e.capHeight = float ( m.capHeight );
			}

			// The typographic ascent and descent of the OS/2 table, where it has them (CSS and browsers take these)
			const void*	table = nullptr;
			UINT32		tableSize = 0;
			void*		context = nullptr;
			BOOL		exists = FALSE;
			if ( SUCCEEDED ( face->TryGetFontTable ( DWRITE_MAKE_OPENTYPE_TAG ( 'O', 'S', '/', '2' ), &table, &tableSize, &context, &exists ) ) && exists && tableSize >= 74 )
			{
				const auto*	os2 = static_cast<const uint8_t*> ( table );
				e.ascent = float ( int16_t ( be16 ( os2 + 68 ) ) );
				e.descent = float ( int16_t ( be16 ( os2 + 70 ) ) );
				if ( tableSize >= 90 && be16 ( os2 ) >= 2 && be16 ( os2 + 88 ) != 0 )
					e.capHeight = float ( int16_t ( be16 ( os2 + 88 ) ) );
			}
			if ( exists )
				face->ReleaseFontTable ( context );

			engine.faces.push_back ( std::move ( e ) );
		}

		void loadFaces ()
		{
			const auto	info = FindResourceW ( g_module_handle, L"ULTRA_FONT", MAKEINTRESOURCEW ( 10 ) );	// RT_RCDATA
			const auto	handle = info != nullptr ? LoadResource ( g_module_handle, info ) : nullptr;
			engine.data = handle != nullptr ? static_cast<const unsigned char*> ( LockResource ( handle ) ) : nullptr;
			engine.size = info != nullptr ? SizeofResource ( g_module_handle, info ) : 0;
			if ( engine.data == nullptr )
				return;

			ComPtr<IDWriteFactory6>	factory6;
			if ( FAILED ( DWriteCreateFactory ( DWRITE_FACTORY_TYPE_SHARED, __uuidof ( IDWriteFactory6 ), reinterpret_cast<IUnknown**> ( factory6.GetAddressOf () ) ) ) )
				return;

			ComPtr<IDWriteInMemoryFontFileLoader>	loader;
			ComPtr<IDWriteFontFile>					file;
			ComPtr<IDWriteFontResource>				resource;
			if ( FAILED ( factory6->CreateInMemoryFontFileLoader ( &loader ) ) || FAILED ( factory6->RegisterFontFileLoader ( loader.Get () ) )
				 || FAILED ( loader->CreateInMemoryFontFileReference ( factory6.Get (), engine.data, UINT32 ( engine.size ), nullptr, &file ) )
				 || FAILED ( factory6->CreateFontResource ( file.Get (), 0, &resource ) ) )
				return;

			for ( const auto weight : weights )
			{
				const DWRITE_FONT_AXIS_VALUE	axis { DWRITE_FONT_AXIS_TAG_WEIGHT, float ( weight ) };
				ComPtr<IDWriteFontFace5>		face;
				if ( FAILED ( resource->CreateFontFace ( DWRITE_FONT_SIMULATIONS_NONE, &axis, 1, &face ) ) )
				{
					engine.faces.clear ();
					return;
				}
				ComPtr<IDWriteFontFace>	plain;
				face.As ( &plain );
				addFace ( plain );
			}

			factory6.As ( &engine.factory );
			engine.factory->CreateTextAnalyzer ( &engine.analyzer );
			ComPtr<IDWriteFactory2>	factory2;
			if ( SUCCEEDED ( factory6.As ( &factory2 ) ) )
				factory2->GetSystemFontFallback ( &engine.fallback );
		}

		[[ nodiscard ]] bool isReady ()
		{
			std::call_once ( engine.loaded, loadFaces );
			return engine.faces.size () >= size_t ( weightCount ) && engine.analyzer != nullptr;
		}

		// Under the lock: the glyph of a code point in a face, 0 where it has none
		[[ nodiscard ]] uint16_t glyphIn ( int slot, uint32_t cp )
		{
			UINT16	glyph = 0;
			engine.faces[ size_t ( slot ) ].face->GetGlyphIndices ( &cp, 1, &glyph );
			return glyph;
		}

		// Under the lock: the system font that has the characters of a missing run (the slot it is kept in), or -1 where no installed font has them
		[[ nodiscard ]] int systemSlotFor ( const std::wstring& run, int weight, UINT32& mappedLength )
		{
			mappedLength = UINT32 ( run.size () );
			if ( engine.fallback == nullptr )
				return -1;

			TextSource				source ( run.c_str (), UINT32 ( run.size () ) );
			ComPtr<IDWriteFont>		font;
			FLOAT					scale = 1.0f;
			if ( FAILED ( engine.fallback->MapCharacters ( &source, 0, UINT32 ( run.size () ), nullptr, nullptr, DWRITE_FONT_WEIGHT ( weight ), DWRITE_FONT_STYLE_NORMAL,
														   DWRITE_FONT_STRETCH_NORMAL, &mappedLength, &font, &scale ) ) || font == nullptr )
				return -1;

			ComPtr<IDWriteFontFamily>	family;
			ComPtr<IDWriteLocalizedStrings>	names;
			WCHAR						name[ 128 ] {};
			if ( FAILED ( font->GetFontFamily ( &family ) ) || FAILED ( family->GetFamilyNames ( &names ) ) || FAILED ( names->GetString ( 0, name, 128 ) ) )
				return -1;

			const auto	key = std::wstring ( name ) + L"/" + std::to_wstring ( int ( font->GetWeight () ) ) + L"/" + std::to_wstring ( int ( font->GetStyle () ) );
			if ( const auto found = engine.systemSlots.find ( key ); found != engine.systemSlots.end () )
				return found->second;

			ComPtr<IDWriteFontFace>	face;
			if ( FAILED ( font->CreateFontFace ( &face ) ) )
				return -1;
			addFace ( face );
			const auto	slot = int ( engine.faces.size () ) - 1;
			engine.systemSlots[ key ] = slot;
			return slot;
		}

		[[ nodiscard ]] std::wstring wideOf ( uint32_t cp )
		{
			std::wstring	out;
			if ( cp >= 0x10000 )
			{
				cp -= 0x10000;
				out.push_back ( wchar_t ( 0xD800 + ( cp >> 10 ) ) );
				out.push_back ( wchar_t ( 0xDC00 + ( cp & 0x3FF ) ) );
			}
			else
				out.push_back ( wchar_t ( cp ) );
			return out;
		}

		// UTF-8 as code points; a broken sequence counts its first byte as one unknown character
		[[ nodiscard ]] std::vector<uint32_t> codePoints ( std::string_view text )
		{
			std::vector<uint32_t>	out;
			for ( size_t i = 0; i < text.size (); )
			{
				const auto	b = uint8_t ( text[ i ] );
				const auto	length = b < 0x80 ? 1 : ( b >> 5 ) == 0x6 ? 2 : ( b >> 4 ) == 0xE ? 3 : ( b >> 3 ) == 0x1E ? 4 : 0;
				auto		cp = length == 1 ? uint32_t ( b ) : length == 2 ? uint32_t ( b & 0x1F ) : length == 3 ? uint32_t ( b & 0x0F ) : uint32_t ( b & 0x07 );
				auto		whole = length > 0 && i + size_t ( length ) <= text.size ();
				for ( int k = 1; whole && k < length; ++k )
				{
					const auto	c = uint8_t ( text[ i + size_t ( k ) ] );
					whole = ( c & 0xC0 ) == 0x80;
					cp = ( cp << 6 ) | uint32_t ( c & 0x3F );
				}
				out.push_back ( whole ? cp : 0xFFFD );
				i += whole ? size_t ( length ) : 1;
			}
			return out;
		}

		// Under the lock: one run of code points in one face shaped from the pen on, its glyphs appended
		void shapeRun ( int slot, float size, const std::vector<uint32_t>& cps, bool tabular, float& pen, Shaped& out )
		{
			std::wstring	text;
			for ( const auto cp : cps )
				text += wideOf ( cp );
			if ( text.empty () )
				return;

			TextSource	source ( text.c_str (), UINT32 ( text.size () ) );
			ScriptSink	sink;
			if ( FAILED ( engine.analyzer->AnalyzeScript ( &source, 0, UINT32 ( text.size () ), &sink ) ) )
				return;

			auto*	face = engine.faces[ size_t ( slot ) ].face.Get ();
			for ( const auto& range : sink.ranges )
			{
				const auto*	part = text.c_str () + range.position;
				const auto	length = range.length;
				const auto	capacity = length * 3 / 2 + 16;

				std::vector<UINT16>							clusters ( length );
				std::vector<DWRITE_SHAPING_TEXT_PROPERTIES>	textProps ( length );
				std::vector<UINT16>							indices ( capacity );
				std::vector<DWRITE_SHAPING_GLYPH_PROPERTIES>	glyphProps ( capacity );
				UINT32										count = 0;

				// Tabular digits: Inter's own tnum, so every digit is as wide as the widest and the numbers do not shift the text after them
				DWRITE_FONT_FEATURE							tnum { DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1 };
				const DWRITE_TYPOGRAPHIC_FEATURES			features { &tnum, 1 };
				const DWRITE_TYPOGRAPHIC_FEATURES*			featureList[] { &features };
				const UINT32								featureLength = length;
				const auto**								usedFeatures = tabular ? featureList : nullptr;
				const auto*									usedLengths = tabular ? &featureLength : nullptr;
				const auto									featureRanges = tabular ? 1u : 0u;

				if ( FAILED ( engine.analyzer->GetGlyphs ( part, length, face, FALSE, FALSE, &range.script, L"", nullptr, usedFeatures, usedLengths, featureRanges, capacity,
														   clusters.data (), textProps.data (), indices.data (), glyphProps.data (), &count ) ) )
					continue;

				std::vector<float>			advances ( count );
				std::vector<DWRITE_GLYPH_OFFSET>	offsets ( count );
				if ( FAILED ( engine.analyzer->GetGlyphPlacements ( part, clusters.data (), textProps.data (), length, indices.data (), glyphProps.data (), count, face, size, FALSE, FALSE,
																	&range.script, L"", usedFeatures, usedLengths, featureRanges, advances.data (), offsets.data () ) ) )
					continue;

				for ( UINT32 i = 0; i < count; ++i )
				{
					out.glyphs.push_back ( { ( uint32_t ( slot ) << 16 ) | indices[ i ], pen + offsets[ i ].advanceOffset } );
					pen += advances[ i ];
				}
			}
		}

		// The text laid out in a weight at a size: kerning and the font's other default features, tabular digits where asked, what Inter
		// lacks from the system fonts, what no font has as Inter's replacement character
		[[ nodiscard ]] Shaped shapeNow ( int weightSlot, float size, std::string_view utf8, bool tabular )
		{
			Shaped	out;

			// Each code point's face: Inter's, a control character none, a missing one resolved over its whole run
			struct Piece
			{
				int			slot;
				uint32_t	cp;
			};
			std::vector<Piece>	pieces;
			const auto			cps = codePoints ( utf8 );
			for ( size_t i = 0; i < cps.size (); )
			{
				if ( cps[ i ] < 0x20 )
				{
					++i;
					continue;
				}
				if ( glyphIn ( weightSlot, cps[ i ] ) != 0 )
				{
					pieces.push_back ( { weightSlot, cps[ i ] } );
					++i;
					continue;
				}

				// A run Inter lacks, to the system's fallback
				std::wstring			run;
				std::vector<size_t>		starts;
				auto					j = i;
				while ( j < cps.size () && cps[ j ] >= 0x20 && glyphIn ( weightSlot, cps[ j ] ) == 0 )
				{
					starts.push_back ( run.size () );
					run += wideOf ( cps[ j ] );
					++j;
				}

				size_t	done = 0;
				while ( done < j - i )
				{
					UINT32		mapped = 0;
					const auto	rest = run.substr ( starts[ done ] );
					const auto	slot = systemSlotFor ( rest, weights[ weightSlot ], mapped );
					auto		used = size_t ( 0 );
					while ( done + used < j - i && starts[ done + used ] - starts[ done ] < mapped )
						++used;
					used = std::max ( used, size_t ( 1 ) );

					for ( size_t k = 0; k < used; ++k )
					{
						const auto	cp = cps[ i + done + k ];
						if ( slot >= 0 && glyphIn ( slot, cp ) != 0 )
							pieces.push_back ( { slot, cp } );
						else
							pieces.push_back ( { weightSlot, glyphIn ( weightSlot, 0xFFFD ) != 0 ? 0xFFFDu : uint32_t ( '?' ) } );
					}
					done += used;
				}
				i = j;
			}

			auto	pen = 0.0f;
			for ( size_t i = 0; i < pieces.size (); )
			{
				auto	end = i;
				std::vector<uint32_t>	run;
				while ( end < pieces.size () && pieces[ end ].slot == pieces[ i ].slot )
					run.push_back ( pieces[ end++ ].cp );
				shapeRun ( pieces[ i ].slot, size, run, tabular, pen, out );
				i = end;
			}
			out.width = pen;
			return out;
		}

		// A size to the sixteenth of a pixel
		[[ nodiscard ]] uint64_t sizeKey ( float size )
		{
			return uint64_t ( std::lround ( size * 16.0f ) );
		}

		// The same text again costs a lookup: the frame draws the same labels over and over
		[[ nodiscard ]] Shaped shape ( int weightSlot, float size, std::string_view utf8, bool tabular )
		{
			constexpr size_t	shapesMost = 4096;

			std::string	key ( utf8 );
			key.push_back ( char ( weightSlot ) );
			key.push_back ( tabular ? 1 : 0 );
			key.append ( reinterpret_cast<const char*> ( &size ), sizeof size );

			std::scoped_lock	held ( engine.lock );
			if ( const auto found = engine.shapes.find ( key ); found != engine.shapes.end () )
				return found->second;

			if ( engine.shapes.size () >= shapesMost )
				engine.shapes.clear ();
			return engine.shapes.emplace ( key, shapeNow ( weightSlot, size, utf8, tabular ) ).first->second;
		}

		[[ nodiscard ]] int slotOf ( Face face )
		{
			return isReady () ? std::clamp ( int ( face ), 0, weightCount - 1 ) : -1;
		}

		// A glyph rendered with DirectWrite at a size and a quarter-pixel shift of its origin: its coverage, whole pixels from the origin on
		// the baseline
		[[ nodiscard ]] Bitmap bitmapOf ( uint32_t index, float size, int quarter )
		{
			Bitmap	out;
			const auto	slot = size_t ( index >> 16 );

			std::scoped_lock	held ( engine.lock );
			if ( slot >= engine.faces.size () || engine.factory == nullptr )
				return out;

			UINT16			glyph = UINT16 ( index & 0xFFFF );
			DWRITE_GLYPH_RUN	run {};
			run.fontFace = engine.faces[ slot ].face.Get ();
			run.fontEmSize = size;
			run.glyphCount = 1;
			run.glyphIndices = &glyph;

			ComPtr<IDWriteGlyphRunAnalysis>	analysis;
			if ( FAILED ( engine.factory->CreateGlyphRunAnalysis ( &run, 1.0f, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, DWRITE_MEASURING_MODE_NATURAL, float ( quarter ) * 0.25f, 0.0f, &analysis ) ) )
				return out;

			RECT	bounds {};
			if ( FAILED ( analysis->GetAlphaTextureBounds ( DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds ) ) || bounds.right <= bounds.left || bounds.bottom <= bounds.top )
				return out;

			const auto			w = bounds.right - bounds.left;
			const auto			h = bounds.bottom - bounds.top;
			std::vector<uint8_t>	samples ( size_t ( w ) * size_t ( h ) * 3 );
			if ( FAILED ( analysis->CreateAlphaTexture ( DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds, samples.data (), UINT32 ( samples.size () ) ) ) )
				return out;

			out.left = bounds.left;
			out.top = bounds.top;
			out.width = w;
			out.height = h;
			out.alpha.resize ( size_t ( w ) * size_t ( h ) );
			for ( size_t i = 0; i < out.alpha.size (); ++i )
				out.alpha[ i ] = uint8_t ( ( int ( samples[ i * 3 ] ) + int ( samples[ i * 3 + 1 ] ) + int ( samples[ i * 3 + 2 ] ) + 1 ) / 3 );
			return out;
		}

		//-------------------------------------------------------------------------
		// The atlas

		// Rows of the atlas changed: every device's texture is behind by them
		void markDirty ( int from, int to )
		{
			for ( auto& [ device, d ] : engine.devices )
			{
				d.dirtyFrom = std::min ( d.dirtyFrom, from );
				d.dirtyTo = std::max ( d.dirtyTo, to );
			}
		}

		void startOver ()
		{
			engine.entries.clear ();
			std::ranges::fill ( engine.rgba, 0x00FFFFFFu );
			engine.cursorX = engine.cursorY = engine.rowHeight = 0;
			engine.isFull = false;
			markDirty ( 0, atlasHeight );
		}

		// The glyph in that size and quarter pixel, rendered into the atlas the first time; null for one with no pixels and when the atlas
		// has no room (it starts over at the next frame)
		[[ nodiscard ]] const Entry* entryOf ( uint32_t index, float size, int quarter )
		{
			const auto	key = ( uint64_t ( index ) << 32 ) | ( sizeKey ( size ) << 4 ) | uint64_t ( quarter );
			const auto	found = engine.entries.find ( key );
			if ( found != engine.entries.end () )
				return found->second.width > 0 ? &found->second : nullptr;

			const auto	bitmap = bitmapOf ( index, size, quarter );
			Entry		entry;
			entry.width = bitmap.width;
			entry.height = bitmap.height;
			entry.xoff = float ( bitmap.left );
			entry.yoff = float ( bitmap.top );

			if ( entry.width > 0 && entry.height > 0 )
			{
				// A pixel round each glyph, so a texture sample never reaches its neighbour
				if ( engine.cursorX + entry.width + 2 > atlasWidth )
				{
					engine.cursorX = 0;
					engine.cursorY += engine.rowHeight;
					engine.rowHeight = 0;
				}
				if ( entry.width + 2 > atlasWidth || engine.cursorY + entry.height + 2 > atlasHeight )
				{
					engine.isFull = true;
					return nullptr;
				}

				entry.x = engine.cursorX + 1;
				entry.y = engine.cursorY + 1;
				engine.cursorX += entry.width + 2;
				engine.rowHeight = std::max ( engine.rowHeight, entry.height + 2 );

				for ( int row = 0; row < entry.height; ++row )
					for ( int column = 0; column < entry.width; ++column )
						engine.rgba[ size_t ( entry.y + row ) * size_t ( atlasWidth ) + size_t ( entry.x + column ) ] =
							0x00FFFFFFu | ( uint32_t ( bitmap.alpha[ size_t ( row ) * size_t ( entry.width ) + size_t ( column ) ] ) << 24 );
				markDirty ( entry.y, entry.y + entry.height );
			}

			const auto&	stored = engine.entries.emplace ( key, entry ).first->second;
			return stored.width > 0 ? &stored : nullptr;
		}

		struct Vertical
		{
			float	ascent = 0.0f;
			float	descent = 0.0f;
			float	capHeight = 0.0f;
		};

		// The face's ascent, descent (below 0) and cap height at a size, from its OS/2 table
		[[ nodiscard ]] Vertical verticalOf ( int slot, float size )
		{
			std::scoped_lock	held ( engine.lock );
			const auto&			f = engine.faces[ size_t ( slot ) ];
			const auto			k = size / f.unitsPerEm;
			return { f.ascent * k, f.descent * k, f.capHeight * k };
		}
	}

	void forgetDevice ( api::device* device )
	{
		if ( auto it = engine.devices.find ( device ); it != engine.devices.end () )
		{
			if ( engine.current == &it->second )
				engine.current = nullptr;

			device->destroy_resource_view ( it->second.view );
			device->destroy_resource ( it->second.texture );
			engine.devices.erase ( it );
		}
	}

	std::span<const unsigned char> faceData ()
	{
		return isReady () ? std::span<const unsigned char> ( engine.data, engine.size ) : std::span<const unsigned char> ();
	}

	void prepare ( api::device* device )
	{
		const auto	isOn = isReady ();

		if ( engine.isFull )
			startOver ();

		engine.current = nullptr;
		if ( device == nullptr || ! isOn )
			return;

		auto [ it, fresh ] = engine.devices.try_emplace ( device );
		auto&	d = it->second;
		if ( fresh )
		{
			const auto	format = api::format::r8g8b8a8_unorm;
			const api::subresource_data	data { engine.rgba.data (), uint32_t ( atlasWidth ) * 4, uint32_t ( atlasWidth ) * uint32_t ( atlasHeight ) * 4 };
			if ( ! device->create_resource ( api::resource_desc ( uint32_t ( atlasWidth ), uint32_t ( atlasHeight ), 1, 1, format, 1, api::memory_heap::default_,
																  api::resource_usage::shader_resource | api::resource_usage::copy_dest ),
											 &data, api::resource_usage::shader_resource, &d.texture )
				 || ! device->create_resource_view ( d.texture, api::resource_usage::shader_resource, api::resource_view_desc ( format ), &d.view ) )
			{
				if ( d.texture.handle != 0 )
					device->destroy_resource ( d.texture );
				engine.devices.erase ( it );
				return;
			}
		}

		engine.current = &d;
	}

	void flush ( api::command_list* cmdList, api::device* device )
	{
		const auto	it = engine.devices.find ( device );
		if ( it == engine.devices.end () || it->second.dirtyTo <= it->second.dirtyFrom )
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
		cmdList->update_texture_region ( api::subresource_data { &engine.rgba[ size_t ( d.dirtyFrom ) * size_t ( atlasWidth ) ], uint32_t ( atlasWidth ) * 4, uint32_t ( atlasWidth ) * 4 * rows },
										 d.texture, 0, &box );
		cmdList->barrier ( d.texture, api::resource_usage::copy_dest, api::resource_usage::shader_resource );

		d.dirtyFrom = atlasHeight;
		d.dirtyTo = 0;
	}

	float draw ( ImDrawList* drawList, ImVec2 pos, Face face, float size, ImU32 color, std::string_view text, bool tabular )
	{
		const auto	slot = slotOf ( face );
		if ( slot < 0 || engine.current == nullptr )
			return 0.0f;

		const auto	shaped = shape ( slot, size, text, tabular );
		const auto	baseline = std::round ( pos.y + verticalOf ( slot, size ).ascent );
		const auto	w = float ( atlasWidth );
		const auto	h = float ( atlasHeight );

		struct Quad
		{
			const Entry*	entry;
			float			x;
		};
		static std::vector<Quad>	quads;
		quads.clear ();

		for ( const auto& g : shaped.glyphs )
		{
			// Each glyph's origin to the quarter pixel, so the spacing stays even
			const auto	x = std::floor ( pos.x + g.pen );
			const auto	quarter = std::min ( int ( ( pos.x + g.pen - x ) * 4.0f ), 3 );
			if ( const auto* entry = entryOf ( g.index, size, quarter ) )
				quads.push_back ( { entry, x } );
		}

		const auto	count = int ( quads.size () );
		drawList->PushTexture ( ImTextureRef ( ImTextureID ( engine.current->view.handle ) ) );
		drawList->PrimReserve ( count * 6, count * 4 );
		for ( const auto& q : quads )
			drawList->PrimRectUV ( ImVec2 ( q.x + q.entry->xoff, baseline + q.entry->yoff ), ImVec2 ( q.x + q.entry->xoff + float ( q.entry->width ), baseline + q.entry->yoff + float ( q.entry->height ) ),
								   ImVec2 ( float ( q.entry->x ) / w, float ( q.entry->y ) / h ), ImVec2 ( float ( q.entry->x + q.entry->width ) / w, float ( q.entry->y + q.entry->height ) / h ), color );
		drawList->PopTexture ();
		return shaped.width;
	}

	float width ( Face face, float size, std::string_view text, bool tabular )
	{
		const auto	slot = slotOf ( face );
		return slot < 0 ? 0.0f : shape ( slot, size, text, tabular ).width;
	}

	float lineHeight ( Face face, float size )
	{
		const auto	slot = slotOf ( face );
		if ( slot < 0 )
			return size;

		const auto	v = verticalOf ( slot, size );
		return v.ascent - v.descent;
	}

	float centredTop ( Face face, float size, float top, float height )
	{
		const auto	slot = slotOf ( face );
		if ( slot < 0 )
			return top + ( height - size ) * 0.5f;

		const auto	v = verticalOf ( slot, size );
		return top + ( height + v.capHeight ) * 0.5f - v.ascent;
	}

	namespace
	{
		// A pixel to spare on either side for the glyphs' overhang
		constexpr int	margin = 1;

		[[ nodiscard ]] int lineWidth ( float wide )
		{
			return int ( std::ceil ( wide ) ) + 2 * margin;
		}
	}

	PlacedLine placeLine ( Face face, float size, std::string_view text, float maxWidth )
	{
		const auto	slot = slotOf ( face );
		if ( slot < 0 || size <= 0.0f )
			return {};

		// Cut as CSS's text-overflow: ellipsis does where wider than maxWidth, with the face's ellipsis where it has one
		auto		shaped = shape ( slot, size, text, false );
		if ( shaped.width > maxWidth )
		{
			auto		cps = codePoints ( text );
			const auto	hasEllipsis = [ & ] { std::scoped_lock held ( engine.lock ); return glyphIn ( slot, 0x2026 ) != 0; } ();
			while ( ! cps.empty () )
			{
				cps.pop_back ();
				while ( ! cps.empty () && cps.back () == ' ' )
					cps.pop_back ();

				// Back to UTF-8 for the shaper
				std::string	cut;
				for ( const auto cp : cps )
				{
					if ( cp < 0x80 )
						cut.push_back ( char ( cp ) );
					else if ( cp < 0x800 )
					{
						cut.push_back ( char ( 0xC0 | ( cp >> 6 ) ) );
						cut.push_back ( char ( 0x80 | ( cp & 0x3F ) ) );
					}
					else if ( cp < 0x10000 )
					{
						cut.push_back ( char ( 0xE0 | ( cp >> 12 ) ) );
						cut.push_back ( char ( 0x80 | ( ( cp >> 6 ) & 0x3F ) ) );
						cut.push_back ( char ( 0x80 | ( cp & 0x3F ) ) );
					}
					else
					{
						cut.push_back ( char ( 0xF0 | ( cp >> 18 ) ) );
						cut.push_back ( char ( 0x80 | ( ( cp >> 12 ) & 0x3F ) ) );
						cut.push_back ( char ( 0x80 | ( ( cp >> 6 ) & 0x3F ) ) );
						cut.push_back ( char ( 0x80 | ( cp & 0x3F ) ) );
					}
				}
				cut += hasEllipsis ? "\xE2\x80\xA6" : "...";
				shaped = shape ( slot, size, cut, false );
				if ( shaped.width <= maxWidth )
					break;
			}
		}

		const auto	v = verticalOf ( slot, size );
		PlacedLine	l;
		l.face = slot;
		l.scale = size;
		l.width = lineWidth ( shaped.width );
		l.glyphs.reserve ( shaped.glyphs.size () );
		for ( const auto& g : shaped.glyphs )
			l.glyphs.push_back ( { int ( g.index ), g.pen } );
		l.ascent = int ( std::ceil ( v.ascent ) );
		l.height = l.ascent + int ( std::ceil ( -v.descent ) );
		return l;
	}

	namespace
	{
		// rasterizeGlyph's glyphs as rendered, for the DLL's lifetime: at most this many bytes of pixels, dropped a whole size (face and
		// size) at a time, the least recently used first
		constexpr size_t	glyphCacheLimit = size_t ( 4 ) << 20;

		struct CachedSize
		{
			std::unordered_map<uint64_t, Bitmap>	glyphs;	// by glyph index and quarter pixel shift
			size_t									bytes = 0;
			uint64_t								used = 0;
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
		// The pen rounded to quarter pixels, so glyphs repeat in the cache: each sits at most 1/8 px from its exact place
		const auto	x = std::round ( ( float ( margin ) + g.pen ) * 4.0f ) * 0.25f;
		const auto	whole = std::floor ( x );
		const auto	quarter = int ( ( x - whole ) * 4.0f );

		std::scoped_lock	lock ( glyphCache.lock );
		const auto			sizeKeyOf = std::bit_cast<uint32_t> ( line.scale );
		const auto			key = uint64_t ( sizeKeyOf );
		const auto			glyphKey = ( uint64_t ( uint32_t ( g.index ) ) << 2 ) | uint64_t ( quarter );
		auto*				size = &glyphCache.sizes[ key ];
		size->used = ++glyphCache.uses;
		auto				found = size->glyphs.find ( glyphKey );
		const auto			isHit = found != size->glyphs.end ();
		if ( ! isHit )
		{
			auto	bitmap = bitmapOf ( uint32_t ( g.index ), line.scale, quarter );
			const auto	bytes = bitmap.alpha.size ();
			makeRoom ( bytes, key );
			size = &glyphCache.sizes[ key ];
			size->bytes += bytes;
			glyphCache.bytes += bytes;
			found = size->glyphs.emplace ( glyphKey, std::move ( bitmap ) ).first;
		}

		const auto&	c = found->second;
		for ( int gy = 0; gy < c.height; ++gy )
		{
			const auto	ly = line.ascent + c.top + gy;
			const auto	ty = top + ly;
			if ( ly < 0 || ly >= line.height || ty < 0 || ty >= rows )
				continue;
			for ( int gx = 0; gx < c.width; ++gx )
			{
				const auto	lx = int ( whole ) + c.left + gx;
				const auto	tx = left + lx;
				if ( lx < 0 || lx >= line.width || tx < 0 || tx >= stride )
					continue;
				auto&	to = coverage[ size_t ( ty ) * size_t ( stride ) + size_t ( tx ) ];
				to = std::max ( to, c.alpha[ size_t ( gy ) * size_t ( c.width ) + size_t ( gx ) ] );
			}
		}
		return isHit;
	}

	size_t cachedGlyphBytes ()
	{
		std::scoped_lock	lock ( glyphCache.lock );
		return glyphCache.bytes;
	}

	float baselineInLine ( Face face, float size, float lineHeight )
	{
		const auto	slot = slotOf ( face );
		if ( slot < 0 )
			return size * lineHeight * 0.8f;

		const auto	v = verticalOf ( slot, size );
		return ( lineHeight * size - ( v.ascent - v.descent ) ) * 0.5f + v.ascent;
	}
}
