// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#pragma once

#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <ctime>

#include "common/common_types.h"
#include "core/hle/service/kernel_helpers.h"

namespace Service::Nvidia {
class Module;
} // namespace Service::Nvidia

namespace Service::VI {
class Display;
class Layer;
} // namespace Service::VI

namespace Service::NVFlinger {

class BufferQueue;

class NVFlinger final {
public:
    explicit NVFlinger();
    ~NVFlinger();

    /// Sets the NVDrv module instance to use to send buffers to the GPU.
    void SetNVDrvInstance(std::shared_ptr<Shared<Nvidia::Module>> instance);

    /// Opens the specified display and returns the ID.
    ///
    /// If an invalid display name is provided, then an empty optional is returned.
    [[nodiscard]] std::optional<u64> OpenDisplay(std::string_view name);

    /// Creates a layer on the specified display and returns the layer ID.
    ///
    /// If an invalid display ID is specified, then an empty optional is returned.
    [[nodiscard]] std::optional<u64> CreateLayer(u64 display_id, ::pid_t pid);

    /// Closes a layer on all displays for the given layer ID.
    void CloseLayer(u64 layer_id);

    /// Closes any layers for the specified session (by requester PID).
    void CloseSessionLayers(::pid_t pid);

    /// Finds the buffer queue ID of the specified layer in the specified display.
    ///
    /// If an invalid display ID or layer ID is provided, then an empty optional is returned.
    [[nodiscard]] std::optional<u32> FindBufferQueueId(u64 display_id, u64 layer_id, ::pid_t pid);

    /// Gets the vsync event for the specified display.
    ///
    /// If an invalid display ID is provided, then nullptr is returned.
    [[nodiscard]] int FindVsyncEvent(u64 display_id) const;

    /// Obtains a buffer queue identified by the ID.
    [[nodiscard]] BufferQueue* FindBufferQueue(u32 id) const;

    /// Performs a composition request to the emulated nvidia GPU and triggers the vsync events when
    /// finished.
    void Compose();

    [[nodiscard]] s64 GetNextTicks() const;

private:
    [[nodiscard]] std::unique_lock<std::mutex> Lock() const {
        return std::unique_lock{*guard};
    }

    /// Finds the display identified by the specified ID.
    [[nodiscard]] VI::Display* FindDisplay(u64 display_id);

    /// Finds the display identified by the specified ID.
    [[nodiscard]] const VI::Display* FindDisplay(u64 display_id) const;

    /// Finds the layer identified by the specified ID in the desired display.
    [[nodiscard]] VI::Layer* FindLayer(u64 display_id, u64 layer_id);

    /// Finds the layer identified by the specified ID in the desired display.
    [[nodiscard]] const VI::Layer* FindLayer(u64 display_id, u64 layer_id) const;

    /// Finds the layer identified by the specified ID in the desired display,
    /// or creates the layer if it is not found.
    /// To be used when the system expects the specified ID to already exist.
    [[nodiscard]] VI::Layer* FindOrCreateLayer(u64 display_id, u64 layer_id, ::pid_t pid);

    /// Creates a layer with the specified layer ID in the desired display.
    void CreateLayerAtId(VI::Display& display, u64 layer_id, ::pid_t pid);

    void SplitVSync(std::stop_token stop_token);

    std::shared_ptr<Shared<Nvidia::Module>> nvdrv;

    std::list<VI::Display> displays;
    std::vector<std::unique_ptr<BufferQueue>> buffer_queues;

    /// Id to use for the next layer that is created, this counter is shared among all displays.
    u64 next_layer_id = 1;
    /// Id to use for the next buffer queue that is created, this counter is shared among all
    /// layers.
    u32 next_buffer_queue_id = 1;

    u32 swap_interval = 1;

    /// Event that handles screen composition.
    ::timer_t composition_event;

    std::shared_ptr<std::mutex> guard;

    std::jthread vsync_thread;
};

} // namespace Service::NVFlinger
