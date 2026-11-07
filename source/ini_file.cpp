/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include "ini_file.hpp"
#include "dll_log.hpp"
#include "ultra_config.hpp"
#include <mutex>
#include <shared_mutex>
#include <cctype> // std::toupper
#include <cassert>
#include <algorithm> // std::min, std::sort, std::transform
#include <utf8/core.h>

static std::shared_mutex s_ini_cache_mutex;
static std::unordered_map<std::filesystem::path::string_type, std::unique_ptr<reshade::ini_file>> s_ini_cache;
static thread_local bool s_is_recording_defaults = false;

reshade::ini_file &reshade::global_config()
{
	return ini_file::load_cache(g_reshade_base_path / std::filesystem::u8path(ultra::configBaseName() + ".ini"));
}

reshade::ini_file::ini_file(const std::filesystem::path &path) : _path(path)
{
	load();
}
reshade::ini_file::ini_file(const std::filesystem::path &path, const std::filesystem::path &user_path) : _path(path), _user_path(user_path)
{
	load();
}

reshade::ini_file::defaults_scope::defaults_scope()
{
	s_is_recording_defaults = true;
}
reshade::ini_file::defaults_scope::~defaults_scope()
{
	s_is_recording_defaults = false;
}

static const std::vector<std::string> *find_in(const std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> &sections, const std::string &section, const std::string &key)
{
	const auto it1 = sections.find(section);
	if (it1 == sections.end())
		return nullptr;
	const auto it2 = it1->second.find(key);
	return it2 != it1->second.end() ? &it2->second : nullptr;
}

// As 'save' writes the value: elements that differ only there are the same value
static std::string serialize(const std::vector<std::string> &elements)
{
	std::string value;
	for (const std::string &element : elements)
	{
		// Empty elements mess with escaped commas, so simply skip them
		if (element.empty())
			continue;

		value.reserve(value.size() + element.size() + 1);
		for (const char c : element)
			value.append(c == ',' ? 2 : 1, c);
		value += ','; // Separate multiple values with a comma
	}

	// Remove the last comma
	if (!value.empty())
	{
		assert(value.back() == ',');
		value.pop_back();
	}

	return value;
}

const std::vector<std::string> *reshade::ini_file::find(const std::string &section, const std::string &key) const
{
	if (const value_type *const value = find_in(_sections, section, key))
		return value;
	return is_layered() ? find_in(_curated, section, key) : nullptr;
}

void reshade::ini_file::assign(const std::string &section, const std::string &key, value_type &&value)
{
	if (!is_layered())
	{
		if (s_is_recording_defaults)
			return;
		_sections[section][key] = std::move(value);
		mark_modified();
		return;
	}

	const value_type *const curated = find_in(_curated, section, key);
	const value_type *const user = find_in(_sections, section, key);

	if (s_is_recording_defaults)
	{
		if (curated == nullptr && user == nullptr)
			_defaults[section][key] = std::move(value);
		return;
	}

	const std::string serialized = serialize(value);

	if (const value_type *const fallback = curated != nullptr ? curated : find_in(_defaults, section, key);
		fallback != nullptr && serialize(*fallback) == serialized)
	{
		if (user != nullptr)
			remove_key(section, key);
		return;
	}

	if (user != nullptr && serialize(*user) == serialized)
		return;

	_sections[section][key] = std::move(value);
	mark_modified();
}

void reshade::ini_file::remove_key(const std::string &section, const std::string &key)
{
	if (s_is_recording_defaults)
		return;

	const auto it1 = _sections.find(section);
	if (it1 == _sections.end())
		return;
	const auto it2 = it1->second.find(key);
	if (it2 == it1->second.end())
		return;
	it1->second.erase(it2);
	if (it1->second.empty() && is_layered())
		_sections.erase(it1);
	mark_modified();
}

