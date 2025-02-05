// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#include <atomic>
#include <list>
#include <mutex>
#include <utility>
#include "common/assert.h"
#include "common/threadsafe_queue.h"
#include "input_common/gcadapter/gc_adapter.h"
#include "input_common/gcadapter/gc_poller.h"

namespace InputCommon {

class GCButton final : public Input::ButtonDevice {
public:
    explicit GCButton(u32 port_, s32 button_, const GCAdapter::Adapter* adapter)
        : port(port_), button(button_), gcadapter(adapter) {}

    ~GCButton() override;

    bool GetStatus() const override {
        if (gcadapter->DeviceConnected(port)) {
            return (gcadapter->GetPadState(port).buttons & button) != 0;
        }
        return false;
    }

private:
    const u32 port;
    const s32 button;
    const GCAdapter::Adapter* gcadapter;
};

class GCAxisButton final : public Input::ButtonDevice {
public:
    explicit GCAxisButton(u32 port_, u32 axis_, float threshold_, bool trigger_if_greater_,
                          const GCAdapter::Adapter* adapter)
        : port(port_), axis(axis_), threshold(threshold_), trigger_if_greater(trigger_if_greater_),
          gcadapter(adapter) {}

    bool GetStatus() const override {
        if (gcadapter->DeviceConnected(port)) {
            const float current_axis_value = gcadapter->GetPadState(port).axis_values.at(axis);
            const float axis_value = current_axis_value / 128.0f;
            if (trigger_if_greater) {
                // TODO: Might be worthwile to set a slider for the trigger threshold. It is
                // currently always set to 0.5 in configure_input_player.cpp ZL/ZR HandleClick
                return axis_value > threshold;
            }
            return axis_value < -threshold;
        }
        return false;
    }

private:
    const u32 port;
    const u32 axis;
    float threshold;
    bool trigger_if_greater;
    const GCAdapter::Adapter* gcadapter;
};

GCButtonFactory::GCButtonFactory(std::shared_ptr<GCAdapter::Adapter> adapter_)
    : adapter(std::move(adapter_)) {}

GCButton::~GCButton() = default;

std::unique_ptr<Input::ButtonDevice> GCButtonFactory::Create(const Common::ParamPackage& params) {
    const auto button_id = params.Get("button", 0);
    const auto port = static_cast<u32>(params.Get("port", 0));

    constexpr s32 PAD_STICK_ID = static_cast<s32>(GCAdapter::PadButton::Stick);

    // button is not an axis/stick button
    if (button_id != PAD_STICK_ID) {
        return std::make_unique<GCButton>(port, button_id, adapter.get());
    }

    // For Axis buttons, used by the binary sticks.
    if (button_id == PAD_STICK_ID) {
        const int axis = params.Get("axis", 0);
        const float threshold = params.Get("threshold", 0.25f);
        const std::string direction_name = params.Get("direction", "");
        bool trigger_if_greater;
        if (direction_name == "+") {
            trigger_if_greater = true;
        } else if (direction_name == "-") {
            trigger_if_greater = false;
        } else {
            trigger_if_greater = true;
            LOG_ERROR(Input, "Unknown direction {}", direction_name);
        }
        return std::make_unique<GCAxisButton>(port, axis, threshold, trigger_if_greater,
                                              adapter.get());
    }

    return nullptr;
}

Common::ParamPackage GCButtonFactory::GetNextInput() const {
    Common::ParamPackage params;
    GCAdapter::GCPadStatus pad;
    auto& queue = adapter->GetPadQueue();
    while (queue.Pop(pad)) {
        // This while loop will break on the earliest detected button
        params.Set("engine", "gcpad");
        params.Set("port", static_cast<s32>(pad.port));
        if (pad.button != GCAdapter::PadButton::Undefined) {
            params.Set("button", static_cast<u16>(pad.button));
        }

        // For Axis button implementation
        if (pad.axis != GCAdapter::PadAxes::Undefined) {
            params.Set("axis", static_cast<u8>(pad.axis));
            params.Set("button", static_cast<u16>(GCAdapter::PadButton::Stick));
            params.Set("threshold", "0.25");
            if (pad.axis_value > 0) {
                params.Set("direction", "+");
            } else {
                params.Set("direction", "-");
            }
            break;
        }
    }
    return params;
}

void GCButtonFactory::BeginConfiguration() {
    polling = true;
    adapter->BeginConfiguration();
}

void GCButtonFactory::EndConfiguration() {
    polling = false;
    adapter->EndConfiguration();
}

class GCAnalog final : public Input::AnalogDevice {
public:
    explicit GCAnalog(u32 port_, u32 axis_x_, u32 axis_y_, bool invert_x_, bool invert_y_,
                      float deadzone_, float range_, const GCAdapter::Adapter* adapter)
        : port(port_), axis_x(axis_x_), axis_y(axis_y_), invert_x(invert_x_), invert_y(invert_y_),
          deadzone(deadzone_), range(range_), gcadapter(adapter) {}

