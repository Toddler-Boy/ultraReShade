/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if RESHADE_GUI

#include "runtime.hpp"
#include "runtime_internal.hpp"
#include "dll_log.hpp"
#include "dll_resources.hpp"
#include "ini_file.hpp"
#include "addon_manager.hpp"
#include "input.hpp"
#include "imgui_widgets.hpp"
#include "platform_utils.hpp"
#include "ultra_home.hpp"
#include "ultra_overlay.hpp"
#include "ultra_ui_font.hpp"
#include "fonts/forkawesome.inl"
#include <cmath> // std::abs
#include <cstdlib> // std::strtol
#include <cstring> // std::memcmp, std::memcpy
#include <algorithm> // std::find_if, std::max, std::min, std::rotate, std::search, std::swap
#include <imgui_internal.h>

static bool string_contains(const std::string_view text, const std::string_view filter)
{
	return filter.empty() ||
		std::search(text.cbegin(), text.cend(), filter.cbegin(), filter.cend(),
			[](const char c1, const char c2) { // Search case-insensitive
				return (('a' <= c1 && c1 <= 'z') ? static_cast<char>(c1 - ' ') : c1) == (('a' <= c2 && c2 <= 'z') ? static_cast<char>(c2 - ' ') : c2);
			}) != text.cend();
}
template <typename F>
static void parse_errors(const std::string_view errors, F &&callback)
{
	for (size_t offset = 0, next; offset != std::string_view::npos; offset = next)
	{
		const size_t pos_error = errors.find(": ", offset);
		const size_t pos_error_line = errors.rfind('(', pos_error); // Paths can contain '(', but no ": ", so search backwards from the error location to find the line info
		if (pos_error == std::string_view::npos)
			break;

		const size_t pos_linefeed = errors.find('\n', pos_error);

		if (pos_error_line != std::string_view::npos && pos_error_line >= offset)
		{
			const std::string_view error_file = errors.substr(offset, pos_error_line - offset);
			const int error_line = static_cast<int>(std::strtol(errors.data() + pos_error_line + 1, nullptr, 10));
			const std::string_view error_text = errors.substr(pos_error + 2 /* skip space */, pos_linefeed - pos_error - 2);

			callback(error_file, error_line, error_text);
		}
		else
		{
			callback(std::string_view(), 0, errors.substr(offset, pos_linefeed - offset));
		}

		next = pos_linefeed != std::string_view::npos ? pos_linefeed + 1 : std::string_view::npos;
	}
}

static const ImVec4 COLOR_RED = ImColor(240, 100, 100);
static const ImVec4 COLOR_YELLOW = ImColor(204, 204, 0);

// The panel scales to the display (the font scale), the base size is the same everywhere
static constexpr float overlay_font_size = 13.0f;
// HDR reference white as per BT.2408
static constexpr float hdr_overlay_brightness = 203.0f;

void reshade::runtime::init_gui()
{
	// Default shortcut: Home
	_overlay_key_data[0] = 0x24;
	_overlay_key_data[1] = false;
	_overlay_key_data[2] = false;
	_overlay_key_data[3] = false;

	ImGuiContext *const backup_context = ImGui::GetCurrentContext();
	_imgui_context = ImGui::CreateContext();

	ImGuiIO &imgui_io = _imgui_context->IO;
	imgui_io.IniFilename = nullptr;
	imgui_io.ConfigFlags = ImGuiConfigFlags_NavEnableKeyboard;
	imgui_io.BackendFlags = ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

	ImGuiStyle &imgui_style = _imgui_context->Style;
	// Disable rounding by default
	imgui_style.GrabRounding = 0.0f;
	imgui_style.FrameRounding = 0.0f;
	imgui_style.ChildRounding = 0.0f;
	imgui_style.ScrollbarRounding = 0.0f;
	imgui_style.WindowRounding = 0.0f;
	imgui_style.WindowBorderSize = 0.0f;
	apply_overlay_style();

	// Restore previous context in case this was called from a new runtime being created from an add-on event triggered by an existing runtime
	ImGui::SetCurrentContext(backup_context);
}
void reshade::runtime::deinit_gui()
{
	ImGui::DestroyContext(_imgui_context);
}

void reshade::runtime::build_font_atlas()
{
	_imgui_context->Style.FontSizeBase = overlay_font_size;

	if (!_rebuild_font_atlas)
		return;

	ImGuiContext *const backup_context = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(_imgui_context);

	// Remove any existing fonts from atlas first
	ImFontAtlas *const atlas = _imgui_context->IO.Fonts;
	atlas->Clear();

	// Our own face is ImGui's font: the text fields edit in it, invisibly, under the text we draw
	const auto face = ::ultra::ui::font::faceData(::ultra::ui::font::Face::regular);
	ImFontConfig face_cfg;
	face_cfg.FontDataOwnedByAtlas = false;
	atlas->AddFontFromMemoryTTF(const_cast<unsigned char *>(face.data()), static_cast<int>(face.size()), 0.0f, &face_cfg);

	// Merge icons into the font
	ImFontConfig cfg;
	cfg.MergeMode = true;
	cfg.PixelSnapH = true;

	atlas->AddFontFromMemoryCompressedBase85TTF(FONT_ICON_BUFFER_NAME_FK, 0.0f, &cfg);

	ImGui::SetCurrentContext(backup_context);

	_rebuild_font_atlas = false;
}

void reshade::runtime::load_config_gui(const ini_file &config)
{
	if (_input_gamepad != nullptr)
		_imgui_context->IO.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	else
		_imgui_context->IO.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;

	config.get("INPUT", "KeyOverlay", _overlay_key_data);
	config.get("OVERLAY", "NoFontScaling", _no_font_scaling);
	config.get("OVERLAY", "VariableListHeight", _variable_editor_height);
	config.get("OVERLAY", "Position", _overlay_position);
	config.get("OVERLAY", "Size", _overlay_size);
	config.get("OVERLAY", "Tab", _overlay_tab);

	// Placed again from the fractions at the next frame
	_overlay_placed_for[0] = _overlay_placed_for[1] = 0.0f;
	_is_overlay_tab_selected = false;
}
void reshade::runtime::save_config_gui(ini_file &config) const
{
	config.set("OVERLAY", "VariableListHeight", _variable_editor_height);
	save_overlay_window(config);
}
void reshade::runtime::save_overlay_window(ini_file &config) const
{
	const auto fraction = [](float value) { return std::round(std::clamp(value, 0.0f, 1.0f) * 1e6f) / 1e6f; };

	config.set("OVERLAY", "Position", std::vector<float> { fraction(_overlay_position[0]), fraction(_overlay_position[1]) });
	config.set("OVERLAY", "Size", std::vector<float> { fraction(_overlay_size[0]), fraction(_overlay_size[1]) });
	config.set("OVERLAY", "Tab", _overlay_tab);
}

void reshade::runtime::apply_overlay_style()
{
	ImVec4 *const colors = _imgui_context->Style.Colors;
	colors[ImGuiCol_Text] = ImVec4(0.862745f, 0.862745f, 0.862745f, 1.00f);
	colors[ImGuiCol_TextDisabled] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.58f);
	colors[ImGuiCol_WindowBg] = ImVec4(0.117647f, 0.117647f, 0.117647f, 1.00f);
	colors[ImGuiCol_ChildBg] = ImVec4(0.156863f, 0.156863f, 0.156863f, 0.00f);
	colors[ImGuiCol_Border] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.30f);
	colors[ImGuiCol_FrameBg] = ImVec4(0.156863f, 0.156863f, 0.156863f, 1.00f);
	colors[ImGuiCol_FrameBgHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.470588f);
	colors[ImGuiCol_FrameBgActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.588235f);
	colors[ImGuiCol_TitleBg] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.45f);
	colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.35f);
	colors[ImGuiCol_TitleBgActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.58f);
	colors[ImGuiCol_MenuBarBg] = ImVec4(0.156863f, 0.156863f, 0.156863f, 0.57f);
	colors[ImGuiCol_ScrollbarBg] = ImVec4(0.156863f, 0.156863f, 0.156863f, 1.00f);
	colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.31f);
	colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.78f);
	colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
	colors[ImGuiCol_PopupBg] = ImVec4(0.117647f, 0.117647f, 0.117647f, 0.92f);
	colors[ImGuiCol_CheckMark] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.80f);
	colors[ImGuiCol_SliderGrab] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.784314f);
	colors[ImGuiCol_SliderGrabActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
	colors[ImGuiCol_Button] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.44f);
	colors[ImGuiCol_ButtonHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.86f);
	colors[ImGuiCol_ButtonActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
	colors[ImGuiCol_Header] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.76f);
	colors[ImGuiCol_HeaderHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.86f);
	colors[ImGuiCol_HeaderActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
	colors[ImGuiCol_Separator] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.32f);
	colors[ImGuiCol_SeparatorHovered] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.78f);
	colors[ImGuiCol_SeparatorActive] = ImVec4(0.862745f, 0.862745f, 0.862745f, 1.00f);
	colors[ImGuiCol_ResizeGrip] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.20f);
	colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.78f);
	colors[ImGuiCol_ResizeGripActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
	colors[ImGuiCol_Tab] = colors[ImGuiCol_Button];
	colors[ImGuiCol_TabSelected] = colors[ImGuiCol_ButtonActive];
	colors[ImGuiCol_TabSelectedOverline] = colors[ImGuiCol_ButtonActive];
	colors[ImGuiCol_TabHovered] = colors[ImGuiCol_ButtonHovered];
	colors[ImGuiCol_TabDimmed] = ImLerp(colors[ImGuiCol_Tab], colors[ImGuiCol_TitleBg], 0.80f);
	colors[ImGuiCol_TabDimmedSelected] = ImLerp(colors[ImGuiCol_TabSelected], colors[ImGuiCol_TitleBg], 0.40f);
	colors[ImGuiCol_TabDimmedSelectedOverline] = colors[ImGuiCol_TabDimmedSelected];
	colors[ImGuiCol_DockingPreview] = colors[ImGuiCol_Header] * ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
	colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
	colors[ImGuiCol_PlotLines] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.63f);
	colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
	colors[ImGuiCol_PlotHistogram] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.63f);
	colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
	colors[ImGuiCol_TextSelectedBg] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.43f);
}

