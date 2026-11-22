/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace reshadefx
{
	/// <summary>
	/// The assembled binaries of one effect, by their keys (see 'binary_key'), as one file: ultra_effect_compile writes it at build time, the runtime reads it.
	/// </summary>
	class binary_pack
	{
	public:
		void add(const std::string &key, std::string binary);
		bool find(const std::string &key, std::string &binary) const;
		size_t size() const { return _binaries.size(); }

		/// <summary>
		/// False with the reason in 'error' when the file cannot be written.
		/// </summary>
		bool write(const std::filesystem::path &path, std::string &error) const;
		/// <summary>
		/// False with the reason in 'error' when the file cannot be read or is not a pack; a file that is missing sets no error.
		/// </summary>
		bool read(const std::filesystem::path &path, std::string &error);

	private:
		std::map<std::string, std::string> _binaries;
	};
}
