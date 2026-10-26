#include "inspector.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void write_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned int i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xFFU);
    }
}

static void write_u64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned int i = 0; i < 8; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xFFU);
    }
}

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("someboxone-scan-test-" + std::to_string(stamp));
    fs::create_directories(root / "System Update");
    const fs::path fixture = root / "System Update" / "system image.xvd";

    // Minimal synthetic dynamic/encrypted SystemOS XVD header. Not a real image.
    std::vector<std::uint8_t> bytes(0x6000, 0);
    constexpr std::size_t header = 0x200;
    constexpr char magic[] = "msft-xvd";
    for (std::size_t i = 0; i < 8; ++i) bytes[header + i] = static_cast<std::uint8_t>(magic[i]);
    write_u32(bytes, header + 0x08, 0x01); // read-only; encryption flag clear -> encryption enabled
    write_u32(bytes, header + 0x0C, 3);
    write_u64(bytes, header + 0x18, 0x1000);
    write_u32(bytes, header + 0x80, 1); // Dynamic
    write_u32(bytes, header + 0x84, 2); // SystemOS
    write_u32(bytes, header + 0x94, 0x1000);
    write_u32(bytes, header + 0x98, 0xAA000);
    {
        std::ofstream out(fixture, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            std::cerr << "Could not write synthetic scan fixture.\n";
            fs::remove_all(root);
            return 1;
        }
    }

    const auto result = someboxone::scan_directory(root);
    const bool checks_pass =
        result.exit_code == 0 &&
        result.report.find("Content type | Encryption | Hash sample") != std::string::npos &&
        result.report.find("system image.xvd") != std::string::npos &&
        result.report.find("SystemOS") != std::string::npos &&
        result.report.find("Enabled (header flag clear)") != std::string::npos &&
        result.report.find("does not establish signature authenticity") != std::string::npos;

    fs::remove_all(root);
    if (!checks_pass) {
        std::cerr << "Scan report did not include per-image encryption/hash metadata or limitations.\n"
                  << result.report << '\n';
        return 1;
    }
    std::cout << "SomeBoxOne scan metadata tests passed.\n";
    return 0;
}
