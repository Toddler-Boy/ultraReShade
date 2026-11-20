/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include "ini_file.hpp"
#include "dll_log.hpp"
#include "ultra_home.hpp"
#include <reshade_ultra_json.hpp>
#include <mutex>
#include <shared_mutex>
#include <cctype> // std::toupper
#include <cassert>
#include <charconv>
#include <cstring>
#include <optional>
#include <algorithm> // std::sort

static std::shared_mutex s_ini_cache_mutex;
static std::unordered_map<std::filesystem::path::string_type, std::unique_ptr<reshade::ini_file>> s_ini_cache;
static thread_local bool s_is_recording_defaults = false;

reshade::ini_file &reshade::global_config()
{
	return ini_file::load_cache(ultra::curatedSettingsPath());
}

reshade::ini_file::ini_file(const std::filesystem::path &path, const std::filesystem::path &user_path, const std::filesystem::path &app_path) :
	_path(path), _user_path(user_path), _app_path(app_path)
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

namespace
{
	// Json's number grammar: such a text is written as a number, any other as a string
	bool is_plain_number(std::string_view text)
	{
		size_t i = 0;
		const auto digits = [&text, &i]() {
			const size_t start = i;
			while (i < text.size() && text[i] >= '0' && text[i] <= '9')
				++i;
			return i - start;
		};

		if (i < text.size() && text[i] == '-')
			++i;
		const size_t start = i;
		if (digits() == 0 || (text[start] == '0' && i - start > 1))
			return false;
		if (i < text.size() && text[i] == '.' && (++i, digits() == 0))
			return false;
		if (i < text.size() && (text[i] == 'e' || text[i] == 'E'))
		{
			if (++i < text.size() && (text[i] == '+' || text[i] == '-'))
				++i;
			if (digits() == 0)
				return false;
		}
		return i == text.size();
	}

	// The text a json number reads back as: ReShade's "0.500000" and the file's 0.5 are the same value
	std::string number_text(double number)
	{
		char buffer[32] = {};
		const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), number);
		return std::string(buffer, end);
	}

	std::string normalized(const std::string &text)
	{
		if (!is_plain_number(text))
			return text;

		if (text.find_first_of(".eE") == std::string::npos)
		{
			int64_t integer = 0;
			if (const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), integer); ec == std::errc())
				return std::to_string(integer);
		}

		double number = 0.0;
		std::from_chars(text.data(), text.data() + text.size(), number);
		return number_text(number);
	}

	bool is_same(const std::vector<std::string> &a, const std::vector<std::string> &b)
	{
		return std::ranges::equal(a, b, [](const std::string &x, const std::string &y) { return normalized(x) == normalized(y); });
	}

	// A scalar's text as ReShade's settings hold it; nullopt for a value that is no scalar
	std::optional<std::string> scalar_text(const reshade::json::value *value)
	{
		const char *text = nullptr;
		size_t length = 0;
		int64_t integer = 0;
		double number = 0.0;
		bool flag = false;

		switch (ReShadeJsonGetType(value))
		{
		case reshade::json::type::null:
			return std::string();
		case reshade::json::type::boolean:
			ReShadeJsonGetBool(value, &flag);
			return std::string(flag ? "1" : "0");
		case reshade::json::type::integer:
			ReShadeJsonGetInt(value, &integer);
			return std::to_string(integer);
		case reshade::json::type::number:
			ReShadeJsonGetNumber(value, &number);
			return number_text(number);
		case reshade::json::type::string:
			ReShadeJsonGetString(value, &text, &length);
			return std::string(text, length);
		case reshade::json::type::array:
		case reshade::json::type::object:
			break;
		}
		return std::nullopt;
	}

	void set_scalar(reshade::json::value *target, const std::string &text, bool is_flag)
	{
		if (is_flag)
		{
			ReShadeJsonSetBool(target, text != "0" && !text.empty());
			return;
		}

		if (is_plain_number(text))
		{
			const std::string value = normalized(text);
			int64_t integer = 0;
			if (const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), integer); ec == std::errc() && end == value.data() + value.size())
			{
				ReShadeJsonSetInt(target, integer);
				return;
			}

			double number = 0.0;
			std::from_chars(value.data(), value.data() + value.size(), number);
			if (ReShadeJsonSetNumber(target, number))
				return;
		}

		ReShadeJsonSetString(target, text.data(), text.size());
	}

	bool case_insensitive_less(std::string a, std::string b)
	{
		std::transform(a.begin(), a.end(), a.begin(), [](std::string::value_type c) { return static_cast<std::string::value_type>(std::toupper(c)); });
		std::transform(b.begin(), b.end(), b.begin(), [](std::string::value_type c) { return static_cast<std::string::value_type>(std::toupper(c)); });
		return a < b;
	}

	template <typename Sections>
	auto find_in(Sections &sections, const std::string &section, const std::string &key) -> decltype(&sections.begin()->second.begin()->second)
	{
		const auto it1 = sections.find(section);
		if (it1 == sections.end())
			return nullptr;
		const auto it2 = it1->second.find(key);
		return it2 != it1->second.end() ? &it2->second : nullptr;
	}
}

