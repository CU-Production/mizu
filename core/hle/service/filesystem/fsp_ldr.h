// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#pragma once

#include "core/hle/service/service.h"

namespace Service::FileSystem {

class FSP_LDR final : public ServiceFramework<FSP_LDR> {
public:
    explicit FSP_LDR();
    ~FSP_LDR() override;
};

} // namespace Service::FileSystem
