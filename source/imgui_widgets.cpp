/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "imgui_widgets.hpp"
#include "input.hpp" // input::key_name
#include "fonts/forkawesome.h"
#include <cassert>
#include <vector>
#include <algorithm> // std::max, std::min, std::replace

bool reshade::imgui::key_input_box(const char *name, unsigned int key[4], const reshade::input &input)
{
	bool res = false;

	char buf[48]; buf[0] = '\0';
	if (key[0] || key[1] || key[2] || key[3])
		buf[input::key_name(key).copy(buf, sizeof(buf) - 1)] = '\0';

	ImGui::BeginDisabled(ImGui::GetCurrentContext()->NavInputSource == ImGuiInputSource_Gamepad);

	ImGui::InputTextWithHint(name, "Click to set keyboard shortcut", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoUndoRedo | ImGuiInputTextFlags_NoHorizontalScroll);

	if (ImGui::IsItemActive())
	{
		const unsigned int last_key_pressed = input.last_key_pressed();
		if (last_key_pressed != 0)
		{
			if (last_key_pressed == 0x08) // Backspace
			{
				key[0] = 0;
				key[1] = 0;
				key[2] = 0;
				key[3] = 0;
			}
			else if (last_key_pressed < 0x10 || last_key_pressed > 0x12) // Exclude modifier keys
			{
				key[0] = last_key_pressed;
				key[1] = input.is_key_down(0x11); // Ctrl
				key[2] = input.is_key_down(0x10); // Shift
				key[3] = input.is_key_down(0x12); // Alt
			}

			res = true;
		}
	}
	else if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
	{
		ImGui::SetTooltip("Click in the field and press any key to change the shortcut to that key or press backspace to remove the shortcut.");
	}

	ImGui::EndDisabled();

	return res;
}

bool reshade::imgui::search_input_box(char *filter, int filter_size, float width)
{
	bool res = false;

	const bool show_clear_button = filter[0] != '\0';

	if (0.0f == width)
		width = ImGui::GetContentRegionAvail().x;

	ImGui::SetNextItemWidth(width - (show_clear_button ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x : 0.0f));
	if (ImGui::InputTextWithHint("##filter", "Search " ICON_FK_SEARCH, filter, filter_size, ImGuiInputTextFlags_AutoSelectAll))
		res = true;

	if (show_clear_button)
	{
		ImGui::SameLine();

		if (ImGui::Button(ICON_FK_CANCEL, ImVec2(ImGui::GetFrameHeight(), 0)))
			res = true, filter[0] = '\0';
	}

	return res;
}

bool reshade::imgui::radio_list(const char *label, const std::string_view ui_items, int *v)
{
	bool res = false;

	const auto item_width = ImGui::CalcItemWidth();

	// Group all radio buttons together into a list
	ImGui::BeginGroup();

	for (size_t offset = 0, next, i = 0; (next = ui_items.find('\0', offset)) != std::string_view::npos; offset = next + 1, ++i)
		res |= ImGui::RadioButton(ui_items.data() + offset, v, static_cast<int>(i));

	if (ImGui::FindRenderedTextEnd(label) != label)
	{
		ImGui::SameLine(item_width, ImGui::GetStyle().ItemInnerSpacing.x);
		ImGui::TextUnformatted(label);
	}

	ImGui::EndGroup();

	return res;
}

bool reshade::imgui::checkbox_list(const char *label, const std::string_view ui_items, unsigned int *v, int components)
{
	if (ui_items.empty())
		return ImGui::Checkbox(label, reinterpret_cast<bool *>(v));

	bool res = false;

	const auto item_width = ImGui::CalcItemWidth();

	ImGui::BeginGroup();

	for (size_t offset = 0, next, i = 0; (next = ui_items.find('\0', offset)) != std::string_view::npos && i < static_cast<size_t>(components); offset = next + 1, ++i)
		res |= ImGui::Checkbox(ui_items.data() + offset, reinterpret_cast<bool *>(&v[i]));

	if (ImGui::FindRenderedTextEnd(label) != label)
	{
		ImGui::SameLine(item_width, ImGui::GetStyle().ItemInnerSpacing.x);
		ImGui::TextUnformatted(label);
	}

	ImGui::EndGroup();

	return res;
}
static bool popup_button(const char *label, float width = 0.0f, ImGuiWindowFlags flags = 0)
{
	if (ImGui::Button(label, ImVec2(width, 0)))
		ImGui::OpenPopup(label); // Popups can have the same ID as other items without conflict
	return ImGui::BeginPopup(label, flags);
}

