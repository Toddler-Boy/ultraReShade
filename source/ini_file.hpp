/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include "ultra_json.hpp"

extern std::filesystem::path g_reshade_dll_path;
extern std::filesystem::path g_reshade_base_path;
extern std::filesystem::path g_target_executable_path;

inline std::string_view trim(std::string_view str, const char chars[] = " \t")
{
	const size_t first = str.find_first_not_of(chars);
	return first != std::string_view::npos ? std::string_view(str.data() + first, str.find_last_not_of(chars) + 1 - first) : std::string_view();
}

namespace reshade
{
	/// <summary>
	/// The 'config' of reshade-user.json over reshade.json's, [INPUT] over ultraGame's hotkeys (its settings.json, kept whole): a section is an object, a key a value or an array.
	/// Writes go only into the user file and only where they differ from what is below.
	/// </summary>
	class ini_file
	{
	public:
		ini_file(const std::filesystem::path &path, const std::filesystem::path &user_path, const std::filesystem::path &app_path);

		/// <summary>
		/// Gets the path to the curated file.
		/// </summary>
		const std::filesystem::path &path() const { return _path; }

		/// <summary>
		/// While one exists on a thread, writes there to keys no layer has record the value as the default instead.
		/// </summary>
		class defaults_scope
		{
		public:
			defaults_scope();
			~defaults_scope();
		};

		/// <summary>
		/// Checks whether the specified <paramref name="section"/> and <paramref name="key"/> currently exist in the INI.
		/// </summary>
		bool has(const std::string &section, const std::string &key) const
		{
			return find(section, key) != nullptr;
		}

		/// <summary>
		/// Gets the value of the specified <paramref name="section"/> and <paramref name="key"/> from the INI.
		/// </summary>
		/// <param name="value">Reference filled with the data of this INI entry.</param>
		/// <returns><see langword="true"/> if the key exists, <see langword="false"/> otherwise.</returns>
		template <typename T>
		bool get(const std::string &section, const std::string &key, T &value) const
		{
			const value_type *const found = find(section, key);
			if (found == nullptr)
				return false;
			value = convert<T>(found->items, 0);
			return true;
		}
		template <typename T, size_t SIZE>
		bool get(const std::string &section, const std::string &key, T(&values)[SIZE]) const
		{
			const value_type *const found = find(section, key);
			if (found == nullptr)
				return false;
			for (size_t i = 0; i < SIZE; ++i)
				values[i] = convert<T>(found->items, i);
			return true;
		}
		template <typename T>
		bool get(const std::string &section, const std::string &key, std::vector<T> &values) const
		{
			const value_type *const found = find(section, key);
			if (found == nullptr)
				return false;
			if constexpr (std::is_same_v<T, std::string>)
			{
				values = found->items;
			}
			else if constexpr (std::is_same_v<T, std::pair<std::string, std::string>>)
			{
				values.resize(found->items.size());
				for (size_t i = 0; i < found->items.size(); ++i)
				{
					std::string value = convert<std::string>(found->items, i);
					if (const size_t equals_sign = value.find('=');
						equals_sign != std::string::npos)
						values[i] = { value.substr(0, equals_sign), value.substr(equals_sign + 1) };
					else
						values[i].first = std::move(value);
				}
			}
			else
			{
				values.resize(found->items.size());
				for (size_t i = 0; i < found->items.size(); ++i)
					values[i] = convert<T>(found->items, i);
			}
			return true;
		}

		/// <summary>
		/// Returns <see langword="true"/> only if the specified <paramref name="section"/> and <paramref name="key"/> exists and is not zero.
		/// </summary>
		/// <returns><see langword="true"/> if the key exists and is not zero, <see langword="false"/> otherwise.</returns>
		bool get(const std::string &section, const std::string &key) const
		{
			bool value = false;
			return get<bool>(section, key, value) && value;
		}

		/// <summary>
		/// Sets the value of the specified <paramref name="section"/> and <paramref name="key"/> to a new <paramref name="value"/>.
		/// </summary>
		/// <param name="value">Data to set this INI entry to.</param>
		template <typename T>
		void set(const std::string &section, const std::string &key, const T &value)
		{
			value_type v;
			if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, const char *>)
			{
				v.items.assign(1, value);
			}
			else if constexpr (std::is_same_v<T, bool>)
			{
				v.items.assign(1, value ? "1" : "0");
				v.is_flag = true;
			}
			else if constexpr (std::is_same_v<T, std::filesystem::path>)
			{
				v.items.assign(1, value.u8string());
			}
			else
			{
				v.items.assign(1, std::to_string(value));
			}
			assign(section, key, std::move(v));
		}
		template <typename T, size_t SIZE>
		void set(const std::string &section, const std::string &key, const T(&values)[SIZE], const size_t size = SIZE)
		{
			value_type v;
			v.items.resize(size);
			v.is_list = true;
			for (size_t i = 0; i < size; ++i)
				v.items[i] = std::to_string(values[i]);
			assign(section, key, std::move(v));
		}
		template <typename T>
		void set(const std::string &section, const std::string &key, const std::vector<T> &values)
		{
			value_type v;
			v.is_list = true;
			if constexpr (std::is_same_v<T, std::string>)
			{
				v.items = values;
			}
			else if constexpr (std::is_same_v<T, bool>)
			{
				v.items.resize(values.size());
				for (size_t i = 0; i < values.size(); ++i)
					v.items[i] = values[i] ? "1" : "0";
				v.is_flag = true;
			}
			else if constexpr (std::is_same_v<T, std::filesystem::path>)
			{
				v.items.resize(values.size());
				for (size_t i = 0; i < values.size(); ++i)
					v.items[i] = values[i].u8string();
			}
			else if constexpr (std::is_same_v<T, std::pair<std::string, std::string>>)
			{
				v.items.resize(values.size());
				for (size_t i = 0; i < values.size(); ++i)
				{
					const std::pair<std::string, std::string> &value = values[i];
					v.items[i] = value.first;
					if (!value.second.empty())
						v.items[i] += '=' + value.second;
				}
			}
			else
			{
				v.items.resize(values.size());
				for (size_t i = 0; i < values.size(); ++i)
					v.items[i] = std::to_string(values[i]);
			}
			assign(section, key, std::move(v));
		}

