// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#include <algorithm>
#include <cstring>
#include <vector>
#include <sys/mman.h>
#include <fcntl.h>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/system_archive/system_archive.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/physical_memory.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/ns/pl_u.h"

namespace Service::NS {

struct FontRegion {
    u32 offset;
    u32 size;
};

// The below data is specific to shared font data dumped from Switch on f/w 2.2
// Virtual address and offsets/sizes likely will vary by dump
[[maybe_unused]] constexpr VAddr SHARED_FONT_MEM_VADDR{0x00000009d3016000ULL};
constexpr u32 EXPECTED_RESULT{0x7f9a0218}; // What we expect the decrypted bfttf first 4 bytes to be
constexpr u32 EXPECTED_MAGIC{0x36f81a1e};  // What we expect the encrypted bfttf first 4 bytes to be
constexpr u64 SHARED_FONT_MEM_SIZE{0x1100000};
constexpr FontRegion EMPTY_REGION{0, 0};

enum class LoadState : u32 {
    Loading = 0,
    Done = 1,
};

static void DecryptSharedFont(const std::vector<u32>& input, Kernel::PhysicalMemory& output,
                              std::size_t& offset) {
    ASSERT_MSG(offset + (input.size() * sizeof(u32)) < SHARED_FONT_MEM_SIZE,
               "Shared fonts exceeds 17mb!");
    ASSERT_MSG(input[0] == EXPECTED_MAGIC, "Failed to derive key, unexpected magic number");

    const u32 KEY = input[0] ^ EXPECTED_RESULT; // Derive key using an inverse xor
    std::vector<u32> transformed_font(input.size());
    // TODO(ogniK): Figure out a better way to do this
    std::transform(input.begin(), input.end(), transformed_font.begin(),
                   [&KEY](u32 font_data) { return Common::swap32(font_data ^ KEY); });
    transformed_font[1] = Common::swap32(transformed_font[1]) ^ KEY; // "re-encrypt" the size
    std::memcpy(output.data() + offset, transformed_font.data(),
                transformed_font.size() * sizeof(u32));
    offset += transformed_font.size() * sizeof(u32);
}

void DecryptSharedFontToTTF(const std::vector<u32>& input, std::vector<u8>& output) {
    ASSERT_MSG(input[0] == EXPECTED_MAGIC, "Failed to derive key, unexpected magic number");

    if (input.size() < 2) {
        LOG_ERROR(Service_NS, "Input font is empty");
        return;
    }

    const u32 KEY = input[0] ^ EXPECTED_RESULT; // Derive key using an inverse xor
    std::vector<u32> transformed_font(input.size());
    // TODO(ogniK): Figure out a better way to do this
    std::transform(input.begin(), input.end(), transformed_font.begin(),
                   [&KEY](u32 font_data) { return Common::swap32(font_data ^ KEY); });
    std::memcpy(output.data(), transformed_font.data() + 2,
                (transformed_font.size() - 2) * sizeof(u32));
}

void EncryptSharedFont(const std::vector<u32>& input, std::vector<u8>& output,
                       std::size_t& offset) {
    ASSERT_MSG(offset + (input.size() * sizeof(u32)) < SHARED_FONT_MEM_SIZE,
               "Shared fonts exceeds 17mb!");

    const auto key = Common::swap32(EXPECTED_RESULT ^ EXPECTED_MAGIC);
    std::vector<u32> transformed_font(input.size() + 2);
    transformed_font[0] = Common::swap32(EXPECTED_MAGIC);
    transformed_font[1] = Common::swap32(static_cast<u32>(input.size() * sizeof(u32))) ^ key;
    std::transform(input.begin(), input.end(), transformed_font.begin() + 2,
                   [key](u32 in) { return in ^ key; });
    std::memcpy(output.data() + offset, transformed_font.data(),
                transformed_font.size() * sizeof(u32));
    offset += transformed_font.size() * sizeof(u32);
}

// Helper function to make BuildSharedFontsRawRegions a bit nicer
static u32 GetU32Swapped(const u8* data) {
    u32 value;
    std::memcpy(&value, data, sizeof(value));
    return Common::swap32(value);
}

struct PL_U::Impl {
    const FontRegion& GetSharedFontRegion(std::size_t index) const {
        if (index >= shared_font_regions.size() || shared_font_regions.empty()) {
            // No font fallback
            return EMPTY_REGION;
        }
        return shared_font_regions.at(index);
    }