bool reshade::imgui::confirm_button(const char *label, float width, const char *message, ...)
{
	bool res = false;

	if (popup_button(label, width))
	{
		va_list args;
		va_start(args, message);
		ImGui::TextV(message, args);
		va_end(args);

		const float button_width = (ImGui::GetContentRegionAvail().x / 2) - ImGui::GetStyle().ItemInnerSpacing.x;

		if (ImGui::Button(ICON_FK_OK " Yes", ImVec2(button_width, 0)))
		{
			ImGui::CloseCurrentPopup();
			res = true;
		}

		ImGui::SameLine();

		if (ImGui::Button(ICON_FK_CANCEL " No", ImVec2(button_width, 0)))
		{
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	return res;
}

bool reshade::imgui::list_with_buttons(const char *label, const std::string_view ui_items, int *v)
{
	bool res = false;

	const auto button_size = ImGui::GetFrameHeight();
	const auto button_spacing = ImGui::GetStyle().ItemInnerSpacing.x;

	const auto item_width = ImGui::CalcItemWidth() - (2 * (button_spacing + button_size));

	std::vector<std::string_view> items;
	for (size_t offset = 0, next; (next = ui_items.find('\0', offset)) != std::string_view::npos; offset = next + 1)
		items.push_back(ui_items.substr(offset, next + 1 - offset));

	ImGui::BeginGroup();

	ImGui::BeginGroup();
	ImGui::PushID(label);

	ImVec2 hover_pos = ImGui::GetCursorScreenPos();
	hover_pos.y += button_size;

	ImGui::SetNextItemWidth(item_width);

	if (ImGui::BeginCombo("##value", *v >= 0 && static_cast<size_t>(*v) < items.size() ? items[*v].data() : nullptr, ImGuiComboFlags_NoArrowButton))
	{
		for (int i = 0; i < static_cast<int>(items.size()); ++i)
		{
			ImGui::PushID(i);

			bool selected = (*v == i);
			if (ImGui::Selectable(items[i].data(), &selected))
			{
				*v = i;
				res = true;
			}

			if (selected)
				ImGui::SetItemDefaultFocus();

			ImGui::PopID();
		}

		ImGui::EndCombo();
	}

	ImGui::BeginDisabled(items.empty());

	ImGui::SameLine(0, button_spacing);
	if (ImGui::Button("<", ImVec2(button_size, 0)))
	{
		res = true;
		*v = (*v == 0) ? static_cast<int>(items.size() - 1) : *v - 1;
	}

	ImGui::SameLine(0, button_spacing);
	if (ImGui::Button(">", ImVec2(button_size, 0)))
	{
		res = true;
		*v = (*v == static_cast<int>(items.size() - 1)) ? 0 : *v + 1;
	}

	ImGui::EndDisabled();

	ImGui::PopID();
	ImGui::EndGroup();
	const bool is_hovered = ImGui::IsItemHovered();

	if (ImGui::FindRenderedTextEnd(label) != label)
	{
		ImGui::SameLine(0, button_spacing);
		ImGui::TextUnformatted(label);
	}

	ImGui::EndGroup();

	if (is_hovered)
	{
		const ImGuiStyle &style = ImGui::GetStyle();

		ImGui::SetNextWindowPos(hover_pos);
		ImGui::SetNextWindowSizeConstraints(ImVec2(item_width, 0.0f), ImVec2(FLT_MAX, (ImGui::GetFontSize() + style.ItemSpacing.y) * 8 - style.ItemSpacing.y + (style.WindowPadding.y * 2))); // 8 by ImGuiComboFlags_HeightRegular
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.FramePadding.x, style.WindowPadding.y));
		ImGui::Begin("##spinner_items", NULL, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking);
		ImGui::PopStyleVar();

		for (int i = 0; i < static_cast<int>(items.size()); ++i)
		{
			const bool selected = (*v == i);
			ImGui::Selectable(items[i].data(), selected);
			if (selected)
				ImGui::SetScrollHereY();
		}

		ImGui::End();
	}

	return res;
}

