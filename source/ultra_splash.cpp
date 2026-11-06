/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if RESHADE_GUI

#include "ultra_splash.hpp"
#include "dll_log.hpp"
#include "dll_resources.hpp"
#include "reshade_api_device.hpp"
#include <imgui.h>
#include <stb_image.h>
#include <algorithm>
#include <atomic>
#include <chrono>

namespace
{
	// Start logo: total time in seconds, then shares of that time, of the logo width and of the screen height
	constexpr float logo_duration = 1.5f;
	constexpr float logo_hold = 0.3f;
	constexpr float logo_edge = 0.25f;
	constexpr float logo_height = 0.03f;
	constexpr unsigned char logo_tint[3] = { 0xff, 0x14, 0x93 };
	constexpr int logo_strips = 48;

	// Top-left corner of both the logo and the ring, as shares of the screen width and height
	constexpr float anchor_x = 0.03f;
	constexpr float anchor_y = 0.04f;

	// Compile ring: sizes as shares of the screen height (24 and 2 pixels at 1080p), fades in seconds
	constexpr float ring_diameter = 24.0f / 1080.0f;
	constexpr float ring_thickness = 2.0f / 1080.0f;
	constexpr float ring_track_alpha = 0.15f;
	constexpr float ring_arc_alpha = 0.7f;
	constexpr float ring_fade_in = 0.2f;
	constexpr float ring_fade_out = 0.3f;

	// A loading hitch at game start would otherwise play the whole logo within one frame
	constexpr float max_frame_step = 1.0f / 20.0f;
	// A swap chain that stops presenting while showing the logo must not hold back every other one's ring
	constexpr std::chrono::milliseconds logo_owner_timeout { 500 };

	constexpr float pi = 3.14159265f;

	// The logo plays once per process; a runtime destroyed or gone quiet while showing it hands the rest to the next one
	std::atomic<bool> s_logo_done { false };
	std::atomic<const reshade::ultra::splash_state *> s_logo_owner { nullptr };
	std::atomic<std::chrono::steady_clock::rep> s_logo_owner_seen { 0 };
	std::atomic<float> s_logo_elapsed { 0.0f };

