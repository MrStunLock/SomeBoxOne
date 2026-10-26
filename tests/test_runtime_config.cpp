#include "runtime_config.hpp"
#include <cassert>
#include <iostream>

int main() {
    const auto parsed = someboxone::parse_runtime_config(
        "GraphicsWidth=1600\nGraphicsHeight=900\n"
        "MasterVolumePercent=37\nGameVolumePercent=82\n"
        "KeepLogs=false\nGraphicsBackend=vulkan\n");
    assert(parsed.graphics_width == 1600);
    assert(parsed.graphics_height == 900);
    assert(parsed.master_volume_percent == 37);
    assert(parsed.game_volume_percent == 82);
    assert(!parsed.keep_logs);
    assert(parsed.graphics_backend == "vulkan");

    const auto serialized = someboxone::serialize_runtime_config(parsed);
    const auto round_trip = someboxone::parse_runtime_config(serialized);
    assert(round_trip.master_volume_percent == 37);
    assert(round_trip.game_volume_percent == 82);
    assert(!round_trip.keep_logs);
    assert(round_trip.graphics_backend == "vulkan");

    const auto clamped = someboxone::parse_runtime_config(
        "MasterVolumePercent=101\nGameVolumePercent=4294967295\nGraphicsBackend=unknown\n");
    assert(clamped.master_volume_percent == 100);
    assert(clamped.game_volume_percent == 100);
    assert(clamped.graphics_backend == "d3d11");

    std::cout << "Runtime settings parsing/serialization tests passed.\n";
    return 0;
}