		void set(const std::string &section, const std::string &key, std::string &&value)
		{
			value_type v;
			v.items.assign(1, std::move(value));
			assign(section, key, std::move(v));
		}
		void set(const std::string &section, const std::string &key, std::vector<std::string> &&values)
		{
			value_type v;
			v.items = std::move(values);
			v.is_list = true;
			assign(section, key, std::move(v));
		}

		/// <summary>
		/// Removes the specified <paramref name="key"/> from the <paramref name="section"/> of the user file, so the value below applies again.
		/// </summary>
		void remove_key(const std::string &section, const std::string &key);

		/// <summary>
		/// The curated file's member beside 'config' (null when it has none), and the user file's.
		/// </summary>
		const json::value *curated_member(const char *name) const;
		const json::value *user_member(const char *name) const;
		/// <summary>
		/// The member of ultraGame's settings.json (null when it has none).
		/// </summary>
		const json::value *app_member(const char *name) const;
		/// <summary>
		/// Replaces the user file's member (null removes it); saved with the next flush.
		/// </summary>
		void set_user_member(const char *name, ultra::JsonTree value);

		/// <summary>
		/// Loads what changed on disk since the last load.
		/// </summary>
		void load();
		/// <summary>
		/// Saves the user file when something changed.
		/// </summary>
		bool save();

		/// <summary>
		/// Saves all changes to files that were loaded through <see cref="load_cache"/> to disk, once they are a second old.
		/// </summary>
		static bool flush_cache();
		static bool flush_cache(const std::filesystem::path &path);

		/// <summary>
		/// Gets the specified file from cache or opens it when it was not cached yet.
		/// </summary>
		/// <param name="path">Absolute path to the curated file.</param>
		/// <returns>Reference to the cached data.</returns>
		static ini_file &load_cache(const std::filesystem::path &path);

	private:
		template <typename T>
		static const T convert(const std::vector<std::string> &values, size_t i)
		{
			if constexpr (std::is_same_v<T, bool>)
				return convert<int>(values, i) != 0 || (i < values.size() && (values[i] == "true" || values[i] == "True" || values[i] == "TRUE"));
			if constexpr (std::is_same_v<T, int>)
				return static_cast<int>(convert<long>(values, i));
			if constexpr (std::is_same_v<T, unsigned int>)
				return static_cast<unsigned int>(convert<unsigned long>(values, i));
			if constexpr (std::is_same_v<T, long>)
				return i < values.size() ? std::strtol(values[i].c_str(), nullptr, 10) : 0l;
			if constexpr (std::is_same_v<T, unsigned long>)
				return i < values.size() ? std::strtoul(values[i].c_str(), nullptr, 10) : 0ul;
			if constexpr (std::is_same_v<T, long long>)
				return i < values.size() ? std::strtoll(values[i].c_str(), nullptr, 10) : 0ll;
			if constexpr (std::is_same_v<T, unsigned long long>)
				return i < values.size() ? std::strtoull(values[i].c_str(), nullptr, 10) : 0ull;
			if constexpr (std::is_same_v<T, float>)
				return static_cast<float>(convert<double>(values, i));
			if constexpr (std::is_same_v<T, double>)
				return i < values.size() ? std::strtod(values[i].c_str(), nullptr) : 0.0;
			if constexpr (std::is_same_v<T, std::string>)
				return i < values.size() ? values[i] : std::string();
			if constexpr (std::is_same_v<T, std::filesystem::path>)
				return i < values.size() ? std::filesystem::u8path(values[i]) : std::filesystem::path();
		}

		/// <summary>
		/// A key's value: its items as text (a scalar is one item), whether it is written as an array, and as booleans.
		/// </summary>
		struct value_type
		{
			std::vector<std::string> items;
			bool is_list = false;
			bool is_flag = false;
		};
		using section_type = std::unordered_map<std::string, value_type>;
		using sections_type = std::unordered_map<std::string, section_type>;

		// A file of the home as last read: its whole tree, the 'config' object as sections, and when it was written
		struct layer
		{
			ultra::JsonTree document;
			sections_type sections;
			std::filesystem::file_time_type modified_at = std::filesystem::file_time_type::min();
			bool is_failed = false;
		};

		const value_type *find(const std::string &section, const std::string &key) const;
		void assign(const std::string &section, const std::string &key, value_type &&value);
		void mark_modified();

		void load_layer(const std::filesystem::path &path, layer &into, const char *config_name, bool is_required);

		const std::filesystem::path _path;
		const std::filesystem::path _user_path;
		const std::filesystem::path _app_path;

		layer _curated;
		layer _user;
		layer _app;
		bool _modified = false;
		std::filesystem::file_time_type _modified_at;

		// What ReShade writes for keys no layer has, recorded in a defaults_scope: writing it again is no change
		sections_type _defaults;
	};

	/// <summary>
	/// The home's settings, shared by every effect runtime of the process.
	/// </summary>
	ini_file &global_config();
}
