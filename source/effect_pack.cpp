/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "effect_pack.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <share.h>

namespace
{
	constexpr char magic[4] = { 'U', 'F', 'X', 'P' };
	constexpr uint32_t format_version = 1;

	void put(std::string &out, uint32_t value)
	{
		out.append(reinterpret_cast<const char *>(&value), sizeof value);
	}

	bool take(const std::string &in, size_t &at, uint32_t &value)
	{
		if (in.size() - at < sizeof value)
			return false;
		std::memcpy(&value, in.data() + at, sizeof value);
		at += sizeof value;
		return true;
	}
}

void reshadefx::binary_pack::add(const std::string &key, std::string binary)
{
	_binaries[key] = std::move(binary);
}

bool reshadefx::binary_pack::find(const std::string &key, std::string &binary) const
{
	const auto it = _binaries.find(key);
	if (it == _binaries.end())
		return false;
	binary = it->second;
	return true;
}

bool reshadefx::binary_pack::write(const std::filesystem::path &path, std::string &error) const
{
	std::string data(magic, sizeof magic);
	put(data, format_version);
	put(data, static_cast<uint32_t>(_binaries.size()));

	for (const auto &[key, binary] : _binaries)
	{
		put(data, static_cast<uint32_t>(key.size()));
		data += key;
		put(data, static_cast<uint32_t>(binary.size()));
		data += binary;
	}

	FILE *const file = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
	if (file == nullptr)
	{
		error = "cannot open for writing";
		return false;
	}

	const bool is_written = std::fwrite(data.data(), 1, data.size(), file) == data.size();
	const bool is_closed = std::fclose(file) == 0;

	if (!is_written || !is_closed)
		error = "cannot write";
	return is_written && is_closed;
}

bool reshadefx::binary_pack::read(const std::filesystem::path &path, std::string &error)
{
	_binaries.clear();

	FILE *const file = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
	if (file == nullptr)
		return false;

	std::string data;
	char chunk[1 << 16];
	for (size_t count; (count = std::fread(chunk, 1, sizeof chunk, file)) != 0;)
		data.append(chunk, count);
	const bool is_failed = std::ferror(file) != 0;
	std::fclose(file);

	if (is_failed)
	{
		error = "cannot read";
		return false;
	}

	size_t at = 0;
	uint32_t version = 0, count = 0;
	if (data.size() < sizeof magic || std::memcmp(data.data(), magic, sizeof magic) != 0)
	{
		error = "not a prebuilt set";
		return false;
	}
	at = sizeof magic;
	if (!take(data, at, version) || version != format_version || !take(data, at, count))
	{
		error = "not a prebuilt set of this version";
		return false;
	}

	bool is_whole = true;
	for (uint32_t i = 0; i < count && is_whole; ++i)
	{
		uint32_t key_size = 0, size = 0;
		is_whole = take(data, at, key_size) && data.size() - at >= key_size;
		if (!is_whole)
			break;
		std::string key = data.substr(at, key_size);
		at += key_size;
		is_whole = take(data, at, size) && data.size() - at >= size;
		if (!is_whole)
			break;
		_binaries[std::move(key)] = data.substr(at, size);
		at += size;
	}

	if (is_whole && at == data.size())
		return true;

	_binaries.clear();
	error = "cut short";
	return false;
}