// The hotkeys are ultraGame's alone: [INPUT] of the user file does not count
const reshade::ini_file::value_type *reshade::ini_file::find(const std::string &section, const std::string &key) const
{
	if (const value_type *const value = section != "INPUT" ? find_in(_user.sections, section, key) : nullptr)
		return value;
	if (const value_type *const value = find_in(_curated.sections, section, key))
		return value;
	return find_in(_app.sections, section, key);
}

void reshade::ini_file::assign(const std::string &section, const std::string &key, value_type &&value)
{
	const value_type *const below = find_in(_curated.sections, section, key) != nullptr ? find_in(_curated.sections, section, key) : find_in(_app.sections, section, key);
	const value_type *const user = find_in(_user.sections, section, key);

	if (s_is_recording_defaults)
	{
		if (below == nullptr && user == nullptr)
			_defaults[section][key] = std::move(value);
		return;
	}

	if (const value_type *const fallback = below != nullptr ? below : find_in(_defaults, section, key);
		fallback != nullptr && is_same(fallback->items, value.items))
	{
		if (user != nullptr)
			remove_key(section, key);
		return;
	}

	if (user != nullptr && is_same(user->items, value.items) && user->is_list == value.is_list)
		return;

	_user.sections[section][key] = std::move(value);
	mark_modified();
}

void reshade::ini_file::remove_key(const std::string &section, const std::string &key)
{
	if (s_is_recording_defaults)
		return;

	const auto it1 = _user.sections.find(section);
	if (it1 == _user.sections.end())
		return;
	const auto it2 = it1->second.find(key);
	if (it2 == it1->second.end())
		return;
	it1->second.erase(it2);
	if (it1->second.empty())
		_user.sections.erase(it1);
	mark_modified();
}

void reshade::ini_file::mark_modified()
{
	_modified = true;
	_modified_at = std::filesystem::file_time_type::clock::now();
}

const reshade::json::value *reshade::ini_file::curated_member(const char *name) const
{
	return ReShadeJsonFind(_curated.document.get(), name);
}
const reshade::json::value *reshade::ini_file::user_member(const char *name) const
{
	return ReShadeJsonFind(_user.document.get(), name);
}
const reshade::json::value *reshade::ini_file::app_member(const char *name) const
{
	return ReShadeJsonFind(_app.document.get(), name);
}
void reshade::ini_file::set_user_member(const char *name, ultra::JsonTree value)
{
	if (_user.document == nullptr)
		_user.document.reset(ReShadeJsonCreate(json::type::object));

	if (value == nullptr)
		ReShadeJsonRemove(_user.document.get(), name);
	else
		ReShadeJsonAssign(ReShadeJsonMember(_user.document.get(), name), value.get());

	mark_modified();
}

