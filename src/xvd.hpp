#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include <unordered_set>

#include "sha256.hpp"

namespace someboxone {

// The XVD signature is the first 0x200 bytes. Known fixed metadata fields occupy
// 0xE00 bytes beginning at 0x200, while public XVD implementations reserve a
// nominal 0x2E00-byte header region after the signature (payload base 0x3000).
inline constexpr std::uint64_t xvd_header_start = 0x200;
inline constexpr std::size_t xvd_header_body_size = 0xE00;
inline constexpr std::uint64_t xvd_nominal_header_size = 0x2E00;
inline constexpr std::uint64_t xvd_nominal_payload_offset = 0x3000;
inline constexpr std::uint64_t xvd_page_size = 0x1000;
inline constexpr std::uint64_t xvd_block_size = 0xAA000;
inline constexpr std::uint32_t xvd_invalid_bat_entry = 0xFFFFFFFFU;
inline constexpr std::uint64_t xvd_fixed_fields_end_offset = xvd_header_start + xvd_header_body_size;
inline constexpr std::uint64_t xvd_nominal_header_end_offset = xvd_nominal_payload_offset;

struct XvdExtensionEntry {
    std::uint32_t code{};
    std::uint32_t length{};
    std::uint64_t offset{};
    std::uint32_t data_length{};
    std::uint32_t reserved{};
};

struct XvdHeaderInfo {
    std::uint32_t volume_flags{};
    std::uint32_t format_version{};
    std::uint64_t file_time_created_raw{};
    std::uint64_t drive_size{};
    std::array<std::uint8_t, 16> vduid{};
    std::array<std::uint8_t, 16> uduid{};
    std::array<std::uint8_t, 32> top_hash_block_hash{};
    std::array<std::uint8_t, 32> original_xvc_data_hash{};
    std::uint32_t type{};
    std::uint32_t content_type{};
    std::uint32_t embedded_xvd_length{};
    std::uint32_t user_data_length{};
    std::uint32_t xvc_data_length{};
    std::uint32_t dynamic_header_length{};
    std::uint32_t block_size{};
    std::uint64_t header_offset{};
    std::vector<XvdExtensionEntry> extension_entries;
    std::vector<std::uint16_t> capabilities;
    std::array<std::uint8_t, 32> pe_catalog_hash{};
    std::array<std::uint8_t, 16> embedded_xvd_pduid{};
    std::array<std::uint8_t, 32> user_data_hash{};
    std::array<std::uint8_t, 16> sandbox_id_raw{};
    std::array<std::uint8_t, 16> product_id{};
    std::array<std::uint8_t, 16> pduid{};
    std::array<std::uint16_t, 8> pe_catalog_caps{};
    std::array<std::uint8_t, 0x80> pe_catalogs{};
    std::array<std::uint8_t, 32> key_material_opaque{};
    std::uint32_t writeable_expiration_date{};
    std::uint32_t writeable_policy_flags{};
    std::uint32_t persistent_local_storage_size{};
    std::uint8_t mutable_data_page_count{};
    std::uint8_t unknown_271{};
    std::uint64_t sequence_number_raw{};
    std::array<std::uint16_t, 4> package_version{}; // Stored as components 1..4.
    std::array<std::uint16_t, 4> required_system_version{}; // Stored as components 1..4.
    std::uint32_t odk_keyslot_id{};
    std::uint64_t resilient_data_offset{};
    std::uint32_t resilient_data_length{};
    std::uint32_t signature_nonzero_bytes{};

