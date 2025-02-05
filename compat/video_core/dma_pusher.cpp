// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#include "common/cityhash.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"

namespace Tegra {

DmaPusher::DmaPusher(GPU& gpu_) : gpu{gpu_} {}

DmaPusher::~DmaPusher() = default;

MICROPROFILE_DEFINE(DispatchCalls, "GPU", "Execute command buffer", MP_RGB(128, 128, 192));

void DmaPusher::DispatchCalls() {
    MICROPROFILE_SCOPE(DispatchCalls);

    gpu.Maxwell3D().OnMemoryWrite();

    dma_pushbuffer_subindex = 0;

    while (Step());

    gpu.FlushCommands();
}

bool DmaPusher::Step() {
    if (!ib_enable || dma_pushbuffer.empty()) {
        // pushbuffer empty and IB empty or nonexistent - nothing to do
        return false;
    }

    CommandList& command_list{dma_pushbuffer.front()};

    ASSERT_OR_EXECUTE(
        command_list.command_lists.size() || command_list.prefetch_command_list.size(), {
            // Somehow the command_list is empty, in order to avoid a crash
            // We ignore it and assume its size is 0.
            dma_pushbuffer.pop();
            dma_pushbuffer_subindex = 0;
            return true;
        });

    if (command_list.prefetch_command_list.size()) {
        // Prefetched command list from nvdrv, used for things like synchronization
        command_headers = std::move(command_list.prefetch_command_list);
        dma_pushbuffer.pop();
    } else {
        const CommandListHeader command_list_header{
            command_list.command_lists[dma_pushbuffer_subindex++]};
        const GPUVAddr dma_get = command_list_header.addr;

        if (dma_pushbuffer_subindex >= command_list.command_lists.size()) {
            // We've gone through the current list, remove it from the queue
            dma_pushbuffer.pop();
            dma_pushbuffer_subindex = 0;
        }

        if (command_list_header.size == 0) {
            return true;
        }

        // Push buffer non-empty, read a word
        command_headers.resize(command_list_header.size);
        if (Settings::IsGPULevelHigh()) {
            gpu.MemoryManager().ReadBlock(dma_get, command_headers.data(),
                                          command_list_header.size * sizeof(u32));
        } else {
            gpu.MemoryManager().ReadBlockUnsafe(dma_get, command_headers.data(),
                                                command_list_header.size * sizeof(u32));
        }
    }
    for (std::size_t index = 0; index < command_headers.size();) {
        const CommandHeader& command_header = command_headers[index];

        if (dma_state.method_count) {
            // Data word of methods command
            CallMethod(command_header.argument);

            if (!dma_state.non_incrementing) {
                dma_state.method++;
            }

            if (dma_increment_once) {
                dma_state.non_incrementing = true;
            }

            dma_state.method_count--;
        } else {
            // No command active - this is the first word of a new one
            switch (command_header.mode) {
            case SubmissionMode::Increasing:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = false;
                break;
            case SubmissionMode::NonIncreasing:
                SetState(command_header);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::Inline:
                dma_state.method = command_header.method;
                dma_state.subchannel = command_header.subchannel;
                CallMethod(command_header.arg_count);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::IncreaseOnce:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = true;
                break;
            default:
                break;
            }
        }
        index++;
    }

    return true;
}

void DmaPusher::SetState(const CommandHeader& command_header) {
    dma_state.method = command_header.method;
    dma_state.subchannel = command_header.subchannel;
    dma_state.method_count = command_header.method_count;
}

void DmaPusher::CallMethod(u32 argument) const {
    gpu.CallMethod({dma_state.method, argument, dma_state.subchannel, dma_state.method_count});
}

} // namespace Tegra
