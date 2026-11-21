/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ultra_json.hpp"

namespace reshade { class ini_file; }

//-----------------------------------------------------------------------------

// The effects of the home's two files: the route's (reshade.json) run first in their order, hidden and never saved; then
// the user's (reshade-user.json), saved on every change. Read on the render thread, a snapshot the loading workers share
namespace reshade::ultra
{
	struct EffectEntry
	{
		std::string		technique;
		std::string		file;
		bool			enabled = true;	// the route's: its state at start
	};

	using Definitions = std::vector<std::pair<std::string, std::string>>;

	struct EffectFileSettings
	{
		std::unordered_map<std::string, std::vector<double>>	values;		// a uniform's components
		Definitions												definitions;
	};

	struct EffectLayer
	{
		std::vector<EffectEntry>									effects;
		std::unordered_map<std::string, EffectFileSettings>			files;
	};

	struct EffectSettings
	{
		EffectLayer	route;
		EffectLayer	user;
		Definitions	definitions;	// reshade.json's, for every effect

		[[ nodiscard ]] bool isRouteFile ( const std::string& file ) const;

		// The route's for a route file, else the user's; null when the file has none
		[[ nodiscard ]] const EffectFileSettings* fileSettings ( const std::string& file ) const;
	};

	// Each entry that does not fit the format is left out with one log line
	[[ nodiscard ]] EffectSettings readEffectSettings ( const ini_file& home );

	// reshade-user.json's members
	[[ nodiscard ]] JsonTree effectsJson ( const std::vector<EffectEntry>& effects );
	[[ nodiscard ]] JsonTree effectFileJson ( const EffectFileSettings& settings );

	// A float uniform's value as the shortest double that reads back as the same float (0.15f as 0.15)
	[[ nodiscard ]] double shortestDouble ( float value );
}