    bool layout_estimated{};
    std::uint64_t hashed_page_count{};
    std::uint64_t hash_tree_page_count{};
    std::uint64_t embedded_xvd_offset{xvd_nominal_payload_offset};
    std::uint64_t mdu_offset{};
    std::uint64_t hash_tree_offset{};
    std::uint64_t user_data_offset{};
    std::uint64_t xvc_info_offset{};
    std::uint64_t dynamic_header_offset{};
    std::uint64_t drive_data_offset{};
    std::uint64_t expected_bat_entries{};
    std::uint64_t bat_entries_read{};
    std::uint64_t bat_not_invalid_count{};
    std::uint64_t bat_invalid_count{};
    std::uint64_t bat_out_of_file_count{};
    std::uint64_t bat_duplicate_count{};
    std::uint64_t bat_in_range_count{};
    bool bat_values_plaintext{};
    std::vector<std::uint32_t> bat_sample; // Raw 32-bit words; semantics depend on encryption flag.
    std::string top_hash_check_status{"Not checked"};
    bool top_hash_check_matches{};
    std::vector<std::string> warnings;
};

inline std::uint32_t xvd_u32_le(const std::vector<std::uint8_t>& b, std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) |
           (static_cast<std::uint32_t>(b[o + 1]) << 8U) |
           (static_cast<std::uint32_t>(b[o + 2]) << 16U) |
           (static_cast<std::uint32_t>(b[o + 3]) << 24U);
}

inline std::uint64_t xvd_u64_le(const std::vector<std::uint8_t>& b, std::size_t o) {
    return static_cast<std::uint64_t>(xvd_u32_le(b, o)) |
           (static_cast<std::uint64_t>(xvd_u32_le(b, o + 4)) << 32U);
}

inline std::uint16_t xvd_u16_le(const std::vector<std::uint8_t>& b, std::size_t o) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[o]) |
           (static_cast<std::uint16_t>(b[o + 1]) << 8U));
}

inline bool has_xvd_magic(const std::vector<std::uint8_t>& bytes) {
    constexpr char magic[] = "msft-xvd";
    if (bytes.size() < 8) return false;
    for (std::size_t i = 0; i < 8; ++i) {
        if (bytes[i] != static_cast<std::uint8_t>(magic[i])) return false;
    }
    return true;
}

template <std::size_t N>
inline std::array<std::uint8_t, N> xvd_copy_array(const std::vector<std::uint8_t>& bytes,
                                                   std::size_t offset) {
    std::array<std::uint8_t, N> value{};
    for (std::size_t i = 0; i < N; ++i) value[i] = bytes[offset + i];
    return value;
}

inline std::uint64_t xvd_ceil_div(std::uint64_t value, std::uint64_t divisor) {
    return value / divisor + (value % divisor == 0 ? 0U : 1U);
}

inline bool xvd_checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& result) {
    if (a > (std::numeric_limits<std::uint64_t>::max)() - b) return false;
    result = a + b;
    return true;
}

inline bool xvd_add_pages(std::uint64_t base, std::uint64_t pages, std::uint64_t& result) {
    if (pages > (std::numeric_limits<std::uint64_t>::max)() / xvd_page_size) return false;
    return xvd_checked_add(base, pages * xvd_page_size, result);
}

inline bool xvd_calculate_hash_pages(std::uint64_t hashed_pages, bool resilient,
                                     std::uint64_t& hash_pages) {
    hash_pages = xvd_ceil_div(hashed_pages, 0xAA);
    if (hash_pages > 1) {
        std::uint64_t result = 2;
        unsigned int level = 1;
        while (result > 1) {
            std::uint64_t denominator = 0;
            switch (level) {
            case 1: denominator = 0x70E4; break;
            case 2: denominator = 0x4AF768; break;
            case 3: denominator = 0x31C84B10ULL; break;
            default:
                // Public XVD references document at most four hash-tree levels.
                // Do not silently under-estimate a hypothetical larger format.
                return false;
            }
            result = xvd_ceil_div(hashed_pages, denominator);
            ++level;
            if (!xvd_checked_add(hash_pages, result, hash_pages)) return false;
        }
    }
    if (resilient) {
        if (hash_pages > (std::numeric_limits<std::uint64_t>::max)() / 2) return false;
        hash_pages *= 2;
    }
    return true;
}