    float GetAxis(u32 axis) const {
        if (gcadapter->DeviceConnected(port)) {
            std::lock_guard lock{mutex};
            const auto axis_value =
                static_cast<float>(gcadapter->GetPadState(port).axis_values.at(axis));
            return (axis_value) / (100.0f * range);
        }
        return 0.0f;
    }

    std::pair<float, float> GetAnalog(u32 analog_axis_x, u32 analog_axis_y) const {
        float x = GetAxis(analog_axis_x);
        float y = GetAxis(analog_axis_y);
        if (invert_x) {
            x = -x;
        }
        if (invert_y) {
            y = -y;
        }
        // Make sure the coordinates are in the unit circle,
        // otherwise normalize it.
        float r = x * x + y * y;
        if (r > 1.0f) {
            r = std::sqrt(r);
            x /= r;
            y /= r;
        }

        return {x, y};
    }

    std::tuple<float, float> GetStatus() const override {
        const auto [x, y] = GetAnalog(axis_x, axis_y);
        const float r = std::sqrt((x * x) + (y * y));
        if (r > deadzone) {
            return {x / r * (r - deadzone) / (1 - deadzone),
                    y / r * (r - deadzone) / (1 - deadzone)};
        }
        return {0.0f, 0.0f};
    }

    std::tuple<float, float> GetRawStatus() const override {
        const float x = GetAxis(axis_x);
        const float y = GetAxis(axis_y);
        return {x, y};
    }

    Input::AnalogProperties GetAnalogProperties() const override {
        return {deadzone, range, 0.5f};
    }