bool reshade::ini_file::read(const std::filesystem::path &path, int share_flag, sections_type &sections)
{
	FILE *const file = _wfsopen(path.c_str(), L"r", share_flag);
	if (file == nullptr)
		return false;

	// Remove BOM (0xefbbbf means 0xfeff)
	if (fgetc(file) != utf8::bom[0] || fgetc(file) != utf8::bom[1] || fgetc(file) != utf8::bom[2])
		fseek(file, 0, SEEK_SET);

	std::string line_data, section;
	line_data.resize(BUFSIZ);
	while (fgets(line_data.data(), static_cast<int>(line_data.size() + 1), file))
	{
		size_t line_length;
		while ((line_length = std::strlen(line_data.data())) == line_data.size() && line_data.back() != '\n')
		{
			line_data.resize(line_data.size() + BUFSIZ);
			if (!fgets(line_data.data() + line_length, static_cast<int>(line_data.size() - line_length + 1), file))
				break;
		}

		const std::string_view line = trim(std::string_view(line_data.data(), line_length), " \t\r\n");

		if (line.empty() || line[0] == ';' || line[0] == '/' || line[0] == '#')
			continue;

		// Read section name
		if (line[0] == '[')
		{
			section = trim(line.substr(0, line.find(']')), " \t[]");
			continue;
		}

		// Read section content
		const size_t assign_index = line.find('=');
		if (assign_index != std::string::npos)
		{
			const std::string_view key = trim(line.substr(0, assign_index));
			const std::string_view value = trim(line.substr(assign_index + 1));

			if (value.empty())
			{
				sections[section].insert({ std::string(key), {} });
				continue;
			}

			// Append to key if it already exists
			std::vector<std::string> &elements = sections[section][std::string(key)];
			for (size_t offset = 0, base = 0, len = value.size(); offset <= len;)
			{
				// Treat ",," as an escaped comma and only split on single ","
				const size_t found = std::min(value.find(',', offset), len);
				if (found + 1 < len && value[found + 1] == ',')
				{
					offset = found + 2;
				}
				else
				{
					std::string &element = elements.emplace_back();
					element.reserve(found - base);

					while (base < found)
					{
						const char c = value[base++];
						element += c;

						if (c == ',' && base < found && value[base] == ',')
							base++; // Skip second comma in a ",," escape sequence
					}

					offset = base = found + 1;
				}
			}
		}
		else
		{
			sections[section].insert({ std::string(line), {} });
		}
	}

	fclose(file);

	return true;
}

bool reshade::ini_file::load()
{
	if (is_layered())
		load_curated();

	const std::filesystem::path &path = is_layered() ? _user_path : _path;

	std::error_code ec;
	const std::filesystem::file_time_type modified_at = std::filesystem::last_write_time(path, ec);
	if (!ec && _modified_at >= modified_at)
		return true; // Skip loading if there was no modification to the file since it was last loaded

	// Clear when file does not exist too
	_sections.clear();

	if (!read(path, SH_DENYWR, _sections))
	{
		// A user file that cannot be read is left out, the curated file still applies
		if (is_layered() && !ec && !_is_user_file_broken)
			log::message(log::level::error, "Failed to read '%s', the changes in it are ignored!", path.u8string().c_str());
		_is_user_file_broken = is_layered() && !ec;
		return false;
	}

	_is_user_file_broken = false;
	_modified = false;
	_modified_at = modified_at;

	return true;
}

// Without share restrictions, so the add-on's own writes into it never fail while ReShade reads
bool reshade::ini_file::load_curated()
{
	std::error_code ec;
	const std::filesystem::file_time_type modified_at = std::filesystem::last_write_time(_path, ec);
	// A missing one is an empty layer (only the global one must exist), one gone meanwhile keeps what it had
	if (ec)
		return false;
	if (_curated_modified_at >= modified_at)
		return true;

	sections_type sections;
	if (!read(_path, _SH_DENYNO, sections))
	{
		log::message(log::level::error, "Failed to read '%s', the settings read from it before stay in use!", _path.u8string().c_str());
		return false;
	}

	_curated = std::move(sections);
	_curated_modified_at = modified_at;
	return true;
}

