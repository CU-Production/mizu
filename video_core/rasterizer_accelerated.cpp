// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#include <atomic>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "core/memory.h"
#include "video_core/rasterizer_accelerated.h"

namespace VideoCore {

using namespace Core::Memory;

RasterizerAccelerated::RasterizerAccelerated(Tegra::GPU& gpu_)
    : RasterizerInterface(gpu_) {}

RasterizerAccelerated::~RasterizerAccelerated() = default;

void RasterizerAccelerated::UpdatePagesCachedCount(VAddr addr, u64 size, int delta) {
    u64 uncache_begin = 0;
    u64 cache_begin = 0;
    u64 uncache_bytes = 0;
    u64 cache_bytes = 0;

    std::atomic_thread_fence(std::memory_order_acquire);
    const u64 page_end = Common::DivCeil(addr + size, PAGE_SIZE);
    for (u64 page = addr >> PAGE_BITS; page != page_end; ++page) {
        std::atomic_uint16_t& count = cached_pages.at(page >> 2).Count(page);

        if (delta > 0) {
            ASSERT_MSG(count.load(std::memory_order::relaxed) < UINT16_MAX, "Count may overflow!");
        } else if (delta < 0) {
            ASSERT_MSG(count.load(std::memory_order::relaxed) > 0, "Count may underflow!");
        } else {
            ASSERT_MSG(false, "Delta must be non-zero!");
        }

        // Adds or subtracts 1, as count is a unsigned 8-bit value
        count.fetch_add(static_cast<u16>(delta), std::memory_order_release);

        // Assume delta is either -1 or 1
        if (count.load(std::memory_order::relaxed) == 0) {
            if (uncache_bytes == 0) {
                uncache_begin = page;
            }
            uncache_bytes += PAGE_SIZE;
        } else if (uncache_bytes > 0) {
            /* cpu_memory.RasterizerMarkRegionCached(uncache_begin << PAGE_BITS, uncache_bytes, false); */
            uncache_bytes = 0;
        }
        if (count.load(std::memory_order::relaxed) == 1 && delta > 0) {
            if (cache_bytes == 0) {
                cache_begin = page;
            }
            cache_bytes += PAGE_SIZE;
        } else if (cache_bytes > 0) {
            /* cpu_memory.RasterizerMarkRegionCached(cache_begin << PAGE_BITS, cache_bytes, true); */
            cache_bytes = 0;
        }
    }
    /* if (uncache_bytes > 0) { */
    /*     cpu_memory.RasterizerMarkRegionCached(uncache_begin << PAGE_BITS, uncache_bytes, false); */
    /* } */
    /* if (cache_bytes > 0) { */
    /*     cpu_memory.RasterizerMarkRegionCached(cache_begin << PAGE_BITS, cache_bytes, true); */
    /* } */
}

} // namespace VideoCore
