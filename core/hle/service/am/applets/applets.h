// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#pragma once

#include <memory>
#include <queue>

#include "common/swap.h"
#include "core/hle/service/kernel_helpers.h"

union ResultCode;

namespace Core::Frontend {
class ControllerApplet;
class ECommerceApplet;
class ErrorApplet;
class ParentalControlsApplet;
class PhotoViewerApplet;
class ProfileSelectApplet;
class SoftwareKeyboardApplet;
class WebBrowserApplet;
} // namespace Core::Frontend

namespace Service::AM {

class IStorage;

namespace Applets {

enum class AppletId : u32 {
    OverlayDisplay = 0x02,
    QLaunch = 0x03,
    Starter = 0x04,
    Auth = 0x0A,
    Cabinet = 0x0B,
    Controller = 0x0C,
    DataErase = 0x0D,
    Error = 0x0E,
    NetConnect = 0x0F,
    ProfileSelect = 0x10,
    SoftwareKeyboard = 0x11,
    MiiEdit = 0x12,
    Web = 0x13,
    Shop = 0x14,
    PhotoViewer = 0x15,
    Settings = 0x16,
    OfflineWeb = 0x17,
    LoginShare = 0x18,
    WebAuth = 0x19,
    MyPage = 0x1A,
};

enum class LibraryAppletMode : u32 {
    AllForeground = 0,
    Background = 1,
    NoUI = 2,
    BackgroundIndirectDisplay = 3,
    AllForegroundInitiallyHidden = 4,
};

class AppletDataBroker final {
public:
    explicit AppletDataBroker(LibraryAppletMode applet_mode_);
    ~AppletDataBroker();

    struct RawChannelData {
        std::vector<std::vector<u8>> normal;
        std::vector<std::vector<u8>> interactive;
    };

    // Retrieves but does not pop the data sent to applet.
    RawChannelData PeekDataToAppletForDebug() const;

    std::shared_ptr<IStorage> PopNormalDataToGame();
    std::shared_ptr<IStorage> PopNormalDataToApplet();

    std::shared_ptr<IStorage> PopInteractiveDataToGame();
    std::shared_ptr<IStorage> PopInteractiveDataToApplet();

    void PushNormalDataFromGame(std::shared_ptr<IStorage>&& storage);
    void PushNormalDataFromApplet(std::shared_ptr<IStorage>&& storage);

    void PushInteractiveDataFromGame(std::shared_ptr<IStorage>&& storage);
    void PushInteractiveDataFromApplet(std::shared_ptr<IStorage>&& storage);

    void SignalStateChanged();

    int GetNormalDataEvent();
    int GetInteractiveDataEvent();
    int GetStateChangedEvent();

    void SetRequesterPid(::pid_t pid) {
        requester_pid = pid;
    }

private:
    LibraryAppletMode applet_mode;

    // Queues are named from applet's perspective

    // PopNormalDataToApplet and PushNormalDataFromGame
    std::deque<std::shared_ptr<IStorage>> in_channel;

    // PopNormalDataToGame and PushNormalDataFromApplet
    std::deque<std::shared_ptr<IStorage>> out_channel;

    // PopInteractiveDataToApplet and PushInteractiveDataFromGame
    std::deque<std::shared_ptr<IStorage>> in_interactive_channel;

    // PopInteractiveDataToGame and PushInteractiveDataFromApplet
    std::deque<std::shared_ptr<IStorage>> out_interactive_channel;

    int state_changed_event;

    // Signaled on PushNormalDataFromApplet
    int pop_out_data_event;

    // Signaled on PushInteractiveDataFromApplet
    int pop_interactive_out_data_event;

    ::pid_t requester_pid = -1;
};

class Applet {
public:
    explicit Applet(LibraryAppletMode applet_mode_);
    virtual ~Applet();

    virtual void Initialize();

    virtual bool TransactionComplete() const = 0;
    virtual ResultCode GetStatus() const = 0;
    virtual void ExecuteInteractive() = 0;
    virtual void Execute() = 0;

    AppletDataBroker& GetBroker() {
        return broker;
    }

    const AppletDataBroker& GetBroker() const {
        return broker;
    }

    LibraryAppletMode GetLibraryAppletMode() const {
        return applet_mode;
    }

    bool IsInitialized() const {
        return initialized;
    }

protected:
    struct CommonArguments {
        u32_le arguments_version;
        u32_le size;
        u32_le library_version;
        u32_le theme_color;
        u8 play_startup_sound;
        u64_le system_tick;
    };
    static_assert(sizeof(CommonArguments) == 0x20, "CommonArguments has incorrect size.");

    CommonArguments common_args{};
    AppletDataBroker broker;
    LibraryAppletMode applet_mode;
    bool initialized = false;

private:
    ::pid_t requester_pid = -1;
};

struct AppletFrontendSet {
    using ControllerApplet = std::unique_ptr<Core::Frontend::ControllerApplet>;
    using ErrorApplet = std::unique_ptr<Core::Frontend::ErrorApplet>;
    using ParentalControlsApplet = std::unique_ptr<Core::Frontend::ParentalControlsApplet>;
    using PhotoViewer = std::unique_ptr<Core::Frontend::PhotoViewerApplet>;
    using ProfileSelect = std::unique_ptr<Core::Frontend::ProfileSelectApplet>;
    using SoftwareKeyboard = std::unique_ptr<Core::Frontend::SoftwareKeyboardApplet>;
    using WebBrowser = std::unique_ptr<Core::Frontend::WebBrowserApplet>;

    AppletFrontendSet();
    AppletFrontendSet(ControllerApplet controller_applet, ErrorApplet error_applet,
                      ParentalControlsApplet parental_controls_applet, PhotoViewer photo_viewer_,
                      ProfileSelect profile_select_, SoftwareKeyboard software_keyboard_,
                      WebBrowser web_browser_);
    ~AppletFrontendSet();

    AppletFrontendSet(const AppletFrontendSet&) = delete;
    AppletFrontendSet& operator=(const AppletFrontendSet&) = delete;

    AppletFrontendSet(AppletFrontendSet&&) noexcept;
    AppletFrontendSet& operator=(AppletFrontendSet&&) noexcept;

    ControllerApplet controller;
    ErrorApplet error;
    ParentalControlsApplet parental_controls;
    PhotoViewer photo_viewer;
    ProfileSelect profile_select;
    SoftwareKeyboard software_keyboard;
    WebBrowser web_browser;
};

class AppletManager {
public:
    explicit AppletManager();
    ~AppletManager();

    const AppletFrontendSet& GetAppletFrontendSet() const;

    void SetAppletFrontendSet(AppletFrontendSet set);
    void SetDefaultAppletFrontendSet();
    void SetDefaultAppletsIfMissing();
    void ClearAll();

    std::shared_ptr<Applet> GetApplet(AppletId id, LibraryAppletMode mode,
                                      ::pid_t requester_pid) const;

private:
    AppletFrontendSet frontend;
};

} // namespace Applets
} // namespace Service::AM