    void BuildSharedFontsRawRegions(const Kernel::PhysicalMemory& input) {
        // As we can derive the xor key we can just populate the offsets
        // based on the shared memory dump
        unsigned cur_offset = 0;

        for (std::size_t i = 0; i < SHARED_FONTS.size(); i++) {
            // Out of shared fonts/invalid font
            if (GetU32Swapped(input.data() + cur_offset) != EXPECTED_RESULT) {
                break;
            }

            // Derive key withing inverse xor
            const u32 KEY = GetU32Swapped(input.data() + cur_offset) ^ EXPECTED_MAGIC;
            const u32 SIZE = GetU32Swapped(input.data() + cur_offset + 4) ^ KEY;
            shared_font_regions.push_back(FontRegion{cur_offset + 8, SIZE});
            cur_offset += SIZE + 8;
        }
    }

    /// Backing memory for the shared font data
    std::shared_ptr<Kernel::PhysicalMemory> shared_font;

    // Automatically populated based on shared_fonts dump or system archives.
    std::vector<FontRegion> shared_font_regions;
};

PL_U::PL_U()
    : ServiceFramework{"pl:u"}, impl{std::make_unique<Impl>()},
      shared_mem{nullptr, shared_mem_deleter} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &PL_U::RequestLoad, "RequestLoad"},
        {1, &PL_U::GetLoadState, "GetLoadState"},
        {2, &PL_U::GetSize, "GetSize"},
        {3, &PL_U::GetSharedMemoryAddressOffset, "GetSharedMemoryAddressOffset"},
        {4, &PL_U::GetSharedMemoryNativeHandle, "GetSharedMemoryNativeHandle"},
        {5, &PL_U::GetSharedFontInOrderOfPriority, "GetSharedFontInOrderOfPriority"},
        {6, nullptr, "GetSharedFontInOrderOfPriorityForSystem"},
        {100, nullptr, "RequestApplicationFunctionAuthorization"},
        {101, nullptr, "RequestApplicationFunctionAuthorizationByProcessId"},
        {102, nullptr, "RequestApplicationFunctionAuthorizationByApplicationId"},
        {103, nullptr, "RefreshApplicationFunctionBlackListDebugRecord"},
        {104, nullptr, "RequestApplicationFunctionAuthorizationByProgramId"},
        {105, nullptr, "GetFunctionBlackListSystemVersionToAuthorize"},
        {106, nullptr, "GetFunctionBlackListVersion"},
        {1000, nullptr, "LoadNgWordDataForPlatformRegionChina"},
        {1001, nullptr, "GetNgWordDataSizeForPlatformRegionChina"},
    };
    // clang-format on
    RegisterHandlers(functions);

    shared_mem_fd = ::memfd_create("mizu_pl_u", MFD_ALLOW_SEALING);
    if (shared_mem_fd == -1) {
        LOG_CRITICAL(Service_HID, "memfd_create failed: {}", ::strerror(errno));
    } else {
        if (::ftruncate(shared_mem_fd, shared_mem_size) == -1 ||
            ::fcntl(shared_mem_fd, F_ADD_SEALS, F_SEAL_SHRINK) == -1) {
            LOG_CRITICAL(Service_HID, "memfd setup failed: {}", ::strerror(errno));
        } else {
            u8 *shared_mapping = static_cast<u8 *>(::mmap(NULL, shared_mem_size, PROT_READ | PROT_WRITE,
                                                          MAP_SHARED, shared_mem_fd, 0));
            if (shared_mapping == MAP_FAILED) {
                LOG_CRITICAL(Service_HID, "mmap failed: {}", ::strerror(errno));
            } else {
                shared_mem.reset(shared_mapping);
            }
        }
    }

    // Attempt to load shared font data from disk
    const auto* nand = SharedReader(filesystem_controller)->GetSystemNANDContents();
    std::size_t offset = 0;
    // Rebuild shared fonts from data ncas or synthesize

    impl->shared_font = std::make_shared<Kernel::PhysicalMemory>(SHARED_FONT_MEM_SIZE);
    for (auto font : SHARED_FONTS) {
        FileSys::VirtualFile romfs;
        const auto nca =
            nand->GetEntry(static_cast<u64>(font.first), FileSys::ContentRecordType::Data);
        if (nca) {
            romfs = nca->GetRomFS();
        }

        if (!romfs) {
            romfs = FileSys::SystemArchive::SynthesizeSystemArchive(static_cast<u64>(font.first));
        }

        if (!romfs) {
            LOG_ERROR(Service_NS, "Failed to find or synthesize {:016X}! Skipping", font.first);
            continue;
        }

        const auto extracted_romfs = FileSys::ExtractRomFS(romfs);
        if (!extracted_romfs) {
            LOG_ERROR(Service_NS, "Failed to extract RomFS for {:016X}! Skipping", font.first);
            continue;
        }
        const auto font_fp = extracted_romfs->GetFile(font.second);
        if (!font_fp) {
            LOG_ERROR(Service_NS, "{:016X} has no file \"{}\"! Skipping", font.first, font.second);
            continue;
        }
        std::vector<u32> font_data_u32(font_fp->GetSize() / sizeof(u32));
        font_fp->ReadBytes<u32>(font_data_u32.data(), font_fp->GetSize());
        // We need to be BigEndian as u32s for the xor encryption
        std::transform(font_data_u32.begin(), font_data_u32.end(), font_data_u32.begin(),
                       Common::swap32);
        // Font offset and size do not account for the header
        const FontRegion region{static_cast<u32>(offset + 8),
                                static_cast<u32>((font_data_u32.size() * sizeof(u32)) - 8)};
        DecryptSharedFont(font_data_u32, *impl->shared_font, offset);
        impl->shared_font_regions.push_back(region);
    }
}

