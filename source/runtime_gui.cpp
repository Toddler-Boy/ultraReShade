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
#include "ultra_home.hpp"
#include "ultra_ui_font.hpp"
#include "ultra_ui_layout.hpp"
#include "ultra_ui_shaders.hpp"
#include "ultra_ui_window.hpp"
#include <cmath> // std::abs
#include <cstdlib> // std::strtol
#include <cstring> // std::memcmp, std::memcpy
#include <algorithm> // std::find_if, std::max, std::min, std::rotate, std::search, std::swap
#include <imgui_internal.h>

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
	imgui_io.ConfigFlags = ImGuiConfigFlags_None;
	imgui_io.BackendFlags = ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

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
	const auto face = ::ultra::ui::font::faceData();
	ImFontConfig face_cfg;
	face_cfg.FontDataOwnedByAtlas = false;
	atlas->AddFontFromMemoryTTF(const_cast<unsigned char *>(face.data()), static_cast<int>(face.size()), 0.0f, &face_cfg);
	ImGui::SetCurrentContext(backup_context);

	_rebuild_font_atlas = false;
}

void reshade::runtime::load_config_gui(const ini_file &config)
{
	config.get("INPUT", "KeyOverlay", _overlay_key_data);
	config.get("OVERLAY", "Position", _overlay_position);
	config.get("OVERLAY", "Size", _overlay_size);
	config.get("OVERLAY", "Tab", _overlay_tab);

}
void reshade::runtime::save_config_gui(ini_file &config) const
{
	save_overlay_window(config);
}
void reshade::runtime::save_overlay_window(ini_file &config) const
{
	const auto fraction = [](float value) { return std::round(std::clamp(value, 0.0f, 1.0f) * 1e6f) / 1e6f; };

	config.set("OVERLAY", "Position", std::vector<float> { fraction(_overlay_position[0]), fraction(_overlay_position[1]) });
	config.set("OVERLAY", "Size", std::vector<float> { fraction(_overlay_size[0]), fraction(_overlay_size[1]) });
	config.set("OVERLAY", "Tab", _overlay_tab);
}

void reshade::runtime::draw_gui()
{
	assert(_is_initialized);

	bool show_overlay = _show_overlay;
	api::input_source show_overlay_source = _imgui_context->NavInputSource == ImGuiInputSource_Mouse ? api::input_source::mouse : api::input_source::keyboard;

	if (_input != nullptr)
	{
		// Escape closes a drop-down of ours first and the overlay with the next press, never while text is typed
		if (_show_overlay && !_ignore_shortcuts && _input->is_key_pressed(input::key_escape) && _imgui_context->OpenPopupStack.Size == 0)
			show_overlay = false;
		else if (!_ignore_shortcuts && _input->is_key_pressed(_overlay_key_data, _force_shortcut_modifiers) && _imgui_context->ActiveId == 0)
			show_overlay = !_show_overlay;
	}

	if (show_overlay != _show_overlay)
		open_overlay(show_overlay, show_overlay_source);

	// The messages: the screenshot being saved (ReShade's writer, a key press waiting for its frame, the add-on's own files), then saved for
	// two seconds; what failed, red
	std::vector<::ultra::ui::window::Message> messages;
	{
		const bool is_saving = _screenshots_saving > 0 || _screenshot_files_pending > 0 || _should_save_screenshot;
		const auto now = std::chrono::steady_clock::now();
		const std::string file = _last_screenshot_file.filename().u8string();

		if (_was_saving_screenshot && !is_saving && _last_screenshot_save_successful)
			_screenshot_saved_time = now;
		_was_saving_screenshot = is_saving;

		if (!_settings_save_successful)
			messages.push_back({ "Unable to save the settings to " + ultra::userSettingsPath().u8string() + ", the log says why.", ::ultra::ui::theme::colors::danger });
		if (is_saving)
			messages.push_back({ "Saving screenshot " + file + "...", 0 });
		else if (!_last_screenshot_save_successful && (_last_present_time - _last_screenshot_time) < std::chrono::seconds(5))
			messages.push_back({ _screenshot_directory_creation_successful ? std::string("Unable to save the screenshot because of an internal error (the drive may be full).")
				: "Unable to save the screenshot because its folder could not be created: " + _screenshot_path.u8string(), ::ultra::ui::theme::colors::danger });
		else if (_screenshot_saved_time != std::chrono::steady_clock::time_point() && now - _screenshot_saved_time < std::chrono::seconds(2))
			messages.push_back({ "Screenshot saved " + file, 0 });
	}
	const bool show_message_window = !messages.empty();

	_ignore_shortcuts = false;

	if (!show_message_window && !_show_overlay
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

	// The look's scale follows the display, measured again at each frame; ImGui's own font (its text fields) goes with it
	::ultra::ui::beginFrame(this);
	::ultra::ui::window::applyStyle();

	if (_input != nullptr)
	{
		imgui_io.MouseDrawCursor = false;

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

	ImGui::NewFrame();

	// Reset input source to mouse when the cursor is moved
	if (_input != nullptr && (_input->mouse_movement_delta_x() != 0 || _input->mouse_movement_delta_y() != 0))
		_imgui_context->NavInputSource = ImGuiInputSource_Mouse;

	ultra::drawSplash(_ultra_splash, { _device, ImGui::GetBackgroundDrawList(), imgui_io.DisplaySize.x, imgui_io.DisplaySize.y, imgui_io.DeltaTime, _effects.size(), _reload_remaining_effects.load(), is_loading(), _show_overlay });

	if (_show_overlay)
		draw_overlay_window();
	else
		::ultra::ui::forgetMouseOver();


	// Disable keyboard shortcuts while typing into input boxes
	_ignore_shortcuts |= ImGui::IsAnyItemActive();

	::ultra::ui::window::drawMessages(messages);
	if (_show_overlay)
		::ultra::ui::window::drawCursor(this);

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
	using Tab = ::ultra::ui::window::Tab;

	// The add-ons' overlays: the first one is ultraGame's panel, the Shaders tab comes after it, then the others
	std::vector<addon_info::overlay_callback> overlays;
	for (const addon_info &info : addon_loaded_info)
		overlays.insert(overlays.end(), info.overlay_callbacks.begin(), info.overlay_callbacks.end());

	std::vector<Tab> tabs;
	const auto add_overlay = [this, &tabs](const addon_info::overlay_callback &widget) {
		tabs.push_back({ widget.title, widget.title, [this, callback = widget.callback]() {
			if (!ultra::hasProcessExitBegun())
				callback(this);
		} });
	};

	if (!overlays.empty())
		add_overlay(overlays.front());
	if (_has_user_shaders)
		tabs.push_back({ "Shaders", "shaders", [this]() { draw_gui_shaders(); } });
	for (size_t i = 1; i < overlays.size(); ++i)
		add_overlay(overlays[i]);

	::ultra::ui::window::State state;
	std::copy_n(_overlay_position, 2, state.position);
	std::copy_n(_overlay_size, 2, state.size);
	state.tab = _overlay_tab;

	const auto result = ::ultra::ui::window::draw(state, tabs);

	std::copy_n(state.position, 2, _overlay_position);
	std::copy_n(state.size, 2, _overlay_size);
	if (result.isMovedOrResized || result.isTabChanged)
	{
		_overlay_tab = state.tab;
		save_overlay_window(global_config());
	}
	if (result.isCloseWanted)
	{
		// Closed as the key closes it: nothing held, so the key opens it again
		ImGui::ClearActiveID();
		open_overlay(false, api::input_source::mouse);
	}
}

void reshade::runtime::draw_gui_shaders()
{
	ultra::drawShaders(*this);
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
