/*
 * Copyright (C) 2022 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <filesystem>

namespace reshade::utils
{
	/// <summary>
	/// Opens a file explorer window with the specified file selected.
	/// </summary>
	bool open_explorer(const std::filesystem::path &path);
}
