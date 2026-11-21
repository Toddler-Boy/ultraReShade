/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <chrono>

#include <Windows.h>
#include <winternl.h>

#include <reshade.hpp>

#include "addon_manager.hpp"
#include "dll_log.hpp"
#include "hook_manager.hpp"
#include "ultra_log.hpp"
#include "ultra_process_exit.hpp"

//-----------------------------------------------------------------------------

namespace
{
	namespace api = reshade::api;

	using ExitUserProcess = void ( NTAPI* ) ( NTSTATUS status );
	using TerminateProcessCall = NTSTATUS ( NTAPI* ) ( HANDLE process, NTSTATUS status );

	constexpr DWORD		handlerBound = 5000;		// ms for all handlers together: an add-on stuck in its handler must not keep the process
	constexpr NTSTATUS	controlCExit = static_cast<NTSTATUS> ( 0xC000013A );

	ExitUserProcess			realExit = nullptr;
	TerminateProcessCall	realTerminate = nullptr;

	// The watchdog waits from the first load on, so the exit never has to start a thread
	HANDLE		begun = nullptr;
	HANDLE		handled = nullptr;
	api::process_exit	reason = api::process_exit::exit;
	NTSTATUS	exitStatus = 0;

	[[ nodiscard ]] const char* nameOf ( api::process_exit why )
	{
		switch ( why )
		{
			case api::process_exit::exit:			return "exit";
			case api::process_exit::self_terminate:	return "self-terminate";
		}
		return "unknown";
	}

	// An exit with an error status is how a crash ends (the unhandled exception's code): not ours to announce
	[[ nodiscard ]] bool isCrash ( NTSTATUS status )
	{
		return ( static_cast<ULONG> ( status ) & 0xC0000000 ) == 0xC0000000 && status != controlCExit;
	}

	[[ nodiscard ]] bool isOwnProcess ( HANDLE process )
	{
		return process == GetCurrentProcess () || ( process != nullptr && GetProcessId ( process ) == GetCurrentProcessId () );
	}

	DWORD WINAPI watchExit ( void* )
	{
		WaitForSingleObject ( begun, INFINITE );

		if ( WaitForSingleObject ( handled, handlerBound ) == WAIT_OBJECT_0 )
			return 0;

		reshade::log::message ( reshade::log::level::warning, "An add-on's exit handler still runs after %lu ms: the process ends without waiting for it.", handlerBound );
		ultralog::endRuns ();

		if ( reason == api::process_exit::exit )
			realExit ( exitStatus );
		else
			realTerminate ( GetCurrentProcess (), exitStatus );
		return 0;
	}

	// Called directly, past the gate every other event now meets
	void announce ( api::process_exit why, NTSTATUS status )
	{
		if ( ! reshade::ultra::beginProcessExit () )
			return;

		reason = why;
		exitStatus = status;
		reshade::log::message ( reshade::log::level::info, "The process ends (%s, status 0x%08lX): the add-ons hear it now.", nameOf ( why ), static_cast<unsigned long> ( status ) );
		SetEvent ( begun );

		const auto	start = std::chrono::steady_clock::now ();
		{
			const reshade::ultra::AddonEventGuard	guard;
			const auto	handlers = reshade::addon_event_list[ static_cast<uint32_t> ( reshade::addon_event::ultra_process_exit ) ];

			for ( const auto handler : handlers )
				reinterpret_cast<reshade::addon_event_traits<reshade::addon_event::ultra_process_exit>::decl> ( handler ) ( why );
		}
		SetEvent ( handled );

		const auto	took = std::chrono::duration_cast<std::chrono::milliseconds> ( std::chrono::steady_clock::now () - start );
		reshade::log::message ( reshade::log::level::info, "The add-ons' exit handlers took %lld ms.", static_cast<long long> ( took.count () ) );
		ultralog::endRuns ();
	}

	void endOrNote ( api::process_exit why, NTSTATUS status )
	{
		if ( ! isCrash ( status ) )
			return announce ( why, status );

		if ( ! reshade::ultra::beginProcessExit () )
			return;

		reshade::log::message ( reshade::log::level::info, "The process ends (%s) with status 0x%08lX, a crash: the add-ons get no exit event.", nameOf ( why ), static_cast<unsigned long> ( status ) );
		ultralog::noteCrash ();
	}

	void NTAPI onExitUserProcess ( NTSTATUS status )
	{
		endOrNote ( api::process_exit::exit, status );
		realExit ( status );
	}

	// A null process is the exit itself ending the other threads
	NTSTATUS NTAPI onTerminateProcess ( HANDLE process, NTSTATUS status )
	{
		if ( process != nullptr && ! reshade::ultra::hasProcessExitBegun () && isOwnProcess ( process ) )
			endOrNote ( api::process_exit::self_terminate, status );

		return realTerminate ( process, status );
	}

	template <typename T>
	[[ nodiscard ]] bool hookNtdll ( const char* name, T replacement, T& real )
	{
		const auto	target = reinterpret_cast<reshade::hook::address> ( GetProcAddress ( GetModuleHandleW ( L"ntdll.dll" ), name ) );

		if ( target == nullptr || ! reshade::hooks::install ( name, target, reinterpret_cast<reshade::hook::address> ( replacement ) ) )
			return false;

		real = reshade::hooks::call ( replacement, target );
		return real != nullptr;
	}
}
//-----------------------------------------------------------------------------

void reshade::ultra::followProcessExit ()
{
	if ( begun != nullptr )
		return;

	begun = CreateEventW ( nullptr, TRUE, FALSE, nullptr );
	handled = CreateEventW ( nullptr, TRUE, FALSE, nullptr );

	const auto	watchdog = begun != nullptr && handled != nullptr ? CreateThread ( nullptr, 0, watchExit, nullptr, 0, nullptr ) : nullptr;

	if ( watchdog == nullptr )
	{
		log::message ( log::level::error, "Add-ons get no exit event: its watchdog did not start (error code %lu).", GetLastError () );
		return;
	}

	CloseHandle ( watchdog );

	if ( ! hookNtdll ( "RtlExitUserProcess", onExitUserProcess, realExit ) || ! hookNtdll ( "NtTerminateProcess", onTerminateProcess, realTerminate ) )
		log::message ( log::level::error, "Add-ons may get no exit event: the process's exit could not be hooked." );
}
//-----------------------------------------------------------------------------

