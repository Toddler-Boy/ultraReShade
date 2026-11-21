/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "runtime.hpp"
#include "runtime_manager.hpp"
#include "ini_file.hpp"
#include <cassert>
#include <mutex>

static std::mutex s_runtime_count_mutex;
static size_t s_runtime_count = 0;

void reshade::create_effect_runtime(api::swapchain *swapchain, api::command_queue *graphics_queue)
{
	if (graphics_queue == nullptr || swapchain->get_private_data<reshade::runtime>() != nullptr)
		return;

	assert((graphics_queue->get_type() & api::command_queue_type::graphics) != 0);

	const ini_file &config = global_config();
	if (config.get("GENERAL", "Disable"))
		return;

	{
		const std::unique_lock<std::mutex> lock(s_runtime_count_mutex);

		if (size_t max_runtimes = std::numeric_limits<size_t>::max();
			config.get("INSTALL", "MaxEffectRuntimes", max_runtimes) &&
			s_runtime_count >= max_runtimes)
			return;

		++s_runtime_count;
	}

	swapchain->create_private_data<reshade::runtime>(swapchain, graphics_queue);
}
void reshade::destroy_effect_runtime(api::swapchain *swapchain)
{
	if (swapchain->get_private_data<reshade::runtime>() != nullptr)
	{
		const std::unique_lock<std::mutex> lock(s_runtime_count_mutex);

		--s_runtime_count;
	}

	swapchain->destroy_private_data<reshade::runtime>();
}

void reshade::init_effect_runtime(api::swapchain *swapchain)
{
	if (const auto runtime = swapchain->get_private_data<reshade::runtime>())
		runtime->on_init();
}
void reshade::reset_effect_runtime(api::swapchain *swapchain)
{
	if (const auto runtime = swapchain->get_private_data<reshade::runtime>())
		runtime->on_reset();
}
void reshade::present_effect_runtime(api::swapchain *swapchain)
{
	if (const auto runtime = swapchain->get_private_data<reshade::runtime>())
		runtime->on_present();
}