	[[nodiscard]] float smoothstep(const float edge0, const float edge1, const float x)
	{
		const auto t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	[[nodiscard]] float sweep_front(const float phase)
	{
		return -logo_edge + std::clamp(phase, 0.0f, 1.0f) * (1.0f + 2.0f * logo_edge);
	}

	[[nodiscard]] float front_band(const float front, const float x)
	{
		const auto band = smoothstep(front - logo_edge, front - logo_edge * 0.5f, x) * (1.0f - smoothstep(front - logo_edge * 0.5f, front, x)) * 4.0f;
		return std::min(band, 1.0f);
	}

	[[nodiscard]] ImU32 white(const float alpha)
	{
		return IM_COL32(255, 255, 255, static_cast<int>(alpha * 255.0f + 0.5f));
	}

	[[nodiscard]] ImU32 logo_color(const float band, const float alpha)
	{
		const auto channel = [band](const unsigned char tint) { return static_cast<int>(255.0f + (tint - 255.0f) * band + 0.5f); };
		return IM_COL32(channel(logo_tint[0]), channel(logo_tint[1]), channel(logo_tint[2]), static_cast<int>(alpha * 255.0f + 0.5f));
	}

	[[nodiscard]] std::chrono::steady_clock::rep now_ticks()
	{
		return std::chrono::steady_clock::now().time_since_epoch().count();
	}

	[[nodiscard]] bool is_logo_owner_gone(const std::chrono::steady_clock::rep now)
	{
		return now - s_logo_owner_seen.load() > std::chrono::duration_cast<std::chrono::steady_clock::duration>(logo_owner_timeout).count();
	}

	[[nodiscard]] bool claim_logo(const reshade::ultra::splash_state &state)
	{
		if (s_logo_done)
			return false;

		const auto now = now_ticks();
		const reshade::ultra::splash_state *expected = nullptr;
		const auto is_owner = s_logo_owner.compare_exchange_strong(expected, &state) || expected == &state ||
			(is_logo_owner_gone(now) && s_logo_owner.compare_exchange_strong(expected, &state));
		if (is_owner)
			s_logo_owner_seen = now;
		return is_owner;
	}

	[[nodiscard]] bool is_logo_pending_for(const reshade::ultra::splash_state &state)
	{
		if (s_logo_done)
			return false;

		const auto owner = s_logo_owner.load();
		return owner == nullptr || owner == &state || is_logo_owner_gone(now_ticks());
	}

	[[nodiscard]] bool load_logo(reshade::ultra::splash_state &state, reshade::api::device *const device)
	{
		if (state.logo_view != 0)
			return true;

		const auto png = reshade::resources::load_data_resource(IDB_ULTRA_LOGO);
		if (png.data == nullptr || png.data_size == 0)
		{
			reshade::log::message(reshade::log::level::error, "Failed to find the ultraReShade logo in the DLL resources.");
			return false;
		}

		int width = 0, height = 0, channels = 0;
		stbi_uc *const pixels = stbi_load_from_memory(static_cast<const stbi_uc *>(png.data), static_cast<int>(png.data_size), &width, &height, &channels, STBI_rgb_alpha);
		if (pixels == nullptr)
		{
			reshade::log::message(reshade::log::level::error, "Failed to decode the ultraReShade logo (%s).", stbi_failure_reason());
			return false;
		}

		const auto row_pitch = static_cast<uint32_t>(width) * 4;
		const reshade::api::subresource_data initial_data = { pixels, row_pitch, row_pitch * static_cast<uint32_t>(height) };
		const auto is_created = device->create_resource(
			reshade::api::resource_desc(static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1, 1, reshade::api::format::r8g8b8a8_unorm, 1, reshade::api::memory_heap::default_, reshade::api::resource_usage::shader_resource | reshade::api::resource_usage::copy_dest),
			&initial_data, reshade::api::resource_usage::shader_resource, &state.logo_texture);
		stbi_image_free(pixels);

		if (!is_created)
		{
			reshade::log::message(reshade::log::level::error, "Failed to create the ultraReShade logo texture.");
			state.logo_texture = {};
			return false;
		}

		if (!device->create_resource_view(state.logo_texture, reshade::api::resource_usage::shader_resource, reshade::api::resource_view_desc(reshade::api::format::r8g8b8a8_unorm), &state.logo_view))
		{
			reshade::log::message(reshade::log::level::error, "Failed to create the ultraReShade logo texture view.");
			device->destroy_resource(state.logo_texture);
			state.logo_texture = {};
			state.logo_view = {};
			return false;
		}

		state.logo_width = static_cast<float>(width);
		state.logo_height = static_cast<float>(height);
		return true;
	}

	// Vertical strips with per-vertex colour carry the sweeping edges; the tint is exact for white artwork
	void draw_logo(const reshade::ultra::splash_state &state, const reshade::ultra::splash_frame &frame, const float time)
	{
		const auto height = frame.height * logo_height;
		const auto width = height * state.logo_width / state.logo_height;
		const auto left = frame.width * anchor_x;
		const auto top = frame.height * anchor_y;

		const auto sweep = (1.0f - logo_hold) * 0.5f;
		const auto front_in = sweep_front(time / sweep);
		const auto front_out = sweep_front((time - sweep - logo_hold) / sweep);

		ImDrawList *const draw_list = frame.draw_list;
		draw_list->PushTexture(ImTextureRef(state.logo_view.handle));
		draw_list->PrimReserve(logo_strips * 6, (logo_strips + 1) * 2);

		const auto first = static_cast<int>(draw_list->_VtxCurrentIdx);
		for (int i = 0; i <= logo_strips; ++i)
		{
			const auto u = static_cast<float>(i) / logo_strips;
			const auto alpha = (1.0f - smoothstep(front_in - logo_edge, front_in, u)) * smoothstep(front_out - logo_edge, front_out, u);
			const auto color = logo_color(std::max(front_band(front_in, u), front_band(front_out, u)), alpha);
			const auto x = left + u * width;

			draw_list->PrimWriteVtx(ImVec2(x, top), ImVec2(u, 0.0f), color);
			draw_list->PrimWriteVtx(ImVec2(x, top + height), ImVec2(u, 1.0f), color);
		}
		for (int i = 0; i < logo_strips; ++i)
		{
			const auto v = first + i * 2;
			draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(v));
			draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(v + 2));
			draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(v + 3));
			draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(v));
			draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(v + 3));
			draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(v + 1));
		}

		draw_list->PopTexture();
	}

	void draw_ring(ImDrawList *const draw_list, const ImVec2 center, const float screen_height, const float progress, const float alpha)
	{
		const auto thickness = std::max(1.0f, screen_height * ring_thickness);
		const auto radius = (screen_height * ring_diameter - thickness) * 0.5f;

		draw_list->AddCircle(center, radius, white(ring_track_alpha * alpha), 0, thickness);

		if (progress > 0.0f)
		{
			const auto twelve_o_clock = -0.5f * pi;
			draw_list->PathArcTo(center, radius, twelve_o_clock, twelve_o_clock + 2.0f * pi * progress);
			draw_list->PathStroke(white(ring_arc_alpha * alpha), ImDrawFlags_None, thickness);
		}
	}

	void advance_logo(reshade::ultra::splash_state &state, const reshade::ultra::splash_frame &frame, const float step)
	{
		if (!claim_logo(state))
			return;

		if (!load_logo(state, frame.device))
		{
			s_logo_done = true;
			return;
		}

		const auto elapsed = s_logo_elapsed.load();
		if (elapsed >= logo_duration)
		{
			s_logo_done = true;
			return;
		}

		draw_logo(state, frame, elapsed / logo_duration);
		s_logo_elapsed = elapsed + step;
	}

	void advance_ring(reshade::ultra::splash_state &state, const reshade::ultra::splash_frame &frame, const float step)
	{
		const auto is_ring_wanted = frame.is_loading && !frame.is_overlay_open && s_logo_done;
		state.ring_alpha = is_ring_wanted ?
			std::min(1.0f, state.ring_alpha + step / ring_fade_in) :
			std::max(0.0f, state.ring_alpha - step / ring_fade_out);
		state.ring_progress = frame.is_loading ? reshade::ultra::compile_progress(frame.effect_count, frame.remaining_effects) : 1.0f;

		if (state.ring_alpha <= 0.0f)
			return;

		const auto radius = frame.height * ring_diameter * 0.5f;
		const ImVec2 center(frame.width * anchor_x + radius, frame.height * anchor_y + radius);
		draw_ring(frame.draw_list, center, frame.height, state.ring_progress, state.ring_alpha);
	}
}