void reshade::runtime::draw_gui()
{
	assert(_is_initialized);

	bool show_overlay = _show_overlay;
	api::input_source show_overlay_source = _imgui_context->NavInputSource == ImGuiInputSource_Mouse ? api::input_source::mouse : api::input_source::keyboard;

	if (_input != nullptr)
	{
		if (_show_overlay && !_ignore_shortcuts && _input->is_key_pressed(input::key_escape) && !_imgui_context->IO.NavVisible)
			show_overlay = false; // Close when pressing the escape button and not currently navigating with the keyboard
		else if (!_ignore_shortcuts && _input->is_key_pressed(_overlay_key_data, _force_shortcut_modifiers) && _imgui_context->ActiveId == 0)
			show_overlay = !_show_overlay;
	}

	if (_input_gamepad != nullptr)
	{
		if (_input_gamepad->is_button_down(input_gamepad::button_left_shoulder) &&
			_input_gamepad->is_button_down(input_gamepad::button_right_shoulder) &&
			_input_gamepad->is_button_pressed(input_gamepad::button_start))
		{
			show_overlay = !_show_overlay;
			show_overlay_source = api::input_source::gamepad;
		}
	}

	if (show_overlay != _show_overlay)
		open_overlay(show_overlay, show_overlay_source);

	const bool show_screenshot_message = _screenshots_saving > 0 || (!_last_screenshot_save_successful && (_last_present_time - _last_screenshot_time) < std::chrono::seconds(5));
	const bool show_message_window = show_screenshot_message || !_settings_save_successful;

	_ignore_shortcuts = false;
	_effects_expanded_state &= 2;

	if (!show_message_window && !_show_overlay
		&& !has_addon_event<addon_event::reshade_overlay>()
		&& !(_overlay_wanted && has_addon_event<addon_event::ultra_overlay>())
		&& !ultra::wantsSplashFrame(_ultra_splash, is_loading())
		)
	{
		if (_primary_input_handler && _input != nullptr)
		{
			_input->block_mouse_input(_block_input_next_frame);
			_input->block_keyboard_input(_block_input_next_frame);
			_input->block_mouse_cursor_warping(_block_input_next_frame);
		}
		return; // Early-out to avoid costly ImGui calls when no GUI elements are on the screen
	}

	build_font_atlas();

	ImGuiContext *const backup_context = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(_imgui_context);

	ImGuiIO &imgui_io = _imgui_context->IO;
	imgui_io.DeltaTime = _last_frame_duration.count() * 1e-9f;
	imgui_io.DisplaySize.x = static_cast<float>(_width);
	imgui_io.DisplaySize.y = static_cast<float>(_height);

	// The font scale follows the display: measured again after a resize and twice a second (window resized, moved to another monitor)
	if (imgui_io.DisplaySize.y != _display_scale_height || _last_present_time - _display_scale_time >= std::chrono::milliseconds(500))
	{
		const ultra::DisplayScale measured = ultra::measureDisplayScale(get_hwnd(), imgui_io.DisplaySize.y);

		if (measured.scale != _display_scale)
			log::message(log::level::info, "Overlay scale %.2f (monitor scale %.2f, back buffer %ux%u, pixel size %.3f).", measured.scale, measured.monitorScale, _width, _height, measured.pixelSize);

		_display_scale = measured.scale;
		_display_scale_height = imgui_io.DisplaySize.y;
		_display_scale_time = _last_present_time;
	}

	_imgui_context->Style.FontScaleMain = _display_scale * _font_scale_adjust;

	if (_input != nullptr)
	{
		imgui_io.MouseDrawCursor = _show_overlay;

		// Scale mouse position in case render resolution does not match the window size
		unsigned int max_position[2];
		_input->max_mouse_position(max_position);
		imgui_io.AddMousePosEvent(
			_input->mouse_position_x() * (imgui_io.DisplaySize.x / max_position[0]),
			_input->mouse_position_y() * (imgui_io.DisplaySize.y / max_position[1]));

		// Add wheel delta to the current absolute mouse wheel position
		imgui_io.AddMouseWheelEvent(0.0f, _input->mouse_wheel_delta());

		// Update all the button states
		constexpr std::pair<ImGuiKey, unsigned int> key_mappings[] = {
			{ ImGuiKey_Tab, input::key_tab },
			{ ImGuiKey_LeftArrow, input::key_left },
			{ ImGuiKey_RightArrow, input::key_right },
			{ ImGuiKey_UpArrow, input::key_up },
			{ ImGuiKey_DownArrow, input::key_down },
			{ ImGuiKey_PageUp, input::key_page_up },
			{ ImGuiKey_PageDown, input::key_page_down },
			{ ImGuiKey_End, input::key_end },
			{ ImGuiKey_Home, input::key_home },
			{ ImGuiKey_Insert, input::key_insert },
			{ ImGuiKey_Delete, input::key_delete },
			{ ImGuiKey_Backspace, input::key_backspace },
			{ ImGuiKey_Space, input::key_space },
			{ ImGuiKey_Enter, input::key_return },
			{ ImGuiKey_Escape, input::key_escape },
			{ ImGuiKey_LeftCtrl, input::key_left_ctrl },
			{ ImGuiKey_LeftShift, input::key_left_shift },
			{ ImGuiKey_LeftAlt, input::key_left_alt },
			{ ImGuiKey_LeftSuper, input::key_left_windows },
			{ ImGuiKey_RightCtrl, input::key_right_ctrl },
			{ ImGuiKey_RightShift, input::key_right_shift },
			{ ImGuiKey_RightAlt, input::key_right_alt },
			{ ImGuiKey_RightSuper, input::key_right_windows },
			{ ImGuiKey_Menu, input::key_application },
			{ ImGuiKey_0, '0' },
			{ ImGuiKey_1, '1' },
			{ ImGuiKey_2, '2' },
			{ ImGuiKey_3, '3' },
			{ ImGuiKey_4, '4' },
			{ ImGuiKey_5, '5' },
			{ ImGuiKey_6, '6' },
			{ ImGuiKey_7, '7' },
			{ ImGuiKey_8, '8' },
			{ ImGuiKey_9, '9' },
			{ ImGuiKey_A, 'A' },
			{ ImGuiKey_B, 'B' },
			{ ImGuiKey_C, 'C' },
			{ ImGuiKey_D, 'D' },
			{ ImGuiKey_E, 'E' },
			{ ImGuiKey_F, 'F' },
			{ ImGuiKey_G, 'G' },
			{ ImGuiKey_H, 'H' },
			{ ImGuiKey_I, 'I' },
			{ ImGuiKey_J, 'J' },
			{ ImGuiKey_K, 'K' },
			{ ImGuiKey_L, 'L' },
			{ ImGuiKey_M, 'M' },
			{ ImGuiKey_N, 'N' },
			{ ImGuiKey_O, 'O' },
			{ ImGuiKey_P, 'P' },
			{ ImGuiKey_Q, 'Q' },
			{ ImGuiKey_R, 'R' },
			{ ImGuiKey_S, 'S' },
			{ ImGuiKey_T, 'T' },
			{ ImGuiKey_U, 'U' },
			{ ImGuiKey_V, 'V' },
			{ ImGuiKey_W, 'W' },
			{ ImGuiKey_X, 'X' },
			{ ImGuiKey_Y, 'Y' },
			{ ImGuiKey_Z, 'Z' },
			{ ImGuiKey_F1, input::key_f1 },
			{ ImGuiKey_F2, input::key_f2 },
			{ ImGuiKey_F3, input::key_f3 },
			{ ImGuiKey_F4, input::key_f4 },
			{ ImGuiKey_F5, input::key_f5 },
			{ ImGuiKey_F6, input::key_f6 },
			{ ImGuiKey_F7, input::key_f7 },
			{ ImGuiKey_F8, input::key_f8 },
			{ ImGuiKey_F9, input::key_f9 },
			{ ImGuiKey_F10, input::key_f10 },
			{ ImGuiKey_F11, input::key_f11 },
			{ ImGuiKey_F12, input::key_f12 },
			{ ImGuiKey_Apostrophe, input::key_apostrophe },
			{ ImGuiKey_Comma, input::key_comma },
			{ ImGuiKey_Minus, input::key_minus },
			{ ImGuiKey_Period, input::key_period },
			{ ImGuiKey_Slash, input::key_slash },
			{ ImGuiKey_Semicolon, input::key_semicolon },
			{ ImGuiKey_Equal, input::key_plus },
			{ ImGuiKey_LeftBracket, input::key_left_bracket },
			{ ImGuiKey_Backslash, input::key_backslash },
			{ ImGuiKey_RightBracket, input::key_right_bracket },
			{ ImGuiKey_GraveAccent, input::key_grave_accent },
			{ ImGuiKey_CapsLock, input::key_caps_lock },
			{ ImGuiKey_ScrollLock, input::key_scroll_lock },
			{ ImGuiKey_NumLock, input::key_num_lock },
			{ ImGuiKey_PrintScreen, input::key_print_screen },
			{ ImGuiKey_Pause, input::key_pause },
			{ ImGuiKey_Keypad0, input::key_numpad_0 },
			{ ImGuiKey_Keypad1, input::key_numpad_1 },
			{ ImGuiKey_Keypad2, input::key_numpad_2 },
			{ ImGuiKey_Keypad3, input::key_numpad_3 },
			{ ImGuiKey_Keypad4, input::key_numpad_4 },
			{ ImGuiKey_Keypad5, input::key_numpad_5 },
			{ ImGuiKey_Keypad6, input::key_numpad_6 },
			{ ImGuiKey_Keypad7, input::key_numpad_7 },
			{ ImGuiKey_Keypad8, input::key_numpad_8 },
			{ ImGuiKey_Keypad9, input::key_numpad_9 },
			{ ImGuiKey_KeypadDecimal, input::key_numpad_decimal },
			{ ImGuiKey_KeypadDivide, input::key_numpad_divide },
			{ ImGuiKey_KeypadMultiply, input::key_numpad_multiply },
			{ ImGuiKey_KeypadSubtract, input::key_numpad_subtract },
			{ ImGuiKey_KeypadAdd, input::key_numpad_add },
			{ ImGuiMod_Ctrl, input::key_ctrl },
			{ ImGuiMod_Shift, input::key_shift },
			{ ImGuiMod_Alt, input::key_alt },
			{ ImGuiMod_Super, input::key_application },
		};

		for (const std::pair<ImGuiKey, unsigned int> &mapping : key_mappings)
			imgui_io.AddKeyEvent(mapping.first, _input->is_key_down(mapping.second));
		for (ImGuiMouseButton i = 0; i < ImGuiMouseButton_COUNT; i++)
			imgui_io.AddMouseButtonEvent(i, _input->is_mouse_button_down(i));
		for (ImWchar16 c : _input->text_input())
			imgui_io.AddInputCharacterUTF16(c);
	}

	if (_input_gamepad != nullptr)
	{
		if (_input_gamepad->is_connected())
		{
			imgui_io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

			constexpr std::pair<ImGuiKey, input_gamepad::button> button_mappings[] = {
				{ ImGuiKey_GamepadStart, input_gamepad::button_start },
				{ ImGuiKey_GamepadBack, input_gamepad::button_back },
				{ ImGuiKey_GamepadFaceLeft, input_gamepad::button_x },
				{ ImGuiKey_GamepadFaceRight, input_gamepad::button_b },
				{ ImGuiKey_GamepadFaceUp, input_gamepad::button_y },
				{ ImGuiKey_GamepadFaceDown, input_gamepad::button_a },
				{ ImGuiKey_GamepadDpadLeft, input_gamepad::button_dpad_left },
				{ ImGuiKey_GamepadDpadRight, input_gamepad::button_dpad_right },
				{ ImGuiKey_GamepadDpadUp, input_gamepad::button_dpad_up },
				{ ImGuiKey_GamepadDpadDown, input_gamepad::button_dpad_down },
				{ ImGuiKey_GamepadL1, input_gamepad::button_left_shoulder },
				{ ImGuiKey_GamepadR1, input_gamepad::button_right_shoulder },
				{ ImGuiKey_GamepadL3, input_gamepad::button_left_thumb },
				{ ImGuiKey_GamepadR3, input_gamepad::button_right_thumb },
			};

			for (const std::pair<ImGuiKey, input_gamepad::button> &mapping : button_mappings)
				imgui_io.AddKeyEvent(mapping.first, _input_gamepad->is_button_down(mapping.second));

			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, _input_gamepad->left_trigger_position() != 0.0f, _input_gamepad->left_trigger_position());
			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, _input_gamepad->right_trigger_position() != 0.0f, _input_gamepad->right_trigger_position());
			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, _input_gamepad->left_thumb_axis_x() < 0.0f, -std::min(_input_gamepad->left_thumb_axis_x(), 0.0f));
			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, _input_gamepad->left_thumb_axis_x() > 0.0f, std::max(_input_gamepad->left_thumb_axis_x(), 0.0f));
			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, _input_gamepad->left_thumb_axis_y() > 0.0f, std::max(_input_gamepad->left_thumb_axis_y(), 0.0f));
			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, _input_gamepad->left_thumb_axis_y() < 0.0f, -std::min(_input_gamepad->left_thumb_axis_y(), 0.0f));
		}
		else
		{
			imgui_io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
		}
	}

	ImGui::NewFrame();

	// Reset input source to mouse when the cursor is moved
	if (_input != nullptr && (_input->mouse_movement_delta_x() != 0 || _input->mouse_movement_delta_y() != 0))
		_imgui_context->NavInputSource = ImGuiInputSource_Mouse;

	ultra::drawSplash(_ultra_splash, { _device, ImGui::GetBackgroundDrawList(), imgui_io.DisplaySize.x, imgui_io.DisplaySize.y, imgui_io.DeltaTime, _effects.size(), _reload_remaining_effects.load(), is_loading(), _show_overlay });

	// Create ImGui widgets and windows
	if (show_message_window)
	{
		ImGui::SetNextWindowPos(_imgui_context->Style.WindowPadding);
		ImGui::SetNextWindowSize(ImVec2(imgui_io.DisplaySize.x - 20.0f, 0.0f));
		ImGui::Begin("Message Window", nullptr,
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoInputs |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_NoFocusOnAppearing);

		if (!_settings_save_successful)
		{
			ImGui::TextColored(COLOR_RED, "Unable to save the settings to %s, the log says why.", ultra::userSettingsPath().u8string().c_str());
		}
		else if (show_screenshot_message)
		{
			if (_screenshots_saving > 0)
				ImGui::Text("Saving screenshot %s...", _last_screenshot_file.filename().u8string().c_str());
			else if (_screenshot_directory_creation_successful)
				ImGui::TextColored(COLOR_RED, "Unable to save screenshot because of an internal error (the drive may be full).");
			else
				ImGui::TextColored(COLOR_RED, "Unable to save screenshot because path could not be created: %s", _screenshot_path.u8string().c_str());
		}

		ImGui::End();
	}

	if (_show_overlay)
	{
		const ImGuiViewport *const viewport = ImGui::GetMainViewport();

		// Change font size if user presses the control key and moves the mouse wheel
		if (!_no_font_scaling && imgui_io.KeyCtrl && imgui_io.MouseWheel != 0 && ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
		{
			// Only for this session: the scale itself always comes from the display
			_font_scale_adjust = ImClamp(_font_scale_adjust + imgui_io.MouseWheel * 0.25f, 0.25f, 4.0f);
			_imgui_context->Style.FontScaleMain = _display_scale * _font_scale_adjust;

			_is_font_scaling = true;
		}

		if (_is_font_scaling)
		{
			if (!imgui_io.KeyCtrl)
				_is_font_scaling = false;

			ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, _imgui_context->Style.WindowPadding * 2.0f);
			ImGui::Begin("FontScaling", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
			ImGui::Text("Scaling font size (%d) with 'Ctrl' + mouse wheel", static_cast<int>(_imgui_context->Style.FontSizeBase * _imgui_context->Style.FontScaleMain));
			ImGui::End();
			ImGui::PopStyleVar();
		}

		// Ensure there is always a window that has navigation focus when keyboard or gamepad navigation is used (choose the overlay window created next)
		if (_imgui_context->NavInputSource > ImGuiInputSource_Mouse && _imgui_context->NavWindowingTarget == nullptr && !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
			ImGui::SetNextWindowFocus();

		draw_overlay_window();
	}

	invoke_addon_event<addon_event::reshade_overlay>(this);
	invoke_addon_event<addon_event::ultra_overlay>(this);

	// Disable keyboard shortcuts while typing into input boxes
	_ignore_shortcuts |= ImGui::IsAnyItemActive();

	// Render ImGui widgets and windows
	ImGui::Render();

	if (_primary_input_handler && _input != nullptr)
	{
		// The game gets no input while the overlay is open
		const bool block_input = _show_overlay || _block_input_next_frame;

		_input->block_mouse_input(block_input);
		_input->block_keyboard_input(block_input);
		_input->block_mouse_cursor_warping(block_input);
	}

	if (ImDrawData *const draw_data = ImGui::GetDrawData();
		draw_data != nullptr && draw_data->CmdLists.Size != 0 && draw_data->TotalVtxCount != 0)
	{
		api::command_list *const cmd_list = _graphics_queue->get_immediate_command_list();

		if (_back_buffer_resolved != 0)
		{
			render_imgui_draw_data(cmd_list, draw_data, _back_buffer_targets[0]);
		}
		else
		{
			uint32_t back_buffer_index = get_current_back_buffer_index() * 2;
			const api::resource back_buffer_resource = _device->get_resource_from_view(_back_buffer_targets[back_buffer_index]);

			cmd_list->barrier(back_buffer_resource, api::resource_usage::present, api::resource_usage::render_target);
			render_imgui_draw_data(cmd_list, draw_data, _back_buffer_targets[back_buffer_index]);
			cmd_list->barrier(back_buffer_resource, api::resource_usage::render_target, api::resource_usage::present);
		}
	}

	ImGui::SetCurrentContext(backup_context);
}

void reshade::runtime::draw_overlay_window()
{
	const ImGuiIO &imgui_io = _imgui_context->IO;

	if (_overlay_placed_for[0] != imgui_io.DisplaySize.x || _overlay_placed_for[1] != imgui_io.DisplaySize.y)
	{
		ImGui::SetNextWindowPos(ImVec2(_overlay_position[0] * imgui_io.DisplaySize.x, _overlay_position[1] * imgui_io.DisplaySize.y));
		ImGui::SetNextWindowSize(ImVec2(_overlay_size[0] * imgui_io.DisplaySize.x, _overlay_size[1] * imgui_io.DisplaySize.y));
		_overlay_placed_for[0] = imgui_io.DisplaySize.x;
		_overlay_placed_for[1] = imgui_io.DisplaySize.y;
	}

	// No focus so that window state is preserved between opening/closing the GUI
	if (ImGui::Begin("###overlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		const ImVec2 position = ImGui::GetWindowPos();
		const ImVec2 size = ImGui::GetWindowSize();
		const float position_fraction[2] = { position.x / imgui_io.DisplaySize.x, position.y / imgui_io.DisplaySize.y };
		const float size_fraction[2] = { size.x / imgui_io.DisplaySize.x, size.y / imgui_io.DisplaySize.y };

		// Saved once moving or resizing ends; the pixel rounding of the placement is no move
		const auto is_moved = [](float actual, float saved, float display) { return std::abs(actual - saved) * display > 1.5f; };
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
			(is_moved(position_fraction[0], _overlay_position[0], imgui_io.DisplaySize.x) || is_moved(position_fraction[1], _overlay_position[1], imgui_io.DisplaySize.y) ||
			 is_moved(size_fraction[0], _overlay_size[0], imgui_io.DisplaySize.x) || is_moved(size_fraction[1], _overlay_size[1], imgui_io.DisplaySize.y)))
		{
			std::copy_n(position_fraction, 2, _overlay_position);
			std::copy_n(size_fraction, 2, _overlay_size);
			save_overlay_window(global_config());
		}

		if (ImGui::BeginTabBar("##overlay_tabs", ImGuiTabBarFlags_FittingPolicyScroll))
		{
			bool has_saved_tab = false;
			bool is_first_tab = true;

			// The saved tab is selected when the overlay first shows; from then on the selected one is saved
			const auto begin_tab = [this, &has_saved_tab, &is_first_tab](const std::string &label, const std::string &name) {
				const bool is_saved_tab = name == _overlay_tab;
				const bool is_default_tab = std::exchange(is_first_tab, false);
				has_saved_tab |= is_saved_tab;

				if (!ImGui::BeginTabItem(label.c_str(), nullptr, !_is_overlay_tab_selected && is_saved_tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
					return false;

				if (!_is_overlay_tab_selected)
				{
					_is_overlay_tab_selected = is_saved_tab;
				}
				else if (!is_saved_tab && !(_overlay_tab.empty() && is_default_tab))
				{
					_overlay_tab = name;
					save_overlay_window(global_config());
				}

				ImGui::BeginChild("##tab", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
				return true;
			};
			const auto end_tab = []() {
				ImGui::EndChild();
				ImGui::EndTabItem();
			};

			// The add-ons' overlays: the first one is ultraGame's panel
			std::vector<addon_info::overlay_callback> overlays;
			{
				for (const addon_info &info : addon_loaded_info)
					overlays.insert(overlays.end(), info.overlay_callbacks.begin(), info.overlay_callbacks.end());
			}

			const auto draw_overlay = [this, &begin_tab, &end_tab](const addon_info::overlay_callback &widget) {
				if (!begin_tab(widget.title + "###addon_" + widget.title, widget.title))
					return;

				if (!ultra::hasProcessExitBegun())
					widget.callback(this);
				end_tab();
			};

			if (!overlays.empty())
				draw_overlay(overlays.front());

			if (_has_user_shaders && begin_tab("Shaders###shaders", "shaders"))
			{
				draw_gui_shaders();
				end_tab();
			}

			for (size_t i = 1; i < overlays.size(); ++i)
				draw_overlay(overlays[i]);

			// A saved tab that is not there (an add-on that did not load, no shaders) leaves the first one selected, which is ultraGame's panel
			if (!has_saved_tab)
				_is_overlay_tab_selected = true;

			ImGui::EndTabBar();
		}
	}
	ImGui::End();
}

void reshade::runtime::draw_gui_shaders()
{
	const float auto_save_button_spacing = 2.0f;
	const float button_width = 12.5f * ImGui::GetFontSize();

	if (_reload_remaining_effects != std::numeric_limits<size_t>::max())
	{
		ultra::drawWindowProgressRing(ultra::compileProgress(_effects.size(), _reload_remaining_effects.load()));
		return; // Cannot show techniques and variables while effects are loading, since they are being modified in other threads during that time
	}

	if (imgui::search_input_box(_effect_filter, sizeof(_effect_filter), -(2 * (_imgui_context->Style.ItemSpacing.x + 2.0f + button_width))))
	{
		_effects_expanded_state = 3;

		for (technique &tech : _techniques)
		{
			std::string_view label = tech.annotation_as_string("ui_label");
			if (label.empty())
				label = tech.name;

			tech.hidden = tech.is_route || tech.annotation_as_int("hidden") != 0 || !(string_contains(label, _effect_filter) || string_contains(_effects[tech.effect_index].source_file.filename().u8string(), _effect_filter));
		}
	}

	ImGui::SameLine();

	if (ImGui::Button("Active to top", ImVec2(auto_save_button_spacing + button_width, 0)))
	{
		std::vector<size_t> technique_indices = _technique_sorting;

		for (auto it = technique_indices.begin(), target_it = it; it != technique_indices.end(); ++it)
		{
			const technique &tech = _techniques[*it];

			if (tech.enabled || tech.toggle_key_data[0] != 0)
			{
				target_it = std::rotate(target_it, it, std::next(it));
			}
		}

		reorder_techniques(std::move(technique_indices));

		save_effect_settings();
	}

	ImGui::SameLine();

	if (ImGui::Button((_effects_expanded_state & 2) ? "Collapse all" : "Expand all", ImVec2(auto_save_button_spacing + button_width, 0)))
		_effects_expanded_state = (~_effects_expanded_state & 2) | 1;

	ImGui::Spacing();

	if (!_last_reload_successful)
	{
		ImGui::PushTextWrapPos();
		ImGui::PushStyleColor(ImGuiCol_Text, COLOR_RED);
		ImGui::TextUnformatted("There were errors loading some effects.");
		ImGui::TextUnformatted("Hover the cursor over any red entries below to see the related error messages and/or check the log for more details if there are none.");
		ImGui::PopStyleColor();
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
	}

	if (!_effects_enabled)
	{
		ImGui::Text("Effects are disabled. Press '%s' to enable them again.", input::key_name(_effects_key_data).c_str());
		ImGui::Spacing();
	}

	float bottom_height = _variable_editor_height;
	bottom_height = std::max(bottom_height, 20.0f);
	bottom_height = ImGui::GetFrameHeightWithSpacing() + _imgui_context->Style.ItemSpacing.y + (
		_performance_mode ? 0 : (17 /* splitter */ + bottom_height));
	bottom_height = std::min(bottom_height, ImGui::GetContentRegionAvail().y - 20.0f);

	// The list only takes the height its techniques need, so the variables below get the rest of the window
	ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, ImGui::GetContentRegionAvail().y - bottom_height));
	if (ImGui::BeginChild("##techniques", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY))
	{
		draw_technique_editor();
	}
	ImGui::EndChild();

	if (!_performance_mode)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
		ImGui::ButtonEx("##splitter", ImVec2(ImGui::GetContentRegionAvail().x, 5));
		ImGui::PopStyleVar();

		if (ImGui::IsItemHovered())
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
		if (ImGui::IsItemActive())
		{
			ImVec2 move_delta = _imgui_context->IO.MouseDelta;
			move_delta += ImGui::GetKeyMagnitude2d(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown) * _imgui_context->IO.DeltaTime * 500.0f;

			_variable_editor_height = std::max(_variable_editor_height - move_delta.y, 0.0f);
			save_config();
		}

		const float bottom_height = ImGui::GetFrameHeightWithSpacing() + _imgui_context->Style.ItemSpacing.y;

		if (ImGui::BeginChild("##variables", ImVec2(0, -bottom_height), ImGuiChildFlags_Borders))
			draw_variable_editor();
		ImGui::EndChild();
	}

	ImGui::Spacing();

	if (ImGui::Button(ICON_FK_REFRESH " Reload", ImVec2(-(auto_save_button_spacing + button_width), 0)))
	{
		load_config(); // Reload configuration too

		if (!_no_effect_cache && (_imgui_context->IO.KeyCtrl || _imgui_context->IO.KeyShift))
			clear_effect_cache();

		reload_effects();
	}

	ImGui::SetItemTooltip("Reload all effects (can hold 'Ctrl' while clicking to clear the effect cache before loading).");

	ImGui::SameLine();

	if (ImGui::Checkbox("Performance Mode", &_performance_mode))
	{
		save_config();
		reload_effects(); // Reload effects after switching
	}

	ImGui::SetItemTooltip("Reload all effects into a more optimal representation that can give a performance boost, but disables variable tweaking.");
}
void reshade::runtime::draw_variable_editor()
{
	ImGui::BeginChild("##variables", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
	for (size_t effect_index = 0, id = 0; effect_index < _effects.size(); ++effect_index)
	{
		reshade::effect &effect = _effects[effect_index];

		// Hide variables that are not currently used in any of the active effects, and the route's effects
		// Also skip showing this effect in the variable list if it doesn't have any uniform variables to show
		if (!effect.rendering || effect.is_route || (effect.uniforms.empty() && effect.definitions.empty()))
			continue;
		assert(effect.compiled);

		bool force_reload_effect = false;
		const bool is_focused = _focused_effect == effect_index;
		const std::string effect_name = effect.source_file.filename().u8string();

		if (is_focused || _effects_expanded_state & 1)
			ImGui::SetNextItemOpen(is_focused || (_effects_expanded_state >> 1) != 0);

		if (!ImGui::TreeNodeEx(effect_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
			continue; // Skip rendering invisible items

		if (is_focused)
		{
			ImGui::SetScrollHereY(0.0f);
			_focused_effect = std::numeric_limits<size_t>::max();
		}

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(_imgui_context->Style.FramePadding.x, 0));
		if (imgui::confirm_button(
				ICON_FK_UNDO " Reset all to default",
				ImGui::CalcItemWidth(),
				"Do you really want to reset all values in '%s' to their defaults?", effect_name.c_str()))
		{
			// Reset all uniform variables
			for (uniform &variable_it : effect.uniforms)
				if (variable_it.special == special_uniform::none && !variable_it.annotation_as_uint("noreset"))
					reset_uniform_value(variable_it);

			// Reset the effect's own preprocessor definitions; reload after that to get accurate defaults again
			if (const auto own_it = _preset_preprocessor_definitions.find(effect_name);
				own_it != _preset_preprocessor_definitions.end() && !own_it->second.empty())
			{
				own_it->second.clear();
				force_reload_effect = true;
			}

			save_effect_settings();
		}
		ImGui::PopStyleVar();

		bool category_closed = false;
		bool category_visible = true;
		std::string current_category;

		size_t active_variable = 0;
		size_t active_variable_index = std::numeric_limits<size_t>::max();
		size_t hovered_variable = 0;
		size_t hovered_variable_index = std::numeric_limits<size_t>::max();

		for (size_t variable_index = 0; variable_index < effect.uniforms.size(); ++variable_index)
		{
			reshade::uniform &variable = effect.uniforms[variable_index];

			// Skip hidden and special variables
			if (variable.annotation_as_int("hidden") || variable.special != special_uniform::none)
			{
				if (variable.special == special_uniform::overlay_active)
					active_variable_index = variable_index;
				else if (variable.special == special_uniform::overlay_hovered)
					hovered_variable_index = variable_index;
				continue;
			}

			if (const std::string_view category = variable.annotation_as_string("ui_category");
				category != current_category)
			{
				current_category = category;

				if (!current_category.empty())
				{
					std::string category_label(variable.annotation_as_string("ui_category"));
					size_t num_spaces = 0;
					for (float x = 0, space_x = ImGui::CalcTextSize(" ").x, width = (ImGui::CalcItemWidth() - ImGui::CalcTextSize(category_label.data()).x - 45) / 2; x < width; x += space_x)
						num_spaces++;
					category_label.insert(0, num_spaces, ' ');
					// Ensure widget ID does not change with varying width
					category_label += "###" + current_category;
					// Append a unique value so that the context menu does not contain duplicated widgets when a category is made current multiple times
					category_label += std::to_string(variable_index);

					if (category_visible = true;
						variable.annotation_as_uint("ui_category_toggle") != 0)
						get_uniform_value(variable, &category_visible);

					if (category_visible)
					{
						ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_NoTreePushOnOpen;
						if (!variable.annotation_as_int("ui_category_closed"))
							flags |= ImGuiTreeNodeFlags_DefaultOpen;

						category_closed = !ImGui::TreeNodeEx(category_label.c_str(), flags);

						if (ImGui::BeginPopupContextItem(category_label.c_str()))
						{
							char label[64] = "";
							ImFormatString(label, IM_ARRAYSIZE(label), ICON_FK_UNDO " Reset all in '%s' to default", current_category.c_str());

							if (imgui::confirm_button(
									label,
									ImGui::GetContentRegionAvail().x,
									"Do you really want to reset all values in '%s' to their defaults?", current_category.c_str()))
							{
								for (uniform &variable_it : effect.uniforms)
									if (variable_it.special == special_uniform::none && !variable_it.annotation_as_uint("noreset") &&
										variable_it.annotation_as_string("ui_category") == category)
										reset_uniform_value(variable_it);

								save_effect_settings();
							}

							ImGui::EndPopup();
						}
					}
					else
					{
						category_closed = false;
					}
				}
				else
				{
					category_closed = false;
					category_visible = true;
				}
			}

			// Skip rendering invisible items
			if (category_closed || (!category_visible && !variable.annotation_as_uint("ui_category_toggle")))
				continue;

			bool modified = false;
			bool is_default_value = true;

			ImGui::PushID(static_cast<int>(id++));

			reshadefx::constant value;
			switch (variable.type.base)
			{
			case reshadefx::type::t_bool:
				get_uniform_value(variable, value.as_uint, variable.type.components());
				for (size_t i = 0; is_default_value && i < variable.type.components(); i++)
					is_default_value = (value.as_uint[i] != 0) == (variable.initializer_value.as_uint[i] != 0);
				break;
			case reshadefx::type::t_int:
			case reshadefx::type::t_uint:
				get_uniform_value(variable, value.as_int, variable.type.components());
				is_default_value = std::memcmp(value.as_int, variable.initializer_value.as_int, variable.type.components() * sizeof(int)) == 0;
				break;
			case reshadefx::type::t_float:
				const float threshold = variable.annotation_as_float("ui_step", 0, 0.001f) * 0.75f + FLT_EPSILON;
				get_uniform_value(variable, value.as_float, variable.type.components());
				for (size_t i = 0; is_default_value && i < variable.type.components(); i++)
					is_default_value = std::abs(value.as_float[i] - variable.initializer_value.as_float[i]) < threshold;
				break;
			}

			if (invoke_addon_event<addon_event::reshade_overlay_uniform_variable>(this, api::effect_uniform_variable{ reinterpret_cast<uintptr_t>(&variable) }))
			{
				reshadefx::constant new_value;
				switch (variable.type.base)
				{
				case reshadefx::type::t_bool:
					get_uniform_value(variable, new_value.as_uint, variable.type.components());
					for (size_t i = 0; !modified && i < variable.type.components(); i++)
						modified = (new_value.as_uint[i] != 0) != (value.as_uint[i] != 0);
					break;
				case reshadefx::type::t_int:
				case reshadefx::type::t_uint:
					get_uniform_value(variable, new_value.as_int, variable.type.components());
					modified = std::memcmp(new_value.as_int, value.as_int, variable.type.components() * sizeof(int)) != 0;
					break;
				case reshadefx::type::t_float:
					get_uniform_value(variable, new_value.as_float, variable.type.components());
					for (size_t i = 0; !modified && i < variable.type.components(); i++)
						modified = std::abs(new_value.as_float[i] - value.as_float[i]) > FLT_EPSILON;
					break;
				}
			}
			else
			{
				// Add spacing before variable widget
				for (int i = 0, spacing = variable.annotation_as_int("ui_spacing"); i < spacing; ++i)
					ImGui::Spacing();

				// Add user-configurable text before variable widget
				if (const std::string_view text = variable.annotation_as_string("ui_text");
					!text.empty())
				{
					ImGui::PushTextWrapPos();
					ImGui::TextUnformatted(text.data(), text.data() + text.size());
					ImGui::PopTextWrapPos();
				}

				ImGui::BeginDisabled(variable.annotation_as_uint("noedit") != 0);

				std::string_view label = variable.annotation_as_string("ui_label");
				if (label.empty())
					label = variable.name;
				const std::string_view ui_type = variable.annotation_as_string("ui_type");

				switch (variable.type.base)
				{
				case reshadefx::type::t_bool:
					{
						if (ui_type == "button")
						{
							if (ImGui::Button(label.data(), ImVec2(ImGui::CalcItemWidth(), 0)))
							{
								value.as_uint[0] = 1;
								modified = true;
							}
							else if (value.as_uint[0] != 0)
							{
								// Reset value again next frame after button was pressed
								value.as_uint[0] = 0;
								modified = true;
							}
						}
						else if (ui_type == "combo")
							modified = imgui::combo_with_buttons(label.data(), reinterpret_cast<bool *>(&value.as_uint[0]));
						else
							modified = imgui::checkbox_list(label.data(), variable.annotation_as_string("ui_items"), value.as_uint, variable.type.components());

						if (modified)
							set_uniform_value(variable, value.as_uint, variable.type.components());
					}
					break;
				case reshadefx::type::t_int:
				case reshadefx::type::t_uint:
					{
						const int ui_min_val = variable.annotation_as_int("ui_min", 0, ui_type == "slider" ? 0 : std::numeric_limits<int>::lowest());
						const int ui_max_val = variable.annotation_as_int("ui_max", 0, ui_type == "slider" ? 1 : std::numeric_limits<int>::max());
						const int ui_stp_val = variable.annotation_as_int("ui_step", 0, 1);

						// Append units
						std::string format = "%d";
						format += variable.annotation_as_string("ui_units");

						if (ui_type == "slider")
							modified = imgui::slider_with_buttons(label.data(), variable.type.is_signed() ? ImGuiDataType_S32 : ImGuiDataType_U32, value.as_int, variable.type.rows, &ui_stp_val, &ui_min_val, &ui_max_val, format.c_str());
						else if (ui_type == "drag")
							modified = variable.annotation_as_int("ui_step") == 0 ?
								ImGui::DragScalarN(label.data(), variable.type.is_signed() ? ImGuiDataType_S32 : ImGuiDataType_U32, value.as_int, variable.type.rows, 1.0f, &ui_min_val, &ui_max_val, format.c_str()) :
								imgui::drag_with_buttons(label.data(), variable.type.is_signed() ? ImGuiDataType_S32 : ImGuiDataType_U32, value.as_int, variable.type.rows, &ui_stp_val, &ui_min_val, &ui_max_val, format.c_str());
						else if (ui_type == "list")
							modified = imgui::list_with_buttons(label.data(), variable.annotation_as_string("ui_items"), &value.as_int[0]);
						else if (ui_type == "combo")
							modified = imgui::combo_with_buttons(label.data(), variable.annotation_as_string("ui_items"), &value.as_int[0]);
						else if (ui_type == "radio")
							modified = imgui::radio_list(label.data(), variable.annotation_as_string("ui_items"), &value.as_int[0]);
						else if (variable.type.is_matrix())
							for (unsigned int row = 0; row < variable.type.rows; ++row)
								modified |= ImGui::InputScalarN((std::string(label) + " [row " + std::to_string(row) + ']').c_str(), variable.type.is_signed() ? ImGuiDataType_S32 : ImGuiDataType_U32, &value.as_int[variable.type.cols * row], variable.type.cols) || modified;
						else
							modified = ImGui::InputScalarN(label.data(), variable.type.is_signed() ? ImGuiDataType_S32 : ImGuiDataType_U32, value.as_int, variable.type.rows);

						if (modified)
							set_uniform_value(variable, value.as_int, variable.type.components());
					}
					break;
				case reshadefx::type::t_float:
					{
						const float ui_min_val = variable.annotation_as_float("ui_min", 0, ui_type == "slider" ? 0.0f : std::numeric_limits<float>::lowest());
						const float ui_max_val = variable.annotation_as_float("ui_max", 0, ui_type == "slider" ? 1.0f : std::numeric_limits<float>::max());
						const float ui_stp_val = variable.annotation_as_float("ui_step", 0, 0.001f);

						// Calculate display precision based on step value
						std::string precision_format = "%.0f";
						for (float x = 1.0f; x * ui_stp_val < 1.0f && precision_format[2] < '9'; x *= 10.0f)
							++precision_format[2]; // This changes the text to "%.1f", "%.2f", "%.3f", ...

						// Append units
						precision_format += variable.annotation_as_string("ui_units");

						if (ui_type == "slider")
							modified = imgui::slider_with_buttons(label.data(), ImGuiDataType_Float, value.as_float, variable.type.rows, &ui_stp_val, &ui_min_val, &ui_max_val, precision_format.c_str());
						else if (ui_type == "drag")
							modified = variable.annotation_as_float("ui_step") == 0.0f ?
								ImGui::DragScalarN(label.data(), ImGuiDataType_Float, value.as_float, variable.type.rows, ui_stp_val, &ui_min_val, &ui_max_val, precision_format.c_str()) :
								imgui::drag_with_buttons(label.data(), ImGuiDataType_Float, value.as_float, variable.type.rows, &ui_stp_val, &ui_min_val, &ui_max_val, precision_format.c_str());
						else if (ui_type == "color" && variable.type.rows == 1)
							modified = imgui::slider_for_alpha_value(label.data(), value.as_float);
						else if (ui_type == "color" && variable.type.rows == 3)
							modified = ImGui::ColorEdit3(label.data(), value.as_float, ImGuiColorEditFlags_NoOptions);
						else if (ui_type == "color" && variable.type.rows == 4)
							modified = ImGui::ColorEdit4(label.data(), value.as_float, ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_AlphaBar);
						else if (variable.type.is_matrix())
							for (unsigned int row = 0; row < variable.type.rows; ++row)
								modified |= ImGui::InputScalarN((std::string(label) + " [row " + std::to_string(row) + ']').c_str(), ImGuiDataType_Float, &value.as_float[variable.type.cols * row], variable.type.cols) || modified;
						else
							modified = ImGui::InputScalarN(label.data(), ImGuiDataType_Float, value.as_float, variable.type.rows);

						if (modified)
							set_uniform_value(variable, value.as_float, variable.type.components());
					}
					break;
				}

				ImGui::EndDisabled();

				// Display tooltip
				if (const std::string_view tooltip = variable.annotation_as_string("ui_tooltip");
					!tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
				{
					if (ImGui::BeginTooltip())
					{
						ImGui::TextUnformatted(tooltip.data(), tooltip.data() + tooltip.size());
						ImGui::EndTooltip();
					}
				}
			}

			if (ImGui::IsItemActive())
				active_variable = variable_index + 1;
			if (ImGui::IsItemHovered())
				hovered_variable = variable_index + 1;

			// Create context menu
			if (ImGui::BeginPopupContextItem("##context"))
			{
				ImGui::SetNextItemWidth(18.0f * ImGui::GetFontSize());
				if (variable.supports_toggle_key() &&
					_input != nullptr &&
					imgui::key_input_box("##toggle_key", variable.toggle_key_data, *_input))
					modified = true;

				if (ImGui::Button(ICON_FK_UNDO " Reset to default", ImVec2(18.0f * ImGui::GetFontSize(), 0)))
				{
					modified = true;
					reset_uniform_value(variable);
					ImGui::CloseCurrentPopup();
				}

				ImGui::EndPopup();
			}

			if (!is_default_value && !variable.annotation_as_uint("noreset"))
			{
				ImGui::SameLine();
				if (ImGui::SmallButton(ICON_FK_UNDO))
				{
					modified = true;
					reset_uniform_value(variable);
				}
			}

			if (variable.toggle_key_data[0] != 0)
			{
				ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120);
				ImGui::TextDisabled("%s", input::key_name(variable.toggle_key_data).c_str());
			}

			ImGui::PopID();

			// A value has changed, so save the user's effects
			if (modified && !variable.annotation_as_uint("nosave"))
				save_effect_settings();
		}

		if (active_variable_index < effect.uniforms.size())
			set_uniform_value(effect.uniforms[active_variable_index], static_cast<uint32_t>(active_variable));
		if (hovered_variable_index < effect.uniforms.size())
			set_uniform_value(effect.uniforms[hovered_variable_index], static_cast<uint32_t>(hovered_variable));

		// Draw preprocessor definition list after all uniforms of an effect file: the user's own are the effect file's saved ones
		std::vector<std::pair<std::string, std::string>> &effect_definitions = _preset_preprocessor_definitions[effect_name];
		std::string modified_definition;

		if (!effect.definitions.empty())
		{
			std::string category_label = "Preprocessor definitions";
			for (float x = 0, space_x = ImGui::CalcTextSize(" ").x, width = (ImGui::CalcItemWidth() - ImGui::CalcTextSize(category_label.c_str()).x - 45) / 2; x < width; x += space_x)
				category_label.insert(0, " ");
			category_label += "###ppdefinitions";

			if (ImGui::TreeNodeEx(category_label.c_str(), ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_DefaultOpen))
			{
				for (const std::pair<std::string, std::string> &definition : effect.definitions)
				{
					const auto own_it = std::find_if(effect_definitions.begin(), effect_definitions.end(),
						[&definition](const std::pair<std::string, std::string> &own) { return own.first == definition.first; });
					const bool is_own = own_it != effect_definitions.end();

					char value[256];
					if (const std::optional<std::string> current = find_preprocessor_definition(effect_name, definition.first, 0b111))
						value[current->copy(value, sizeof(value) - 1)] = '\0';
					else
						value[0] = '\0';

					if (ImGui::InputTextWithHint(definition.first.c_str(), definition.second.c_str(), value, sizeof(value), ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue))
					{
						if (value[0] == '\0') // An empty value removes the definition
						{
							if (is_own)
							{
								force_reload_effect = true;
								effect_definitions.erase(own_it);
							}
						}
						else
						{
							force_reload_effect = true;
							modified_definition = definition.first;

							if (is_own)
								own_it->second = value;
							else
								effect_definitions.emplace_back(definition.first, value);
						}
					}

					if (force_reload_effect) // Cannot use the iterator if definitions were just modified above
						continue;

					if (ImGui::BeginPopupContextItem())
					{
						if (ImGui::Button(ICON_FK_UNDO " Reset to default", ImVec2(18.0f * ImGui::GetFontSize(), 0)))
						{
							if (is_own)
							{
								force_reload_effect = true;
								effect_definitions.erase(own_it);
							}

							ImGui::CloseCurrentPopup();
						}

						ImGui::EndPopup();
					}

					if (is_own && !force_reload_effect)
					{
						ImGui::PushID(definition.first.c_str());

						ImGui::SameLine();
						if (ImGui::SmallButton(ICON_FK_UNDO))
						{
							force_reload_effect = true;
							effect_definitions.erase(own_it);
						}

						ImGui::PopID();
					}
				}
			}
		}

		ImGui::TreePop();

		if (force_reload_effect)
		{
			save_effect_settings();

			const bool reload_successful_before = _last_reload_successful;

			// Reload current effect file
			if (!reload_effect(effect_index) && !modified_definition.empty())
			{
				// The preprocessor definition that was just modified caused the effect to not compile, so reset to default and try again
				std::erase_if(effect_definitions, [&modified_definition](const std::pair<std::string, std::string> &own) { return own.first == modified_definition; });

				if (reload_effect(effect_index))
				{
					_last_reload_successful = reload_successful_before;
					ImGui::OpenPopup("##pperror"); // Notify the user about this

					// Once the reloaded effect has its settings again (see 'update_effects'), so its techniques are saved as they are
					_wants_effect_settings_save = true;
				}
			}
		}
	}

	if (ImGui::BeginPopup("##pperror"))
	{
		ImGui::TextColored(COLOR_RED, "The effect failed to compile after this change, so reverted back to the default value.");
		ImGui::EndPopup();
	}

	ImGui::EndChild();
}
void reshade::runtime::draw_technique_editor()
{
	if (!_last_reload_successful)
	{
		// Add fake items at the top for effects that failed to compile
		for (size_t effect_index = 0; effect_index < _effects.size(); ++effect_index)
		{
			const effect &effect = _effects[effect_index];

			if (effect.compiled || effect.is_route)
				continue;

			ImGui::PushID(static_cast<int>(_technique_sorting.size() + effect_index));

			ImGui::PushStyleColor(ImGuiCol_Text, COLOR_RED);
			ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);

			{
				char label[128] = "";
				ImFormatString(label, IM_ARRAYSIZE(label), "[%s] failed to compile", effect.source_file.filename().u8string().c_str());

				bool value = false;
				ImGui::Checkbox(label, &value);
			}

			ImGui::PopItemFlag();

			// Display tooltip
			if (!effect.errors.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip))
			{
				if (ImGui::BeginTooltip())
				{
					parse_errors(effect.errors,
						[](const std::string_view file, int line, const std::string_view message) {
							if (file.empty())
								ImGui::TextUnformatted(message.data(), message.data() + message.size());
							else
								ImGui::Text("%s(%d): %.*s", std::filesystem::path(file).filename().u8string().c_str(), line, static_cast<int>(message.size()), message.data());
						});
					ImGui::EndTooltip();
				}
			}

			ImGui::PopStyleColor();

			// Create context menu
			if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::OpenPopup("##context", ImGuiPopupFlags_MouseButtonRight);

			if (ImGui::BeginPopup("##context"))
			{
				if (ImGui::Button(ICON_FK_FOLDER " Open folder in explorer", ImVec2(18.0f * ImGui::GetFontSize(), 0)))
					utils::open_explorer(effect.source_file);

				ImGui::EndPopup();
			}

			ImGui::PopID();
		}
	}

	size_t hovered_technique_index = std::numeric_limits<size_t>::max();

	for (size_t index = 0; index < _technique_sorting.size(); ++index)
	{
		const size_t technique_index = _technique_sorting[index];
		{
			technique &tech = _techniques[technique_index];
			const effect &effect = _effects[tech.effect_index];

			// Skip hidden techniques
			if (tech.hidden || !effect.compiled)
				continue;

			bool modified = false;

			ImGui::PushID(static_cast<int>(index));

			// Draw border around the item if it is selected
			const bool draw_border = _selected_technique == index;
			if (draw_border)
				ImGui::Separator();

			// Prevent user from disabling the technique when it is set to always be enabled via annotation
			const bool force_enabled = tech.annotation_as_int("enabled");

			if (bool was_enabled = tech.enabled;
				invoke_addon_event<addon_event::reshade_overlay_technique>(this, api::effect_technique { reinterpret_cast<uintptr_t>(&tech) }))
			{
				modified = tech.enabled != was_enabled;
			}
			else
			{
				ImGui::BeginDisabled(tech.annotation_as_uint("noedit") != 0);

				// Gray out disabled techniques
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(tech.enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled));

				std::string label(tech.annotation_as_string("ui_label"));
				if (label.empty())
					label = tech.name;
				label += " [" + effect.source_file.filename().u8string() + ']';

				if (bool status = tech.enabled;
					ImGui::Checkbox(label.c_str(), &status) && !force_enabled)
				{
					modified = true;

					if (status)
						enable_technique(tech);
					else
						disable_technique(tech);
				}

				ImGui::PopStyleColor();

				ImGui::EndDisabled();

				// Display tooltip
				if (const std::string_view tooltip = tech.annotation_as_string("ui_tooltip");
					!tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
				{
					if (ImGui::BeginTooltip())
					{
						ImGui::TextUnformatted(tooltip.data(), tooltip.data() + tooltip.size());
						ImGui::EndTooltip();
					}
				}
			}

			if (ImGui::IsItemActive())
				_selected_technique = index;
			if (ImGui::IsItemClicked())
				_focused_effect = tech.effect_index;
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_RectOnly | ImGuiHoveredFlags_AllowWhenDisabled))
				hovered_technique_index = index;

			// Create context menu
			if (ImGui::BeginPopupContextItem("##context"))
			{
				ImGui::TextUnformatted(tech.name.c_str(), tech.name.c_str() + tech.name.size());
				ImGui::Separator();

				ImGui::SetNextItemWidth(18.0f * ImGui::GetFontSize());
				if (_input != nullptr && !force_enabled &&
					imgui::key_input_box("##toggle_key", tech.toggle_key_data, *_input))
				{
					save_effect_settings();
				}

				const bool is_not_top = index > 0;
				const bool is_not_bottom = index < _technique_sorting.size() - 1;

				if (is_not_top && ImGui::Button("Move to top", ImVec2(18.0f * ImGui::GetFontSize(), 0)))
				{
					std::vector<size_t> technique_indices = _technique_sorting;
					technique_indices.insert(technique_indices.begin(), technique_indices[index]);
					technique_indices.erase(technique_indices.begin() + 1 + index);
					reorder_techniques(std::move(technique_indices));

					save_effect_settings();

					ImGui::CloseCurrentPopup();
				}
				if (is_not_bottom && ImGui::Button("Move to bottom", ImVec2(18.0f * ImGui::GetFontSize(), 0)))
				{
					std::vector<size_t> technique_indices = _technique_sorting;
					technique_indices.push_back(technique_indices[index]);
					technique_indices.erase(technique_indices.begin() + index);
					reorder_techniques(std::move(technique_indices));

					save_effect_settings();

					ImGui::CloseCurrentPopup();
				}

				if (is_not_top || is_not_bottom || (_input != nullptr && !force_enabled))
					ImGui::Separator();

				if (ImGui::Button(ICON_FK_FOLDER " Open folder in explorer", ImVec2(18.0f * ImGui::GetFontSize(), 0)))
					utils::open_explorer(effect.source_file);

				ImGui::EndPopup();
			}

			if (tech.toggle_key_data[0] != 0)
			{
				ImGui::SameLine(ImGui::GetContentRegionAvail().x - 10.0f * ImGui::GetFontSize());
				ImGui::TextDisabled("%s", input::key_name(tech.toggle_key_data).c_str());
			}

			if (draw_border)
				ImGui::Separator();

			ImGui::PopID();

			if (modified)
				save_effect_settings();
		}
	}

	// Move the selected technique to the position of the mouse in the list
	if (_selected_technique < _technique_sorting.size() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
	{
		if (hovered_technique_index < _technique_sorting.size() && hovered_technique_index != _selected_technique)
		{
			std::vector<size_t> technique_indices = _technique_sorting;

			const auto move_technique = [&technique_indices](size_t from_index, size_t to_index) {
				if (to_index < from_index) // Up
					for (size_t i = from_index; to_index < i; --i)
						std::swap(technique_indices[i - 1], technique_indices[i]);
				else // Down
					for (size_t i = from_index; i < to_index; ++i)
						std::swap(technique_indices[i], technique_indices[i + 1]);
			};

			move_technique(_selected_technique, hovered_technique_index);

			// Pressing shift moves all techniques from the same effect file to the new location as well
			if (_imgui_context->IO.KeyShift)
			{
				for (size_t i = hovered_technique_index + 1, offset = 1; i < technique_indices.size(); ++i)
				{
					if (_techniques[technique_indices[i]].effect_index == _focused_effect)
					{
						if ((i - hovered_technique_index) > offset)
							move_technique(i, hovered_technique_index + offset);
						offset++;
					}
				}
				for (size_t i = hovered_technique_index - 1, offset = 0; i >= 0 && i != std::numeric_limits<size_t>::max(); --i)
				{
					if (_techniques[technique_indices[i]].effect_index == _focused_effect)
					{
						offset++;
						if ((hovered_technique_index - i) > offset)
							move_technique(i, hovered_technique_index - offset);
					}
				}
			}

			reorder_techniques(std::move(technique_indices));
			_selected_technique = hovered_technique_index;

			save_effect_settings();
		}
	}
	else
	{
		_selected_technique = std::numeric_limits<size_t>::max();
	}
}

