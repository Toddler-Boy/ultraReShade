/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include "dll_log.hpp"
#include "ultra_log.hpp"
#include <cstdarg>
#include <Windows.h>

bool reshade::log::open_log_file(const std::filesystem::path &path, std::error_code &ec)
{
	return reshade::ultra::openLog(path, ec);
}

void reshade::log::message(level level, const char *format, ...)
{
	static constexpr char level_names[][6] = { "ERROR", "WARN ", "INFO ", "DEBUG" };

	if (static_cast<size_t>(level) == 0)
		level = level::error;
	if (static_cast<size_t>(level) > std::size(level_names))
		level = level::debug;

	SYSTEMTIME time;
	GetLocalTime(&time);

	std::string line_string(256, '\0');

	// Start a new line
	const auto meta_length = std::snprintf(line_string.data(), line_string.size(),
#if RESHADE_VERBOSE_LOG
		"%04hd-%02hd-%02hdT"
#endif
		"%02hd:%02hd:%02hd:%03hd [%5lu] | %.5s | ",
#if RESHADE_VERBOSE_LOG
		time.wYear, time.wMonth, time.wDay,
#endif
		time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, GetCurrentThreadId(), level_names[static_cast<size_t>(level) - 1]);

	va_list args;
	va_start(args, format);
	const auto content_length = std::vsnprintf(line_string.data() + meta_length, line_string.size() + 1 - meta_length, format, args);
	va_end(args);

	const bool remaining_content = static_cast<size_t>(meta_length) + static_cast<size_t>(content_length) > line_string.size();
	line_string.resize(static_cast<size_t>(meta_length) + static_cast<size_t>(content_length));

	if (remaining_content)
	{
		va_start(args, format);
		std::vsnprintf(line_string.data() + meta_length, line_string.size() + 1 - meta_length, format, args);
		va_end(args);
	}

	line_string += '\n'; // Terminate line with line feed

#ifndef NDEBUG
	// Write line to the debug output
	OutputDebugStringA(line_string.c_str());
#endif

	reshade::ultra::logLine(std::move(line_string));
}
