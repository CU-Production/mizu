// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#include "core/hle/service/am/idle.h"

namespace Service::AM {

IdleSys::IdleSys() : ServiceFramework{"idle:sys"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "GetAutoPowerDownEvent"},
        {1, nullptr, "IsAutoPowerDownRequested"},
        {2, nullptr, "Unknown2"},
        {3, nullptr, "SetHandlingContext"},
        {4, nullptr, "LoadAndApplySettings"},
        {5, nullptr, "ReportUserIsActive"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IdleSys::~IdleSys() = default;

} // namespace Service::AM
