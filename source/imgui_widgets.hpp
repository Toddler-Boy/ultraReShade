/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <string>
#include <imgui.h>
#include <imgui_internal.h>

namespace reshade
{
	class input;
}

namespace reshade::imgui
{
	/// <summary>
	/// Adds a keyboard shortcut widget.
	/// </summary>
	/// <param name="label">Label text describing this widget.</param>
	/// <param name="key">Shortcut, consisting of the [virtual key code, Ctrl, Shift, Alt].</param>
	/// <param name="input">Reference to the input state.</param>
	bool key_input_box(const char *label, unsigned int key[4], const reshade::input &input);

	/// <summary>
	/// Adds a search text box widget.
	/// </summary>
	bool search_input_box(char *filter, int filter_size, float width = 0.0f);

	/// <summary>
	/// Adds a widget which shows a vertical list of radio buttons plus a label to the right.
	/// </summary>
	/// <param name="label">Label text describing this widget.</param>
	/// <param name="ui_items">List of labels for the items, separated with '\0' characters.</param>
	/// <param name="v">Index of the active item in the <paramref name="ui_items"/> list.</param>
	bool radio_list(const char *label, const std::string_view ui_items, int *v);

	/// <summary>
	/// Adds a widget which shows a vertical list of check boxes plus a label to the right.
	/// </summary>
	/// <param name="label">Label text describing this widget.</param>
	/// <param name="ui_items">List of labels for the items, separated with '\0' characters.</param>
	/// <param name="v">Item values.</param>
	bool checkbox_list(const char *label, const std::string_view ui_items, unsigned int *v, int components);

	/// <summary>
	/// Adds a button that asks for confirmation when pressed. Only returns <see langword="true"/> once that is acknowledged.
	/// </summary>
	bool confirm_button(const char *label, float width, const char *message, ...);

	/// <summary>
	/// Adds an ImGui drag widget but with additional "&lt;" and "&gt;" buttons to decrease/increase the value.
	/// </summary>
	bool drag_with_buttons(const char *label, ImGuiDataType data_type, void *v, int components, const void *v_speed, const void *v_min, const void *v_max, const char *format = nullptr);
	/// <summary>
	/// Adds an ImGui list widget but with additional "&lt;" and "&gt;" buttons to decrease/increase the value.
	/// </summary>
	bool list_with_buttons(const char *label, const std::string_view ui_items, int *v);
	/// <summary>
	/// Adds a combo widget for a boolean value, with the items "On" and "Off".
	/// </summary>
	bool combo_with_buttons(const char *label, bool *v);
	/// <summary>
	/// Adds an ImGui combo widget but with additional "&lt;" and "&gt;" buttons to decrease/increase the value.
	/// </summary>
	bool combo_with_buttons(const char *label, const std::string_view ui_items, int *v);
	/// <summary>
	/// Adds an ImGui slider widget but with additional "&lt;" and "&gt;" buttons to decrease/increase the value.
	/// </summary>
	bool slider_with_buttons(const char *label, ImGuiDataType data_type, void *v, int components, const void *v_speed, const void *v_min, const void *v_max, const char *format = nullptr);

	/// <summary>
	/// Adds an ImGui slider widget but with an additional color preview and picker for alpha values.
	/// </summary>
	bool slider_for_alpha_value(const char *label, float *v);
}
