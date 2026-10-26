#include "vbi.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

static void put32(std::array<std::uint8_t, 0x40>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned int i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU);
    }
}
static void put64(std::array<std::uint8_t, 0x40>& bytes, std::size_t offset, std::uint64_t value) {
    put32(bytes, offset, static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
    put32(bytes, offset + 4, static_cast<std::uint32_t>(value >> 32U));
}

int main() {
    const auto temp = std::filesystem::temp_directory_path() / "someboxone_test_vbi.bin";
    std::array<std::uint8_t, 0x40> bytes{};
    bytes[0] = '1'; bytes[1] = 'I'; bytes[2] = 'B'; bytes[3] = 'V';
    put32(bytes, 0x04, 7);
    put32(bytes, 0x08, 0x34);
    put32(bytes, 0x0C, 0x40);
    put64(bytes, 0x10, 0x100000);
    put64(bytes, 0x18, 0xFFFF800000001000ULL);
    put32(bytes, 0x20, 0x34);
    put32(bytes, 0x24, 1);
    put32(bytes, 0x28, 1);
    put32(bytes, 0x2C, 0x34);
    put32(bytes, 0x30, 0x0C);
    {
        std::ofstream out(temp, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    std::string error;
    auto parsed = someboxone::read_vbi_header(temp, bytes.size(), error);
    const bool valid = parsed.has_value() &&
        parsed->minimum_version == 7 &&
        parsed->image_size == 0x40 &&
        parsed->base_physical_address == 0x100000ULL &&
        parsed->trampoline_virtual_address == 0xFFFF800000001000ULL &&
        parsed->directory_count == 1 &&
        parsed->directories.size() == 1 &&
        parsed->directories[0].offset == 0x34 &&
        parsed->directories[0].size == 0x0C &&
        parsed->warnings.empty() && error.empty();
    if (!valid) {
        std::cerr << "VBI parser test failed: " << error << '\n';
        if (parsed) for (const auto& warning : parsed->warnings) std::cerr << warning << '\n';
        std::filesystem::remove(temp);
        return 1;
    }

    // Excessive directory counts must be rejected before the table is read.
    put32(bytes, 0x28, 24);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    parsed = someboxone::read_vbi_header(temp, bytes.size(), error);
    if (parsed || error.find("23") == std::string::npos) {
        std::cerr << "VBI oversized directory-count guard test failed: " << error << '\n';
        std::filesystem::remove(temp);
        return 1;
    }

    // Offset + size validation must use wide arithmetic and become a warning,
    // rather than wrapping around to an apparently valid in-file range.
    put32(bytes, 0x28, 1);
    put32(bytes, 0x2C, 0xFFFFFFF0U);
    put32(bytes, 0x30, 0x40U);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    parsed = someboxone::read_vbi_header(temp, bytes.size(), error);
    bool has_bounds_warning = false;
    if (parsed) {
        for (const auto& warning : parsed->warnings) {
            if (warning.find("extends beyond the physical file") != std::string::npos) has_bounds_warning = true;
        }
    }
    std::filesystem::remove(temp);
    if (!parsed || !has_bounds_warning || !error.empty()) {
        std::cerr << "VBI directory bounds warning test failed: " << error << '\n';
        return 1;
    }
    return 0;
}