    bool GetAnalogDirectionStatus(Input::AnalogDirection direction) const override {
        const auto [x, y] = GetStatus();
        const float directional_deadzone = 0.5f;
        switch (direction) {
        case Input::AnalogDirection::RIGHT:
            return x > directional_deadzone;
        case Input::AnalogDirection::LEFT:
            return x < -directional_deadzone;
        case Input::AnalogDirection::UP:
            return y > directional_deadzone;
        case Input::AnalogDirection::DOWN:
            return y < -directional_deadzone;
        }
        return false;
    }

private:
    const u32 port;
    const u32 axis_x;
    const u32 axis_y;
    const bool invert_x;
    const bool invert_y;
    const float deadzone;
    const float range;
    const GCAdapter::Adapter* gcadapter;
    mutable std::mutex mutex;
};

/// An analog device factory that creates analog devices from GC Adapter
GCAnalogFactory::GCAnalogFactory(std::shared_ptr<GCAdapter::Adapter> adapter_)
    : adapter(std::move(adapter_)) {}

/**
 * Creates analog device from joystick axes
 * @param params contains parameters for creating the device:
 *     - "port": the nth gcpad on the adapter
 *     - "axis_x": the index of the axis to be bind as x-axis
 *     - "axis_y": the index of the axis to be bind as y-axis
 */
std::unique_ptr<Input::AnalogDevice> GCAnalogFactory::Create(const Common::ParamPackage& params) {
    const auto port = static_cast<u32>(params.Get("port", 0));
    const auto axis_x = static_cast<u32>(params.Get("axis_x", 0));
    const auto axis_y = static_cast<u32>(params.Get("axis_y", 1));
    const auto deadzone = std::clamp(params.Get("deadzone", 0.0f), 0.0f, 1.0f);
    const auto range = std::clamp(params.Get("range", 1.0f), 0.50f, 1.50f);
    const std::string invert_x_value = params.Get("invert_x", "+");
    const std::string invert_y_value = params.Get("invert_y", "+");
    const bool invert_x = invert_x_value == "-";
    const bool invert_y = invert_y_value == "-";

    return std::make_unique<GCAnalog>(port, axis_x, axis_y, invert_x, invert_y, deadzone, range,
                                      adapter.get());
}

void GCAnalogFactory::BeginConfiguration() {
    polling = true;
    adapter->BeginConfiguration();
}

void GCAnalogFactory::EndConfiguration() {
    polling = false;
    adapter->EndConfiguration();
}

Common::ParamPackage GCAnalogFactory::GetNextInput() {
    GCAdapter::GCPadStatus pad;
    Common::ParamPackage params;
    auto& queue = adapter->GetPadQueue();
    while (queue.Pop(pad)) {
        if (pad.button != GCAdapter::PadButton::Undefined) {
            params.Set("engine", "gcpad");
            params.Set("port", static_cast<s32>(pad.port));
            params.Set("button", static_cast<u16>(pad.button));
            return params;
        }
        if (pad.axis == GCAdapter::PadAxes::Undefined ||
            std::abs(static_cast<float>(pad.axis_value) / 128.0f) < 0.1f) {
            continue;
        }
        // An analog device needs two axes, so we need to store the axis for later and wait for
        // a second input event. The axes also must be from the same joystick.
        const u8 axis = static_cast<u8>(pad.axis);
        if (axis == 0 || axis == 1) {
            analog_x_axis = 0;
            analog_y_axis = 1;
            controller_number = static_cast<s32>(pad.port);
            break;
        }
        if (axis == 2 || axis == 3) {
            analog_x_axis = 2;
            analog_y_axis = 3;
            controller_number = static_cast<s32>(pad.port);
            break;
        }

        if (analog_x_axis == -1) {
            analog_x_axis = axis;
            controller_number = static_cast<s32>(pad.port);
        } else if (analog_y_axis == -1 && analog_x_axis != axis &&
                   controller_number == static_cast<s32>(pad.port)) {
            analog_y_axis = axis;
            break;
        }
    }
    if (analog_x_axis != -1 && analog_y_axis != -1) {
        params.Set("engine", "gcpad");
        params.Set("port", controller_number);
        params.Set("axis_x", analog_x_axis);
        params.Set("axis_y", analog_y_axis);
        params.Set("invert_x", "+");
        params.Set("invert_y", "+");
        analog_x_axis = -1;
        analog_y_axis = -1;
        controller_number = -1;
        return params;
    }
    return params;
}

class GCVibration final : public Input::VibrationDevice {
public:
    explicit GCVibration(u32 port_, GCAdapter::Adapter* adapter)
        : port(port_), gcadapter(adapter) {}

    u8 GetStatus() const override {
        return gcadapter->RumblePlay(port, 0);
    }

    bool SetRumblePlay(f32 amp_low, [[maybe_unused]] f32 freq_low, f32 amp_high,
                       [[maybe_unused]] f32 freq_high) const override {
        const auto mean_amplitude = (amp_low + amp_high) * 0.5f;
        const auto processed_amplitude =
            static_cast<u8>((mean_amplitude + std::pow(mean_amplitude, 0.3f)) * 0.5f * 0x8);

        return gcadapter->RumblePlay(port, processed_amplitude);
    }

private:
    const u32 port;
    GCAdapter::Adapter* gcadapter;
};

/// An vibration device factory that creates vibration devices from GC Adapter
GCVibrationFactory::GCVibrationFactory(std::shared_ptr<GCAdapter::Adapter> adapter_)
    : adapter(std::move(adapter_)) {}

/**
 * Creates a vibration device from a joystick
 * @param params contains parameters for creating the device:
 *     - "port": the nth gcpad on the adapter
 */
std::unique_ptr<Input::VibrationDevice> GCVibrationFactory::Create(
    const Common::ParamPackage& params) {
    const auto port = static_cast<u32>(params.Get("port", 0));

    return std::make_unique<GCVibration>(port, adapter.get());
}

} // namespace InputCommon