bool reshade::imgui::combo_with_buttons(const char *label, bool *v)
{
	std::string items = "Off\nOn\n";
	std::replace(items.begin(), items.end(), '\n', '\0');

	int current_item = *v ? 1 : 0;
	const bool res = combo_with_buttons(label, items, &current_item);
	*v = current_item != 0;
	return res;
}
bool reshade::imgui::combo_with_buttons(const char *label, const std::string_view ui_items, int *v)
{
	bool res = false;

	const auto button_size = ImGui::GetFrameHeight();
	const auto button_spacing = ImGui::GetStyle().ItemInnerSpacing.x;

	const auto item_width = ImGui::CalcItemWidth() - (2 * (button_spacing + button_size));

	std::vector<std::string_view> items;
	for (size_t offset = 0, next; (next = ui_items.find('\0', offset)) != std::string_view::npos; offset = next + 1)
		items.push_back(ui_items.substr(offset, next + 1 - offset));

	ImGui::BeginGroup();
	ImGui::PushID(label);

	ImGui::SetNextItemWidth(item_width);

	if (ImGui::BeginCombo("##value", *v >= 0 && static_cast<size_t>(*v) < items.size() ? items[*v].data() : nullptr, ImGuiComboFlags_None))
	{
		for (int i = 0; i < static_cast<int>(items.size()); ++i)
		{
			ImGui::PushID(i);

			bool selected = (*v == i);
			if (ImGui::Selectable(items[i].data(), &selected))
			{
				*v = i;
				res = true;
			}

			if (selected)
				ImGui::SetItemDefaultFocus();

			ImGui::PopID();
		}

		ImGui::EndCombo();
	}

	ImGui::BeginDisabled(items.empty());

	ImGui::SameLine(0, button_spacing);
	if (ImGui::Button("<", ImVec2(button_size, 0)))
	{
		res = true;
		*v = (*v == 0) ? static_cast<int>(items.size() - 1) : *v - 1;
	}

	ImGui::SameLine(0, button_spacing);
	if (ImGui::Button(">", ImVec2(button_size, 0)))
	{
		res = true;
		*v = (*v == static_cast<int>(items.size() - 1)) ? 0 : *v + 1;
	}

	ImGui::EndDisabled();

	ImGui::PopID();

	if (ImGui::FindRenderedTextEnd(label) != label)
	{
		ImGui::SameLine(0, button_spacing);
		ImGui::TextUnformatted(label);
	}

	ImGui::EndGroup();

	return res;
}