bool reshade::ultra::wants_splash_frame(const splash_state &state, const bool is_loading)
{
	return is_loading || state.ring_alpha > 0.0f || is_logo_pending_for(state);
}

void reshade::ultra::draw_splash(splash_state &state, const splash_frame &frame)
{
	const auto step = std::clamp(frame.delta_time, 0.0f, max_frame_step);
	advance_logo(state, frame, step);
	advance_ring(state, frame, step);
}

void reshade::ultra::destroy_splash(splash_state &state, api::device *const device)
{
	const splash_state *expected = &state;
	s_logo_owner.compare_exchange_strong(expected, nullptr);

	device->destroy_resource_view(state.logo_view);
	device->destroy_resource(state.logo_texture);
	state = splash_state();
}

float reshade::ultra::compile_progress(const size_t effect_count, const size_t remaining_effects)
{
	if (effect_count == 0 || remaining_effects > effect_count)
		return 1.0f;

	return static_cast<float>(effect_count - remaining_effects) / static_cast<float>(effect_count);
}

void reshade::ultra::draw_window_progress_ring(const float progress)
{
	const auto pos = ImGui::GetWindowPos();
	const auto size = ImGui::GetWindowSize();
	draw_ring(ImGui::GetWindowDrawList(), ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f), ImGui::GetIO().DisplaySize.y, progress, 1.0f);
}

#endif
