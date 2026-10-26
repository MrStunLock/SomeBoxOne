#pragma once

#include "xvd.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace someboxone {

// Read-only XVC descriptor support based on publicly documented structures.
// XVCs reuse the XVD fixed header; game content regions can remain encrypted.
inline constexpr std::uint64_t xvc_info_fixed_size = 0xDA8;
inline constexpr std::uint64_t xvc_region_header_size = 0x80;
inline constexpr std::uint64_t xvc_update_segment_size = 0x0C;
inline constexpr std::uint64_t xvc_region_specifier_size = 0x188;
inline constexpr std::uint64_t xvc_metadata_read_limit = 16ULL * 1024ULL * 1024ULL;

inline bool xvc_content_type(std::uint32_t value) {
    // XVC-enabled content types documented by public XVD tooling.
    switch (value) {
    case 1:  // Title
    case 6:  // Application
    case 15: // MteApp
    case 16: // MteTitle
    case 24: // AppDlc
    case 25: // TitleDlc
    case 26: // UniversalDlc
        return true;
    default:
        return false;
    }
}

struct XvcRegionInfo {
    std::uint32_t id{};
    std::uint16_t key_id{};
    std::uint32_t flags{};
    std::uint32_t first_segment_index{};
    std::string description;
    std::uint64_t offset{};
    std::uint64_t length{};
    std::uint64_t hash{};
    bool extent_in_file{};
};

struct XvcDescriptorInfo {
    std::array<std::uint8_t, 16> content_id{};
    std::string description;
    std::uint32_t version{};
    std::uint32_t region_count{};
    std::uint32_t flags{};
    std::uint16_t key_count{};
    std::uint32_t initial_play_region_id{};
    std::uint64_t initial_play_offset{};
    std::uint64_t file_time_created_raw{};
    std::uint32_t preview_region_id{};
    std::uint32_t update_segment_count{};
    std::uint64_t preview_offset{};
    std::uint32_t region_specifier_count{};
    std::vector<std::array<std::uint8_t, 16>> key_ids;
    std::vector<XvcRegionInfo> regions;
    std::vector<std::uint32_t> update_page_numbers;
    std::vector<std::string> warnings;
};

inline std::string xvc_text_utf16le(const std::vector<std::uint8_t>& bytes,
                                    std::size_t offset, std::size_t byte_count) {
    std::string value;
    if (offset > bytes.size() || byte_count > bytes.size() - offset) return value;
    for (std::size_t i = 0; i + 1 < byte_count; i += 2) {
        const auto code = static_cast<std::uint16_t>(bytes[offset + i]) |
                          (static_cast<std::uint16_t>(bytes[offset + i + 1]) << 8U);
        if (code == 0) break;
        if (code >= 0x20 && code <= 0x7E) value.push_back(static_cast<char>(code));
        else value.push_back('?'); // preserve a printable report without pretending to decode Unicode fully
    }
    return value;
}

