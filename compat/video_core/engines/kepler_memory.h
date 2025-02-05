// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <cstddef>
#include <vector>
#include "common/bit_field.h"
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "video_core/engines/engine_upload.h"
#include "video_core/gpu.h"

namespace Tegra {
class MemoryManager;
}

namespace Tegra::Engines {

/**
 * This Engine is known as P2MF. Documentation can be found in:
 * https://github.com/envytools/envytools/blob/master/rnndb/graph/gk104_p2mf.xml
 * https://cgit.freedesktop.org/mesa/mesa/tree/src/gallium/drivers/nouveau/nvc0/nve4_p2mf.xml.h
 */

#define KEPLERMEMORY_REG_INDEX(field_name)                                                         \
    (offsetof(Tegra::Engines::KeplerMemory::Regs, field_name) / sizeof(u32))

class KeplerMemory final {
public:
    KeplerMemory(MemoryManager& memory_manager);
    ~KeplerMemory();

    /// Write the value to the register identified by method.
    void CallMethod(const GPU::MethodCall& method_call);

    struct Regs {
        static constexpr size_t NUM_REGS = 0x7F;

        union {
            struct {
                INSERT_PADDING_WORDS_NOINIT(0x60);

                Upload::Registers upload;

                struct {
                    union {
                        BitField<0, 1, u32> linear;
                    };
                } exec;

                u32 data;

                INSERT_PADDING_WORDS_NOINIT(0x11);
            };
            std::array<u32, NUM_REGS> reg_array;
        };
    } regs{};

private:
    Upload::State upload_state;
    Tegra::GPU& gpu;
};

#define ASSERT_REG_POSITION(field_name, position)                                                  \
    static_assert(offsetof(KeplerMemory::Regs, field_name) == position * 4,                        \
                  "Field " #field_name " has invalid position")

ASSERT_REG_POSITION(upload, 0x60);
ASSERT_REG_POSITION(exec, 0x6C);
ASSERT_REG_POSITION(data, 0x6D);
#undef ASSERT_REG_POSITION

} // namespace Tegra::Engines
