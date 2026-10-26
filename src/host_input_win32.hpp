#pragma once

#include "gamepad_input.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cstdint>

namespace someboxone {
namespace host_input_detail {
struct XInputGamepad {
    WORD buttons;
    BYTE left_trigger;
    BYTE right_trigger;
    SHORT left_x;
    SHORT left_y;
    SHORT right_x;
    SHORT right_y;
};
struct XInputState {
    DWORD packet_number;
    XInputGamepad gamepad;
};
using XInputGetStateFn = DWORD (WINAPI*)(DWORD, XInputState*);

inline XInputGetStateFn xinput_get_state() noexcept {
    static XInputGetStateFn fn = []() noexcept -> XInputGetStateFn {
        constexpr const wchar_t* dlls[] = {L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_3.dll"};
        for (const auto* name : dlls) {
            HMODULE module = LoadLibraryW(name);
            if (!module) continue;
            auto* symbol = GetProcAddress(module, "XInputGetState");
            if (symbol) return reinterpret_cast<XInputGetStateFn>(symbol);
            FreeLibrary(module);
        }
        return nullptr;
    }();
    return fn;
}

inline bool key_down(int virtual_key) noexcept {
    return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

inline float keyboard_axis(bool positive, bool negative) noexcept {
    return positive == negative ? 0.0F : (positive ? 1.0F : -1.0F);
}
} // namespace host_input_detail

// Polls one connected XInput device plus keyboard bindings. Keyboard input is
// accepted while the main SomeBoxOne process or separate graphics-window process
// owns the foreground window, avoiding keys leaking from unrelated applications.
inline HostGamepadState poll_host_gamepad_input(DWORD display_process_id = 0) noexcept {
    HostGamepadState out{};
    if (const auto get_state = host_input_detail::xinput_get_state()) {
        for (DWORD index = 0; index < 4; ++index) {
            host_input_detail::XInputState state{};
            if (get_state(index, &state) != ERROR_SUCCESS) continue;
            const auto& pad = state.gamepad;
            out.buttons |= map_xinput_buttons(pad.buttons);
            out.left_trigger = static_cast<float>(pad.left_trigger) / 255.0F;
            out.right_trigger = static_cast<float>(pad.right_trigger) / 255.0F;
            out.left_x = normalize_xinput_axis(pad.left_x, 7849);
            out.left_y = normalize_xinput_axis(pad.left_y, 7849);
            out.right_x = normalize_xinput_axis(pad.right_x, 8689);
            out.right_y = normalize_xinput_axis(pad.right_y, 8689);
            break; // one virtual gamepad exposed to the guest runtime
        }
    }

    DWORD foreground_pid = 0;
    const HWND foreground = GetForegroundWindow();
    if (foreground) GetWindowThreadProcessId(foreground, &foreground_pid);
    const bool keyboard_allowed = foreground_pid == GetCurrentProcessId() ||
        (display_process_id != 0 && foreground_pid == display_process_id);
    if (!keyboard_allowed) return out;

    using host_input_detail::key_down;
    const bool up = key_down(VK_UP) || key_down('W');
    const bool down = key_down(VK_DOWN) || key_down('S');
    const bool left = key_down(VK_LEFT) || key_down('A');
    const bool right = key_down(VK_RIGHT) || key_down('D');
    if (key_down(VK_RETURN)) out.buttons |= gamepad_button::menu;
    if (key_down(VK_ESCAPE)) out.buttons |= gamepad_button::view;
    if (up) out.buttons |= gamepad_button::up;
    if (down) out.buttons |= gamepad_button::down;
    if (left) out.buttons |= gamepad_button::left;
    if (right) out.buttons |= gamepad_button::right;
    if (key_down(VK_SPACE) || key_down('Z')) out.buttons |= gamepad_button::a;
    if (key_down('X')) out.buttons |= gamepad_button::b;
    if (key_down('C')) out.buttons |= gamepad_button::x;
    if (key_down('V')) out.buttons |= gamepad_button::y;
    if (key_down('Q')) out.buttons |= gamepad_button::left_shoulder;
    if (key_down('E')) out.buttons |= gamepad_button::right_shoulder;
    if (key_down(VK_SHIFT)) out.left_trigger = 1.0F;
    if (key_down(VK_CONTROL)) out.right_trigger = 1.0F;

    if (up || down || left || right) {
        out.left_x = host_input_detail::keyboard_axis(right, left);
        out.left_y = host_input_detail::keyboard_axis(up, down);
    }
    const bool right_stick_left = key_down('J'), right_stick_right = key_down('L');
    const bool right_stick_up = key_down('I'), right_stick_down = key_down('K');
    if (right_stick_left || right_stick_right) out.right_x = host_input_detail::keyboard_axis(right_stick_right, right_stick_left);
    if (right_stick_up || right_stick_down) out.right_y = host_input_detail::keyboard_axis(right_stick_up, right_stick_down);
    return out;
}
} // namespace someboxone
#else
namespace someboxone {
inline HostGamepadState poll_host_gamepad_input(std::uint32_t = 0) noexcept { return {}; }
}
#endif
