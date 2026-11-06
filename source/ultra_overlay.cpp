/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if RESHADE_GUI

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

#endif
