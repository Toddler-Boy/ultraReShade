/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include "dll_log.hpp"
#include "ultra_main_log.hpp"
#include <cstdarg>
#include <Windows.h>

void reshade::log::message(level level, const char *format, ...)
{
	if (static_cast<size_t>(level) == 0)
		level = level::error;
	if (static_cast<size_t>(level) > static_cast<size_t>(level::debug))
		level = level::debug;

	std::string message_string(256, '\0');

	va_list args;
	va_start(args, format);
	const auto content_length = std::vsnprintf(message_string.data(), message_string.size() + 1, format, args);
	va_end(args);

	if (content_length < 0)
		return;

	const bool remaining_content = static_cast<size_t>(content_length) > message_string.size();
	message_string.resize(static_cast<size_t>(content_length));

	if (remaining_content)
	{
		va_start(args, format);
		std::vsnprintf(message_string.data(), message_string.size() + 1, format, args);
		va_end(args);
	}

#ifndef NDEBUG
	// Write line to the debug output
	OutputDebugStringA((message_string + '\n').c_str());
#endif

	ultralog::write(reshade::ultra::mainLog(), static_cast<ultralog::Level>(level), message_string);
}