bool reshade::runtime::init_imgui_resources()
{
	const bool has_combined_sampler_and_view = _device->check_capability(api::device_caps::sampler_with_resource_view);

	if (_imgui_sampler_state == 0)
	{
		api::sampler_desc sampler_desc = {};
		sampler_desc.filter = api::filter_mode::min_mag_mip_linear;
		sampler_desc.address_u = api::texture_address_mode::clamp;
		sampler_desc.address_v = api::texture_address_mode::clamp;
		sampler_desc.address_w = api::texture_address_mode::clamp;

		if (!_device->create_sampler(sampler_desc, &_imgui_sampler_state))
		{
			log::message(log::level::error, "Failed to create ImGui sampler object!");
			return false;
		}
	}

	if (_imgui_pipeline_layout == 0)
	{
		uint32_t num_layout_params = 0;
		api::pipeline_layout_param layout_params[3];

		if (has_combined_sampler_and_view)
		{
			layout_params[num_layout_params++] = api::descriptor_range { 0, 0, 0, 1, api::shader_stage::pixel, 1, api::descriptor_type::sampler_with_resource_view }; // s0
		}
		else
		{
			layout_params[num_layout_params++] = api::descriptor_range { 0, 0, 0, 1, api::shader_stage::pixel, 1, api::descriptor_type::sampler }; // s0
			layout_params[num_layout_params++] = api::descriptor_range { 0, 0, 0, 1, api::shader_stage::pixel, 1, api::descriptor_type::shader_resource_view }; // t0
		}

		layout_params[num_layout_params++] = api::constant_range { 0, 0, 0, 18, api::shader_stage::vertex | api::shader_stage::pixel }; // b0

		if (!_device->create_pipeline_layout(num_layout_params, layout_params, &_imgui_pipeline_layout))
		{
			log::message(log::level::error, "Failed to create ImGui pipeline layout!");
			return false;
		}
	}

	if (_imgui_pipeline != 0)
		return true;

	const resources::data_resource vs_res = resources::load_data_resource(
		_renderer_id >= 0x20000 ? IDR_IMGUI_VS_SPIRV :
		_renderer_id >= 0x10000 ? IDR_IMGUI_VS_GLSL :
		_renderer_id >= 0x0a000 ? IDR_IMGUI_VS_4_0 : IDR_IMGUI_VS_3_0);
	api::shader_desc vs_desc;
	vs_desc.code = vs_res.data;
	vs_desc.code_size = vs_res.data_size;

	const resources::data_resource ps_res = resources::load_data_resource(
		_renderer_id >= 0x20000 ? IDR_IMGUI_PS_SPIRV :
		_renderer_id >= 0x10000 ? IDR_IMGUI_PS_GLSL :
		_renderer_id >= 0x0a000 ? IDR_IMGUI_PS_4_0 : IDR_IMGUI_PS_3_0);
	api::shader_desc ps_desc;
	ps_desc.code = ps_res.data;
	ps_desc.code_size = ps_res.data_size;

	std::vector<api::pipeline_subobject> subobjects;
	subobjects.push_back({ api::pipeline_subobject_type::vertex_shader, 1, &vs_desc });
	subobjects.push_back({ api::pipeline_subobject_type::pixel_shader, 1, &ps_desc });

	const api::input_element input_layout[3] = {
		{ 0, "POSITION", 0, api::format::r32g32_float,   0, offsetof(ImDrawVert, pos), sizeof(ImDrawVert), 0 },
		{ 1, "TEXCOORD", 0, api::format::r32g32_float,   0, offsetof(ImDrawVert, uv ), sizeof(ImDrawVert), 0 },
		{ 2, "COLOR",    0, api::format::r8g8b8a8_unorm, 0, offsetof(ImDrawVert, col), sizeof(ImDrawVert), 0 }
	};
	subobjects.push_back({ api::pipeline_subobject_type::input_layout, 3, (void *)input_layout });

	api::primitive_topology topology = api::primitive_topology::triangle_list;
	subobjects.push_back({ api::pipeline_subobject_type::primitive_topology, 1, &topology });

	api::blend_desc blend_state;
	blend_state.blend_enable[0] = true;
	blend_state.source_color_blend_factor[0] = api::blend_factor::source_alpha;
	blend_state.dest_color_blend_factor[0] = api::blend_factor::one_minus_source_alpha;
	blend_state.color_blend_op[0] = api::blend_op::add;
	blend_state.source_alpha_blend_factor[0] = api::blend_factor::one;
	blend_state.dest_alpha_blend_factor[0] = api::blend_factor::one_minus_source_alpha;
	blend_state.alpha_blend_op[0] = api::blend_op::add;
	blend_state.render_target_write_mask[0] = 0xF;
	subobjects.push_back({ api::pipeline_subobject_type::blend_state, 1, &blend_state });

	api::rasterizer_desc rasterizer_state;
	rasterizer_state.cull_mode = api::cull_mode::none;
	rasterizer_state.scissor_enable = true;
	subobjects.push_back({ api::pipeline_subobject_type::rasterizer_state, 1, &rasterizer_state });

	api::depth_stencil_desc depth_stencil_state;
	depth_stencil_state.depth_enable = false;
	depth_stencil_state.stencil_enable = false;
	subobjects.push_back({ api::pipeline_subobject_type::depth_stencil_state, 1, &depth_stencil_state });

	// Always choose non-sRGB format variant, since 'render_imgui_draw_data' is called with the non-sRGB render target (see 'draw_gui')
	api::format render_target_format = api::format_to_default_typed(_back_buffer_format, 0);
	subobjects.push_back({ api::pipeline_subobject_type::render_target_formats, 1, &render_target_format });

	if (!_device->create_pipeline(_imgui_pipeline_layout, static_cast<uint32_t>(subobjects.size()), subobjects.data(), &_imgui_pipeline))
	{
		log::message(log::level::error, "Failed to create ImGui pipeline!");
		return false;
	}

	return true;
}
void reshade::runtime::render_imgui_draw_data(api::command_list *cmd_list, ImDrawData *draw_data, api::resource_view rtv)
{
	::ultra::ui::font::flush(cmd_list, _device);

	assert(draw_data->Textures != nullptr);
	for (ImTextureData *const texture_data : *draw_data->Textures)
	{
		if (texture_data->Status == ImTextureStatus_OK)
			continue;

		if (texture_data->Status == ImTextureStatus_WantCreate)
		{
			assert(texture_data->GetTexID() == ImTextureID_Invalid);

			api::format format = api::format::unknown;
			switch (texture_data->Format)
			{
			case ImTextureFormat_RGBA32:
				format = api::format::r8g8b8a8_unorm;
				break;
			case ImTextureFormat_Alpha8:
				format = api::format::r8_unorm;
				break;
			}

			const api::subresource_data initial_data = { texture_data->GetPixels(), static_cast<uint32_t>(texture_data->GetPitch()), static_cast<uint32_t>(texture_data->GetSizeInBytes()) };

			api::resource imgui_tex;
			if (!_device->create_resource(
					api::resource_desc(texture_data->Width, texture_data->Height, 1, 1, format, 1, api::memory_heap::default_, api::resource_usage::shader_resource | api::resource_usage::copy_dest),
					&initial_data, api::resource_usage::shader_resource, &imgui_tex))
			{
				log::message(log::level::error, "Failed to create imgui texture resource!");

				texture_data->SetStatus(ImTextureStatus_Destroyed);
				continue;
			}

			api::resource_view imgui_srv;
			if (!_device->create_resource_view(imgui_tex, api::resource_usage::shader_resource, api::resource_view_desc(format), &imgui_srv))
			{
				log::message(log::level::error, "Failed to create imgui texture resource view!");

				_device->destroy_resource(imgui_tex);

				texture_data->SetStatus(ImTextureStatus_Destroyed);
				continue;
			}

			texture_data->SetTexID(imgui_srv.handle);
			texture_data->SetStatus(ImTextureStatus_OK);
			continue;
		}

		if (texture_data->Status == ImTextureStatus_WantUpdates)
		{
			const auto imgui_srv = api::resource_view { texture_data->GetTexID() };
			const auto imgui_tex = _device->get_resource_from_view(imgui_srv);

			cmd_list->barrier(imgui_tex, api::resource_usage::shader_resource, api::resource_usage::copy_dest);
			for (const ImTextureRect &update_rect : texture_data->Updates)
			{
				api::subresource_box box;
				box.left = update_rect.x;
				box.top = update_rect.y;
				box.front = 0;
				box.right = update_rect.x + update_rect.w;
				box.bottom = update_rect.y + update_rect.h;
				box.back = 1;

				cmd_list->update_texture_region(
					api::subresource_data { texture_data->GetPixelsAt(update_rect.x, update_rect.y), static_cast<uint32_t>(texture_data->GetPitch()), static_cast<uint32_t>(texture_data->GetSizeInBytes()) },
					imgui_tex,
					0,
					&box);
			}
			cmd_list->barrier(imgui_tex, api::resource_usage::copy_dest, api::resource_usage::shader_resource);

			texture_data->SetStatus(ImTextureStatus_OK);
			continue;
		}

		if (texture_data->Status == ImTextureStatus_WantDestroy && texture_data->UnusedFrames > 8)
		{
			const auto imgui_srv = api::resource_view { texture_data->GetTexID() };
			const auto imgui_tex = _device->get_resource_from_view(imgui_srv);

			_device->destroy_resource_view(imgui_srv);
			_device->destroy_resource(imgui_tex);

			texture_data->SetTexID(ImTextureID_Invalid);
			texture_data->SetStatus(ImTextureStatus_Destroyed);
			continue;
		}
	}

	// Need to multi-buffer vertex data so not to modify data below when the previous frame is still in flight
	const size_t buffer_index = _frame_count % (_renderer_id & 0x20000 ? 8 : 4);
	assert(buffer_index < std::size(_imgui_vertices));

	// Create and grow vertex/index buffers if needed
	if (_imgui_num_indices[buffer_index] < draw_data->TotalIdxCount)
	{
		if (_imgui_indices[buffer_index] != 0)
		{
			_graphics_queue->wait_idle(); // Be safe and ensure nothing still uses this buffer

			_device->destroy_resource(_imgui_indices[buffer_index]);
		}

		const int new_size = draw_data->TotalIdxCount + 10000;
		if (!_device->create_resource(api::resource_desc(new_size * sizeof(ImDrawIdx), api::memory_heap::upload, api::resource_usage::index_buffer), nullptr, api::resource_usage::cpu_access, &_imgui_indices[buffer_index]))
		{
			log::message(log::level::error, "Failed to create ImGui index buffer!");
			return;
		}

		_device->set_resource_name(_imgui_indices[buffer_index], "ImGui index buffer");

		_imgui_num_indices[buffer_index] = new_size;
	}
	if (_imgui_num_vertices[buffer_index] < draw_data->TotalVtxCount)
	{
		if (_imgui_vertices[buffer_index] != 0)
		{
			_graphics_queue->wait_idle();

			_device->destroy_resource(_imgui_vertices[buffer_index]);
		}

		const int new_size = draw_data->TotalVtxCount + 5000;
		if (!_device->create_resource(api::resource_desc(new_size * sizeof(ImDrawVert), api::memory_heap::upload, api::resource_usage::vertex_buffer), nullptr, api::resource_usage::cpu_access, &_imgui_vertices[buffer_index]))
		{
			log::message(log::level::error, "Failed to create ImGui vertex buffer!");
			return;
		}

		_device->set_resource_name(_imgui_vertices[buffer_index], "ImGui vertex buffer");

		_imgui_num_vertices[buffer_index] = new_size;
	}

#ifndef NDEBUG
	cmd_list->begin_debug_event("ReShade overlay");
#endif

	if (ImDrawIdx *idx_dst;
		_device->map_buffer_region(_imgui_indices[buffer_index], 0, UINT64_MAX, api::map_access::write_only, reinterpret_cast<void **>(&idx_dst)))
	{
		for (int n = 0; n < draw_data->CmdLists.Size; ++n)
		{
			const ImDrawList *const draw_list = draw_data->CmdLists[n];
			std::memcpy(idx_dst, draw_list->IdxBuffer.Data, draw_list->IdxBuffer.Size * sizeof(ImDrawIdx));
			idx_dst += draw_list->IdxBuffer.Size;
		}

		_device->unmap_buffer_region(_imgui_indices[buffer_index]);
	}
	if (ImDrawVert *vtx_dst;
		_device->map_buffer_region(_imgui_vertices[buffer_index], 0, UINT64_MAX, api::map_access::write_only, reinterpret_cast<void **>(&vtx_dst)))
	{
		for (int n = 0; n < draw_data->CmdLists.Size; ++n)
		{
			const ImDrawList *const draw_list = draw_data->CmdLists[n];
			std::memcpy(vtx_dst, draw_list->VtxBuffer.Data, draw_list->VtxBuffer.Size * sizeof(ImDrawVert));
			vtx_dst += draw_list->VtxBuffer.Size;
		}

		_device->unmap_buffer_region(_imgui_vertices[buffer_index]);
	}

	api::render_pass_render_target_desc render_target = {};
	render_target.view = rtv;

	cmd_list->begin_render_pass(1, &render_target, nullptr);

	// Setup render state
	cmd_list->bind_pipeline(api::pipeline_stage::all_graphics, _imgui_pipeline);

	cmd_list->bind_index_buffer(_imgui_indices[buffer_index], 0, sizeof(ImDrawIdx));
	cmd_list->bind_vertex_buffer(0, _imgui_vertices[buffer_index], 0, sizeof(ImDrawVert));

	const api::viewport viewport = { 0, 0, draw_data->DisplaySize.x, draw_data->DisplaySize.y, 0.0f, 1.0f };
	cmd_list->bind_viewports(0, 1, &viewport);

	// Setup orthographic projection matrix
	const bool flip_y = (_renderer_id & 0x10000) != 0;
	const bool adjust_half_pixel = _renderer_id < 0xa000; // Bake half-pixel offset into matrix in D3D9
	const bool depth_clip_zero_to_one = (_renderer_id & 0x10000) == 0;

	// For a game that states the wrong colour space (1 SDR, 2 scRGB, 3 HDR10, 4 HLG), read at each draw so the panel's choice shows at once
	int overwrite_color_space = 0;
	global_config().get("STYLE", "HdrOverlayOverwriteColorSpaceTo", overwrite_color_space);

	const struct {
		float ortho_projection[16];
		api::color_space color_space;
		float hdr_overlay_brightness;
	} push_constants = {
		{
			2.0f / draw_data->DisplaySize.x, 0.0f, 0.0f, 0.0f,
			0.0f, (flip_y ? 2.0f : -2.0f) / draw_data->DisplaySize.y, 0.0f, 0.0f,
			0.0f,                            0.0f, depth_clip_zero_to_one ? 0.5f : -1.0f, 0.0f,
							   -(2 * draw_data->DisplayPos.x + draw_data->DisplaySize.x + (adjust_half_pixel ? 1.0f : 0.0f)) / draw_data->DisplaySize.x,
			(flip_y ? -1 : 1) * (2 * draw_data->DisplayPos.y + draw_data->DisplaySize.y + (adjust_half_pixel ? 1.0f : 0.0f)) / draw_data->DisplaySize.y, depth_clip_zero_to_one ? 0.5f : 0.0f, 1.0f,
		},
		overwrite_color_space != 0 ? static_cast<api::color_space>(overwrite_color_space) : _back_buffer_color_space,
		hdr_overlay_brightness
	};

	const bool has_combined_sampler_and_view = _device->check_capability(api::device_caps::sampler_with_resource_view);

	cmd_list->push_constants(api::shader_stage::vertex | api::shader_stage::pixel, _imgui_pipeline_layout, has_combined_sampler_and_view ? 1 : 2, 0, (_renderer_id != 0x9000 ? sizeof(push_constants) : sizeof(push_constants.ortho_projection)) / 4, &push_constants);
	if (!has_combined_sampler_and_view)
		cmd_list->push_descriptors(api::shader_stage::pixel, _imgui_pipeline_layout, 0, api::descriptor_table_update { {}, 0, 0, 1, api::descriptor_type::sampler, &_imgui_sampler_state });

	int vtx_offset = 0, idx_offset = 0;
	for (int n = 0; n < draw_data->CmdLists.Size; ++n)
	{
		const ImDrawList *const draw_list = draw_data->CmdLists[n];

		for (const ImDrawCmd &cmd : draw_list->CmdBuffer)
		{
			if (cmd.UserCallback != nullptr)
			{
				cmd.UserCallback(draw_list, &cmd);
				continue;
			}

			const api::rect scissor_rect = {
				static_cast<int32_t>(cmd.ClipRect.x - draw_data->DisplayPos.x),
				flip_y ? static_cast<int32_t>(_height - cmd.ClipRect.w + draw_data->DisplayPos.y) : static_cast<int32_t>(cmd.ClipRect.y - draw_data->DisplayPos.y),
				static_cast<int32_t>(cmd.ClipRect.z - draw_data->DisplayPos.x),
				flip_y ? static_cast<int32_t>(_height - cmd.ClipRect.y + draw_data->DisplayPos.y) : static_cast<int32_t>(cmd.ClipRect.w - draw_data->DisplayPos.y)
			};

			cmd_list->bind_scissor_rects(0, 1, &scissor_rect);

			const api::resource_view srv = { cmd.GetTexID() };
			if (has_combined_sampler_and_view)
			{
				api::sampler_with_resource_view sampler_and_view = { _imgui_sampler_state, srv };
				cmd_list->push_descriptors(api::shader_stage::pixel, _imgui_pipeline_layout, 0, api::descriptor_table_update { {}, 0, 0, 1, api::descriptor_type::sampler_with_resource_view, &sampler_and_view });
			}
			else
			{
				cmd_list->push_descriptors(api::shader_stage::pixel, _imgui_pipeline_layout, 1, api::descriptor_table_update { {}, 0, 0, 1, api::descriptor_type::shader_resource_view, &srv });
			}

			cmd_list->draw_indexed(cmd.ElemCount, 1, cmd.IdxOffset + idx_offset, cmd.VtxOffset + vtx_offset, 0);
		}

		idx_offset += draw_list->IdxBuffer.Size;
		vtx_offset += draw_list->VtxBuffer.Size;
	}

	cmd_list->end_render_pass();

#ifndef NDEBUG
	cmd_list->end_debug_event();
#endif
}
void reshade::runtime::destroy_imgui_resources()
{
	ImFontAtlas *const atlas = _imgui_context->IO.Fonts;
	atlas->Clear();

	// Have to rebuild font atlas next time it is used again, since it is being destroyed here
	_rebuild_font_atlas = true;

	for (ImTextureData *const texture_data : _imgui_context->PlatformIO.Textures)
	{
		if (texture_data->Status == ImTextureStatus_Destroyed || (texture_data->Status == ImTextureStatus_WantCreate && !texture_data->WantDestroyNextFrame))
			continue;

		assert(texture_data->RefCount == 1);

		const auto imgui_srv = api::resource_view { texture_data->GetTexID() };
		const auto imgui_tex = _device->get_resource_from_view(imgui_srv);

		_device->destroy_resource_view(imgui_srv);
		_device->destroy_resource(imgui_tex);

		texture_data->SetTexID(ImTextureID_Invalid);
		texture_data->SetStatus(ImTextureStatus_Destroyed);
	}

	// Remove texture from font atlas, since it was destroyed among all texture above
	atlas->TexList.clear_delete();
	atlas->TexData = nullptr;
	// Also remove from the platform texture list, so that the now deleted font atlas texture data is not accessed again
	_imgui_context->PlatformIO.Textures.clear();

	for (size_t i = 0; i < std::size(_imgui_vertices); ++i)
	{
		_device->destroy_resource(_imgui_indices[i]);
		_imgui_indices[i] = {};
		_imgui_num_indices[i] = 0;
		_device->destroy_resource(_imgui_vertices[i]);
		_imgui_vertices[i] = {};
		_imgui_num_vertices[i] = 0;
	}

	_device->destroy_sampler(_imgui_sampler_state);
	_imgui_sampler_state = {};
	_device->destroy_pipeline(_imgui_pipeline);
	_imgui_pipeline = {};
	_device->destroy_pipeline_layout(_imgui_pipeline_layout);
	_imgui_pipeline_layout = {};

	ultra::destroySplash(_ultra_splash, _device);
}

bool reshade::runtime::open_overlay(bool open, api::input_source source)
{
	if (invoke_addon_event<addon_event::reshade_open_overlay>(this, open, source))
		return false;

	_show_overlay = open;

	if (open)
		_imgui_context->NavInputSource = static_cast<ImGuiInputSource>(source);

	return true;
}

#endif
