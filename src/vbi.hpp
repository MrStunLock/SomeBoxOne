#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace someboxone {

struct VbiDirectoryEntry {
    std::uint32_t offset{};
    std::uint32_t size{};
};

struct VbiHeader {
    std::uint32_t minimum_version{};
    std::uint32_t header_size{};
    std::uint32_t image_size{};
    std::uint64_t base_physical_address{};
    std::uint64_t trampoline_virtual_address{};
    std::uint32_t data_offset{};
    std::uint32_t flags{};
    std::uint32_t directory_count{};
    std::vector<VbiDirectoryEntry> directories;
    std::vector<std::string> warnings;
};

inline std::uint32_t read_u32_le(const std::array<std::uint8_t, 0x100>& b, std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) |
           (static_cast<std::uint32_t>(b[o + 1]) << 8U) |
           (static_cast<std::uint32_t>(b[o + 2]) << 16U) |
           (static_cast<std::uint32_t>(b[o + 3]) << 24U);
}

inline std::uint64_t read_u64_le(const std::array<std::uint8_t, 0x100>& b, std::size_t o) {
    return static_cast<std::uint64_t>(read_u32_le(b, o)) |
           (static_cast<std::uint64_t>(read_u32_le(b, o + 4)) << 32U);
}

inline bool has_vbi_magic(const std::array<std::uint8_t, 4>& magic) {
    return magic == std::array<std::uint8_t, 4>{'1', 'I', 'B', 'V'};
}

// Parses metadata only. Directory offsets are treated as file-relative for basic
// bounds diagnostics; this does not validate cryptographic signatures or bootability.
inline std::optional<VbiHeader> read_vbi_header(const std::filesystem::path& path,
                                                 std::uintmax_t file_size,
                                                 std::string& error) {
    constexpr std::size_t fixed_header_size = 0x2C;
    constexpr std::size_t directory_entry_size = 8;
    // Public format documentation reserves 0xB8 bytes for directory entries (23 * 8).
    constexpr std::uint32_t max_directory_entries = 23;

    error.clear();
    if (file_size < fixed_header_size) {
        error = "File is too small to contain a complete fixed VBI header.";
        return std::nullopt;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Could not open file for reading.";
        return std::nullopt;
    }

    std::array<std::uint8_t, 0x100> header_bytes{};
    in.read(reinterpret_cast<char*>(header_bytes.data()), static_cast<std::streamsize>(fixed_header_size));
    if (in.gcount() != static_cast<std::streamsize>(fixed_header_size)) {
        error = "Could not read the complete fixed VBI header.";
        return std::nullopt;
    }

    const std::array<std::uint8_t, 4> magic{
        header_bytes[0], header_bytes[1], header_bytes[2], header_bytes[3]};
    if (!has_vbi_magic(magic)) {
        error = "VBI magic does not match the documented 1IBV signature.";
        return std::nullopt;
    }

    VbiHeader h;
    h.minimum_version = read_u32_le(header_bytes, 0x04);
    h.header_size = read_u32_le(header_bytes, 0x08);
    h.image_size = read_u32_le(header_bytes, 0x0C);
    h.base_physical_address = read_u64_le(header_bytes, 0x10);
    h.trampoline_virtual_address = read_u64_le(header_bytes, 0x18);
    h.data_offset = read_u32_le(header_bytes, 0x20);
    h.flags = read_u32_le(header_bytes, 0x24);
    h.directory_count = read_u32_le(header_bytes, 0x28);

    if (h.directory_count > max_directory_entries) {
        error = "VBI directory count exceeds the documented table capacity (23); refusing to parse entries.";
        return std::nullopt;
    }
    const std::uint64_t table_end = static_cast<std::uint64_t>(fixed_header_size) +
                                    static_cast<std::uint64_t>(h.directory_count) * directory_entry_size;
    if (table_end > file_size) {
        error = "VBI directory table extends beyond the end of the file.";
        return std::nullopt;
    }

    if (h.header_size < table_end) {
        h.warnings.emplace_back("Header-size field is smaller than the fixed header plus directory table.");
    }
    if (h.header_size > file_size) {
        h.warnings.emplace_back("Header-size field extends beyond the physical file size.");
    }
    if (h.image_size != 0 && h.image_size > file_size) {
        h.warnings.emplace_back("Image-size field is larger than the physical file; it may describe a mapped image differently.");
    }
    if (h.data_offset > file_size) {
        h.warnings.emplace_back("Data offset lies beyond the end of the physical file.");
    } else if (h.data_offset != 0 && h.data_offset < table_end) {
        h.warnings.emplace_back("Data offset overlaps the fixed header or directory table (assuming file-relative offsets).");
    }

    for (std::uint32_t i = 0; i < h.directory_count; ++i) {
        const auto base = fixed_header_size + static_cast<std::size_t>(i) * directory_entry_size;
        std::array<std::uint8_t, directory_entry_size> raw_entry{};
        in.seekg(static_cast<std::streamoff>(base), std::ios::beg);
        in.read(reinterpret_cast<char*>(raw_entry.data()), static_cast<std::streamsize>(raw_entry.size()));
        if (in.gcount() != static_cast<std::streamsize>(raw_entry.size())) {
            error = "Could not read a complete VBI directory entry.";
            return std::nullopt;
        }
        const auto read_entry_u32 = [&raw_entry](std::size_t offset) -> std::uint32_t {
            return static_cast<std::uint32_t>(raw_entry[offset]) |
                   (static_cast<std::uint32_t>(raw_entry[offset + 1]) << 8U) |
                   (static_cast<std::uint32_t>(raw_entry[offset + 2]) << 16U) |
                   (static_cast<std::uint32_t>(raw_entry[offset + 3]) << 24U);
        };
        const VbiDirectoryEntry entry{read_entry_u32(0), read_entry_u32(4)};
        h.directories.push_back(entry);
        // 64-bit arithmetic ensures offset + size cannot wrap at 32 bits.
        if (static_cast<std::uint64_t>(entry.offset) + entry.size > file_size) {
            h.warnings.emplace_back("Directory entry " + std::to_string(i) +
                                    " extends beyond the physical file (assuming file-relative offsets).");
        }
    }
    return h;
}

} // namespace someboxone
