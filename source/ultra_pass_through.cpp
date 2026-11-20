/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <intrin.h>
#include <Windows.h>
#include <MinHook.h>

#include "ultra_pass_through.hpp"

//-----------------------------------------------------------------------------

namespace
{
	// One per forwarded export; target is set once, at the export's first call
	struct Slot
	{
		void* volatile	target = nullptr;
		const char*		name = nullptr;
	};

	std::filesystem::path	systemDllPath;
	HMODULE					systemDll = nullptr;
	SRWLOCK					resolveLock = SRWLOCK_INIT;

	void tellDebugger ( const std::string& text )
	{
		OutputDebugStringA ( ( "ultraReShade pass-through: " + text + "\n" ).c_str () );
	}

	// Called by the thunks with the slot of the export called; returns where it jumps to
	void* __stdcall resolve ( Slot* slot )
	{
		AcquireSRWLockExclusive ( &resolveLock );

		if ( slot->target == nullptr )
		{
			if ( systemDll == nullptr )
				systemDll = LoadLibraryW ( systemDllPath.c_str () );

			if ( systemDll != nullptr )
				slot->target = reinterpret_cast<void*> ( GetProcAddress ( systemDll, slot->name ) );
		}

		const auto	target = slot->target;
		ReleaseSRWLockExclusive ( &resolveLock );

		// A function the system DLL lacks (or a system DLL that does not load) can't be called, nor returned from
		if ( target == nullptr )
		{
			tellDebugger ( std::string ( slot->name ) + " is not in " + systemDllPath.u8string () + " (or it does not load), the process ends." );
			__fastfail ( FAST_FAIL_FATAL_APP_EXIT );
		}

		return target;
	}

	void append ( std::vector<uint8_t>& code, std::initializer_list<uint8_t> bytes )
	{
		code.insert ( code.end (), bytes );
	}

	template <typename T>
	void appendValue ( std::vector<uint8_t>& code, T value )
	{
		const auto	at = code.size ();
		code.resize ( at + sizeof ( T ) );
		std::memcpy ( code.data () + at, &value, sizeof ( T ) );
	}

	// Entered by a thunk with the slot in r10 (x64) or eax (x86), everything else as the caller left it: jumps to the slot's
	// target, resolving it first while the argument registers are kept
	[[ nodiscard ]] std::vector<uint8_t> commonCode ()
	{
		std::vector<uint8_t>	code;
#ifdef _WIN64
		append ( code, { 0x49, 0x8B, 0x02, 0x48, 0x85, 0xC0, 0x74, 0x02, 0xFF, 0xE0 } );		// mov rax, [r10]; test rax, rax; jz +2; jmp rax
		append ( code, { 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xEC, 0x68 } );		// push rcx, rdx, r8, r9; sub rsp, 0x68
		append ( code, { 0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30 } );	// movdqu [rsp + 0x20], xmm0; [rsp + 0x30], xmm1
		append ( code, { 0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50 } );	// movdqu [rsp + 0x40], xmm2; [rsp + 0x50], xmm3
		append ( code, { 0x4C, 0x89, 0xD1, 0x48, 0xB8 } );										// mov rcx, r10; mov rax, resolve
		appendValue ( code, reinterpret_cast<uint64_t> ( &resolve ) );
		append ( code, { 0xFF, 0xD0 } );														// call rax
		append ( code, { 0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30 } );	// movdqu xmm0, [rsp + 0x20]; xmm1, [rsp + 0x30]
		append ( code, { 0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50 } );	// movdqu xmm2, [rsp + 0x40]; xmm3, [rsp + 0x50]
		append ( code, { 0x48, 0x83, 0xC4, 0x68, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0xFF, 0xE0 } );	// add rsp, 0x68; pop r9, r8, rdx, rcx; jmp rax
#else
		append ( code, { 0x83, 0x38, 0x00, 0x74, 0x02, 0xFF, 0x20 } );							// cmp dword [eax], 0; jz +2; jmp [eax]
		append ( code, { 0x51, 0x52, 0x50, 0xB8 } );											// push ecx, edx, eax (the argument); mov eax, resolve
		appendValue ( code, reinterpret_cast<uint32_t> ( &resolve ) );
		append ( code, { 0xFF, 0xD0, 0x5A, 0x59, 0xFF, 0xE0 } );								// call eax; pop edx, ecx; jmp eax
#endif
		return code;
	}