template <typename T, ImGuiDataType data_type>
static bool drag_with_buttons(const char *label, T *v, int components, T v_speed, T v_min, T v_max, const char *format)
{
	bool res = false;

	const auto button_size = ImGui::GetFrameHeight();
	const auto button_spacing = ImGui::GetStyle().ItemInnerSpacing.x;

	const auto item_width = ImGui::CalcItemWidth();
	const auto item_width_with_buttons = item_width - (components * 2 * (button_spacing + button_size));

	const bool with_buttons = item_width_with_buttons > 50 * components && !ImGui::GetIO().KeyShift;
	const bool ignore_limits = ImGui::GetIO().KeyCtrl;

	ImGui::BeginGroup();
	ImGui::PushID(label);

	ImGui::PushMultiItemsWidths(components, with_buttons ? item_width_with_buttons : item_width - (2 * (button_spacing + button_size)));

	for (int c = 0; c < components; ++c)
	{
		ImGui::PushID(c);
		if (c > 0)
			ImGui::SameLine(0, button_spacing);
		res |= ImGui::DragScalar("##value", data_type, v + c, static_cast<float>(v_speed), &v_min, &v_max, format);

		ImGui::PopItemWidth();

		if (with_buttons)
		{
			ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
			ImGui::SameLine(0, button_spacing);
			if (ImGui::ButtonEx("<", ImVec2(button_size, 0), ImGuiButtonFlags_PressedOnClick) && (ignore_limits || v[c] > v_min))
			{
				v[c] -= v_speed;
				if (!ignore_limits)
					v[c] = std::max(v[c], v_min);
				res = true;
			}
			ImGui::SameLine(0, button_spacing);
			if (ImGui::ButtonEx(">", ImVec2(button_size, 0), ImGuiButtonFlags_PressedOnClick) && (ignore_limits || v[c] < v_max))
			{
				v[c] += v_speed;
				if (!ignore_limits)
					v[c] = std::min(v[c], v_max);
				res = true;
			}
			ImGui::PopItemFlag();
		}

		ImGui::PopID();
	}

	if (!with_buttons)
	{
		ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
		ImGui::SameLine(0, button_spacing);
		if (ImGui::ButtonEx("<", ImVec2(button_size, 0), ImGuiButtonFlags_PressedOnClick) && (ignore_limits || v[0] > v_min))
		{
			for (int c = 0; c < components; ++c)
			{
				v[c] -= v_speed;
				if (!ignore_limits)
					v[c] = std::max(v[c], v_min);
			}
			res = true;
		}
		ImGui::SameLine(0, button_spacing);
		if (ImGui::ButtonEx(">", ImVec2(button_size, 0), ImGuiButtonFlags_PressedOnClick) && (ignore_limits || v[0] < v_max))
		{
			for (int c = 0; c < components; ++c)
			{
				v[c] += v_speed;
				if (!ignore_limits)
					v[c] = std::min(v[c], v_max);
			}
			res = true;
		}
		ImGui::PopItemFlag();
	}

	ImGui::PopID();

	if (ImGui::FindRenderedTextEnd(label) != label)
	{
		ImGui::SameLine(0, button_spacing);
		ImGui::TextUnformatted(label);
	}

	ImGui::EndGroup();

	return res;

}
bool reshade::imgui::drag_with_buttons(const char *label, ImGuiDataType data_type, void *v, int components, const void *v_speed, const void *v_min, const void *v_max, const char *format)
{
	switch (data_type)
	{
	case ImGuiDataType_S32:
		return ::drag_with_buttons<ImS32, ImGuiDataType_S32>(label, static_cast<ImS32 *>(v), components, *static_cast<const ImS32 *>(v_speed), *static_cast<const ImS32 *>(v_min), *static_cast<const ImS32 *>(v_max), format);
	case ImGuiDataType_U32:
		return ::drag_with_buttons<ImU32, ImGuiDataType_U32>(label, static_cast<ImU32 *>(v), components, *static_cast<const ImU32 *>(v_speed), *static_cast<const ImU32 *>(v_min), *static_cast<const ImU32 *>(v_max), format);
	case ImGuiDataType_Float:
		return ::drag_with_buttons<float, ImGuiDataType_Float>(label, static_cast<float *>(v), components, *static_cast<const float *>(v_speed), *static_cast<const float *>(v_min), *static_cast<const float *>(v_max), format);
#if 0
	case ImGuiDataType_S64:
		return ::drag_with_buttons<ImS64, ImGuiDataType_S64>(label, static_cast<ImS64 *>(v), components, *static_cast<const ImS64 *>(v_speed), *static_cast<const ImS64 *>(v_min), *static_cast<const ImS64 *>(v_max), format);
	case ImGuiDataType_U64:
		return ::drag_with_buttons<ImU64, ImGuiDataType_U64>(label, static_cast<ImU64 *>(v), components, *static_cast<const ImU64 *>(v_speed), *static_cast<const ImU64 *>(v_min), *static_cast<const ImU64 *>(v_max), format);
	case ImGuiDataType_Double:
		return ::drag_with_buttons<double, ImGuiDataType_Double>(label, static_cast<double *>(v), components, *static_cast<const double *>(v_speed), *static_cast<const double *>(v_min), *static_cast<const double *>(v_max), format);
#endif
	default:
		assert(false); // Not implemented
		return false;
	}
}

