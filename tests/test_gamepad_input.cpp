#include "gamepad_input.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>

int main() {
    using namespace someboxone;
    const std::uint16_t bits = 0x0010 | 0x0020 | 0x1000 | 0x2000 | 0x0001 | 0x0200;
    const std::uint32_t mapped = map_xinput_buttons(bits);
    const std::uint32_t expected = gamepad_button::menu | gamepad_button::view |
        gamepad_button::a | gamepad_button::b | gamepad_button::up | gamepad_button::right_shoulder;
    if (mapped != expected) {
        std::cerr << "XInput button mapping failed: " << mapped << " != " << expected << '\n';
        return 1;
    }
    if (normalize_xinput_axis(0, 7849) != 0.0F || normalize_xinput_axis(4000, 7849) != 0.0F ||
        normalize_xinput_axis(32767, 7849) < 0.999F || normalize_xinput_axis(-32768, 7849) > -0.999F) {
        std::cerr << "XInput axis normalization/deadzone failed\n";
        return 1;
    }
    std::cout << "Gamepad input mapping tests passed\n";
    return 0;
}