inline void xvd_calculate_layout(XvdHeaderInfo& h) {
    if (h.drive_size == 0 || h.block_size != xvd_block_size || h.type > 1) {
        h.warnings.emplace_back("Layout/BAT estimate skipped: requires a known block size, non-zero drive size and a known fixed/dynamic XVD type.");
        return;
    }

    const std::uint64_t drive_pages = xvd_ceil_div(h.drive_size, xvd_page_size);
    const std::uint64_t user_pages = xvd_ceil_div(h.user_data_length, xvd_page_size);
    const std::uint64_t xvc_pages = xvd_ceil_div(h.xvc_data_length, xvd_page_size);
    const std::uint64_t dynamic_pages = xvd_ceil_div(h.dynamic_header_length, xvd_page_size);
    std::uint64_t total_pages = 0;
    if (!xvd_checked_add(drive_pages, user_pages, total_pages) ||
        !xvd_checked_add(total_pages, xvc_pages, total_pages) ||
        !xvd_checked_add(total_pages, dynamic_pages, total_pages)) {
        h.warnings.emplace_back("Layout estimate skipped because hashed-page count overflows 64-bit arithmetic.");
        return;
    }
    h.hashed_page_count = total_pages;
    if (!xvd_calculate_hash_pages(total_pages, (h.volume_flags & 0x10U) != 0, h.hash_tree_page_count)) {
        h.warnings.emplace_back("Layout estimate skipped because hash-tree sizing overflows 64-bit arithmetic.");
        return;
    }

    const std::uint64_t embedded_pages = xvd_ceil_div(h.embedded_xvd_length, xvd_page_size);
    if (!xvd_add_pages(h.embedded_xvd_offset, embedded_pages, h.mdu_offset) ||
        !xvd_add_pages(h.mdu_offset, h.mutable_data_page_count, h.hash_tree_offset) ||
        !xvd_add_pages(h.hash_tree_offset,
                       ((h.volume_flags & 0x04U) == 0 ? h.hash_tree_page_count : 0),
                       h.user_data_offset) ||
        !xvd_add_pages(h.user_data_offset, user_pages, h.xvc_info_offset) ||
        !xvd_add_pages(h.xvc_info_offset, xvc_pages, h.dynamic_header_offset) ||
        !xvd_add_pages(h.dynamic_header_offset, dynamic_pages, h.drive_data_offset)) {
        h.warnings.emplace_back("Layout estimate skipped because a computed byte offset overflows 64-bit arithmetic.");
        return;
    }
    h.layout_estimated = true;

    if (h.type == 1) {
        h.expected_bat_entries = xvd_ceil_div(h.drive_size, xvd_block_size);
        const std::uint64_t table_capacity = h.dynamic_header_length / sizeof(std::uint32_t);
        if (table_capacity < h.expected_bat_entries) {
            h.warnings.emplace_back("Dynamic-header length is smaller than the expected block allocation table (BAT).");
        }
        if (h.dynamic_header_offset > (std::numeric_limits<std::uint64_t>::max)() - h.dynamic_header_length) {
            h.warnings.emplace_back("Dynamic-header extent overflows 64-bit arithmetic.");
            return;
        }
    }
}