PL_U::~PL_U() = default;

void PL_U::RequestLoad(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 shared_font_type{rp.Pop<u32>()};
    // Games don't call this so all fonts should be loaded
    LOG_DEBUG(Service_NS, "called, shared_font_type={}", shared_font_type);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void PL_U::GetLoadState(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 font_id{rp.Pop<u32>()};
    LOG_DEBUG(Service_NS, "called, font_id={}", font_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(static_cast<u32>(LoadState::Done));
}

void PL_U::GetSize(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 font_id{rp.Pop<u32>()};
    LOG_DEBUG(Service_NS, "called, font_id={}", font_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(impl->GetSharedFontRegion(font_id).size);
}

void PL_U::GetSharedMemoryAddressOffset(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 font_id{rp.Pop<u32>()};
    LOG_DEBUG(Service_NS, "called, font_id={}", font_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(impl->GetSharedFontRegion(font_id).offset);
}

void PL_U::GetSharedMemoryNativeHandle(Kernel::HLERequestContext& ctx) {
    // Map backing memory for the font data
    LOG_DEBUG(Service_NS, "called");

    // Create shared font memory object
    std::memcpy(shared_mem.get(), impl->shared_font->data(),
                impl->shared_font->size());

    IPC::ResponseBuilder rb{ctx, 2, 1};
    rb.Push(ResultSuccess);
    rb.PushCopyFds(shared_mem_fd);
}

void PL_U::GetSharedFontInOrderOfPriority(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u64 language_code{rp.Pop<u64>()}; // TODO(ogniK): Find out what this is used for
    LOG_DEBUG(Service_NS, "called, language_code={:X}", language_code);

    IPC::ResponseBuilder rb{ctx, 4};
    std::vector<u32> font_codes;
    std::vector<u32> font_offsets;
    std::vector<u32> font_sizes;

    // TODO(ogniK): Have actual priority order
    for (std::size_t i = 0; i < impl->shared_font_regions.size(); i++) {
        font_codes.push_back(static_cast<u32>(i));
        auto region = impl->GetSharedFontRegion(i);
        font_offsets.push_back(region.offset);
        font_sizes.push_back(region.size);
    }

    // Resize buffers if game requests smaller size output.
    font_codes.resize(
        std::min<std::size_t>(font_codes.size(), ctx.GetWriteBufferSize(0) / sizeof(u32)));
    font_offsets.resize(
        std::min<std::size_t>(font_offsets.size(), ctx.GetWriteBufferSize(1) / sizeof(u32)));
    font_sizes.resize(
        std::min<std::size_t>(font_sizes.size(), ctx.GetWriteBufferSize(2) / sizeof(u32)));

    ctx.WriteBuffer(font_codes, 0);
    ctx.WriteBuffer(font_offsets, 1);
    ctx.WriteBuffer(font_sizes, 2);

    rb.Push(ResultSuccess);
    rb.Push<u8>(static_cast<u8>(LoadState::Done)); // Fonts Loaded
    rb.Push<u32>(static_cast<u32>(font_codes.size()));
}

} // namespace Service::NS
