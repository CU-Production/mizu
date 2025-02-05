// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#include <array>
#include <csignal>
#include <fcntl.h>
#include <sys/mman.h>
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/input.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/hid/errors.h"
#include "core/hle/service/hid/hid.h"
#include "core/hle/service/hid/irs.h"
#include "core/hle/service/hid/xcd.h"
#include "core/memory.h"

#include "core/hle/service/hid/controllers/console_sixaxis.h"
#include "core/hle/service/hid/controllers/controller_base.h"
#include "core/hle/service/hid/controllers/debug_pad.h"
#include "core/hle/service/hid/controllers/gesture.h"
#include "core/hle/service/hid/controllers/keyboard.h"
#include "core/hle/service/hid/controllers/mouse.h"
#include "core/hle/service/hid/controllers/npad.h"
#include "core/hle/service/hid/controllers/stubbed.h"
#include "core/hle/service/hid/controllers/touchscreen.h"
#include "core/hle/service/hid/controllers/xpad.h"

namespace Service::HID {

// Updating period for each HID device.
// HID is polled every 15ms, this value was derived from
// https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering#joy-con-status-data-packet
constexpr auto pad_update_ns = std::chrono::nanoseconds{1000 * 1000};         // (1ms, 1000Hz)
constexpr auto motion_update_ns = std::chrono::nanoseconds{15 * 1000 * 1000}; // (15ms, 66.666Hz)

IAppletResource::IAppletResource()
    : ServiceFramework{"IAppletResource"}, shared_mem{nullptr, shared_mem_deleter} {
    static const FunctionInfo functions[] = {
        {0, &IAppletResource::GetSharedMemoryHandle, "GetSharedMemoryHandle"},
    };
    RegisterHandlers(functions);

    shared_mem_fd = ::memfd_create("mizu_hid", MFD_ALLOW_SEALING);
    if (shared_mem_fd == -1) {
        LOG_CRITICAL(Service_HID, "memfd_create failed: {}", ::strerror(errno));
    } else {
        if (::ftruncate(shared_mem_fd, SHARED_MEMORY_SIZE) == -1 ||
            ::fcntl(shared_mem_fd, F_ADD_SEALS, F_SEAL_SHRINK) == -1) {
            LOG_CRITICAL(Service_HID, "memfd setup failed: {}", ::strerror(errno));
        } else {
            u8 *shared_mapping = static_cast<u8 *>(::mmap(NULL, SHARED_MEMORY_SIZE, PROT_READ | PROT_WRITE,
                                                          MAP_SHARED, shared_mem_fd, 0));
            if (shared_mapping == MAP_FAILED) {
                LOG_CRITICAL(Service_HID, "mmap failed: {}", ::strerror(errno));
            } else {
                shared_mem.reset(shared_mapping);
            }
        }
    }

    MakeController<Controller_DebugPad>(HidController::DebugPad);
    MakeController<Controller_Touchscreen>(HidController::Touchscreen);
    MakeController<Controller_Mouse>(HidController::Mouse);
    MakeController<Controller_Keyboard>(HidController::Keyboard);
    MakeController<Controller_XPad>(HidController::XPad);
    MakeController<Controller_Stubbed>(HidController::HomeButton);
    MakeController<Controller_Stubbed>(HidController::SleepButton);
    MakeController<Controller_Stubbed>(HidController::CaptureButton);
    MakeController<Controller_Stubbed>(HidController::InputDetector);
    MakeController<Controller_Stubbed>(HidController::UniquePad);
    MakeController<Controller_NPad>(HidController::NPad);
    MakeController<Controller_Gesture>(HidController::Gesture);
    MakeController<Controller_ConsoleSixAxis>(HidController::ConsoleSixAxisSensor);

    // Homebrew doesn't try to activate some controllers, so we activate them by default
    GetController<Controller_NPad>(HidController::NPad).WriteLocked()->ActivateController();
    GetController<Controller_Touchscreen>(HidController::Touchscreen).WriteLocked()->ActivateController();

    GetController<Controller_Stubbed>(HidController::HomeButton).WriteLocked()->SetCommonHeaderOffset(0x4C00);
    GetController<Controller_Stubbed>(HidController::SleepButton).WriteLocked()->SetCommonHeaderOffset(0x4E00);
    GetController<Controller_Stubbed>(HidController::CaptureButton).WriteLocked()->SetCommonHeaderOffset(0x5000);
    GetController<Controller_Stubbed>(HidController::InputDetector).WriteLocked()->SetCommonHeaderOffset(0x5200);
    GetController<Controller_Stubbed>(HidController::UniquePad).WriteLocked()->SetCommonHeaderOffset(0x5A00);

    // Register update callbacks
    pad_update_event = KernelHelpers::CreateTimerEvent(
        "HID::UpdatePadCallback",
        this,
        [](::sigval sigev_value) {
            auto iar = static_cast<IAppletResource *>(sigev_value.sival_ptr);
            const auto guard = iar->LockService();
            iar->UpdateControllers();
        });
    motion_update_event = KernelHelpers::CreateTimerEvent(
        "HID::MotionPadCallback",
        this,
        [](::sigval sigev_value) {
            auto iar = static_cast<IAppletResource *>(sigev_value.sival_ptr);
            const auto guard = iar->LockService();
            iar->UpdateMotion();
        });

    KernelHelpers::ScheduleTimerEvent(pad_update_ns, pad_update_event);
    KernelHelpers::ScheduleTimerEvent(motion_update_ns, motion_update_event);

    ReloadInputDevices();
}

void IAppletResource::ActivateController(HidController controller) {
    controllers[static_cast<size_t>(controller)]->ActivateController();
}

void IAppletResource::DeactivateController(HidController controller) {
    controllers[static_cast<size_t>(controller)]->DeactivateController();
}

IAppletResource::~IAppletResource() {
    ASSERT(stop_source.request_stop());
    {
        std::unique_lock lock{done_mtx};
        while (!pad_is_done || !motion_is_done)
            done_cv.wait(lock);
    }
    KernelHelpers::CloseTimerEvent(pad_update_event);
    KernelHelpers::CloseTimerEvent(motion_update_event);
    if (shared_mem_fd != -1) {
        ::close(shared_mem_fd);
    }
}

void IAppletResource::GetSharedMemoryHandle(Kernel::HLERequestContext& ctx) {
    LOG_DEBUG(Service_HID, "called");

    IPC::ResponseBuilder rb{ctx, 2, 1};
    rb.Push(ResultSuccess);
    rb.PushCopyFds(shared_mem_fd);
}

void IAppletResource::UpdateControllers() {
    if (stop_source.stop_requested()) {
        std::unique_lock lock{done_mtx};
        pad_is_done = true;
        done_cv.notify_all();
        return;
    }

    const bool should_reload = Settings::values.is_device_reload_pending.exchange(false);
    for (const auto& controller : controllers) {
        if (should_reload) {
            controller->OnLoadInputDevices();
        }
        controller->OnUpdate(shared_mem.get(), SHARED_MEMORY_SIZE);
    }

    KernelHelpers::ScheduleTimerEvent(pad_update_ns, pad_update_event);
}

void IAppletResource::UpdateMotion() {
    if (stop_source.stop_requested()) {
        std::unique_lock lock{done_mtx};
        motion_is_done = true;
        done_cv.notify_all();
        return;
    }

    controllers[static_cast<size_t>(HidController::NPad)]->OnMotionUpdate(
        shared_mem.get(), SHARED_MEMORY_SIZE);

    KernelHelpers::ScheduleTimerEvent(motion_update_ns, motion_update_event);
}

class IActiveVibrationDeviceList final : public ServiceFramework<IActiveVibrationDeviceList> {
public:
    explicit IActiveVibrationDeviceList(std::shared_ptr<IAppletResource> applet_resource_)
        : ServiceFramework{"IActiveVibrationDeviceList"},
          applet_resource(applet_resource_) {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IActiveVibrationDeviceList::InitializeVibrationDevice, "InitializeVibrationDevice"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void InitializeVibrationDevice(Kernel::HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto vibration_device_handle{rp.PopRaw<Controller_NPad::DeviceHandle>()};

        if (applet_resource != nullptr) {
            applet_resource->GetController<Controller_NPad>(HidController::NPad)
                .WriteLocked()->InitializeVibrationDevice(vibration_device_handle);
        }

        LOG_DEBUG(Service_HID, "called, npad_type={}, npad_id={}, device_index={}",
                  vibration_device_handle.npad_type, vibration_device_handle.npad_id,
                  vibration_device_handle.device_index);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    std::shared_ptr<IAppletResource> applet_resource;
};

std::shared_ptr<IAppletResource> Hid::GetAppletResource() {
    if (*SharedReader(applet_resource) == nullptr) {
        *SharedWriter(applet_resource) = std::make_shared<IAppletResource>();
    }

    return *SharedReader(applet_resource);
}

Hid::Hid()
    : ServiceFramework{"hid"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &Hid::CreateAppletResource, "CreateAppletResource"},
        {1, &Hid::ActivateDebugPad, "ActivateDebugPad"},
        {11, &Hid::ActivateTouchScreen, "ActivateTouchScreen"},
        {21, &Hid::ActivateMouse, "ActivateMouse"},
        {31, &Hid::ActivateKeyboard, "ActivateKeyboard"},
        {32, &Hid::SendKeyboardLockKeyEvent, "SendKeyboardLockKeyEvent"},
        {40, nullptr, "AcquireXpadIdEventHandle"},
        {41, nullptr, "ReleaseXpadIdEventHandle"},
        {51, &Hid::ActivateXpad, "ActivateXpad"},
        {55, &Hid::GetXpadIDs, "GetXpadIds"},
        {56, nullptr, "ActivateJoyXpad"},
        {58, nullptr, "GetJoyXpadLifoHandle"},
        {59, nullptr, "GetJoyXpadIds"},
        {60, &Hid::ActivateSixAxisSensor, "ActivateSixAxisSensor"},
        {61, &Hid::DeactivateSixAxisSensor, "DeactivateSixAxisSensor"},
        {62, nullptr, "GetSixAxisSensorLifoHandle"},
        {63, nullptr, "ActivateJoySixAxisSensor"},
        {64, nullptr, "DeactivateJoySixAxisSensor"},
        {65, nullptr, "GetJoySixAxisSensorLifoHandle"},
        {66, &Hid::StartSixAxisSensor, "StartSixAxisSensor"},
        {67, &Hid::StopSixAxisSensor, "StopSixAxisSensor"},
        {68, nullptr, "IsSixAxisSensorFusionEnabled"},
        {69, &Hid::EnableSixAxisSensorFusion, "EnableSixAxisSensorFusion"},
        {70, &Hid::SetSixAxisSensorFusionParameters, "SetSixAxisSensorFusionParameters"},
        {71, &Hid::GetSixAxisSensorFusionParameters, "GetSixAxisSensorFusionParameters"},
        {72, &Hid::ResetSixAxisSensorFusionParameters, "ResetSixAxisSensorFusionParameters"},
        {73, nullptr, "SetAccelerometerParameters"},
        {74, nullptr, "GetAccelerometerParameters"},
        {75, nullptr, "ResetAccelerometerParameters"},
        {76, nullptr, "SetAccelerometerPlayMode"},
        {77, nullptr, "GetAccelerometerPlayMode"},
        {78, nullptr, "ResetAccelerometerPlayMode"},
        {79, &Hid::SetGyroscopeZeroDriftMode, "SetGyroscopeZeroDriftMode"},
        {80, &Hid::GetGyroscopeZeroDriftMode, "GetGyroscopeZeroDriftMode"},
        {81, &Hid::ResetGyroscopeZeroDriftMode, "ResetGyroscopeZeroDriftMode"},
        {82, &Hid::IsSixAxisSensorAtRest, "IsSixAxisSensorAtRest"},
        {83, &Hid::IsFirmwareUpdateAvailableForSixAxisSensor, "IsFirmwareUpdateAvailableForSixAxisSensor"},
        {84, nullptr, "EnableSixAxisSensorUnalteredPassthrough"},
        {85, nullptr, "IsSixAxisSensorUnalteredPassthroughEnabled"},
        {86, nullptr, "StoreSixAxisSensorCalibrationParameter"},
        {87, nullptr, "LoadSixAxisSensorCalibrationParameter"},
        {88, nullptr, "GetSixAxisSensorIcInformation"},
        {89, nullptr, "ResetIsSixAxisSensorDeviceNewlyAssigned"},
        {91, &Hid::ActivateGesture, "ActivateGesture"},
        {100, &Hid::SetSupportedNpadStyleSet, "SetSupportedNpadStyleSet"},
        {101, &Hid::GetSupportedNpadStyleSet, "GetSupportedNpadStyleSet"},
        {102, &Hid::SetSupportedNpadIdType, "SetSupportedNpadIdType"},
        {103, &Hid::ActivateNpad, "ActivateNpad"},
        {104, &Hid::DeactivateNpad, "DeactivateNpad"},
        {106, &Hid::AcquireNpadStyleSetUpdateEventHandle, "AcquireNpadStyleSetUpdateEventHandle"},
        {107, &Hid::DisconnectNpad, "DisconnectNpad"},
        {108, &Hid::GetPlayerLedPattern, "GetPlayerLedPattern"},
        {109, &Hid::ActivateNpadWithRevision, "ActivateNpadWithRevision"},
        {120, &Hid::SetNpadJoyHoldType, "SetNpadJoyHoldType"},
        {121, &Hid::GetNpadJoyHoldType, "GetNpadJoyHoldType"},
        {122, &Hid::SetNpadJoyAssignmentModeSingleByDefault, "SetNpadJoyAssignmentModeSingleByDefault"},
        {123, &Hid::SetNpadJoyAssignmentModeSingle, "SetNpadJoyAssignmentModeSingle"},
        {124, &Hid::SetNpadJoyAssignmentModeDual, "SetNpadJoyAssignmentModeDual"},
        {125, &Hid::MergeSingleJoyAsDualJoy, "MergeSingleJoyAsDualJoy"},
        {126, &Hid::StartLrAssignmentMode, "StartLrAssignmentMode"},
        {127, &Hid::StopLrAssignmentMode, "StopLrAssignmentMode"},
        {128, &Hid::SetNpadHandheldActivationMode, "SetNpadHandheldActivationMode"},
        {129, &Hid::GetNpadHandheldActivationMode, "GetNpadHandheldActivationMode"},
        {130, &Hid::SwapNpadAssignment, "SwapNpadAssignment"},
        {131, &Hid::IsUnintendedHomeButtonInputProtectionEnabled, "IsUnintendedHomeButtonInputProtectionEnabled"},
        {132, &Hid::EnableUnintendedHomeButtonInputProtection, "EnableUnintendedHomeButtonInputProtection"},
        {133, nullptr, "SetNpadJoyAssignmentModeSingleWithDestination"},
        {134, &Hid::SetNpadAnalogStickUseCenterClamp, "SetNpadAnalogStickUseCenterClamp"},
        {135, nullptr, "SetNpadCaptureButtonAssignment"},
        {136, nullptr, "ClearNpadCaptureButtonAssignment"},
        {200, &Hid::GetVibrationDeviceInfo, "GetVibrationDeviceInfo"},
        {201, &Hid::SendVibrationValue, "SendVibrationValue"},
        {202, &Hid::GetActualVibrationValue, "GetActualVibrationValue"},
        {203, &Hid::CreateActiveVibrationDeviceList, "CreateActiveVibrationDeviceList"},
        {204, &Hid::PermitVibration, "PermitVibration"},
        {205, &Hid::IsVibrationPermitted, "IsVibrationPermitted"},
        {206, &Hid::SendVibrationValues, "SendVibrationValues"},
        {207, &Hid::SendVibrationGcErmCommand, "SendVibrationGcErmCommand"},
        {208, &Hid::GetActualVibrationGcErmCommand, "GetActualVibrationGcErmCommand"},
        {209, &Hid::BeginPermitVibrationSession, "BeginPermitVibrationSession"},
        {210, &Hid::EndPermitVibrationSession, "EndPermitVibrationSession"},
        {211, &Hid::IsVibrationDeviceMounted, "IsVibrationDeviceMounted"},
        {212, nullptr, "SendVibrationValueInBool"},
        {300, &Hid::ActivateConsoleSixAxisSensor, "ActivateConsoleSixAxisSensor"},
        {301, &Hid::StartConsoleSixAxisSensor, "StartConsoleSixAxisSensor"},
        {302, &Hid::StopConsoleSixAxisSensor, "StopConsoleSixAxisSensor"},
        {303, &Hid::ActivateSevenSixAxisSensor, "ActivateSevenSixAxisSensor"},
        {304, &Hid::StartSevenSixAxisSensor, "StartSevenSixAxisSensor"},
        {305, &Hid::StopSevenSixAxisSensor, "StopSevenSixAxisSensor"},
        {306, &Hid::InitializeSevenSixAxisSensor, "InitializeSevenSixAxisSensor"},
        {307, &Hid::FinalizeSevenSixAxisSensor, "FinalizeSevenSixAxisSensor"},
        {308, nullptr, "SetSevenSixAxisSensorFusionStrength"},
        {309, nullptr, "GetSevenSixAxisSensorFusionStrength"},
        {310, &Hid::ResetSevenSixAxisSensorTimestamp, "ResetSevenSixAxisSensorTimestamp"},
        {400, nullptr, "IsUsbFullKeyControllerEnabled"},
        {401, nullptr, "EnableUsbFullKeyController"},
        {402, nullptr, "IsUsbFullKeyControllerConnected"},
        {403, nullptr, "HasBattery"},
        {404, nullptr, "HasLeftRightBattery"},
        {405, nullptr, "GetNpadInterfaceType"},
        {406, nullptr, "GetNpadLeftRightInterfaceType"},
        {407, nullptr, "GetNpadOfHighestBatteryLevel"},
        {408, nullptr, "GetNpadOfHighestBatteryLevelForJoyRight"},
        {500, nullptr, "GetPalmaConnectionHandle"},
        {501, nullptr, "InitializePalma"},
        {502, nullptr, "AcquirePalmaOperationCompleteEvent"},
        {503, nullptr, "GetPalmaOperationInfo"},
        {504, nullptr, "PlayPalmaActivity"},
        {505, nullptr, "SetPalmaFrModeType"},
        {506, nullptr, "ReadPalmaStep"},
        {507, nullptr, "EnablePalmaStep"},
        {508, nullptr, "ResetPalmaStep"},
        {509, nullptr, "ReadPalmaApplicationSection"},
        {510, nullptr, "WritePalmaApplicationSection"},
        {511, nullptr, "ReadPalmaUniqueCode"},
        {512, nullptr, "SetPalmaUniqueCodeInvalid"},
        {513, nullptr, "WritePalmaActivityEntry"},
        {514, nullptr, "WritePalmaRgbLedPatternEntry"},
        {515, nullptr, "WritePalmaWaveEntry"},
        {516, nullptr, "SetPalmaDataBaseIdentificationVersion"},
        {517, nullptr, "GetPalmaDataBaseIdentificationVersion"},
        {518, nullptr, "SuspendPalmaFeature"},
        {519, nullptr, "GetPalmaOperationResult"},
        {520, nullptr, "ReadPalmaPlayLog"},
        {521, nullptr, "ResetPalmaPlayLog"},
        {522, &Hid::SetIsPalmaAllConnectable, "SetIsPalmaAllConnectable"},
        {523, nullptr, "SetIsPalmaPairedConnectable"},
        {524, nullptr, "PairPalma"},
        {525, &Hid::SetPalmaBoostMode, "SetPalmaBoostMode"},
        {526, nullptr, "CancelWritePalmaWaveEntry"},
        {527, nullptr, "EnablePalmaBoostMode"},
        {528, nullptr, "GetPalmaBluetoothAddress"},
        {529, nullptr, "SetDisallowedPalmaConnection"},
        {1000, &Hid::SetNpadCommunicationMode, "SetNpadCommunicationMode"},
        {1001, &Hid::GetNpadCommunicationMode, "GetNpadCommunicationMode"},
        {1002, &Hid::SetTouchScreenConfiguration, "SetTouchScreenConfiguration"},
        {1003, nullptr, "IsFirmwareUpdateNeededForNotification"},
        {2000, nullptr, "ActivateDigitizer"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

Hid::~Hid() = default;

void Hid::CreateAppletResource(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    if (*SharedReader(applet_resource) == nullptr) {
        *SharedWriter(applet_resource) = std::make_shared<IAppletResource>();
    }

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IAppletResource>(*SharedReader(applet_resource));
}

void Hid::ActivateDebugPad(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    (*SharedReader(applet_resource))->ActivateController(HidController::DebugPad);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::ActivateTouchScreen(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    (*SharedReader(applet_resource))->ActivateController(HidController::Touchscreen);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::ActivateMouse(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    (*SharedReader(applet_resource))->ActivateController(HidController::Mouse);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::ActivateKeyboard(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    (*SharedReader(applet_resource))->ActivateController(HidController::Keyboard);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::SendKeyboardLockKeyEvent(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto flags{rp.Pop<u32>()};

    LOG_WARNING(Service_HID, "(STUBBED) called. flags={}", flags);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::ActivateXpad(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 basic_xpad_id;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->ActivateController(HidController::XPad);

    LOG_DEBUG(Service_HID, "called, basic_xpad_id={}, applet_resource_user_id={}",
              parameters.basic_xpad_id, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::GetXpadIDs(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_DEBUG(Service_HID, "(STUBBED) called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(0);
}

void Hid::ActivateSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad).WriteLocked()->SetSixAxisEnabled(true);

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
              parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::DeactivateSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad).WriteLocked()->SetSixAxisEnabled(false);

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
              parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::StartSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad).WriteLocked()->SetSixAxisEnabled(true);

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
              parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::StopSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad).WriteLocked()->SetSixAxisEnabled(false);

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
              parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::EnableSixAxisSensorFusion(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        bool enable_sixaxis_sensor_fusion;
        INSERT_PADDING_BYTES_NOINIT(3);
        Controller_NPad::DeviceHandle sixaxis_handle;
        u64 applet_resource_user_id;
    };
    static_assert(sizeof(Parameters) == 0x10, "Parameters has incorrect size.");

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_WARNING(Service_HID,
                "(STUBBED) called, enable_sixaxis_sensor_fusion={}, npad_type={}, npad_id={}, "
                "device_index={}, applet_resource_user_id={}",
                parameters.enable_sixaxis_sensor_fusion, parameters.sixaxis_handle.npad_type,
                parameters.sixaxis_handle.npad_id, parameters.sixaxis_handle.device_index,
                parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::SetSixAxisSensorFusionParameters(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        f32 parameter1;
        f32 parameter2;
        u64 applet_resource_user_id;
    };
    static_assert(sizeof(Parameters) == 0x18, "Parameters has incorrect size.");

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->SetSixAxisFusionParameters(parameters.parameter1, parameters.parameter2);

    LOG_WARNING(Service_HID,
                "(STUBBED) called, npad_type={}, npad_id={}, device_index={}, parameter1={}, "
                "parameter2={}, applet_resource_user_id={}",
                parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
                parameters.sixaxis_handle.device_index, parameters.parameter1,
                parameters.parameter2, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::GetSixAxisSensorFusionParameters(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        u64 applet_resource_user_id;
    };
    static_assert(sizeof(Parameters) == 0x10, "Parameters has incorrect size.");

    f32 parameter1 = 0;
    f32 parameter2 = 0;
    const auto parameters{rp.PopRaw<Parameters>()};

    std::tie(parameter1, parameter2) =
        (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
            .WriteLocked()->GetSixAxisFusionParameters();

    LOG_WARNING(
        Service_HID,
        "(STUBBED) called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
        parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
        parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push(parameter1);
    rb.Push(parameter2);
}

void Hid::ResetSixAxisSensorFusionParameters(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        u64 applet_resource_user_id;
    };
    static_assert(sizeof(Parameters) == 0x10, "Parameters has incorrect size.");

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->ResetSixAxisFusionParameters();

    LOG_WARNING(
        Service_HID,
        "(STUBBED) called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
        parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
        parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::SetGyroscopeZeroDriftMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto sixaxis_handle{rp.PopRaw<Controller_NPad::DeviceHandle>()};
    const auto drift_mode{rp.PopEnum<Controller_NPad::GyroscopeZeroDriftMode>()};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->SetGyroscopeZeroDriftMode(drift_mode);

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, drift_mode={}, "
              "applet_resource_user_id={}",
              sixaxis_handle.npad_type, sixaxis_handle.npad_id, sixaxis_handle.device_index,
              drift_mode, applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::GetGyroscopeZeroDriftMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
              parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushEnum((*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
                    .ReadLocked()->GetGyroscopeZeroDriftMode());
}

void Hid::ResetGyroscopeZeroDriftMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->SetGyroscopeZeroDriftMode(Controller_NPad::GyroscopeZeroDriftMode::Standard);

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
              parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::IsSixAxisSensorAtRest(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
              parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push((*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
                .ReadLocked()->IsSixAxisSensorAtRest());
}

void Hid::IsFirmwareUpdateAvailableForSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_WARNING(
        Service_HID,
        "(STUBBED) called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
        parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
        parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(false);
}

void Hid::ActivateGesture(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 unknown;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->ActivateController(HidController::Gesture);

    LOG_DEBUG(Service_HID, "called, unknown={}, applet_resource_user_id={}", parameters.unknown,
              parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::SetSupportedNpadStyleSet(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto supported_styleset{rp.Pop<u32>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->SetSupportedStyleSet({supported_styleset});

    LOG_DEBUG(Service_HID, "called, supported_styleset={}", supported_styleset);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::GetSupportedNpadStyleSet(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push((*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
                .ReadLocked()->GetSupportedStyleSet()
                .raw);
}

void Hid::SetSupportedNpadIdType(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->SetSupportedNpadIdTypes(ctx.ReadBuffer().data(), ctx.GetReadBufferSize());

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::ActivateNpad(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    (*SharedReader(applet_resource))->ActivateController(HidController::NPad);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::DeactivateNpad(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    (*SharedReader(applet_resource))->DeactivateController(HidController::NPad);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::AcquireNpadStyleSetUpdateEventHandle(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 npad_id;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
        u64 unknown;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_DEBUG(Service_HID, "called, npad_id={}, applet_resource_user_id={}, unknown={}",
              parameters.npad_id, parameters.applet_resource_user_id, parameters.unknown);

    IPC::ResponseBuilder rb{ctx, 2, 1};
    rb.Push(ResultSuccess);
    rb.PushCopyFds((*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
                           .WriteLocked()->GetStyleSetChangedEvent(parameters.npad_id));
}

void Hid::DisconnectNpad(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 npad_id;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->DisconnectNpad(parameters.npad_id);

    LOG_DEBUG(Service_HID, "called, npad_id={}, applet_resource_user_id={}", parameters.npad_id,
              parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::GetPlayerLedPattern(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto npad_id{rp.Pop<u32>()};

    LOG_DEBUG(Service_HID, "called, npad_id={}", npad_id);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push((*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
                .WriteLocked()->GetLedPattern(npad_id)
                .raw);
}

void Hid::ActivateNpadWithRevision(Kernel::HLERequestContext& ctx) {
    // Should have no effect with how our npad sets up the data
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 unknown;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->ActivateController(HidController::NPad);

    LOG_DEBUG(Service_HID, "called, unknown={}, applet_resource_user_id={}", parameters.unknown,
              parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::SetNpadJoyHoldType(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};
    const auto hold_type{rp.PopEnum<Controller_NPad::NpadHoldType>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad).WriteLocked()->SetHoldType(hold_type);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}, hold_type={}",
              applet_resource_user_id, hold_type);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::GetNpadJoyHoldType(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.PushEnum((*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad).ReadLocked()->GetHoldType());
}

void Hid::SetNpadJoyAssignmentModeSingleByDefault(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 npad_id;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->SetNpadMode(parameters.npad_id, Controller_NPad::NpadAssignments::Single);

    LOG_WARNING(Service_HID, "(STUBBED) called, npad_id={}, applet_resource_user_id={}",
                parameters.npad_id, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::SetNpadJoyAssignmentModeSingle(Kernel::HLERequestContext& ctx) {
    // TODO: Check the differences between this and SetNpadJoyAssignmentModeSingleByDefault
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 npad_id;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
        u64 npad_joy_device_type;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->SetNpadMode(parameters.npad_id, Controller_NPad::NpadAssignments::Single);

    LOG_WARNING(Service_HID,
                "(STUBBED) called, npad_id={}, applet_resource_user_id={}, npad_joy_device_type={}",
                parameters.npad_id, parameters.applet_resource_user_id,
                parameters.npad_joy_device_type);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::SetNpadJoyAssignmentModeDual(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 npad_id;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->SetNpadMode(parameters.npad_id, Controller_NPad::NpadAssignments::Dual);

    LOG_WARNING(Service_HID, "(STUBBED) called, npad_id={}, applet_resource_user_id={}",
                parameters.npad_id, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::MergeSingleJoyAsDualJoy(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto npad_id_1{rp.Pop<u32>()};
    const auto npad_id_2{rp.Pop<u32>()};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->MergeSingleJoyAsDualJoy(npad_id_1, npad_id_2);

    LOG_DEBUG(Service_HID, "called, npad_id_1={}, npad_id_2={}, applet_resource_user_id={}",
              npad_id_1, npad_id_2, applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::StartLrAssignmentMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad).WriteLocked()->StartLRAssignmentMode();

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::StopLrAssignmentMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad).WriteLocked()->StopLRAssignmentMode();

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::SetNpadHandheldActivationMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};
    const auto activation_mode{rp.PopEnum<Controller_NPad::NpadHandheldActivationMode>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->SetNpadHandheldActivationMode(activation_mode);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}, activation_mode={}",
              applet_resource_user_id, activation_mode);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::GetNpadHandheldActivationMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.PushEnum((*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
                    .ReadLocked()->GetNpadHandheldActivationMode());
}

void Hid::SwapNpadAssignment(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto npad_id_1{rp.Pop<u32>()};
    const auto npad_id_2{rp.Pop<u32>()};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    const bool res = (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
                         .WriteLocked()->SwapNpadAssignment(npad_id_1, npad_id_2);

    LOG_DEBUG(Service_HID, "called, npad_id_1={}, npad_id_2={}, applet_resource_user_id={}",
              npad_id_1, npad_id_2, applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    if (res) {
        rb.Push(ResultSuccess);
    } else {
        LOG_ERROR(Service_HID, "Npads are not connected!");
        rb.Push(ERR_NPAD_NOT_CONNECTED);
    }
}

void Hid::IsUnintendedHomeButtonInputProtectionEnabled(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 npad_id;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, npad_id={}, applet_resource_user_id={}",
                parameters.npad_id, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push((*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
                .ReadLocked()->IsUnintendedHomeButtonInputProtectionEnabled(parameters.npad_id));
}

void Hid::EnableUnintendedHomeButtonInputProtection(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        bool unintended_home_button_input_protection;
        INSERT_PADDING_BYTES_NOINIT(3);
        u32 npad_id;
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->SetUnintendedHomeButtonInputProtectionEnabled(
            parameters.unintended_home_button_input_protection, parameters.npad_id);

    LOG_WARNING(Service_HID,
                "(STUBBED) called, unintended_home_button_input_protection={}, npad_id={},"
                "applet_resource_user_id={}",
                parameters.unintended_home_button_input_protection, parameters.npad_id,
                parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::SetNpadAnalogStickUseCenterClamp(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        bool analog_stick_use_center_clamp;
        u64 applet_resource_user_id;
    };
    static_assert(sizeof(Parameters) == 0x10, "Parameters has incorrect size.");

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->SetAnalogStickUseCenterClamp(parameters.analog_stick_use_center_clamp);

    LOG_WARNING(Service_HID,
                "(STUBBED) called, analog_stick_use_center_clamp={}, applet_resource_user_id={}",
                parameters.analog_stick_use_center_clamp, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::GetVibrationDeviceInfo(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto vibration_device_handle{rp.PopRaw<Controller_NPad::DeviceHandle>()};

    VibrationDeviceInfo vibration_device_info;

    switch (vibration_device_handle.npad_type) {
    case Controller_NPad::NpadType::ProController:
    case Controller_NPad::NpadType::Handheld:
    case Controller_NPad::NpadType::JoyconDual:
    case Controller_NPad::NpadType::JoyconLeft:
    case Controller_NPad::NpadType::JoyconRight:
    default:
        vibration_device_info.type = VibrationDeviceType::LinearResonantActuator;
        break;
    case Controller_NPad::NpadType::GameCube:
        vibration_device_info.type = VibrationDeviceType::GcErm;
        break;
    case Controller_NPad::NpadType::Pokeball:
        vibration_device_info.type = VibrationDeviceType::Unknown;
        break;
    }

    switch (vibration_device_handle.device_index) {
    case Controller_NPad::DeviceIndex::Left:
        vibration_device_info.position = VibrationDevicePosition::Left;
        break;
    case Controller_NPad::DeviceIndex::Right:
        vibration_device_info.position = VibrationDevicePosition::Right;
        break;
    case Controller_NPad::DeviceIndex::None:
    default:
        UNREACHABLE_MSG("DeviceIndex should never be None!");
        vibration_device_info.position = VibrationDevicePosition::None;
        break;
    }

    LOG_DEBUG(Service_HID, "called, vibration_device_type={}, vibration_device_position={}",
              vibration_device_info.type, vibration_device_info.position);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.PushRaw(vibration_device_info);
}

void Hid::SendVibrationValue(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle vibration_device_handle;
        Controller_NPad::VibrationValue vibration_value;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->VibrateController(parameters.vibration_device_handle, parameters.vibration_value);

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.vibration_device_handle.npad_type,
              parameters.vibration_device_handle.npad_id,
              parameters.vibration_device_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::GetActualVibrationValue(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle vibration_device_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.vibration_device_handle.npad_type,
              parameters.vibration_device_handle.npad_id,
              parameters.vibration_device_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw((*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
                   .ReadLocked()->GetLastVibration(parameters.vibration_device_handle));
}

void Hid::CreateActiveVibrationDeviceList(Kernel::HLERequestContext& ctx) {
    LOG_DEBUG(Service_HID, "called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IActiveVibrationDeviceList>(*SharedReader(applet_resource));
}

void Hid::PermitVibration(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto can_vibrate{rp.Pop<bool>()};

    Settings::values.vibration_enabled.SetValue(can_vibrate);

    LOG_DEBUG(Service_HID, "called, can_vibrate={}", can_vibrate);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::IsVibrationPermitted(Kernel::HLERequestContext& ctx) {
    LOG_DEBUG(Service_HID, "called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(Settings::values.vibration_enabled.GetValue());
}

void Hid::SendVibrationValues(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    const auto handles = ctx.ReadBuffer(0);
    const auto vibrations = ctx.ReadBuffer(1);

    std::vector<Controller_NPad::DeviceHandle> vibration_device_handles(
        handles.size() / sizeof(Controller_NPad::DeviceHandle));
    std::vector<Controller_NPad::VibrationValue> vibration_values(
        vibrations.size() / sizeof(Controller_NPad::VibrationValue));

    std::memcpy(vibration_device_handles.data(), handles.data(), handles.size());
    std::memcpy(vibration_values.data(), vibrations.data(), vibrations.size());

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->VibrateControllers(vibration_device_handles, vibration_values);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::SendVibrationGcErmCommand(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle vibration_device_handle;
        u64 applet_resource_user_id;
        VibrationGcErmCommand gc_erm_command;
    };
    static_assert(sizeof(Parameters) == 0x18, "Parameters has incorrect size.");

    const auto parameters{rp.PopRaw<Parameters>()};

    /**
     * Note: This uses mizu-specific behavior such that the StopHard command produces
     * vibrations where freq_low == 0.0f and freq_high == 0.0f, as defined below,
     * in order to differentiate between Stop and StopHard commands.
     * This is done to reuse the controller vibration functions made for regular controllers.
     */
    const auto vibration_value = [parameters] {
        switch (parameters.gc_erm_command) {
        case VibrationGcErmCommand::Stop:
            return Controller_NPad::VibrationValue{
                .amp_low = 0.0f,
                .freq_low = 160.0f,
                .amp_high = 0.0f,
                .freq_high = 320.0f,
            };
        case VibrationGcErmCommand::Start:
            return Controller_NPad::VibrationValue{
                .amp_low = 1.0f,
                .freq_low = 160.0f,
                .amp_high = 1.0f,
                .freq_high = 320.0f,
            };
        case VibrationGcErmCommand::StopHard:
            return Controller_NPad::VibrationValue{
                .amp_low = 0.0f,
                .freq_low = 0.0f,
                .amp_high = 0.0f,
                .freq_high = 0.0f,
            };
        default:
            return Controller_NPad::DEFAULT_VIBRATION_VALUE;
        }
    }();

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->VibrateController(parameters.vibration_device_handle, vibration_value);

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}, "
              "gc_erm_command={}",
              parameters.vibration_device_handle.npad_type,
              parameters.vibration_device_handle.npad_id,
              parameters.vibration_device_handle.device_index, parameters.applet_resource_user_id,
              parameters.gc_erm_command);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::GetActualVibrationGcErmCommand(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle vibration_device_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    const auto last_vibration = (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
                                    .ReadLocked()->GetLastVibration(parameters.vibration_device_handle);

    const auto gc_erm_command = [last_vibration] {
        if (last_vibration.amp_low != 0.0f || last_vibration.amp_high != 0.0f) {
            return VibrationGcErmCommand::Start;
        }

        /**
         * Note: This uses mizu-specific behavior such that the StopHard command produces
         * vibrations where freq_low == 0.0f and freq_high == 0.0f, as defined in the HID function
         * SendVibrationGcErmCommand, in order to differentiate between Stop and StopHard commands.
         * This is done to reuse the controller vibration functions made for regular controllers.
         */
        if (last_vibration.freq_low == 0.0f && last_vibration.freq_high == 0.0f) {
            return VibrationGcErmCommand::StopHard;
        }

        return VibrationGcErmCommand::Stop;
    }();

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.vibration_device_handle.npad_type,
              parameters.vibration_device_handle.npad_id,
              parameters.vibration_device_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.PushEnum(gc_erm_command);
}

void Hid::BeginPermitVibrationSession(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->SetPermitVibrationSession(true);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::EndPermitVibrationSession(Kernel::HLERequestContext& ctx) {
    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->SetPermitVibrationSession(false);

    LOG_DEBUG(Service_HID, "called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::IsVibrationDeviceMounted(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle vibration_device_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.vibration_device_handle.npad_type,
              parameters.vibration_device_handle.npad_id,
              parameters.vibration_device_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push((*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
                .ReadLocked()->IsVibrationDeviceMounted(parameters.vibration_device_handle));
}

void Hid::ActivateConsoleSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    (*SharedReader(applet_resource))->ActivateController(HidController::ConsoleSixAxisSensor);

    LOG_WARNING(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::StartConsoleSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_WARNING(
        Service_HID,
        "(STUBBED) called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
        parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
        parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::StopConsoleSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_WARNING(
        Service_HID,
        "(STUBBED) called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
        parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
        parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::ActivateSevenSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    (*SharedReader(applet_resource))->ActivateController(HidController::ConsoleSixAxisSensor);

    LOG_WARNING(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::StartSevenSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, applet_resource_user_id={}",
                applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::StopSevenSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, applet_resource_user_id={}",
                applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::InitializeSevenSixAxisSensor(Kernel::HLERequestContext& ctx) {
#if 0 // mizu TEMP
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};
    const auto t_mem_1_size{rp.Pop<u64>()};
    const auto t_mem_2_size{rp.Pop<u64>()};
    const auto t_mem_1_handle{ctx.GetCopyHandle(0)};
    const auto t_mem_2_handle{ctx.GetCopyHandle(1)};

    ASSERT_MSG(t_mem_1_size == 0x1000, "t_mem_1_size is not 0x1000 bytes");
    ASSERT_MSG(t_mem_2_size == 0x7F000, "t_mem_2_size is not 0x7F000 bytes");

    auto t_mem_1 = sYstem.CurrentProcess()->GetHandleTable().GetObject<Kernel::KTransferMemory>(
        t_mem_1_handle);

    if (t_mem_1.IsNull()) {
        LOG_ERROR(Service_HID, "t_mem_1 is a nullptr for handle=0x{:08X}", t_mem_1_handle);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultUnknown);
        return;
    }

    auto t_mem_2 = sYstem.CurrentProcess()->GetHandleTable().GetObject<Kernel::KTransferMemory>(
        t_mem_2_handle);

    if (t_mem_2.IsNull()) {
        LOG_ERROR(Service_HID, "t_mem_2 is a nullptr for handle=0x{:08X}", t_mem_2_handle);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultUnknown);
        return;
    }

    ASSERT_MSG(t_mem_1->GetSize() == 0x1000, "t_mem_1 has incorrect size");
    ASSERT_MSG(t_mem_2->GetSize() == 0x7F000, "t_mem_2 has incorrect size");

    // Activate console six axis controller
    (*SharedReader(applet_resource))->GetController<Controller_ConsoleSixAxis>(HidController::ConsoleSixAxisSensor)
        .WriteLocked()->ActivateController();

    (*SharedReader(applet_resource))->GetController<Controller_ConsoleSixAxis>(HidController::ConsoleSixAxisSensor)
        .WriteLocked()->SetTransferMemoryPointer(sYstem.Memory().GetPointer(t_mem_1->GetSourceAddress()));

    LOG_WARNING(Service_HID,
                "called, t_mem_1_handle=0x{:08X}, t_mem_2_handle=0x{:08X}, "
                "applet_resource_user_id={}",
                t_mem_1_handle, t_mem_2_handle, applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
#endif
    LOG_CRITICAL(Service_HID, "mizu TODO");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultUnknown);
}

void Hid::FinalizeSevenSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, applet_resource_user_id={}",
                applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::ResetSevenSixAxisSensorTimestamp(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    (*SharedReader(applet_resource))->GetController<Controller_ConsoleSixAxis>(HidController::ConsoleSixAxisSensor)
        .WriteLocked()->ResetTimestamp();

    LOG_WARNING(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::SetIsPalmaAllConnectable(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};
    const auto is_palma_all_connectable{rp.Pop<bool>()};

    LOG_WARNING(Service_HID,
                "(STUBBED) called, applet_resource_user_id={}, is_palma_all_connectable={}",
                applet_resource_user_id, is_palma_all_connectable);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::SetPalmaBoostMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto palma_boost_mode{rp.Pop<bool>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, palma_boost_mode={}", palma_boost_mode);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::SetNpadCommunicationMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};
    const auto communication_mode{rp.PopEnum<Controller_NPad::NpadCommunicationMode>()};

    (*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
        .WriteLocked()->SetNpadCommunicationMode(communication_mode);

    LOG_WARNING(Service_HID, "(STUBBED) called, applet_resource_user_id={}, communication_mode={}",
                applet_resource_user_id, communication_mode);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Hid::GetNpadCommunicationMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, applet_resource_user_id={}",
                applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.PushEnum((*SharedReader(applet_resource))->GetController<Controller_NPad>(HidController::NPad)
                    .ReadLocked()->GetNpadCommunicationMode());
}

void Hid::SetTouchScreenConfiguration(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto touchscreen_mode{rp.PopRaw<Controller_Touchscreen::TouchScreenConfigurationForNx>()};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, touchscreen_mode={}, applet_resource_user_id={}",
                touchscreen_mode.mode, applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

class HidDbg final : public ServiceFramework<HidDbg> {
public:
    explicit HidDbg() : ServiceFramework{"hid:dbg"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "DeactivateDebugPad"},
            {1, nullptr, "SetDebugPadAutoPilotState"},
            {2, nullptr, "UnsetDebugPadAutoPilotState"},
            {10, nullptr, "DeactivateTouchScreen"},
            {11, nullptr, "SetTouchScreenAutoPilotState"},
            {12, nullptr, "UnsetTouchScreenAutoPilotState"},
            {13, nullptr, "GetTouchScreenConfiguration"},
            {14, nullptr, "ProcessTouchScreenAutoTune"},
            {15, nullptr, "ForceStopTouchScreenManagement"},
            {16, nullptr, "ForceRestartTouchScreenManagement"},
            {17, nullptr, "IsTouchScreenManaged"},
            {20, nullptr, "DeactivateMouse"},
            {21, nullptr, "SetMouseAutoPilotState"},
            {22, nullptr, "UnsetMouseAutoPilotState"},
            {30, nullptr, "DeactivateKeyboard"},
            {31, nullptr, "SetKeyboardAutoPilotState"},
            {32, nullptr, "UnsetKeyboardAutoPilotState"},
            {50, nullptr, "DeactivateXpad"},
            {51, nullptr, "SetXpadAutoPilotState"},
            {52, nullptr, "UnsetXpadAutoPilotState"},
            {53, nullptr, "DeactivateJoyXpad"},
            {60, nullptr, "ClearNpadSystemCommonPolicy"},
            {61, nullptr, "DeactivateNpad"},
            {62, nullptr, "ForceDisconnectNpad"},
            {91, nullptr, "DeactivateGesture"},
            {110, nullptr, "DeactivateHomeButton"},
            {111, nullptr, "SetHomeButtonAutoPilotState"},
            {112, nullptr, "UnsetHomeButtonAutoPilotState"},
            {120, nullptr, "DeactivateSleepButton"},
            {121, nullptr, "SetSleepButtonAutoPilotState"},
            {122, nullptr, "UnsetSleepButtonAutoPilotState"},
            {123, nullptr, "DeactivateInputDetector"},
            {130, nullptr, "DeactivateCaptureButton"},
            {131, nullptr, "SetCaptureButtonAutoPilotState"},
            {132, nullptr, "UnsetCaptureButtonAutoPilotState"},
            {133, nullptr, "SetShiftAccelerometerCalibrationValue"},
            {134, nullptr, "GetShiftAccelerometerCalibrationValue"},
            {135, nullptr, "SetShiftGyroscopeCalibrationValue"},
            {136, nullptr, "GetShiftGyroscopeCalibrationValue"},
            {140, nullptr, "DeactivateConsoleSixAxisSensor"},
            {141, nullptr, "GetConsoleSixAxisSensorSamplingFrequency"},
            {142, nullptr, "DeactivateSevenSixAxisSensor"},
            {143, nullptr, "GetConsoleSixAxisSensorCountStates"},
            {144, nullptr, "GetAccelerometerFsr"},
            {145, nullptr, "SetAccelerometerFsr"},
            {146, nullptr, "GetAccelerometerOdr"},
            {147, nullptr, "SetAccelerometerOdr"},
            {148, nullptr, "GetGyroscopeFsr"},
            {149, nullptr, "SetGyroscopeFsr"},
            {150, nullptr, "GetGyroscopeOdr"},
            {151, nullptr, "SetGyroscopeOdr"},
            {152, nullptr, "GetWhoAmI"},
            {201, nullptr, "ActivateFirmwareUpdate"},
            {202, nullptr, "DeactivateFirmwareUpdate"},
            {203, nullptr, "StartFirmwareUpdate"},
            {204, nullptr, "GetFirmwareUpdateStage"},
            {205, nullptr, "GetFirmwareVersion"},
            {206, nullptr, "GetDestinationFirmwareVersion"},
            {207, nullptr, "DiscardFirmwareInfoCacheForRevert"},
            {208, nullptr, "StartFirmwareUpdateForRevert"},
            {209, nullptr, "GetAvailableFirmwareVersionForRevert"},
            {210, nullptr, "IsFirmwareUpdatingDevice"},
            {211, nullptr, "StartFirmwareUpdateIndividual"},
            {215, nullptr, "SetUsbFirmwareForceUpdateEnabled"},
            {216, nullptr, "SetAllKuinaDevicesToFirmwareUpdateMode"},
            {221, nullptr, "UpdateControllerColor"},
            {222, nullptr, "ConnectUsbPadsAsync"},
            {223, nullptr, "DisconnectUsbPadsAsync"},
            {224, nullptr, "UpdateDesignInfo"},
            {225, nullptr, "GetUniquePadDriverState"},
            {226, nullptr, "GetSixAxisSensorDriverStates"},
            {227, nullptr, "GetRxPacketHistory"},
            {228, nullptr, "AcquireOperationEventHandle"},
            {229, nullptr, "ReadSerialFlash"},
            {230, nullptr, "WriteSerialFlash"},
            {231, nullptr, "GetOperationResult"},
            {232, nullptr, "EnableShipmentMode"},
            {233, nullptr, "ClearPairingInfo"},
            {234, nullptr, "GetUniquePadDeviceTypeSetInternal"},
            {235, nullptr, "EnableAnalogStickPower"},
            {236, nullptr, "RequestKuinaUartClockCal"},
            {237, nullptr, "GetKuinaUartClockCal"},
            {238, nullptr, "SetKuinaUartClockTrim"},
            {239, nullptr, "KuinaLoopbackTest"},
            {240, nullptr, "RequestBatteryVoltage"},
            {241, nullptr, "GetBatteryVoltage"},
            {242, nullptr, "GetUniquePadPowerInfo"},
            {243, nullptr, "RebootUniquePad"},
            {244, nullptr, "RequestKuinaFirmwareVersion"},
            {245, nullptr, "GetKuinaFirmwareVersion"},
            {246, nullptr, "GetVidPid"},
            {247, nullptr, "GetAnalogStickCalibrationValue"},
            {248, nullptr, "GetUniquePadIdsFull"},
            {249, nullptr, "ConnectUniquePad"},
            {250, nullptr, "IsVirtual"},
            {251, nullptr, "GetAnalogStickModuleParam"},
            {301, nullptr, "GetAbstractedPadHandles"},
            {302, nullptr, "GetAbstractedPadState"},
            {303, nullptr, "GetAbstractedPadsState"},
            {321, nullptr, "SetAutoPilotVirtualPadState"},
            {322, nullptr, "UnsetAutoPilotVirtualPadState"},
            {323, nullptr, "UnsetAllAutoPilotVirtualPadState"},
            {324, nullptr, "AttachHdlsWorkBuffer"},
            {325, nullptr, "ReleaseHdlsWorkBuffer"},
            {326, nullptr, "DumpHdlsNpadAssignmentState"},
            {327, nullptr, "DumpHdlsStates"},
            {328, nullptr, "ApplyHdlsNpadAssignmentState"},
            {329, nullptr, "ApplyHdlsStateList"},
            {330, nullptr, "AttachHdlsVirtualDevice"},
            {331, nullptr, "DetachHdlsVirtualDevice"},
            {332, nullptr, "SetHdlsState"},
            {350, nullptr, "AddRegisteredDevice"},
            {400, nullptr, "DisableExternalMcuOnNxDevice"},
            {401, nullptr, "DisableRailDeviceFiltering"},
            {402, nullptr, "EnableWiredPairing"},
            {403, nullptr, "EnableShipmentModeAutoClear"},
            {404, nullptr, "SetRailEnabled"},
            {500, nullptr, "SetFactoryInt"},
            {501, nullptr, "IsFactoryBootEnabled"},
            {550, nullptr, "SetAnalogStickModelDataTemporarily"},
            {551, nullptr, "GetAnalogStickModelData"},
            {552, nullptr, "ResetAnalogStickModelData"},
            {600, nullptr, "ConvertPadState"},
            {650, nullptr, "AddButtonPlayData"},
            {651, nullptr, "StartButtonPlayData"},
            {652, nullptr, "StopButtonPlayData"},
            {2000, nullptr, "DeactivateDigitizer"},
            {2001, nullptr, "SetDigitizerAutoPilotState"},
            {2002, nullptr, "UnsetDigitizerAutoPilotState"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class HidSys final : public ServiceFramework<HidSys> {
public:
    explicit HidSys() : ServiceFramework{"hid:sys"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {31, nullptr, "SendKeyboardLockKeyEvent"},
            {101, nullptr, "AcquireHomeButtonEventHandle"},
            {111, nullptr, "ActivateHomeButton"},
            {121, nullptr, "AcquireSleepButtonEventHandle"},
            {131, nullptr, "ActivateSleepButton"},
            {141, nullptr, "AcquireCaptureButtonEventHandle"},
            {151, nullptr, "ActivateCaptureButton"},
            {161, nullptr, "GetPlatformConfig"},
            {210, nullptr, "AcquireNfcDeviceUpdateEventHandle"},
            {211, nullptr, "GetNpadsWithNfc"},
            {212, nullptr, "AcquireNfcActivateEventHandle"},
            {213, nullptr, "ActivateNfc"},
            {214, nullptr, "GetXcdHandleForNpadWithNfc"},
            {215, nullptr, "IsNfcActivated"},
            {230, nullptr, "AcquireIrSensorEventHandle"},
            {231, nullptr, "ActivateIrSensor"},
            {232, nullptr, "GetIrSensorState"},
            {233, nullptr, "GetXcdHandleForNpadWithIrSensor"},
            {301, nullptr, "ActivateNpadSystem"},
            {303, &HidSys::ApplyNpadSystemCommonPolicy, "ApplyNpadSystemCommonPolicy"},
            {304, nullptr, "EnableAssigningSingleOnSlSrPress"},
            {305, nullptr, "DisableAssigningSingleOnSlSrPress"},
            {306, nullptr, "GetLastActiveNpad"},
            {307, nullptr, "GetNpadSystemExtStyle"},
            {308, nullptr, "ApplyNpadSystemCommonPolicyFull"},
            {309, nullptr, "GetNpadFullKeyGripColor"},
            {310, nullptr, "GetMaskedSupportedNpadStyleSet"},
            {311, nullptr, "SetNpadPlayerLedBlinkingDevice"},
            {312, nullptr, "SetSupportedNpadStyleSetAll"},
            {313, nullptr, "GetNpadCaptureButtonAssignment"},
            {314, nullptr, "GetAppletFooterUiType"},
            {315, nullptr, "GetAppletDetailedUiType"},
            {316, nullptr, "GetNpadInterfaceType"},
            {317, nullptr, "GetNpadLeftRightInterfaceType"},
            {318, nullptr, "HasBattery"},
            {319, nullptr, "HasLeftRightBattery"},
            {321, nullptr, "GetUniquePadsFromNpad"},
            {322, nullptr, "GetIrSensorState"},
            {323, nullptr, "GetXcdHandleForNpadWithIrSensor"},
            {324, nullptr, "GetUniquePadButtonSet"},
            {325, nullptr, "GetUniquePadColor"},
            {326, nullptr, "GetUniquePadAppletDetailedUiType"},
            {500, nullptr, "SetAppletResourceUserId"},
            {501, nullptr, "RegisterAppletResourceUserId"},
            {502, nullptr, "UnregisterAppletResourceUserId"},
            {503, nullptr, "EnableAppletToGetInput"},
            {504, nullptr, "SetAruidValidForVibration"},
            {505, nullptr, "EnableAppletToGetSixAxisSensor"},
            {510, nullptr, "SetVibrationMasterVolume"},
            {511, nullptr, "GetVibrationMasterVolume"},
            {512, nullptr, "BeginPermitVibrationSession"},
            {513, nullptr, "EndPermitVibrationSession"},
            {514, nullptr, "Unknown514"},
            {520, nullptr, "EnableHandheldHids"},
            {521, nullptr, "DisableHandheldHids"},
            {522, nullptr, "SetJoyConRailEnabled"},
            {523, nullptr, "IsJoyConRailEnabled"},
            {524, nullptr, "IsHandheldHidsEnabled"},
            {525, nullptr, "IsJoyConAttachedOnAllRail"},
            {540, nullptr, "AcquirePlayReportControllerUsageUpdateEvent"},
            {541, nullptr, "GetPlayReportControllerUsages"},
            {542, nullptr, "AcquirePlayReportRegisteredDeviceUpdateEvent"},
            {543, nullptr, "GetRegisteredDevicesOld"},
            {544, nullptr, "AcquireConnectionTriggerTimeoutEvent"},
            {545, nullptr, "SendConnectionTrigger"},
            {546, nullptr, "AcquireDeviceRegisteredEventForControllerSupport"},
            {547, nullptr, "GetAllowedBluetoothLinksCount"},
            {548, nullptr, "GetRegisteredDevices"},
            {549, nullptr, "GetConnectableRegisteredDevices"},
            {700, nullptr, "ActivateUniquePad"},
            {702, nullptr, "AcquireUniquePadConnectionEventHandle"},
            {703, nullptr, "GetUniquePadIds"},
            {751, nullptr, "AcquireJoyDetachOnBluetoothOffEventHandle"},
            {800, nullptr, "ListSixAxisSensorHandles"},
            {801, nullptr, "IsSixAxisSensorUserCalibrationSupported"},
            {802, nullptr, "ResetSixAxisSensorCalibrationValues"},
            {803, nullptr, "StartSixAxisSensorUserCalibration"},
            {804, nullptr, "CancelSixAxisSensorUserCalibration"},
            {805, nullptr, "GetUniquePadBluetoothAddress"},
            {806, nullptr, "DisconnectUniquePad"},
            {807, nullptr, "GetUniquePadType"},
            {808, nullptr, "GetUniquePadInterface"},
            {809, nullptr, "GetUniquePadSerialNumber"},
            {810, nullptr, "GetUniquePadControllerNumber"},
            {811, nullptr, "GetSixAxisSensorUserCalibrationStage"},
            {812, nullptr, "GetConsoleUniqueSixAxisSensorHandle"},
            {821, nullptr, "StartAnalogStickManualCalibration"},
            {822, nullptr, "RetryCurrentAnalogStickManualCalibrationStage"},
            {823, nullptr, "CancelAnalogStickManualCalibration"},
            {824, nullptr, "ResetAnalogStickManualCalibration"},
            {825, nullptr, "GetAnalogStickState"},
            {826, nullptr, "GetAnalogStickManualCalibrationStage"},
            {827, nullptr, "IsAnalogStickButtonPressed"},
            {828, nullptr, "IsAnalogStickInReleasePosition"},
            {829, nullptr, "IsAnalogStickInCircumference"},
            {830, nullptr, "SetNotificationLedPattern"},
            {831, nullptr, "SetNotificationLedPatternWithTimeout"},
            {832, nullptr, "PrepareHidsForNotificationWake"},
            {850, nullptr, "IsUsbFullKeyControllerEnabled"},
            {851, nullptr, "EnableUsbFullKeyController"},
            {852, nullptr, "IsUsbConnected"},
            {870, nullptr, "IsHandheldButtonPressedOnConsoleMode"},
            {900, nullptr, "ActivateInputDetector"},
            {901, nullptr, "NotifyInputDetector"},
            {1000, nullptr, "InitializeFirmwareUpdate"},
            {1001, nullptr, "GetFirmwareVersion"},
            {1002, nullptr, "GetAvailableFirmwareVersion"},
            {1003, nullptr, "IsFirmwareUpdateAvailable"},
            {1004, nullptr, "CheckFirmwareUpdateRequired"},
            {1005, nullptr, "StartFirmwareUpdate"},
            {1006, nullptr, "AbortFirmwareUpdate"},
            {1007, nullptr, "GetFirmwareUpdateState"},
            {1008, nullptr, "ActivateAudioControl"},
            {1009, nullptr, "AcquireAudioControlEventHandle"},
            {1010, nullptr, "GetAudioControlStates"},
            {1011, nullptr, "DeactivateAudioControl"},
            {1050, nullptr, "IsSixAxisSensorAccurateUserCalibrationSupported"},
            {1051, nullptr, "StartSixAxisSensorAccurateUserCalibration"},
            {1052, nullptr, "CancelSixAxisSensorAccurateUserCalibration"},
            {1053, nullptr, "GetSixAxisSensorAccurateUserCalibrationState"},
            {1100, nullptr, "GetHidbusSystemServiceObject"},
            {1120, nullptr, "SetFirmwareHotfixUpdateSkipEnabled"},
            {1130, nullptr, "InitializeUsbFirmwareUpdate"},
            {1131, nullptr, "FinalizeUsbFirmwareUpdate"},
            {1132, nullptr, "CheckUsbFirmwareUpdateRequired"},
            {1133, nullptr, "StartUsbFirmwareUpdate"},
            {1134, nullptr, "GetUsbFirmwareUpdateState"},
            {1150, nullptr, "SetTouchScreenMagnification"},
            {1151, nullptr, "GetTouchScreenFirmwareVersion"},
            {1152, nullptr, "SetTouchScreenDefaultConfiguration"},
            {1153, nullptr, "GetTouchScreenDefaultConfiguration"},
            {1154, nullptr, "IsFirmwareAvailableForNotification"},
            {1155, nullptr, "SetForceHandheldStyleVibration"},
            {1156, nullptr, "SendConnectionTriggerWithoutTimeoutEvent"},
            {1157, nullptr, "CancelConnectionTrigger"},
            {1200, nullptr, "IsButtonConfigSupported"},
            {1201, nullptr, "IsButtonConfigEmbeddedSupported"},
            {1202, nullptr, "DeleteButtonConfig"},
            {1203, nullptr, "DeleteButtonConfigEmbedded"},
            {1204, nullptr, "SetButtonConfigEnabled"},
            {1205, nullptr, "SetButtonConfigEmbeddedEnabled"},
            {1206, nullptr, "IsButtonConfigEnabled"},
            {1207, nullptr, "IsButtonConfigEmbeddedEnabled"},
            {1208, nullptr, "SetButtonConfigEmbedded"},
            {1209, nullptr, "SetButtonConfigFull"},
            {1210, nullptr, "SetButtonConfigLeft"},
            {1211, nullptr, "SetButtonConfigRight"},
            {1212, nullptr, "GetButtonConfigEmbedded"},
            {1213, nullptr, "GetButtonConfigFull"},
            {1214, nullptr, "GetButtonConfigLeft"},
            {1215, nullptr, "GetButtonConfigRight"},
            {1250, nullptr, "IsCustomButtonConfigSupported"},
            {1251, nullptr, "IsDefaultButtonConfigEmbedded"},
            {1252, nullptr, "IsDefaultButtonConfigFull"},
            {1253, nullptr, "IsDefaultButtonConfigLeft"},
            {1254, nullptr, "IsDefaultButtonConfigRight"},
            {1255, nullptr, "IsButtonConfigStorageEmbeddedEmpty"},
            {1256, nullptr, "IsButtonConfigStorageFullEmpty"},
            {1257, nullptr, "IsButtonConfigStorageLeftEmpty"},
            {1258, nullptr, "IsButtonConfigStorageRightEmpty"},
            {1259, nullptr, "GetButtonConfigStorageEmbeddedDeprecated"},
            {1260, nullptr, "GetButtonConfigStorageFullDeprecated"},
            {1261, nullptr, "GetButtonConfigStorageLeftDeprecated"},
            {1262, nullptr, "GetButtonConfigStorageRightDeprecated"},
            {1263, nullptr, "SetButtonConfigStorageEmbeddedDeprecated"},
            {1264, nullptr, "SetButtonConfigStorageFullDeprecated"},
            {1265, nullptr, "SetButtonConfigStorageLeftDeprecated"},
            {1266, nullptr, "SetButtonConfigStorageRightDeprecated"},
            {1267, nullptr, "DeleteButtonConfigStorageEmbedded"},
            {1268, nullptr, "DeleteButtonConfigStorageFull"},
            {1269, nullptr, "DeleteButtonConfigStorageLeft"},
            {1270, nullptr, "DeleteButtonConfigStorageRight"},
            {1271, nullptr, "IsUsingCustomButtonConfig"},
            {1272, nullptr, "IsAnyCustomButtonConfigEnabled"},
            {1273, nullptr, "SetAllCustomButtonConfigEnabled"},
            {1274, nullptr, "SetDefaultButtonConfig"},
            {1275, nullptr, "SetAllDefaultButtonConfig"},
            {1276, nullptr, "SetHidButtonConfigEmbedded"},
            {1277, nullptr, "SetHidButtonConfigFull"},
            {1278, nullptr, "SetHidButtonConfigLeft"},
            {1279, nullptr, "SetHidButtonConfigRight"},
            {1280, nullptr, "GetHidButtonConfigEmbedded"},
            {1281, nullptr, "GetHidButtonConfigFull"},
            {1282, nullptr, "GetHidButtonConfigLeft"},
            {1283, nullptr, "GetHidButtonConfigRight"},
            {1284, nullptr, "GetButtonConfigStorageEmbedded"},
            {1285, nullptr, "GetButtonConfigStorageFull"},
            {1286, nullptr, "GetButtonConfigStorageLeft"},
            {1287, nullptr, "GetButtonConfigStorageRight"},
            {1288, nullptr, "SetButtonConfigStorageEmbedded"},
            {1289, nullptr, "SetButtonConfigStorageFull"},
            {1290, nullptr, "DeleteButtonConfigStorageRight"},
            {1291, nullptr, "DeleteButtonConfigStorageRight"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void ApplyNpadSystemCommonPolicy(Kernel::HLERequestContext& ctx) {
        // We already do this for homebrew so we can just stub it out
        LOG_WARNING(Service_HID, "called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

class HidTmp final : public ServiceFramework<HidTmp> {
public:
    explicit HidTmp() : ServiceFramework{"hid:tmp"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetConsoleSixAxisSensorCalibrationValues"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class HidBus final : public ServiceFramework<HidBus> {
public:
    explicit HidBus() : ServiceFramework{"hidbus"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {1, nullptr, "GetBusHandle"},
            {2, nullptr, "IsExternalDeviceConnected"},
            {3, nullptr, "Initialize"},
            {4, nullptr, "Finalize"},
            {5, nullptr, "EnableExternalDevice"},
            {6, nullptr, "GetExternalDeviceId"},
            {7, nullptr, "SendCommandAsync"},
            {8, nullptr, "GetSendCommandAsynceResult"},
            {9, nullptr, "SetEventForSendCommandAsycResult"},
            {10, nullptr, "GetSharedMemoryHandle"},
            {11, nullptr, "EnableJoyPollingReceiveMode"},
            {12, nullptr, "DisableJoyPollingReceiveMode"},
            {13, nullptr, "GetPollingData"},
            {14, nullptr, "SetStatusManagerType"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

void ReloadInputDevices() {
    Settings::values.is_device_reload_pending.store(true);
}

void InstallInterfaces() {
    MakeService<Hid>();
    MakeService<HidBus>();
    MakeService<HidDbg>();
    MakeService<HidSys>();
    MakeService<HidTmp>();

    MakeService<IRS>();
    MakeService<IRS_SYS>();

    MakeService<XCD_SYS>();
}

} // namespace Service::HID
