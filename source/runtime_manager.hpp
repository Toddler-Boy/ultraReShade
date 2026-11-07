/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "reshade_api_device.hpp"
#include <thread>

namespace reshade
{
	void create_effect_runtime(api::swapchain *swapchain, api::command_queue *graphics_queue, bool vr = false);
	void destroy_effect_runtime(api::swapchain *swapchain);

	void init_effect_runtime(api::swapchain *swapchain);
	void reset_effect_runtime(api::swapchain *swapchain);
	void present_effect_runtime(api::swapchain *swapchain);

	// Joins a thread that may need the D3D10/D3D11 device lock this thread entered for present or reset (dxgi_swapchain.cpp)
	void join_without_device_lock(std::thread &thread);

	// Effect creation threads start long device calls only while present and reset do not hold the D3D10/D3D11 device lock:
	// a call that has to wait for the lock holds it afterwards until it returns, and the next present waits for that
	void mark_effect_creation_thread();
	void wait_for_device_lock_release();
}
