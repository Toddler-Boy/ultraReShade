/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <cctype>
#include <string_view>

#include <Windows.h>

#include <reshade_ultra.hpp>

#include "ultra_build_stamp.h"
#include "ultra_version.hpp"

//-----------------------------------------------------------------------------

namespace
{
	constexpr char	embedded[] = {
#embed "../VERSION"
		, 0 };

	[[ nodiscard ]] reshade::ultra::Version make ()
	{
		std::string_view	number ( embedded );
		while ( ! number.empty () && std::isspace ( static_cast<unsigned char> ( number.back () ) ) )
			number.remove_suffix ( 1 );
		return { std::string ( number ), __DATE__ " " __TIME__ };
	}
}

const reshade::ultra::Version& reshade::ultra::version ()
{
	static const auto	build = make ();
	return build;
}

std::string reshade::ultra::versionText ()
{
	const auto&	build = version ();
	return build.number + " - " + build.built;
}
//-----------------------------------------------------------------------------

#if defined(RESHADE_API_LIBRARY_EXPORT)

// The strings stay as long as the DLL
void ReShadeGetBuild ( const char** number, const char** built )
{
	const auto&	build = reshade::ultra::version ();
	if ( number != nullptr )
		*number = build.number.c_str ();
	if ( built != nullptr )
		*built = build.built.c_str ();
}

#endif
