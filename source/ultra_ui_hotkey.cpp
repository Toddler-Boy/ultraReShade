/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <array>
#include <string>

#include <Windows.h>

#include "ultra_ui_hotkey.hpp"

//-----------------------------------------------------------------------------

namespace ultra::ui
{
	namespace
	{
		// F1 to F24, letters, digits; anything else by Windows' own name for it
		[[ nodiscard ]] const char* name ( int vk )
		{
			static std::array<std::string, 256>	names;

			if ( vk <= 0 || vk > 255 )
				return "";

			auto&	n = names[ size_t ( vk ) ];
			if ( ! n.empty () )
				return n.c_str ();

			if ( vk >= VK_F1 && vk <= VK_F24 )
				n = "F" + std::to_string ( vk - VK_F1 + 1 );
			else if ( ( vk >= '0' && vk <= '9' ) || ( vk >= 'A' && vk <= 'Z' ) )
				n = std::string ( 1, char ( vk ) );
			// Windows names these by the numpad keys that share their scan codes
			else if ( vk == VK_PRIOR )
				n = "Page Up";
			else if ( vk == VK_NEXT )
				n = "Page Down";
			else if ( vk == VK_PAUSE )
				n = "Pause";
			else
			{
				char	text[ 32 ] {};
				if ( GetKeyNameTextA ( LONG ( MapVirtualKeyA ( UINT ( vk ), MAPVK_VK_TO_VSC ) ) << 16, text, sizeof ( text ) ) > 0 )
					n = text;
				else
					n = "Key " + std::to_string ( vk );
			}

			return n.c_str ();
		}
	}

	int keyNames ( const Hotkey& hotkey, std::span<const char*, 4> out )
	{
		auto	n = 0;
		if ( hotkey.ctrl )
			out[ size_t ( n++ ) ] = "Ctrl";
		if ( hotkey.alt )
			out[ size_t ( n++ ) ] = "Alt";
		if ( hotkey.shift )
			out[ size_t ( n++ ) ] = "Shift";
		if ( hotkey )
			out[ size_t ( n++ ) ] = name ( hotkey.vk );

		return n;
	}
}
