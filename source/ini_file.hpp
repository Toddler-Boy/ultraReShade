/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>

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
	class ini_file
	{
	public:
		/// <summary>
		/// Opens the INI file at the specified <paramref name="path"/>.
		/// </summary>
		/// <param name="path">Path to the INI file to access.</param>
		explicit ini_file(const std::filesystem::path &path);
		/// <summary>
		/// Opens the curated INI file at <paramref name="path"/> with the user INI file at <paramref name="user_path"/> on top.
		/// Reads see both (the user file wins), writes go only into the user file and only where they differ from the curated file.
		/// </summary>
		ini_file(const std::filesystem::path &path, const std::filesystem::path &user_path);

		/// <summary>
		/// Gets the path to this INI file.
		/// </summary>
		const std::filesystem::path &path() const { return _path; }

		bool is_layered() const { return !_user_path.empty(); }

		/// <summary>
		/// While one exists on a thread, writes there to keys no layer has record the value as the default instead (layered files only; other files ignore writes meanwhile).
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
			value = convert<T>(*found, 0);
			return true;
		}
		template <typename T, size_t SIZE>
		bool get(const std::string &section, const std::string &key, T(&values)[SIZE]) const
		{
			const value_type *const found = find(section, key);
			if (found == nullptr)
				return false;
			for (size_t i = 0; i < SIZE; ++i)
				values[i] = convert<T>(*found, i);
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
				values = *found;
			}
			else if constexpr (std::is_same_v<T, std::pair<std::string, std::string>>)
			{
				values.resize(found->size());
				for (size_t i = 0; i < found->size(); ++i)
				{
					std::string value = convert<std::string>(*found, i);
					if (const size_t equals_sign = value.find('=');
						equals_sign != std::string::npos)
						values[i] = { value.substr(0, equals_sign), value.substr(equals_sign + 1) };
					else
						values[i].first = std::move(value);
				}
			}
			else
			{
				values.resize(found->size());
				for (size_t i = 0; i < found->size(); ++i)
					values[i] = convert<T>(*found, i);
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
				v.assign(1, value);
			}
			else if constexpr (std::is_same_v<T, bool>)
			{
				v.assign(1, value ? "1" : "0");
			}
			else if constexpr (std::is_same_v<T, std::filesystem::path>)
			{
				v.assign(1, value.u8string());
			}
			else
			{
				v.assign(1, std::to_string(value));
			}
			assign(section, key, std::move(v));
		}
		template <typename T, size_t SIZE>
		void set(const std::string &section, const std::string &key, const T(&values)[SIZE], const size_t size = SIZE)
		{
			value_type v(size);
			for (size_t i = 0; i < size; ++i)
				v[i] = std::to_string(values[i]);
			assign(section, key, std::move(v));
		}
		template <typename T>
		void set(const std::string &section, const std::string &key, const std::vector<T> &values)
		{
			value_type v;
			if constexpr (std::is_same_v<T, std::string>)
			{
				v = values;
			}
			else if constexpr (std::is_same_v<T, bool>)
			{
				v.resize(values.size());
				for (size_t i = 0; i < values.size(); ++i)
					v[i] = values[i] ? "1" : "0";
			}
			else if constexpr (std::is_same_v<T, std::filesystem::path>)
			{
				v.resize(values.size());
				for (size_t i = 0; i < values.size(); ++i)
					v[i] = values[i].u8string();
			}
			else if constexpr (std::is_same_v<T, std::pair<std::string, std::string>>)
			{
				v.resize(values.size());
				for (size_t i = 0; i < values.size(); ++i)
				{
					const std::pair<std::string, std::string> &value = values[i];
					v[i] = value.first;
					if (!value.second.empty())
						v[i] += '=' + value.second;
				}
			}
			else
			{
				v.resize(values.size());
				for (size_t i = 0; i < values.size(); ++i)
					v[i] = std::to_string(values[i]);
			}
			assign(section, key, std::move(v));
		}

		void set(const std::string &section, const std::string &key, std::string &&value)
		{
			assign(section, key, value_type(1, std::forward<std::string>(value)));
		}
		void set(const std::string &section, const std::string &key, std::vector<std::string> &&values)
		{
			assign(section, key, std::forward<std::vector<std::string>>(values));
		}

		/// <summary>
		/// Removes all values (of the user file, when layered).
		/// </summary>
		void clear()
		{
			_sections.clear();
			mark_modified();
		}

		/// <summary>
		/// Removes the specified <paramref name="key"/> from the <paramref name="section"/> (from the user file, when layered, so the curated value applies again).
		/// </summary>
		void remove_key(const std::string &section, const std::string &key);

		/// <summary>
		/// Loads all values from disk.
		/// </summary>
		bool load();
		/// <summary>
		/// Saves all changes to this INI file to disk.
		/// </summary>
		bool save();

		/// <summary>
		/// Saves all changes to INI files that were loaded through <see cref="load_cache"/> to disk.
		/// </summary>
		static bool flush_cache();
		static bool flush_cache(const std::filesystem::path &path);

		/// <summary>
		/// Removes all INI files from cache, without saving changes.
		/// </summary>
		static void clear_cache();
		static void clear_cache(const std::filesystem::path &path);

		/// <summary>
		/// Gets the specified INI file from cache.
		/// </summary>
		/// <param name="path">Absolute path to the INI file to access.</param>
		/// <returns>Reference to the cached data.</returns>
		static ini_file *find_cache(const std::filesystem::path &path);

		/// <summary>
		/// Gets the specified INI file from cache or opens it when it was not cached yet.
		/// </summary>
		/// <param name="path">Absolute path to the INI file to access.</param>
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

		using sections_type = std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>;

		const std::vector<std::string> *find(const std::string &section, const std::string &key) const;
		void assign(const std::string &section, const std::string &key, std::vector<std::string> &&value);
		void mark_modified()
		{
			_modified = true;
			_modified_at = std::filesystem::file_time_type::clock::now();
		}

		static bool read(const std::filesystem::path &path, int share_flag, sections_type &sections);
		bool load_curated();

		/// <summary>
		/// Describes a single value in an INI file.
		/// </summary>
		using value_type = std::vector<std::string>;
		/// <summary>
		/// Describes a section of multiple key/value pairs in an INI file.
		/// </summary>
		using section_type = std::unordered_map<std::string, value_type>;

		const std::filesystem::path _path;
		// The user file when layered, else the file itself
		std::unordered_map<std::string, section_type> _sections;
		bool _modified = false;
		std::filesystem::file_time_type _modified_at;

		const std::filesystem::path _user_path;
		sections_type _curated;
		std::filesystem::file_time_type _curated_modified_at = std::filesystem::file_time_type::min();
		// What ReShade writes for keys no file has, recorded in a defaults_scope: writing it again is no change
		sections_type _defaults;
		bool _is_user_file_broken = false;
	};

	/// <summary>
	/// Global configuration that can be used for general settings that are not specific to an effect runtime instance.
	/// </summary>
	ini_file &global_config();
}
