/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#if defined(RESHADE_API_LIBRARY_EXPORT) && RESHADE_GUI

#include "dll_log.hpp"
#include "imgui_function_table_19300.hpp"

extern const imgui_function_table_19300 init_imgui_function_table_19300();

const imgui_function_table_19300 g_imgui_function_table_19300 = init_imgui_function_table_19300();

extern "C" __declspec(dllexport) const void *ReShadeGetImGuiFunctionTable(uint32_t version)
{
	if (version == 19300)
		return &g_imgui_function_table_19300;

	reshade::log::message(reshade::log::level::error, "Failed to retrieve ImGui function table, because the requested ImGui version (%u) is not supported.", version);
	return nullptr;
}

#endif
