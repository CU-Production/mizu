// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#pragma once

#include "core/hle/service/acc/acc.h"

namespace Service::Account {

class ACC_U1 final : public Module::Interface {
public:
    explicit ACC_U1(std::shared_ptr<Module> module_,
                    std::shared_ptr<Shared<ProfileManager>> profile_manager_);
    ~ACC_U1() override;
};

} // namespace Service::Account
