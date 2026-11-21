/*
 * Copyright (C) 2021 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "reshade_api.hpp"
#include <string>
#include <vector>

namespace reshade
{
	struct addon_info
	{
		struct overlay_callback
		{
			std::string title;
			void(*callback)(api::effect_runtime *) = nullptr;
		};

		void *handle = nullptr;
		std::string name;
		std::string file;
		uint32_t api_version = 0;
		bool external = true;

		std::vector<std::pair<uint32_t, void *>> event_callbacks;
#if RESHADE_GUI
		std::vector<overlay_callback> overlay_callbacks;
#endif
	};
}

