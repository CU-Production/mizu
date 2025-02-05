// Copyright 2020 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#pragma once

#include <map>
#include <vector>
#include "common/common_types.h"
#include "common/swap.h"
#include "core/hle/service/nvdrv/devices/nvdevice.h"

namespace Service::Nvidia {
class SyncpointManager;

namespace Devices {
class nvmap;

class nvhost_nvdec_common : public nvdevice {
public:
    explicit nvhost_nvdec_common(std::shared_ptr<nvmap> nvmap_dev_,
                                 SyncpointManager& syncpoint_manager_);
    ~nvhost_nvdec_common() override;

protected:
    struct IoctlSetNvmapFD {
        s32_le nvmap_fd{};
    };
    static_assert(sizeof(IoctlSetNvmapFD) == 4, "IoctlSetNvmapFD is incorrect size");

    struct IoctlSubmitCommandBuffer {
        u32_le id{};
        u32_le offset{};
        u32_le count{};
    };
    static_assert(sizeof(IoctlSubmitCommandBuffer) == 0xC,
                  "IoctlSubmitCommandBuffer is incorrect size");
    struct IoctlSubmit {
        u32_le cmd_buffer_count{};
        u32_le relocation_count{};
        u32_le syncpoint_count{};
        u32_le fence_count{};
    };
    static_assert(sizeof(IoctlSubmit) == 0x10, "IoctlSubmit has incorrect size");

    struct CommandBuffer {
        s32 memory_id{};
        u32 offset{};
        s32 word_count{};
    };
    static_assert(sizeof(CommandBuffer) == 0xC, "CommandBuffer has incorrect size");

    struct Reloc {
        s32 cmdbuffer_memory{};
        s32 cmdbuffer_offset{};
        s32 target{};
        s32 target_offset{};
    };
    static_assert(sizeof(Reloc) == 0x10, "Reloc has incorrect size");

    struct SyncptIncr {
        u32 id{};
        u32 increments{};
        u32 unk0{};
        u32 unk1{};
        u32 unk2{};
    };
    static_assert(sizeof(SyncptIncr) == 0x14, "SyncptIncr has incorrect size");

    struct IoctlGetSyncpoint {
        // Input
        u32_le param{};
        // Output
        u32_le value{};
    };
    static_assert(sizeof(IoctlGetSyncpoint) == 8, "IocGetIdParams has wrong size");

    struct IoctlGetWaitbase {
        u32_le unknown{}; // seems to be ignored? Nintendo added this
        u32_le value{};
    };
    static_assert(sizeof(IoctlGetWaitbase) == 0x8, "IoctlGetWaitbase is incorrect size");

    struct IoctlMapBuffer {
        u32_le num_entries{};
        u32_le data_address{}; // Ignored by the driver.
        u32_le attach_host_ch_das{};
    };
    static_assert(sizeof(IoctlMapBuffer) == 0x0C, "IoctlMapBuffer is incorrect size");

    struct IocGetIdParams {
        // Input
        u32_le param{};
        // Output
        u32_le value{};
    };
    static_assert(sizeof(IocGetIdParams) == 8, "IocGetIdParams has wrong size");

    // Used for mapping and unmapping command buffers
    struct MapBufferEntry {
        u32_le map_handle{};
        u32_le map_address{};
    };
    static_assert(sizeof(IoctlMapBuffer) == 0x0C, "IoctlMapBuffer is incorrect size");

    /// Ioctl command implementations
    NvResult SetNVMAPfd(const std::vector<u8>& input);
    NvResult Submit(const std::vector<u8>& input, std::vector<u8>& output, Shared<Tegra::GPU>& gpu);
    NvResult GetSyncpoint(const std::vector<u8>& input, std::vector<u8>& output, Shared<Tegra::GPU>& gpu);
    NvResult GetWaitbase(const std::vector<u8>& input, std::vector<u8>& output);
    NvResult MapBuffer(const std::vector<u8>& input, std::vector<u8>& output, Shared<Tegra::GPU>& gpu);
    NvResult UnmapBuffer(const std::vector<u8>& input, std::vector<u8>& output);
    NvResult SetSubmitTimeout(const std::vector<u8>& input, std::vector<u8>& output);

    s32_le nvmap_fd{};
    u32_le submit_timeout{};
    std::shared_ptr<nvmap> nvmap_dev;
    SyncpointManager& syncpoint_manager;
    std::array<u32, MaxSyncPoints> device_syncpoints{};
};
}; // namespace Devices
} // namespace Service::Nvidia
