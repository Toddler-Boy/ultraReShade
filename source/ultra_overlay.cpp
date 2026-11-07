/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if RESHADE_GUI

#include <algorithm>
#include <cmath>

#include <Windows.h>
#include <dwmapi.h>
#include <ShellScalingApi.h>

#include <imgui.h>
#include <imgui_internal.h>

#include "ultra_overlay.hpp"

//-----------------------------------------------------------------------------

namespace
{
	// The rule of the ImGui the official ReShade build uses: a node docked into a dockspace undocks by its title bar
	[[ nodiscard ]] bool canUndock ( ImGuiDockNode* node )
	{
		if ( node->VisibleWindow == nullptr || ( node->VisibleWindow->Flags & ImGuiWindowFlags_NoMove ) != 0 || ( node->MergedFlags & ImGuiDockNodeFlags_NoUndocking ) != 0 )
			return false;

		const auto* const	root = ImGui::DockNodeGetRootNode ( node );
		return ( root->OnlyNodeWithWindows != node || root->CentralNode != nullptr ) && root->IsDockSpace ();
	}

	[[ nodiscard ]] bool isOnTitleBar ( const ImGuiDockNode* node, ImVec2 position )
	{
		return ! node->IsHiddenTabBar () && ImRect ( node->Pos, ImVec2 ( node->Pos.x + node->Size.x, node->Pos.y + ImGui::GetFrameHeight () ) ).Contains ( position );
	}

	[[ nodiscard ]] bool hasTabBar ( const ImGuiWindow* window )
	{
		return window->DockNode != nullptr && window->DockNode->TabBar != nullptr;
	}

	// ImGui turns a title bar drag of a docked node into holding its window, which a dockspace keeps in place
	[[ nodiscard ]] bool isDraggingDockedWindow ( const ImGuiContext& g )
	{
		const auto* const	held = g.ActiveIdWindow;
		return held != nullptr && held->DockNode != nullptr && g.ActiveId == held->MoveId && ImGui::IsMouseDragging ( ImGuiMouseButton_Left );
	}
}
//-----------------------------------------------------------------------------

void reshade::ultra::dockNewAddonWindow ()
{
	if ( const auto* const home = ImGui::FindWindowByName ( "###home" ); home != nullptr && home->DockId != 0 )
		ImGui::SetNextWindowDockID ( home->DockId, ImGuiCond_FirstUseEver );
}
//-----------------------------------------------------------------------------

void reshade::ultra::placeUltraGameTab ( bool& isPlaced )
{
	if ( isPlaced )
		return;

	const auto* const	window = ImGui::GetCurrentWindow ();

	// A window docked this frame gets its tab from the next frame on
	if ( ! hasTabBar ( window ) )
		return;

	const auto	tabBar = window->DockNode->TabBar;
	const auto	tab = ImGui::TabBarFindTabByID ( tabBar, window->TabId );

	if ( tab == nullptr )
		return;

	ImGui::TabBarQueueReorder ( tabBar, tab, -ImGui::TabBarGetTabOrder ( tabBar, tab ) );
	ImGui::TabBarQueueFocus ( tabBar, tab );
	isPlaced = true;
}
//-----------------------------------------------------------------------------

void reshade::ultra::undockDraggedNode ()
{
	auto&	g = *ImGui::GetCurrentContext ();

	if ( ! isDraggingDockedWindow ( g ) )
		return;

	const auto	node = g.ActiveIdWindow->DockNode;

	if ( isOnTitleBar ( node, g.IO.MouseClickedPos[ ImGuiMouseButton_Left ] ) && canUndock ( node ) )
		ImGui::DockContextQueueUndockNode ( &g, node );
}
//-----------------------------------------------------------------------------

reshade::ultra::DisplayScale reshade::ultra::measureDisplayScale ( void* window, float backBufferHeight )
{
	const auto	hwnd = static_cast<HWND> ( window );

	if ( hwnd == nullptr )
	{
		const auto	scale = backBufferHeight >= 2160.0f ? 2.0f : backBufferHeight >= 1440.0f ? 1.5f : 1.0f;
		return { 1.0f, scale, scale };
	}

	// Measured as a per-monitor aware caller: the monitor's real scale and the window's real pixels, also for a game
	// Windows stretches (a DPI unaware one, or a small back buffer in a large window)
	const auto	previous = SetThreadDpiAwarenessContext ( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
	UINT		dpiX = 96;
	UINT		dpiY = 96;
	RECT		frame {};
	const auto	hasDpi = SUCCEEDED ( GetDpiForMonitor ( MonitorFromWindow ( hwnd, MONITOR_DEFAULTTONEAREST ), MDT_EFFECTIVE_DPI, &dpiX, &dpiY ) );
	const auto	hasFrame = SUCCEEDED ( DwmGetWindowAttribute ( hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof ( frame ) ) ) && frame.bottom > frame.top;
	SetThreadDpiAwarenessContext ( previous );

	DisplayScale	measured;
	measured.monitorScale = hasDpi ? float ( dpiY ) / 96.0f : 1.0f;
	measured.pixelSize = hasFrame && backBufferHeight > 0.0f ? backBufferHeight / float ( frame.bottom - frame.top ) : 1.0f;
	measured.scale = std::clamp ( std::round ( measured.monitorScale * measured.pixelSize * 4.0f ) / 4.0f, 0.5f, 4.0f );
	return measured;
}
//-----------------------------------------------------------------------------

#endif