// A layer is read again only when its file was written since; a missing file is an empty layer (an error only where required)
void reshade::ini_file::load_layer(const std::filesystem::path &path, layer &into, const char *config_name, bool is_required)
{
	std::error_code ec;
	const std::filesystem::file_time_type modified_at = std::filesystem::last_write_time(path, ec);
	if (ec)
	{
		if (into.document != nullptr || into.modified_at != std::filesystem::file_time_type::min() || (is_required && !into.is_failed))
		{
			if (is_required)
				log::message(log::level::error, "'%s' does not exist, its settings are missing!", path.u8string().c_str());
			into = {};
			into.is_failed = is_required;
		}
		return;
	}
	if (into.modified_at == modified_at)
		return;

	json::error error;
	ultra::JsonTree document = ultra::readJsonFile(path, error);
	if (document == nullptr)
	{
		log::message(log::level::error, "%s The settings in it are not used.", error.message);
		into.sections.clear();
		into.document.reset();
		into.modified_at = modified_at;
		into.is_failed = true;
		return;
	}

	sections_type sections;
	const json::value *const config = ReShadeJsonFind(document.get(), config_name);
	const bool is_one_section = std::strcmp(config_name, "hotkeys") == 0;

	for (size_t i = 0, count = ReShadeJsonGetSize(config); i < count; ++i)
	{
		const char *section_name = nullptr;
		const json::value *const section = ReShadeJsonAt(config, i, &section_name);

		const auto add_key = [&path, &sections](const std::string &section_name, const char *key_name, const json::value *value) {
			value_type &target = sections[section_name][key_name];
			if (ReShadeJsonGetType(value) == json::type::array)
			{
				target.is_list = true;
				for (size_t k = 0, items = ReShadeJsonGetSize(value); k < items; ++k)
				{
					if (std::optional<std::string> text = scalar_text(ReShadeJsonAt(value, k, nullptr)))
						target.items.push_back(std::move(*text));
					else
						log::message(log::level::error, "'%s': [%s] %s holds a list or object in its list, which is left out.", path.u8string().c_str(), section_name.c_str(), key_name);
				}
			}
			else if (std::optional<std::string> text = scalar_text(value))
			{
				target.is_flag = ReShadeJsonGetType(value) == json::type::boolean;
				target.items.push_back(std::move(*text));
			}
			else
			{
				log::message(log::level::error, "'%s': [%s] %s is an object, which no setting can be; it is left out.", path.u8string().c_str(), section_name.c_str(), key_name);
				sections[section_name].erase(key_name);
			}
		};

		if (is_one_section)
		{
			add_key("INPUT", section_name, section);
			continue;
		}

		if (ReShadeJsonGetType(section) != json::type::object)
		{
			log::message(log::level::error, "'%s': config member '%s' is no section (an object), it is left out.", path.u8string().c_str(), section_name);
			continue;
		}

		for (size_t k = 0, keys = ReShadeJsonGetSize(section); k < keys; ++k)
		{
			const char *key_name = nullptr;
			const json::value *const value = ReShadeJsonAt(section, k, &key_name);
			add_key(section_name, key_name, value);
		}
	}

	if (config != nullptr && ReShadeJsonGetType(config) != json::type::object)
		log::message(log::level::error, "'%s': '%s' is no object, its settings are not used.", path.u8string().c_str(), config_name);
	else if (config == nullptr && is_required)
		log::message(log::level::error, "'%s' has no '%s', its settings are missing!", path.u8string().c_str(), config_name);

	into.document = std::move(document);
	into.sections = std::move(sections);
	into.modified_at = modified_at;
	into.is_failed = false;
}

void reshade::ini_file::load()
{
	load_layer(_path, _curated, "config", true);
	load_layer(_app_path, _app, "hotkeys", true);

	// Own changes not saved yet win over the file
	if (!_modified)
		load_layer(_user_path, _user, "config", false);
}

