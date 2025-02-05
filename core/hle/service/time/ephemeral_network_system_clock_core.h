// Copyright 2019 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#pragma once

#include "core/hle/service/time/system_clock_core.h"

namespace Service::Time::Clock {

class EphemeralNetworkSystemClockCore final : public SystemClockCoreLocked<EphemeralNetworkSystemClockCore> {
public:
    explicit EphemeralNetworkSystemClockCore(SteadyClockCore& steady_clock_core_)
        : SystemClockCoreLocked<EphemeralNetworkSystemClockCore>{steady_clock_core_} {}
};

} // namespace Service::Time::Clock
