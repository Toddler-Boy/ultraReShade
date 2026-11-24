/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "dll_log.hpp"
#include "ultra_float_clone.hpp"

//-----------------------------------------------------------------------------

namespace
{
	// CRC-32 (zlib), eight bytes a step
	constexpr auto	crcTables = []
	{
		std::array<std::array<uint32_t, 256>, 8>	t {};
		for ( uint32_t i = 0; i < 256; ++i )
		{
			auto	c = i;
			for ( int k = 0; k < 8; ++k )
				c = ( c & 1 ) != 0 ? 0xEDB88320u ^ ( c >> 1 ) : c >> 1;
			t[ 0 ][ i ] = c;
		}
		for ( uint32_t i = 0; i < 256; ++i )
			for ( size_t k = 1; k < 8; ++k )
				t[ k ][ i ] = ( t[ k - 1 ][ i ] >> 8 ) ^ t[ 0 ][ t[ k - 1 ][ i ] & 0xFF ];
		return t;
	} ();

	[[ nodiscard ]] uint32_t crc32 ( const void* data, size_t size )
	{
		const auto*	p = static_cast<const uint8_t*> ( data );
		auto		c = 0xFFFFFFFFu;

		for ( ; size >= 8; size -= 8, p += 8 )
		{
			uint32_t	lo, hi;
			std::memcpy ( &lo, p, 4 );
			std::memcpy ( &hi, p + 4, 4 );
			lo ^= c;
			c = crcTables[ 7 ][ lo & 0xFF ] ^ crcTables[ 6 ][ ( lo >> 8 ) & 0xFF ] ^ crcTables[ 5 ][ ( lo >> 16 ) & 0xFF ] ^ crcTables[ 4 ][ lo >> 24 ]
				^ crcTables[ 3 ][ hi & 0xFF ] ^ crcTables[ 2 ][ ( hi >> 8 ) & 0xFF ] ^ crcTables[ 1 ][ ( hi >> 16 ) & 0xFF ] ^ crcTables[ 0 ][ hi >> 24 ];
		}
		for ( ; size != 0; --size, ++p )
			c = crcTables[ 0 ][ ( c ^ *p ) & 0xFF ] ^ ( c >> 8 );
		return ~c;
	}

	std::atomic<uint32_t>	formatBits { 0 };
	std::atomic<size_t>		shaderCount { 0 };
	std::shared_mutex		shaderLock;
	std::vector<uint32_t>	shaders;	// sorted
}

bool reshade::ultra::floatClone::stateRule ( uint32_t formats, const uint32_t* hashes, size_t count )
{
	std::vector<uint32_t>	sorted;
	if ( hashes != nullptr )
		sorted.assign ( hashes, hashes + count );
	std::ranges::sort ( sorted );
	sorted.erase ( std::ranges::unique ( sorted ).begin (), sorted.end () );

	const auto	made = sorted.size ();
	{
		std::unique_lock	lock ( shaderLock );
		if ( formatBits.load ( std::memory_order_relaxed ) == formats && shaders == sorted )
			return false;
		shaders = std::move ( sorted );
		shaderCount.store ( made, std::memory_order_release );
		formatBits.store ( formats, std::memory_order_release );
	}
	reshade::log::message ( reshade::log::level::info, "Float targets: formats 0x%X, %zu pixel shaders make the clone", formats, made );
	return true;
}

uint32_t reshade::ultra::floatClone::ruleFormats ()
{
	return formatBits.load ( std::memory_order_acquire );
}

uint32_t reshade::ultra::floatClone::hashOfCode ( const void* code, size_t size )
{
	return code != nullptr ? crc32 ( code, size ) : 0;
}

bool reshade::ultra::floatClone::isTriggerHash ( uint32_t hash )
{
	if ( shaderCount.load ( std::memory_order_acquire ) == 0 )
		return false;

	std::shared_lock	lock ( shaderLock );
	return std::ranges::binary_search ( shaders, hash );
}
