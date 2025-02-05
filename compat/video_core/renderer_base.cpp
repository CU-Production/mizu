// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/logging/log.h"
#include "core/frontend/emu_window.h"
#include "common/settings.h"
#include "video_core/renderer_base.h"

namespace VideoCore {

RendererBase::RendererBase(Core::Frontend::EmuWindow& window,
                           std::unique_ptr<Core::Frontend::GraphicsContext> context)
    : render_window{window}, context{std::move(context)} {
    RefreshBaseSettings();
}

RendererBase::~RendererBase() = default;

void RendererBase::RefreshBaseSettings() {
    UpdateCurrentFramebufferLayout();

    renderer_settings.use_framelimiter = Settings::values.use_speed_limit.GetValue();
    renderer_settings.set_background_color = true;
}

void RendererBase::UpdateCurrentFramebufferLayout() {
    const Layout::FramebufferLayout& layout = render_window.GetFramebufferLayout();

    render_window.UpdateCurrentFramebufferLayout(layout.width, layout.height);
}

void RendererBase::RequestScreenshot(void* data, std::function<void(bool)> callback,
                                     const Layout::FramebufferLayout& layout) {
    if (renderer_settings.screenshot_requested) {
        LOG_ERROR(Render, "A screenshot is already requested or in progress, ignoring the request");
        return;
    }
    renderer_settings.screenshot_bits = data;
    renderer_settings.screenshot_complete_callback = std::move(callback);
    renderer_settings.screenshot_framebuffer_layout = layout;
    renderer_settings.screenshot_requested = true;
}

} // namespace VideoCore
