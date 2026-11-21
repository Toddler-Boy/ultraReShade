/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hook_manager.hpp"
#include <Windows.h>
#include <Winsock2.h>

extern "C" int WSAAPI HookWSASend(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent, DWORD dwFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
	static const auto trampoline = reshade::hooks::call(HookWSASend);
	const auto status = trampoline(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags, lpOverlapped, lpCompletionRoutine);

	return status;
}
extern "C" int WSAAPI HookWSASendTo(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent, DWORD dwFlags, const struct sockaddr *lpTo, int iToLen, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
	static const auto trampoline = reshade::hooks::call(HookWSASendTo);
	const auto status = trampoline(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags, lpTo, iToLen, lpOverlapped, lpCompletionRoutine);

	return status;
}
extern "C" int WSAAPI HookWSARecv(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
	static const auto trampoline = reshade::hooks::call(HookWSARecv);
	const auto status = trampoline(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, lpOverlapped, lpCompletionRoutine);

	return status;
}
extern "C" int WSAAPI HookWSARecvFrom(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, struct sockaddr *lpFrom, LPINT lpFromlen, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
	static const auto trampoline = reshade::hooks::call(HookWSARecvFrom);
	const auto status = trampoline(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, lpFrom, lpFromlen, lpOverlapped, lpCompletionRoutine);

	return status;
}

extern "C" int WSAAPI HookSend(SOCKET s, const char *buf, int len, int flags)
{
	static const auto trampoline = reshade::hooks::call(HookSend);
	const auto num_bytes_sent = trampoline(s, buf, len, flags);

	return num_bytes_sent;
}
extern "C" int WSAAPI HookSendTo(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
	static const auto trampoline = reshade::hooks::call(HookSendTo);
	const auto num_bytes_sent = trampoline(s, buf, len, flags, to, tolen);

	return num_bytes_sent;
}
extern "C" int WSAAPI HookRecv(SOCKET s, char *buf, int len, int flags)
{
	static const auto trampoline = reshade::hooks::call(HookRecv);
	const auto num_bytes_recieved = trampoline(s, buf, len, flags);

	return num_bytes_recieved;
}
extern "C" int WSAAPI HookRecvFrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
	static const auto trampoline = reshade::hooks::call(HookRecvFrom);
	const auto num_bytes_recieved = trampoline(s, buf, len, flags, from, fromlen);

	return num_bytes_recieved;
}
