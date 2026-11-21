/*
 * Copyright (C) 2022 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "platform_utils.hpp"
#include <Windows.h>
#include <Shellapi.h>

bool reshade::utils::open_explorer(const std::filesystem::path &path)
{
	// Use absolute path to explorer to avoid potential security issues when executable is replaced
	WCHAR explorer_path[MAX_PATH];
	if (GetWindowsDirectoryW(explorer_path, ARRAYSIZE(explorer_path)) == 0)
		return false;
	wcscat_s(explorer_path, L"\\explorer.exe");

	return ShellExecuteW(nullptr, L"open", explorer_path, (L"/select,\"" + path.wstring() + L"\"").c_str(), nullptr, SW_SHOWDEFAULT) != nullptr;
}
