#pragma once

#include <cstdint>
#include <functional>

namespace someboxone {

// Windows.Gaming.Input.GamepadButtons-compatible values used by this runtime's
// WinRT GamepadReading object. Kept platform-neutral so scripts, tests and host
// input backends all share one mapping.
struct HostGamepadState {
    std::uint32_t buttons{};
    float left_trigger{};
    float right_trigger{};
    float left_x{};
    float left_y{};
    float right_x{};
    float right_y{};
};

namespace gamepad_button {
constexpr std::uint32_t menu = 0x0001;
constexpr std::uint32_t view = 0x0002;
constexpr std::uint32_t a = 0x0004;
constexpr std::uint32_t b = 0x0008;
constexpr std::uint32_t x = 0x0010;
constexpr std::uint32_t y = 0x0020;
constexpr std::uint32_t up = 0x0040;
constexpr std::uint32_t down = 0x0080;
constexpr std::uint32_t left = 0x0100;
constexpr std::uint32_t right = 0x0200;
constexpr std::uint32_t left_shoulder = 0x0400;
constexpr std::uint32_t right_shoulder = 0x0800;
constexpr std::uint32_t left_thumb = 0x1000;
constexpr std::uint32_t right_thumb = 0x2000;
}

// XInput button masks are deliberately translated here rather than copied into
// GamepadReading, whose flags use the Windows.Gaming.Input values above.
inline std::uint32_t map_xinput_buttons(std::uint16_t bits) noexcept {
    std::uint32_t out = 0;
    if (bits & 0x0010) out |= gamepad_button::menu;          // START
    if (bits & 0x0020) out |= gamepad_button::view;          // BACK
    if (bits & 0x1000) out |= gamepad_button::a;
    if (bits & 0x2000) out |= gamepad_button::b;
    if (bits & 0x4000) out |= gamepad_button::x;
    if (bits & 0x8000) out |= gamepad_button::y;
    if (bits & 0x0001) out |= gamepad_button::up;
    if (bits & 0x0002) out |= gamepad_button::down;
    if (bits & 0x0004) out |= gamepad_button::left;
    if (bits & 0x0008) out |= gamepad_button::right;
    if (bits & 0x0100) out |= gamepad_button::left_shoulder;
    if (bits & 0x0200) out |= gamepad_button::right_shoulder;
    if (bits & 0x0040) out |= gamepad_button::left_thumb;
    if (bits & 0x0080) out |= gamepad_button::right_thumb;
    return out;
}

inline float normalize_xinput_axis(std::int16_t value, std::int16_t deadzone) noexcept {
    const float normalized = value < 0
        ? static_cast<float>(value) / 32768.0F
        : static_cast<float>(value) / 32767.0F;
    const float dz = static_cast<float>(deadzone) / 32768.0F;
    return (normalized > -dz && normalized < dz) ? 0.0F : normalized;
}

} // namespace someboxone
