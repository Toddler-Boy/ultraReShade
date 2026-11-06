/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if RESHADE_GUI

#include "ultra_overlay.hpp"
#include <imgui.h>
#include <imgui_internal.h>

namespace
{
	// The rule of the ImGui the official ReShade build uses: a node docked into a dockspace undocks by its title bar
	[[nodiscard]] bool can_undock(ImGuiDockNode *const node)
	{
		if (node->VisibleWindow == nullptr || (node->VisibleWindow->Flags & ImGuiWindowFlags_NoMove) != 0 || (node->MergedFlags & ImGuiDockNodeFlags_NoUndocking) != 0)
			return false;

		const ImGuiDockNode *const root = ImGui::DockNodeGetRootNode(node);
		return (root->OnlyNodeWithWindows != node || root->CentralNode != nullptr) && root->IsDockSpace();
	}

	[[nodiscard]] bool is_on_title_bar(const ImGuiDockNode *const node, const ImVec2 position)
	{
		return !node->IsHiddenTabBar() && ImRect(node->Pos, ImVec2(node->Pos.x + node->Size.x, node->Pos.y + ImGui::GetFrameHeight())).Contains(position);
	}
}

void reshade::ultra::dock_new_addon_window()
{
	if (const ImGuiWindow *const home = ImGui::FindWindowByName("###home"); home != nullptr && home->DockId != 0)
		ImGui::SetNextWindowDockID(home->DockId, ImGuiCond_FirstUseEver);
}

void reshade::ultra::place_ultragame_tab(bool &is_placed)
{
	if (is_placed)
		return;

	// A window docked this frame gets its tab from the next frame on
	const ImGuiWindow *const window = ImGui::GetCurrentWindow();
	if (window->DockNode == nullptr || window->DockNode->TabBar == nullptr)
		return;

	ImGuiTabBar *const tab_bar = window->DockNode->TabBar;
	ImGuiTabItem *const tab = ImGui::TabBarFindTabByID(tab_bar, window->TabId);
	if (tab == nullptr)
		return;

	ImGui::TabBarQueueReorder(tab_bar, tab, -ImGui::TabBarGetTabOrder(tab_bar, tab));
	ImGui::TabBarQueueFocus(tab_bar, tab);
	is_placed = true;
}

void reshade::ultra::undock_dragged_node()
{
	ImGuiContext &g = *ImGui::GetCurrentContext();

	// ImGui turns a title bar drag of a docked node into holding its window, which a dockspace keeps in place
	const ImGuiWindow *const held = g.ActiveIdWindow;
	if (held == nullptr || held->DockNode == nullptr || g.ActiveId != held->MoveId || !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
		return;

	ImGuiDockNode *const node = held->DockNode;
	if (is_on_title_bar(node, g.IO.MouseClickedPos[ImGuiMouseButton_Left]) && can_undock(node))
		ImGui::DockContextQueueUndockNode(&g, node);
}

#endif
