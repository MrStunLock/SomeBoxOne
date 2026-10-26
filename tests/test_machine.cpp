#include "machine.hpp"

#include <cstdint>
#include <iostream>
#include <optional>

int main() {
    someboxone::GuestAddressSpace bus(1024);
    if (!bus.map_ram(0x1000, 0x100, "main RAM")) {
        std::cerr << "Could not map test RAM\n";
        return 1;
    }
    if (bus.map_ram(0x1080, 0x100, "overlap") || bus.map_ram(0x2000, 800, "over budget")) {
        std::cerr << "Address-space map validation failed\n";
        return 1;
    }
    if (!bus.write(0x1000, 4, 0x11223344ULL) || bus.read(0x1000, 4) != 0x11223344ULL ||
        bus.read(0x1001, 2) != 0x2233ULL || bus.read(0x10FE, 4).has_value() ||
        bus.read(0x4000, 1).has_value() || bus.write(0x1000, 3, 1)) {
        std::cerr << "RAM access/bounds test failed\n";
        return 1;
    }

    std::uint32_t register_value = 0x12345678U;
    const bool mmio_mapped = bus.map_mmio(
        0xF000, 0x100, "test register",
        [&register_value](std::uint64_t offset, std::uint8_t width) -> std::optional<std::uint64_t> {
            if (offset != 0 || width != 4) return std::nullopt;
            return register_value;
        },
        [&register_value](std::uint64_t offset, std::uint8_t width, std::uint64_t value) {
            if (offset != 0 || width != 4) return false;
            register_value = static_cast<std::uint32_t>(value);
            return true;
        });
    if (!mmio_mapped || bus.read(0xF000, 4) != 0x12345678ULL ||
        !bus.write(0xF000, 4, 0xAABBCCDDULL) || register_value != 0xAABBCCDDU ||
        bus.read(0xF000, 1).has_value() || bus.write(0xF000, 1, 0)) {
        std::cerr << "MMIO callback test failed\n";
        return 1;
    }

    const auto regions = bus.regions();
    if (regions.size() != 2 || regions[0].name != "main RAM" || regions[1].name != "test register") {
        std::cerr << "Address-space inventory test failed\n";
        return 1;
    }
    return 0;
}
