// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#pragma once

#include "common/common_types.h"
#include "common/dynamic_library.h"
#include "core/frontend/emu_window.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

/**
 * Create a Vulkan instance
 *
 * @param library            Dynamic library to load the Vulkan instance from
 * @param dld                Dispatch table to load function pointers into
 * @param required_version   Required Vulkan version (for example, VK_API_VERSION_1_1)
 * @param window_type        Window system type's enabled extension
 * @param enable_debug_utils Whether to enable VK_EXT_debug_utils_extension_name or not
 * @param enable_layers      Whether to enable Vulkan validation layers or not
 *
 * @return A new Vulkan instance
 * @throw vk::Exception on failure
 */
[[nodiscard]] vk::Instance CreateInstance(
    const Common::DynamicLibrary& library, vk::InstanceDispatch& dld, u32 required_version,
    Core::Frontend::WindowSystemType window_type = Core::Frontend::WindowSystemType::Headless,
    bool enable_debug_utils = false, bool enable_layers = false);

} // namespace Vulkan
