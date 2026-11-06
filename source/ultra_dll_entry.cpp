/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <intrin.h>
#include <Windows.h>

//-----------------------------------------------------------------------------

extern "C" BOOL WINAPI _DllMainCRTStartup ( HINSTANCE instance, DWORD reason, LPVOID reserved );

namespace
{
	[[ nodiscard ]]
#ifdef __clang__
	__attribute__ ( ( target ( "xsave" ) ) )
#endif
	unsigned long long readXcr0 ()
	{
		return _xgetbv ( 0 );
	}

	[[ nodiscard ]] bool hasAvx ()
	{
		int	info[ 4 ] {};
		__cpuid ( info, 1 );

		const auto	isXsaveEnabled = ( info[ 2 ] & ( 1 << 27 ) ) != 0;
		const auto	isAvxSupported = ( info[ 2 ] & ( 1 << 28 ) ) != 0;

		// The OS must save the XMM and YMM registers on context switches (XCR0 bits 1 and 2)
		return isXsaveEnabled && isAvxSupported && ( readXcr0 () & 0x6 ) == 0x6;
	}

	[[ nodiscard ]] bool isAttachWithoutAvx ( DWORD reason )
	{
		return reason == DLL_PROCESS_ATTACH && ! hasAvx ();
	}
}
//-----------------------------------------------------------------------------

// DLL entry point of builds that require AVX: runs before the C runtime and any static initializer, so it must not use AVX itself
extern "C" BOOL WINAPI ultraDllEntry ( HINSTANCE instance, DWORD reason, LPVOID reserved )
{
	static auto	isRefused = false;

	if ( isRefused )
		return FALSE;

	if ( isAttachWithoutAvx ( reason ) )
	{
		isRefused = true;
		MessageBoxW ( nullptr, L"ultraReShade needs a processor with AVX support.", L"ultraReShade", MB_OK | MB_ICONERROR );
		return FALSE;
	}

	return _DllMainCRTStartup ( instance, reason, reserved );
}
//-----------------------------------------------------------------------------
