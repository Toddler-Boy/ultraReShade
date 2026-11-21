/*
 * Copyright (C) 2021 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if defined(RESHADE_API_LIBRARY_EXPORT)

#include "reshade.hpp"
#include "addon_manager.hpp"
#include "runtime.hpp"
#include "dll_log.hpp"
#include "ini_file.hpp"
#include "ultra_main_log.hpp"
#include <cstring> // std::strlen

void ReShadeLogMessage([[maybe_unused]] void *module, int level, const char *message)
{
	if (reshade::addon_info *const info = reshade::find_addon(module))
		reshade::log::message(static_cast<reshade::log::level>(level), "[%.*s] %s", static_cast<int>(info->name.size()), info->name.c_str(), message);
	else if (const std::string name = reshade::ultra::logModuleName(module); !name.empty())
		reshade::log::message(static_cast<reshade::log::level>(level), "[%s] %s", name.c_str(), message);
	else
		reshade::log::message(static_cast<reshade::log::level>(level), "%s", message);
}

void ReShadeGetBasePath(char *path, size_t *size)
{
	if (size == nullptr)
		return;

	const std::string path_string = g_reshade_base_path.u8string();

	if (path == nullptr)
	{
		*size = path_string.size() + 1;
	}
	else if (*size != 0)
	{
		*size = path_string.copy(path, *size - 1);
		path[*size] = '\0';
	}
}

bool ReShadeGetConfigValue(void *, reshade::api::effect_runtime *, const char *section, const char *key, char *value, size_t *size)
{
	const reshade::ini_file &config = reshade::global_config();

	std::vector<std::string> elements;
	config.get(section != nullptr ? section : std::string(), key != nullptr ? key : std::string(), elements);

	if (size != nullptr || value != nullptr)
	{
		std::string value_string;
		for (const std::string &element : elements)
		{
			value_string += element;
			value_string += '\0';
		}

		if (size != nullptr)
		{
			if (elements.empty())
			{
				*size = 0;
			}
			else if (value == nullptr)
			{
				*size = value_string.size() + 1;
			}
			else if (*size != 0)
			{
				*size = value_string.copy(value, *size - 1);
				value[*size] = '\0';
			}
		}
	}

	return !elements.empty();
}

void ReShadeSetConfigValue(void *module, reshade::api::effect_runtime *runtime, const char *section, const char *key, const char *value)
{
	if (value == nullptr)
		return ReShadeSetConfigArray(module, runtime, section, key, nullptr, 0);

	reshade::global_config().set(section != nullptr ? section : std::string(), key != nullptr ? key : std::string(), std::string(value));

	if (runtime != nullptr)
		static_cast<reshade::runtime *>(runtime)->load_config();
}
void ReShadeSetConfigArray(void *, reshade::api::effect_runtime *runtime, const char *section, const char *key, const char *value, size_t size)
{
	reshade::ini_file &config = reshade::global_config();

	if (value != nullptr)
	{
		std::vector<std::string> elements;
		for (size_t i = 0, k = 0; i < size; i++)
		{
			if (k >= elements.size())
				elements.resize(k + 1);

			if (value[i] == '\0')
			{
				k++;
				continue;
			}

			elements[k] += value[i];
		}

		config.set(section != nullptr ? section : std::string(), key != nullptr ? key : std::string(), elements);

		// Reload configuration after it was modified
		if (runtime != nullptr)
			static_cast<reshade::runtime *>(runtime)->load_config();
	}
	else
	{
		config.remove_key(section != nullptr ? section : std::string(), key != nullptr ? key : std::string());
	}
}

#endif
