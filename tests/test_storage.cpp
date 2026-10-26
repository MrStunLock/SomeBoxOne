#include "xvd_storage.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void write_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned int i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xFFU);
}

static void write_u64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned int i = 0; i < 8; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xFFU);
}

static fs::path make_xvd_fixture(const std::string& suffix, std::uint32_t flags,
                                 std::uint32_t type, std::uint32_t dynamic_header_length) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path path = fs::temp_directory_path() /
        ("someboxone-storage-" + suffix + "-" + std::to_string(stamp) + ".xvd");
    std::vector<std::uint8_t> bytes(0x6000, 0);
    constexpr std::size_t header = 0x200;
    constexpr char magic[] = "msft-xvd";
    for (std::size_t i = 0; i < 8; ++i) bytes[header + i] = static_cast<std::uint8_t>(magic[i]);
    write_u32(bytes, header + 0x08, flags);
    write_u32(bytes, header + 0x0C, 3);
    write_u64(bytes, header + 0x18, 0x1000);
    write_u32(bytes, header + 0x80, type);
    write_u32(bytes, header + 0x84, 2);
    write_u32(bytes, header + 0x94, dynamic_header_length);
    write_u32(bytes, header + 0x98, 0xAA000);
    // Plaintext fixed-volume payload at calculated offset 0x3000.
    if (type == 0) {
        bytes[0x3000] = 0xDE; bytes[0x3001] = 0xAD;
        bytes[0x3002] = 0xBE; bytes[0x3003] = 0xEF;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return path;
}

int main() {
    // Read-only fixed, unencrypted synthetic XVD: logical offset zero maps to
    // the calculated data base. No real Microsoft image is modified.
    const auto fixed = make_xvd_fixture("fixed", 0x06, 0, 0);
    const auto fixed_probe = someboxone::ReadOnlyXvdVolume::probe(fixed);
    if (!fixed_probe.header_recognized || !fixed_probe.logical_read_supported ||
        fixed_probe.data_encrypted || fixed_probe.drive_size != 0x1000 ||
        fixed_probe.data_offset != 0x3000) {
        std::cerr << "Fixed plaintext XVD probe failed: " << fixed_probe.detail << '\n';
        fs::remove(fixed);
        return 1;
    }

    std::string error;
    const auto volume = someboxone::ReadOnlyXvdVolume::open(fixed, &error);
    if (!volume) {
        std::cerr << "Could not open synthetic fixed volume: " << error << '\n';
        fs::remove(fixed);
        return 1;
    }
    const auto first = volume->read_at(0, 4, &error);
    if (!first || *first != std::vector<std::uint8_t>({0xDE, 0xAD, 0xBE, 0xEF}) ||
        volume->read_at(0xFFF, 2, &error) ||
        volume->read_at(0, someboxone::ReadOnlyXvdVolume::max_read_bytes + 1, &error)) {
        std::cerr << "Read-only logical read/bounds check failed.\n";
        fs::remove(fixed);
        return 1;
    }

    // Retail-style encrypted dynamic images must not expose raw BAT words as
    // logical offsets. The implementation explicitly refuses them.
    const auto encrypted_dynamic = make_xvd_fixture("encrypted-dynamic", 0x01, 1, 0x1000);
    const auto encrypted_probe = someboxone::ReadOnlyXvdVolume::probe(encrypted_dynamic);
    if (!encrypted_probe.header_recognized || encrypted_probe.logical_read_supported ||
        encrypted_probe.detail.find("encryption is enabled") == std::string::npos) {
        std::cerr << "Encrypted dynamic image wasn't safely refused: " << encrypted_probe.detail << '\n';
        fs::remove(fixed); fs::remove(encrypted_dynamic);
        return 1;
    }

    const auto plaintext_dynamic = make_xvd_fixture("plaintext-dynamic", 0x07, 1, 0x1000);
    const auto dynamic_probe = someboxone::ReadOnlyXvdVolume::probe(plaintext_dynamic);
    if (!dynamic_probe.header_recognized || dynamic_probe.logical_read_supported ||
        dynamic_probe.detail.find("BAT-to-file mapping is not implemented") == std::string::npos) {
        std::cerr << "Dynamic BAT image wasn't safely refused: " << dynamic_probe.detail << '\n';
        fs::remove(fixed); fs::remove(encrypted_dynamic); fs::remove(plaintext_dynamic);
        return 1;
    }

    fs::remove(fixed);
    fs::remove(encrypted_dynamic);
    fs::remove(plaintext_dynamic);
    std::cout << "SomeBoxOne read-only XVD storage tests passed (fixed plaintext reads; encrypted/dynamic access safely refused).\n";
    return 0;
}