template <typename T, ImGuiDataType data_type>
static bool slider_with_buttons(const char *label, T *v, int components, T v_speed, T v_min, T v_max, const char *format)
{
	bool res = false;

	const auto button_size = ImGui::GetFrameHeight();
	const auto button_spacing = ImGui::GetStyle().ItemInnerSpacing.x;

	const auto item_width = ImGui::CalcItemWidth();
	const auto item_width_with_buttons = item_width - (components * 2 * (button_spacing + button_size));

	const bool with_buttons = item_width_with_buttons > 50 * components && !ImGui::GetIO().KeyShift;
	const bool ignore_limits = ImGui::GetIO().KeyCtrl;

	ImGui::BeginGroup();
	ImGui::PushID(label);

	ImGui::PushMultiItemsWidths(components, with_buttons ? item_width_with_buttons : item_width - (2 * (button_spacing + button_size)));

	for (int c = 0; c < components; ++c)
	{
		ImGui::PushID(c);
		if (c > 0)
			ImGui::SameLine(0, button_spacing);
		res |= ImGui::SliderScalar("##value", data_type, v + c, &v_min, &v_max, format);

		ImGui::PopItemWidth();

		if (with_buttons)
		{
			ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
			ImGui::SameLine(0, button_spacing);
			if (ImGui::ButtonEx("<", ImVec2(button_size, 0), ImGuiButtonFlags_PressedOnClick) && (ignore_limits || v[c] > v_min))
			{
				v[c] -= v_speed;
				if (!ignore_limits)
					v[c] = std::max(v[c], v_min);
				res = true;
			}
			ImGui::SameLine(0, button_spacing);
			if (ImGui::ButtonEx(">", ImVec2(button_size, 0), ImGuiButtonFlags_PressedOnClick) && (ignore_limits || v[c] < v_max))
			{
				v[c] += v_speed;
				if (!ignore_limits)
					v[c] = std::min(v[c], v_max);
				res = true;
			}
			ImGui::PopItemFlag();
		}

		ImGui::PopID();
	}

	if (!with_buttons)
	{
		ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
		ImGui::SameLine(0, button_spacing);
		if (ImGui::ButtonEx("<", ImVec2(button_size, 0), ImGuiButtonFlags_PressedOnClick) && (ignore_limits || v[0] > v_min))
		{
			for (int c = 0; c < components; ++c)
			{
				v[c] -= v_speed;
				if (!ignore_limits)
					v[c] = std::max(v[c], v_min);
			}
			res = true;
		}
		ImGui::SameLine(0, button_spacing);
		if (ImGui::ButtonEx(">", ImVec2(button_size, 0), ImGuiButtonFlags_PressedOnClick) && (ignore_limits || v[0] < v_max))
		{
			for (int c = 0; c < components; ++c)
			{
				v[c] += v_speed;
				if (!ignore_limits)
					v[c] = std::min(v[c], v_max);
			}
			res = true;
		}
		ImGui::PopItemFlag();
	}

	ImGui::PopID();

	if (ImGui::FindRenderedTextEnd(label) != label)
	{
		ImGui::SameLine(0, button_spacing);
		ImGui::TextUnformatted(label);
	}

	ImGui::EndGroup();

	return res;

}
bool reshade::imgui::slider_with_buttons(const char *label, ImGuiDataType data_type, void *v, int components, const void *v_speed, const void *v_min, const void *v_max, const char *format)
{
	switch (data_type)
	{
	case ImGuiDataType_S32:
		return ::slider_with_buttons<ImS32, ImGuiDataType_S32>(label, static_cast<ImS32 *>(v), components, *static_cast<const ImS32 *>(v_speed), *static_cast<const ImS32 *>(v_min), *static_cast<const ImS32 *>(v_max), format);
	case ImGuiDataType_U32:
		return ::slider_with_buttons<ImU32, ImGuiDataType_U32>(label, static_cast<ImU32 *>(v), components, *static_cast<const ImU32 *>(v_speed), *static_cast<const ImU32 *>(v_min), *static_cast<const ImU32 *>(v_max), format);
	case ImGuiDataType_Float:
		return ::slider_with_buttons<float, ImGuiDataType_Float>(label, static_cast<float *>(v), components, *static_cast<const float *>(v_speed), *static_cast<const float *>(v_min), *static_cast<const float *>(v_max), format);
#if 0
	case ImGuiDataType_S64:
		return ::slider_with_buttons<ImS64, ImGuiDataType_S64>(label, static_cast<ImS64 *>(v), components, *static_cast<const ImS64 *>(v_speed), *static_cast<const ImS64 *>(v_min), *static_cast<const ImS64 *>(v_max), format);
	case ImGuiDataType_U64:
		return ::slider_with_buttons<ImU64, ImGuiDataType_U64>(label, static_cast<ImU64 *>(v), components, *static_cast<const ImU64 *>(v_speed), *static_cast<const ImU64 *>(v_min), *static_cast<const ImU64 *>(v_max), format);
	case ImGuiDataType_Double:
		return ::slider_with_buttons<double, ImGuiDataType_Double>(label, static_cast<double *>(v), components, *static_cast<const double *>(v_speed), *static_cast<const double *>(v_min), *static_cast<const double *>(v_max), format);
#endif
	default:
		assert(false); // Not implemented
		return false;
	}
}

bool reshade::imgui::slider_for_alpha_value(const char *label, float *v)
{
	const auto button_size = ImGui::GetFrameHeight();
	const auto button_spacing = ImGui::GetStyle().ItemInnerSpacing.x;

	ImGui::BeginGroup();
	ImGui::PushID(label);

	ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - (button_spacing + button_size));
	const bool res = ImGui::SliderFloat("##value", v, 0.0f, 1.0f);

	ImGui::SameLine(0, button_spacing);
	ImGui::ColorButton("##preview", ImVec4(1.0f, 1.0f, 1.0f, *v), ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_NoPicker);

	ImGui::PopID();

	if (ImGui::FindRenderedTextEnd(label) != label)
	{
		ImGui::SameLine(0, button_spacing);
		ImGui::TextUnformatted(label);
	}

	ImGui::EndGroup();

	return res;
}