	// Per export: the slot into the register, then on to the common code
	void appendThunk ( std::vector<uint8_t>& code, const uint8_t* base, const Slot& slot, const uint8_t* common )
	{
#ifdef _WIN64
		append ( code, { 0x49, 0xBA } );														// mov r10, slot
		appendValue ( code, reinterpret_cast<uint64_t> ( &slot ) );
		append ( code, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 } );								// jmp [rip]
		appendValue ( code, reinterpret_cast<uint64_t> ( common ) );
#else
		append ( code, { 0xB8 } );																// mov eax, slot
		appendValue ( code, reinterpret_cast<uint32_t> ( &slot ) );
		append ( code, { 0xE9 } );																// jmp common
		appendValue ( code, static_cast<int32_t> ( common - ( base + code.size () + 4 ) ) );
#endif
	}

	struct Export
	{
		const char*	name = nullptr;
		void*		code = nullptr;
	};

	// Each named export of the image at base: its name and RVA
	template <typename Callback>
	void forEachNamedExport ( const uint8_t* base, Callback&& callback )
	{
		const auto	nt = reinterpret_cast<const IMAGE_NT_HEADERS*> ( base + reinterpret_cast<const IMAGE_DOS_HEADER*> ( base )->e_lfanew );
		const auto&	directory = nt->OptionalHeader.DataDirectory[ IMAGE_DIRECTORY_ENTRY_EXPORT ];

		if ( directory.VirtualAddress == 0 )
			return;

		const auto	exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*> ( base + directory.VirtualAddress );
		const auto	names = reinterpret_cast<const DWORD*> ( base + exports->AddressOfNames );
		const auto	ordinals = reinterpret_cast<const WORD*> ( base + exports->AddressOfNameOrdinals );
		const auto	functions = reinterpret_cast<const DWORD*> ( base + exports->AddressOfFunctions );

		for ( DWORD i = 0; i < exports->NumberOfNames; ++i )
			callback ( reinterpret_cast<const char*> ( base + names[ i ] ), functions[ ordinals[ i ] ] );
	}

	// The names the system DLL exports, read from its image without running it (no DllMain, no imports); empty when it can't be read
	[[ nodiscard ]] std::unordered_set<std::string> systemExportNames ( const std::filesystem::path& systemDll )
	{
		std::unordered_set<std::string>	names;
		const auto						image = LoadLibraryExW ( systemDll.c_str (), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE );

		if ( image == nullptr )
		{
			tellDebugger ( systemDll.u8string () + " can't be read (error code " + std::to_string ( GetLastError () ) + ")." );
			return names;
		}

		// The loader tags the handle's low bits when it maps a file as an image resource
		forEachNamedExport ( reinterpret_cast<const uint8_t*> ( reinterpret_cast<uintptr_t> ( image ) & ~uintptr_t { 3 } ), [ &names ] ( const char* name, DWORD )
		{
			names.emplace ( name );
		} );

		FreeLibrary ( image );
		return names;
	}

	// ReShade exports every graphics API's functions from the one DLL: only those the system DLL has too, and in an executable section
	// (ReShade's own functions (ReShade...) and data stay as they are)
	[[ nodiscard ]] std::vector<Export> forwardedExports ( HMODULE self, const std::unordered_set<std::string>& systemNames )
	{
		const auto	base = reinterpret_cast<const uint8_t*> ( self );
		const auto	nt = reinterpret_cast<const IMAGE_NT_HEADERS*> ( base + reinterpret_cast<const IMAGE_DOS_HEADER*> ( base )->e_lfanew );
		const auto	sections = IMAGE_FIRST_SECTION ( nt );

		const auto	isCode = [ &nt, &sections ] ( DWORD rva )
		{
			return std::any_of ( sections, sections + nt->FileHeader.NumberOfSections, [ rva ] ( const IMAGE_SECTION_HEADER& section )
			{
				return rva >= section.VirtualAddress && rva < section.VirtualAddress + section.Misc.VirtualSize && ( section.Characteristics & IMAGE_SCN_MEM_EXECUTE ) != 0;
			} );
		};

		std::vector<Export>	found;

		forEachNamedExport ( base, [ & ] ( const char* name, DWORD rva )
		{
			if ( std::strncmp ( name, "ReShade", 7 ) != 0 && isCode ( rva ) && systemNames.contains ( name ) )
				found.push_back ( { name, const_cast<uint8_t*> ( base + rva ) } );
		} );

		return found;
	}
}
//-----------------------------------------------------------------------------