bool reshade::ini_file::save()
{
	if (!_modified)
		return true;

	// Reset state even on failure to avoid 'flush_cache' repeatedly trying and failing to save
	_modified = false;

	if (_user.is_failed)
	{
		log::message(log::level::error, "'%s' is not saved: the file there could not be read, and saving would lose what it holds.", _user_path.u8string().c_str());
		return false;
	}

	std::error_code ec;
	if (const std::filesystem::file_time_type modified_at = std::filesystem::last_write_time(_user_path, ec);
		!ec && modified_at != _user.modified_at)
	{
		log::message(log::level::error, "'%s' changed on disk since it was read, so the changes made since are not saved!", _user_path.u8string().c_str());
		return false;
	}

	ultra::JsonTree document(ReShadeJsonCreate(json::type::object));
	ReShadeJsonSetInt(ReShadeJsonMember(document.get(), "schema"), 1);
	json::value *const config = ReShadeJsonMember(document.get(), "config");
	ReShadeJsonSetType(config, json::type::object);

	std::vector<std::string> section_names, key_names;
	for (const std::pair<const std::string, section_type> &section : _user.sections)
		section_names.push_back(section.first);
	std::sort(section_names.begin(), section_names.end(), case_insensitive_less);

	for (const std::string &section_name : section_names)
	{
		const section_type &keys = _user.sections.at(section_name);
		if (keys.empty())
			continue;

		key_names.clear();
		for (const std::pair<const std::string, value_type> &key : keys)
			key_names.push_back(key.first);
		std::sort(key_names.begin(), key_names.end(), case_insensitive_less);

		json::value *const section = ReShadeJsonMember(config, section_name.c_str());
		ReShadeJsonSetType(section, json::type::object);

		for (const std::string &key_name : key_names)
		{
			const value_type &value = keys.at(key_name);
			json::value *const target = ReShadeJsonMember(section, key_name.c_str());

			if (value.is_list)
			{
				ReShadeJsonSetType(target, json::type::array);
				for (size_t i = 0; i < value.items.size(); ++i)
					set_scalar(ReShadeJsonInsert(target, i), value.items[i], value.is_flag);
			}
			else
			{
				set_scalar(target, value.items.empty() ? std::string() : value.items.front(), value.is_flag);
			}
		}
	}

	bool has_members = ReShadeJsonGetSize(config) != 0;
	for (size_t i = 0, count = ReShadeJsonGetSize(_user.document.get()); i < count; ++i)
	{
		const char *name = nullptr;
		const json::value *const member = ReShadeJsonAt(_user.document.get(), i, &name);
		if (std::strcmp(name, "schema") == 0 || std::strcmp(name, "config") == 0)
			continue;

		ReShadeJsonAssign(ReShadeJsonMember(document.get(), name), member);
		has_members = true;
	}

	// No user file while there is no change
	if (!has_members && ec)
		return true;

	json::error error;
	if (!ultra::writeJsonFile(_user_path, *document, error))
	{
		log::message(log::level::error, "%s", error.message);
		return false;
	}

	_user.modified_at = std::filesystem::last_write_time(_user_path, ec);
	_user.document = std::move(document);
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

reshade::ini_file &reshade::ini_file::load_cache(const std::filesystem::path &path)
{
	assert(!path.empty() && path.is_absolute());

	const std::unique_lock<std::shared_mutex> lock(s_ini_cache_mutex);

	const auto insert = s_ini_cache.try_emplace(path);
	const auto it = insert.first;

	// Only construct when actually adding a new entry to the cache, since the constructor performs a costly load of the files
	if (insert.second)
		it->second = std::make_unique<ini_file>(path, ultra::userSettingsPath(), ultra::appSettingsPath());
	else
		it->second->load();

	return *it->second;
}