bool reshade::ini_file::save()
{
	if (!_modified)
		return true;

	// Reset state even on failure to avoid 'flush_cache' repeatedly trying and failing to save
	_modified = false;

	const std::filesystem::path &path = is_layered() ? _user_path : _path;

	std::error_code ec;
	const std::filesystem::file_time_type modified_at = std::filesystem::last_write_time(path, ec);
	if (!ec && (modified_at - _modified_at) > std::chrono::seconds(2))
	{
		if (is_layered())
			log::message(log::level::error, "'%s' changed on disk since it was read, so the changes made since are not saved!", path.u8string().c_str());
		return false; // File exists and was modified on disk and therefore may have different data, so cannot save
	}

	std::string data;
	std::vector<std::string> section_names, key_names;

	section_names.reserve(_sections.size());
	for (const std::pair<const std::string, section_type> &section : _sections)
		section_names.push_back(section.first);

	// Sort sections to generate consistent files
	std::sort(section_names.begin(), section_names.end(),
		[](std::string a, std::string b) {
			std::transform(a.begin(), a.end(), a.begin(), [](std::string::value_type c) { return static_cast<std::string::value_type>(std::toupper(c)); });
			std::transform(b.begin(), b.end(), b.begin(), [](std::string::value_type c) { return static_cast<std::string::value_type>(std::toupper(c)); });
			return a < b;
		});

	for (const std::string &section_name : section_names)
	{
		if (const section_type &keys = _sections.at(section_name); !keys.empty())
		{
			key_names.clear();
			key_names.reserve(keys.size());
			for (const std::pair<const std::string, value_type> &key : keys)
				key_names.push_back(key.first);

			std::sort(key_names.begin(), key_names.end(),
				[](std::string a, std::string b) {
					std::transform(a.begin(), a.end(), a.begin(), [](std::string::value_type c) { return static_cast<std::string::value_type>(std::toupper(c)); });
					std::transform(b.begin(), b.end(), b.begin(), [](std::string::value_type c) { return static_cast<std::string::value_type>(std::toupper(c)); });
					return a < b;
				});

			// Empty section should have been sorted to the top, so do not need to append it before keys
			if (!section_name.empty())
				data += '[' + section_name + ']' + '\n';

			for (const std::string &key_name : key_names)
			{
				data += key_name + '=';
				data += serialize(keys.at(key_name));
				data += '\n';
			}

			data += '\n';
		}
	}

	// No user file while there is no change
	if (is_layered() && data.empty() && ec)
		return true;

	FILE *const file = _wfsopen(path.c_str(), L"w", SH_DENYWR);
	if (file == nullptr)
	{
		if (is_layered())
			log::message(log::level::error, "Failed to open '%s' for writing, the changes are not saved!", path.u8string().c_str());
		return false;
	}
	const size_t file_size_written = fwrite(data.data(), 1, data.size(), file);
	fclose(file);
	if (file_size_written != data.size())
	{
		if (is_layered())
			log::message(log::level::error, "Failed to write '%s' completely!", path.u8string().c_str());
		return false;
	}

	// Flush stream to disk before updating last write time
	_modified_at = std::filesystem::last_write_time(path, ec);

	assert(!ec && (is_layered() || std::filesystem::file_size(path, ec) > 0));

	return true;
}

bool reshade::ini_file::flush_cache()
{
	bool success = true;

	const std::shared_lock<std::shared_mutex> lock(s_ini_cache_mutex);

	// Save all files that were modified in one second intervals
	for (auto &file : s_ini_cache)
		// Check modified status before requesting file time, since the latter is costly and therefore should be avoided when not necessary
		if (file.second->_modified && (std::filesystem::file_time_type::clock::now() - file.second->_modified_at) > std::chrono::seconds(1))
			success &= file.second->save();

	return success;
}
bool reshade::ini_file::flush_cache(const std::filesystem::path &path)
{
	assert(!path.empty() && path.is_absolute());

	const std::shared_lock<std::shared_mutex> lock(s_ini_cache_mutex);

	const auto it = s_ini_cache.find(path);
	return it != s_ini_cache.end() && it->second->save();
}

void reshade::ini_file::clear_cache()
{
	const std::unique_lock<std::shared_mutex> lock(s_ini_cache_mutex);

	s_ini_cache.clear();
}
void reshade::ini_file::clear_cache(const std::filesystem::path &path)
{
	assert(!path.empty() && path.is_absolute());

	const std::unique_lock<std::shared_mutex> lock(s_ini_cache_mutex);

	s_ini_cache.erase(path);
}

reshade::ini_file *reshade::ini_file::find_cache(const std::filesystem::path &path)
{
	assert(!path.empty() && path.is_absolute());

	const std::shared_lock<std::shared_mutex> lock(s_ini_cache_mutex);

	const auto it = s_ini_cache.find(path);
	return it != s_ini_cache.end() ? it->second.get() : nullptr;
}
reshade::ini_file &reshade::ini_file::load_cache(const std::filesystem::path &path)
{
	assert(!path.empty() && path.is_absolute());

	const std::unique_lock<std::shared_mutex> lock(s_ini_cache_mutex);

	const auto insert = s_ini_cache.try_emplace(path);
	const auto it = insert.first;

	// Only construct when actually adding a new entry to the cache, since the 'ini_file' constructor performs a costly load of the file
	if (insert.second)
		it->second = ultra::isCuratedConfig(path) ? std::make_unique<ini_file>(path, ultra::userConfigPath(path)) : std::make_unique<ini_file>(path);
	// Don't reload file when it was just loaded or there are still modifications pending
	else if (!it->second->_modified)
		it->second->load();

	return *it->second;
}