std::filesystem::path reshade::ultra::proxiedSystemDll ( HMODULE self )
{
	wchar_t		buffer[ MAX_PATH ] {};
	const auto	selfPath = std::filesystem::path ( std::wstring ( buffer, GetModuleFileNameW ( self, buffer, MAX_PATH ) ) );
	const auto	systemFolder = std::wstring ( buffer, GetSystemDirectoryW ( buffer, MAX_PATH ) );
	const auto	candidate = std::filesystem::path ( systemFolder ) / selfPath.filename ();

	if ( selfPath.empty () || _wcsicmp ( candidate.c_str (), selfPath.c_str () ) == 0 || GetFileAttributesW ( candidate.c_str () ) == INVALID_FILE_ATTRIBUTES )
		return {};

	return candidate;
}
//-----------------------------------------------------------------------------

bool reshade::ultra::startPassThrough ( HMODULE self, const std::filesystem::path& systemDll )
{
	systemDllPath = systemDll;

	const auto	systemNames = systemExportNames ( systemDll );

	if ( systemNames.empty () )
		return false;

	const auto	exports = forwardedExports ( self, systemNames );

	// Exports sharing their code (identical functions folded by the linker) can't each go to their own target
	std::unordered_map<void*, const char*>	byCode;
	for ( const auto& entry : exports )
	{
		if ( const auto [ it, isNew ] = byCode.try_emplace ( entry.code, entry.name ); ! isNew )
		{
			tellDebugger ( std::string ( entry.name ) + " and " + it->second + " share their code, so neither can be forwarded: not loaded." );
			return false;
		}
	}

	// Never freed: the forwarding lasts as long as the process
	const auto	slots = new Slot[ exports.size () ];
	for ( size_t i = 0; i < exports.size (); ++i )
		slots[ i ].name = exports[ i ].name;

	const auto	common = commonCode ();
#ifdef _WIN64
	constexpr size_t	thunkSize = 24;
#else
	constexpr size_t	thunkSize = 10;
#endif
	const auto	size = common.size () + exports.size () * thunkSize;
	const auto	memory = static_cast<uint8_t*> ( VirtualAlloc ( nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE ) );

	if ( memory == nullptr )
	{
		tellDebugger ( "no memory for the forwarding code (error code " + std::to_string ( GetLastError () ) + "): not loaded." );
		return false;
	}

	std::vector<uint8_t>	code = common;
	for ( size_t i = 0; i < exports.size (); ++i )
		appendThunk ( code, memory, slots[ i ], memory );

	std::memcpy ( memory, code.data (), code.size () );

	if ( DWORD previous = 0; ! VirtualProtect ( memory, size, PAGE_EXECUTE_READ, &previous ) || ! FlushInstructionCache ( GetCurrentProcess (), memory, size ) )
	{
		tellDebugger ( "the forwarding code can't be made executable (error code " + std::to_string ( GetLastError () ) + "): not loaded." );
		return false;
	}

	if ( const auto status = MH_Initialize (); status != MH_OK )
	{
		tellDebugger ( std::string ( "MinHook does not start (" ) + MH_StatusToString ( status ) + "): not loaded." );
		return false;
	}

	for ( size_t i = 0; i < exports.size (); ++i )
	{
		void*		trampoline = nullptr;
		const auto	status = MH_CreateHook ( exports[ i ].code, memory + common.size () + i * thunkSize, &trampoline );

		if ( status != MH_OK )
		{
			tellDebugger ( std::string ( exports[ i ].name ) + " can't be forwarded (" + MH_StatusToString ( status ) + "): not loaded." );
			return false;
		}
	}

	if ( const auto status = MH_EnableHook ( MH_ALL_HOOKS ); status != MH_OK )
	{
		tellDebugger ( std::string ( "the exports can't be forwarded (" ) + MH_StatusToString ( status ) + "): not loaded." );
		return false;
	}

	tellDebugger ( std::to_string ( exports.size () ) + " exports forward to " + systemDll.u8string () + "." );
	return true;
}
//-----------------------------------------------------------------------------
