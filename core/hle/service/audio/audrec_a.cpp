// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#include "core/hle/service/audio/audrec_a.h"

namespace Service::Audio {

AudRecA::AudRecA() : ServiceFramework{"audrec:a"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "RequestSuspend"},
        {1, nullptr, "RequestResume"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

AudRecA::~AudRecA() = default;

} // namespace Service::Audio