// Reads the known fixed header and, for dynamic XVDs, a bounded read-only preview
// of its block allocation table (BAT). No signature verification, key handling,
// decryption, mounting, extraction or writing is performed.
inline std::optional<XvdHeaderInfo> read_xvd_header(const std::filesystem::path& path,
                                                     std::uintmax_t file_size,
                                                     std::string& error) {
    error.clear();
    if (file_size < xvd_fixed_fields_end_offset) {
        error = "File is too small for the complete known fixed XVD metadata fields (file offsets through 0xFFF).";
        return std::nullopt;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Could not open file for reading.";
        return std::nullopt;
    }
    in.seekg(static_cast<std::streamoff>(xvd_header_start), std::ios::beg);
    std::vector<std::uint8_t> bytes(xvd_header_body_size);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (in.gcount() != static_cast<std::streamsize>(bytes.size())) {
        error = "Could not read the complete known fixed XVD metadata fields.";
        return std::nullopt;
    }
    if (!has_xvd_magic(bytes)) {
        error = "The XVD magic at offset 0x200 does not match 'msft-xvd'.";
        return std::nullopt;
    }

    XvdHeaderInfo h;
    h.header_offset = xvd_header_start;
    h.volume_flags = xvd_u32_le(bytes, 0x08);
    h.format_version = xvd_u32_le(bytes, 0x0C);
    h.file_time_created_raw = xvd_u64_le(bytes, 0x10);
    h.drive_size = xvd_u64_le(bytes, 0x18);
    h.vduid = xvd_copy_array<16>(bytes, 0x20);
    h.uduid = xvd_copy_array<16>(bytes, 0x30);
    h.top_hash_block_hash = xvd_copy_array<32>(bytes, 0x40);
    h.original_xvc_data_hash = xvd_copy_array<32>(bytes, 0x60);
    h.type = xvd_u32_le(bytes, 0x80);
    h.content_type = xvd_u32_le(bytes, 0x84);
    h.embedded_xvd_length = xvd_u32_le(bytes, 0x88);
    h.user_data_length = xvd_u32_le(bytes, 0x8C);
    h.xvc_data_length = xvd_u32_le(bytes, 0x90);
    h.dynamic_header_length = xvd_u32_le(bytes, 0x94);
    h.block_size = xvd_u32_le(bytes, 0x98);

    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t offset = 0x9C + i * 0x18;
        XvdExtensionEntry entry;
        entry.code = xvd_u32_le(bytes, offset);
        entry.length = xvd_u32_le(bytes, offset + 0x04);
        entry.offset = xvd_u64_le(bytes, offset + 0x08);
        entry.data_length = xvd_u32_le(bytes, offset + 0x10);
        entry.reserved = xvd_u32_le(bytes, offset + 0x14);
        h.extension_entries.push_back(entry);
    }
    for (std::size_t i = 0; i < 8; ++i) h.capabilities.push_back(xvd_u16_le(bytes, 0xFC + i * 2));

    h.pe_catalog_hash = xvd_copy_array<32>(bytes, 0x10C);
    h.embedded_xvd_pduid = xvd_copy_array<16>(bytes, 0x12C);
    h.key_material_opaque = xvd_copy_array<32>(bytes, 0x14C);
    h.user_data_hash = xvd_copy_array<32>(bytes, 0x16C);
    h.sandbox_id_raw = xvd_copy_array<16>(bytes, 0x18C);
    h.product_id = xvd_copy_array<16>(bytes, 0x19C);
    h.pduid = xvd_copy_array<16>(bytes, 0x1AC);
    for (std::size_t i = 0; i < 8; ++i) h.pe_catalog_caps[i] = xvd_u16_le(bytes, 0x1C4 + i * 2);
    h.pe_catalogs = xvd_copy_array<0x80>(bytes, 0x1E4);
    h.writeable_expiration_date = xvd_u32_le(bytes, 0x264);
    h.writeable_policy_flags = xvd_u32_le(bytes, 0x268);
    h.persistent_local_storage_size = xvd_u32_le(bytes, 0x26C);
    h.mutable_data_page_count = bytes[0x270];
    h.unknown_271 = bytes[0x271];
    h.sequence_number_raw = xvd_u64_le(bytes, 0x28C);
    for (std::size_t i = 0; i < 4; ++i) h.package_version[i] = xvd_u16_le(bytes, 0x1BC + i * 2);
    for (std::size_t i = 0; i < 4; ++i) h.required_system_version[i] = xvd_u16_le(bytes, 0x294 + i * 2);
    h.odk_keyslot_id = xvd_u32_le(bytes, 0x29C);
    h.resilient_data_offset = xvd_u64_le(bytes, 0xDF4);
    h.resilient_data_length = xvd_u32_le(bytes, 0xDFC);

    in.clear();
    in.seekg(0, std::ios::beg);
    std::array<std::uint8_t, 0x200> signature_area{};
    in.read(reinterpret_cast<char*>(signature_area.data()), static_cast<std::streamsize>(signature_area.size()));
    if (in.gcount() == static_cast<std::streamsize>(signature_area.size())) {
        for (const auto byte : signature_area) if (byte != 0) ++h.signature_nonzero_bytes;
    }

    if (file_size < xvd_nominal_header_end_offset) {
        h.warnings.emplace_back("File ends before the nominal 0x3000-byte XVD header/payload boundary; known fixed metadata was readable, but the container may be truncated.");
    }
    if (h.format_version == 0) h.warnings.emplace_back("Format-version field is zero; this may be malformed or an unsupported variant.");
    if (h.block_size == 0) h.warnings.emplace_back("Block-size field is zero.");
    else if (h.block_size != xvd_block_size) h.warnings.emplace_back("Block size differs from 0xAA000, a commonly documented XVD value.");
    if (h.drive_size == 0) h.warnings.emplace_back("Declared virtual drive size is zero.");
    if (h.resilient_data_length != 0 && h.resilient_data_offset > file_size) {
        h.warnings.emplace_back("Resilient-data offset begins beyond the physical file size; verify this field's interpretation for this variant.");
    }
    if (h.resilient_data_length != 0 && h.resilient_data_offset <= UINT64_MAX - h.resilient_data_length &&
        h.resilient_data_offset + h.resilient_data_length > file_size) {
        h.warnings.emplace_back("Resilient-data range extends beyond the physical file size; verify this field's interpretation for this variant.");
    }

    // Bit 1 means encryption is disabled. When it is clear, the dynamic-header
    // area may still contain encrypted data; raw words must not be treated as
    // meaningful BAT page mappings without decryption.
    h.bat_values_plaintext = (h.volume_flags & 0x02U) != 0;

    xvd_calculate_layout(h);

    // Verify the first hash-tree page against the header's stored top hash.
    // This does not verify the Microsoft signature or every child hash.
    if ((h.volume_flags & 0x04U) != 0) {
        h.top_hash_check_status = "Skipped (data integrity disabled in header)";
    } else if (!h.layout_estimated || h.hash_tree_page_count == 0) {
        h.top_hash_check_status = "Skipped (hash-tree layout unavailable)";
    } else if (h.hash_tree_offset > file_size || xvd_page_size > file_size - h.hash_tree_offset) {
        h.top_hash_check_status = "Skipped (calculated hash-tree page is outside the file)";
        h.warnings.emplace_back("Calculated first hash-tree page extends beyond the physical file; top-hash check could not run.");
    } else {
        std::array<std::uint8_t, static_cast<std::size_t>(xvd_page_size)> hash_page{};
        in.clear();
        in.seekg(static_cast<std::streamoff>(h.hash_tree_offset), std::ios::beg);
        in.read(reinterpret_cast<char*>(hash_page.data()), static_cast<std::streamsize>(hash_page.size()));
        if (in.gcount() != static_cast<std::streamsize>(hash_page.size())) {
            h.top_hash_check_status = "Skipped (short read of calculated hash-tree page)";
            h.warnings.emplace_back("Could not read the full calculated first hash-tree page; top-hash check could not run.");
        } else {
            const auto digest = sha256(std::span<const std::uint8_t>(hash_page.data(), hash_page.size()));
            h.top_hash_check_matches = (digest == h.top_hash_block_hash);
            h.top_hash_check_status = h.top_hash_check_matches ? "MATCH" : "MISMATCH";
            if (!h.top_hash_check_matches) {
                h.warnings.emplace_back("SHA-256 of the calculated first hash-tree page does not match the header top hash. This may indicate corruption or an unsupported layout variant; signature authenticity is not checked.");
            }
        }
    }

    if (h.layout_estimated && h.type == 1) {
        std::uint64_t dynamic_header_end = 0;
        if (xvd_checked_add(h.dynamic_header_offset, h.dynamic_header_length, dynamic_header_end) &&
            dynamic_header_end > file_size) {
            h.warnings.emplace_back("Computed dynamic-header extent extends beyond the physical file; BAT preview is limited to bytes present.");
        }
        const std::uint64_t table_capacity = h.dynamic_header_length / sizeof(std::uint32_t);
        if (h.dynamic_header_offset > file_size) {
            h.warnings.emplace_back("Computed dynamic-header/BAT offset lies beyond the physical file size; BAT was not read.");
        } else {
            const std::uint64_t available_entries = (static_cast<std::uint64_t>(file_size) - h.dynamic_header_offset) / sizeof(std::uint32_t);
            constexpr std::uint64_t max_bat_entries_to_scan = 1'000'000;
            const std::uint64_t count = (std::min)({h.expected_bat_entries, table_capacity,
                                                   available_entries, max_bat_entries_to_scan});
            if (h.dynamic_header_offset <= static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)()) &&
                count <= static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)() / sizeof(std::uint32_t))) {
                in.clear();
                in.seekg(static_cast<std::streamoff>(h.dynamic_header_offset), std::ios::beg);
                std::vector<std::uint8_t> bat_bytes(static_cast<std::size_t>(count) * sizeof(std::uint32_t));
                in.read(reinterpret_cast<char*>(bat_bytes.data()), static_cast<std::streamsize>(bat_bytes.size()));
                const auto got = in.gcount();
                const auto complete = got <= 0 ? 0U : static_cast<std::uint64_t>(got) / sizeof(std::uint32_t);
                h.bat_entries_read = (std::min)(count, complete);
                const std::uint64_t physical_page_count = xvd_ceil_div(static_cast<std::uint64_t>(file_size), xvd_page_size);
                std::unordered_set<std::uint32_t> seen_page_indices;
                if (h.bat_values_plaintext) seen_page_indices.reserve(static_cast<std::size_t>((std::min)(h.bat_entries_read, static_cast<std::uint64_t>(100'000))));
                for (std::uint64_t i = 0; i < h.bat_entries_read; ++i) {
                    const auto value = xvd_u32_le(bat_bytes, static_cast<std::size_t>(i) * sizeof(std::uint32_t));
                    if (h.bat_sample.size() < 16) h.bat_sample.push_back(value);
                    if (!h.bat_values_plaintext) continue;
                    if (value == xvd_invalid_bat_entry) {
                        ++h.bat_invalid_count;
                        continue;
                    }
                    ++h.bat_not_invalid_count;
                    if (static_cast<std::uint64_t>(value) >= physical_page_count) {
                        ++h.bat_out_of_file_count;
                    } else {
                        ++h.bat_in_range_count;
                        if (!seen_page_indices.insert(value).second) ++h.bat_duplicate_count;
                    }
                }
                if (h.bat_values_plaintext && h.bat_out_of_file_count != 0) {
                    h.warnings.emplace_back("Plaintext BAT contains page indices at or beyond the physical file page count; these entries are outside the file bounds.");
                }
                if (h.bat_entries_read < h.expected_bat_entries) {
                    h.warnings.emplace_back("Only part of the declared BAT could be read from the available dynamic-header bytes.");
                }
                if (h.expected_bat_entries > max_bat_entries_to_scan) {
                    h.warnings.emplace_back("BAT scan was capped at 1,000,000 entries to bound resource use.");
                }
            } else {
                h.warnings.emplace_back("Dynamic-header/BAT offset cannot be represented safely by this build; BAT was not read.");
            }
        }
    }
    return h;
}

} // namespace someboxone