inline std::string xvc_text_ascii(const std::vector<std::uint8_t>& bytes,
                                  std::size_t offset, std::size_t byte_count) {
    std::string value;
    if (offset > bytes.size() || byte_count > bytes.size() - offset) return value;
    for (std::size_t i = 0; i < byte_count; ++i) {
        const auto c = bytes[offset + i];
        if (c == 0) break;
        value.push_back((c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '.');
    }
    return value;
}

inline bool xvc_range_fits(std::uint64_t offset, std::uint64_t length,
                           std::uint64_t extent) {
    return offset <= extent && length <= extent - offset;
}

inline std::optional<XvcDescriptorInfo> read_xvc_descriptor(
    const std::filesystem::path& path, std::uint64_t file_size,
    const XvdHeaderInfo& header, std::string& error) {
    error.clear();
    if (!header.layout_estimated) {
        error = "XVC descriptor offset is unavailable because the XVD layout could not be calculated.";
        return std::nullopt;
    }
    if (header.xvc_data_length < xvc_info_fixed_size) {
        error = "Declared XVC metadata region is too small for the known descriptor fields.";
        return std::nullopt;
    }
    if (!xvc_range_fits(header.xvc_info_offset, header.xvc_data_length, file_size)) {
        error = "Declared XVC metadata region extends beyond the physical file.";
        return std::nullopt;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Could not open the XVC file for read-only metadata parsing.";
        return std::nullopt;
    }
    if (header.xvc_info_offset > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)())) {
        error = "XVC descriptor offset exceeds the host stream offset range.";
        return std::nullopt;
    }
    input.seekg(static_cast<std::streamoff>(header.xvc_info_offset), std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(xvc_info_fixed_size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        error = "Short read while reading the fixed XVC descriptor fields.";
        return std::nullopt;
    }

    XvcDescriptorInfo info;
    for (std::size_t i = 0; i < info.content_id.size(); ++i) info.content_id[i] = bytes[i];
    info.description = xvc_text_utf16le(bytes, 0xC10, 0x100);
    info.version = xvd_u32_le(bytes, 0xD10);
    info.region_count = xvd_u32_le(bytes, 0xD14);
    info.flags = xvd_u32_le(bytes, 0xD18);
    info.key_count = xvd_u16_le(bytes, 0xD1E);
    info.initial_play_region_id = xvd_u32_le(bytes, 0xD24);
    info.initial_play_offset = xvd_u64_le(bytes, 0xD28);
    info.file_time_created_raw = xvd_u64_le(bytes, 0xD30);
    info.preview_region_id = xvd_u32_le(bytes, 0xD38);
    info.update_segment_count = xvd_u32_le(bytes, 0xD3C);
    info.preview_offset = xvd_u64_le(bytes, 0xD40);
    info.region_specifier_count = xvd_u32_le(bytes, 0xD50);

    if (info.version != 1 && info.version != 2) {
        error = "XVC descriptor version " + std::to_string(info.version) +
                " is not one of the currently understood versions (1 or 2).";
        return std::nullopt;
    }
    if (info.region_count > 4096U || info.update_segment_count > 1000000U ||
        (info.version >= 2 && info.region_specifier_count > 8192U)) {
        error = "XVC descriptor table counts exceed the parser's safety limits.";
        return std::nullopt;
    }
    if (info.key_count > 0xC0U) {
        info.warnings.emplace_back("Key count exceeds the 192 key identifiers present in the known XVC descriptor field; only on-disk metadata is reported.");
    }

    std::uint64_t region_bytes = static_cast<std::uint64_t>(info.region_count) * xvc_region_header_size;
    std::uint64_t update_bytes = static_cast<std::uint64_t>(info.update_segment_count) * xvc_update_segment_size;
    std::uint64_t specifier_bytes = info.version >= 2
        ? static_cast<std::uint64_t>(info.region_specifier_count) * xvc_region_specifier_size : 0;
    std::uint64_t required = xvc_info_fixed_size;
    if (!xvd_checked_add(required, region_bytes, required) ||
        !xvd_checked_add(required, update_bytes, required) ||
        !xvd_checked_add(required, specifier_bytes, required) ||
        required > header.xvc_data_length) {
        error = "XVC descriptor table lengths do not fit within the declared XVC metadata region.";
        return std::nullopt;
    }
    if (required > xvc_metadata_read_limit) {
        error = "XVC metadata tables exceed the 16 MiB parser read limit.";
        return std::nullopt;
    }

    // Capture key IDs as GUID byte arrays; these are identifiers, not decryption keys.
    const auto keys_to_read = (std::min<std::uint16_t>)(info.key_count, 0xC0U);
    info.key_ids.reserve(keys_to_read);
    for (std::uint16_t i = 0; i < keys_to_read; ++i) {
        std::array<std::uint8_t, 16> key{};
        const std::size_t at = 0x10 + static_cast<std::size_t>(i) * 0x10;
        for (std::size_t j = 0; j < key.size(); ++j) key[j] = bytes[at + j];
        if (std::any_of(key.begin(), key.end(), [](std::uint8_t b) { return b != 0; })) {
            info.key_ids.push_back(key);
        }
    }

    const std::uint64_t stream_offset_max = static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)());
    const std::uint64_t region_table_offset = header.xvc_info_offset + xvc_info_fixed_size;
    const std::uint64_t region_end = region_table_offset + region_bytes;
    if (region_table_offset > stream_offset_max || region_end > stream_offset_max) {
        error = "XVC region-table offset exceeds the host stream offset range.";
        return std::nullopt;
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(region_table_offset), std::ios::beg);
    std::vector<std::uint8_t> region_table(static_cast<std::size_t>(region_bytes));
    if (!region_table.empty()) {
        input.read(reinterpret_cast<char*>(region_table.data()), static_cast<std::streamsize>(region_table.size()));
        if (input.gcount() != static_cast<std::streamsize>(region_table.size())) {
            error = "Short read while reading XVC region headers.";
            return std::nullopt;
        }
    }
    info.regions.reserve(info.region_count);
    for (std::uint32_t i = 0; i < info.region_count; ++i) {
        const auto at = static_cast<std::size_t>(i) * static_cast<std::size_t>(xvc_region_header_size);
        XvcRegionInfo region;
        region.id = xvd_u32_le(region_table, at);
        region.key_id = xvd_u16_le(region_table, at + 4);
        region.flags = xvd_u32_le(region_table, at + 8);
        region.first_segment_index = xvd_u32_le(region_table, at + 0x0C);
        region.description = xvc_text_utf16le(region_table, at + 0x10, 0x40);
        region.offset = xvd_u64_le(region_table, at + 0x50);
        region.length = xvd_u64_le(region_table, at + 0x58);
        region.hash = xvd_u64_le(region_table, at + 0x60);
        region.extent_in_file = xvc_range_fits(region.offset, region.length, file_size);
        if ((region.flags & 0x10U) != 0 && !region.extent_in_file) {
            info.warnings.emplace_back("Region " + std::to_string(i) +
                " is marked present but its declared extent falls outside the physical file.");
        }
        if (region.key_id != 0xFFFFU && region.key_id >= info.key_count) {
            info.warnings.emplace_back("Region " + std::to_string(i) +
                " references a key index not covered by the descriptor key count.");
        }
        info.regions.push_back(std::move(region));
    }

    const std::uint64_t update_table_offset = region_end;
    if (update_table_offset > stream_offset_max || update_table_offset + update_bytes > stream_offset_max) {
        error = "XVC update-table offset exceeds the host stream offset range.";
        return std::nullopt;
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(update_table_offset), std::ios::beg);
    std::vector<std::uint8_t> update_table(static_cast<std::size_t>(update_bytes));
    if (!update_table.empty()) {
        input.read(reinterpret_cast<char*>(update_table.data()), static_cast<std::streamsize>(update_table.size()));
        if (input.gcount() != static_cast<std::streamsize>(update_table.size())) {
            error = "Short read while reading XVC update segments.";
            return std::nullopt;
        }
    }
    info.update_page_numbers.reserve(info.update_segment_count);
    for (std::uint32_t i = 0; i < info.update_segment_count; ++i) {
        const auto at = static_cast<std::size_t>(i) * static_cast<std::size_t>(xvc_update_segment_size);
        info.update_page_numbers.push_back(xvd_u32_le(update_table, at));
    }

    // Version 2 has an additional table. We only validate its bounded extent here;
    // key/value decoding is left for a later milestone to avoid assuming encodings.
    if (specifier_bytes != 0) {
        const std::uint64_t specifier_offset = update_table_offset + update_bytes;
        if (specifier_offset > stream_offset_max || !xvc_range_fits(specifier_offset, specifier_bytes, file_size)) {
            error = "XVC region-specifier table extends beyond the physical file.";
            return std::nullopt;
        }
    }
    if (info.key_count == 0) {
        info.warnings.emplace_back("Descriptor reports no region encryption keys; this does not prove that all payload data is plaintext.");
    }
    return info;
}

} // namespace someboxone
