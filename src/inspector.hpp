#pragma once

#include "vbi.hpp"
#include "xvd.hpp"
#include "xvc.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace someboxone {
namespace fs = std::filesystem;

inline constexpr const char* version_string = "0.32.1";

struct InspectionResult {
    int exit_code{};
    std::string report;
};

inline std::string hex_value(std::uint64_t value, int width = 0) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setfill('0');
    if (width > 0) out << std::setw(width);
    out << value;
    return out.str();
}

inline std::string path_to_utf8(const fs::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

inline std::vector<std::uint8_t> read_bytes_at(const fs::path& path, std::uint64_t offset, std::size_t count) {
    std::vector<std::uint8_t> bytes(count, 0);
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in) return {};
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    const auto got = in.gcount();
    if (got < 0) return {};
    bytes.resize(static_cast<std::size_t>(got));
    return bytes;
}

template <std::size_t N>
inline std::string bytes_hex(const std::array<std::uint8_t, N>& bytes) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (const auto byte : bytes) out << std::setw(2) << static_cast<unsigned int>(byte);
    return out.str();
}

template <std::size_t N>
inline std::string guid_from_xvd_bytes(const std::array<std::uint8_t, N>& b) {
    static_assert(N == 16);
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0')
        << std::setw(2) << static_cast<unsigned int>(b[3])
        << std::setw(2) << static_cast<unsigned int>(b[2])
        << std::setw(2) << static_cast<unsigned int>(b[1])
        << std::setw(2) << static_cast<unsigned int>(b[0]) << '-'
        << std::setw(2) << static_cast<unsigned int>(b[5])
        << std::setw(2) << static_cast<unsigned int>(b[4]) << '-'
        << std::setw(2) << static_cast<unsigned int>(b[7])
        << std::setw(2) << static_cast<unsigned int>(b[6]) << '-'
        << std::setw(2) << static_cast<unsigned int>(b[8])
        << std::setw(2) << static_cast<unsigned int>(b[9]) << '-';
    for (std::size_t i = 10; i < 16; ++i) out << std::setw(2) << static_cast<unsigned int>(b[i]);
    return out.str();
}

template <std::size_t N>
inline std::string fixed_text(const std::array<std::uint8_t, N>& bytes) {
    std::string value;
    for (const auto byte : bytes) {
        if (byte == 0) break;
        value.push_back((byte >= 0x20 && byte <= 0x7E) ? static_cast<char>(byte) : '.');
    }
    return value;
}

template <std::size_t N>
inline bool has_nonzero(const std::array<std::uint8_t, N>& bytes) {
    return std::any_of(bytes.begin(), bytes.end(), [](std::uint8_t b) { return b != 0; });
}

inline std::string version_quad(const std::array<std::uint16_t, 4>& v) {
    std::ostringstream out;
    out << v[3] << '.' << v[2] << '.' << v[1] << '.' << v[0];
    return out.str();
}

inline const char* xvd_type_name(std::uint32_t value) {
    switch (value) {
    case 0: return "Fixed";
    case 1: return "Dynamic";
    default: return "Unknown";
    }
}

inline std::string xvd_content_type_name(std::uint32_t value) {
    switch (value) {
    case 0: return "Data";
    case 1: return "Title";
    case 2: return "SystemOS";
    case 3: return "EraOS";
    case 4: return "Scratch";
    case 5: return "ResetData";
    case 6: return "Application";
    case 7: return "HostOS";
    case 8: return "X360STFS";
    case 9: return "X360FATX";
    case 10: return "X360GDFX";
    case 11: return "Updater";
    case 12: return "OfflineUpdater";
    case 13: return "Template";
    case 14: return "MteHost";
    case 15: return "MteApp";
    case 16: return "MteTitle";
    case 17: return "MteEraOS";
    case 18: return "EraTools";
    case 19: return "SystemTools";
    case 20: return "SystemAux";
    case 21: return "AcousticModel";
    case 22: return "SystemCodecsVolume";
    case 23: return "QasltPackage";
    case 24: return "AppDlc";
    case 25: return "TitleDlc";
    case 26: return "UniversalDlc";
    case 27: return "SystemDataVolume";
    case 28: return "TestVolume";
    case 29: return "HardwareTestVolume";
    case 30: return "KioskContent";
    case 31: return "DevPackage";
    case 32: return "HostProfiler";
    case 33: return "Roamable";
    case 34: return "ThinProvisioned";
    case 35: return "StreamingOnlySRA";
    case 36: return "StreamingOnlyERA";
    case 37: return "StreamingOnlyHost";
    case 38: return "QuickResume";
    case 39: return "HostData";
    default: return "Unknown";
    }
}

inline std::string xvd_volume_flags_description(std::uint32_t flags) {
    struct FlagName { std::uint32_t mask; const char* name; };
    static constexpr FlagName names[] = {
        {0x01U, "ReadOnly"}, {0x02U, "EncryptionDisabled"},
        {0x04U, "DataIntegrityDisabled"}, {0x08U, "LegacySectorSize"},
        {0x10U, "ResiliencyEnabled"}, {0x20U, "SraReadOnly"},
        {0x40U, "RegionIdInXts"}, {0x80U, "EraSpecific"}
    };
    std::ostringstream out;
    bool any = false;
    std::uint32_t known_mask = 0;
    for (const auto& item : names) {
        known_mask |= item.mask;
        if ((flags & item.mask) == 0) continue;
        if (any) out << ", ";
        out << item.name;
        any = true;
    }
    const auto unknown_bits = flags & ~known_mask;
    if (unknown_bits != 0) {
        if (any) out << ", ";
        out << "unknown bits " << hex_value(unknown_bits, 8);
        any = true;
    }
    if (!any) out << "(none set)";
    return out.str();
}

inline void append_hex_prefix(std::ostringstream& out, const std::vector<std::uint8_t>& bytes) {
    out << "First bytes: ";
    if (bytes.empty()) {
        out << "(unavailable)\n";
        return;
    }
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) out << ' ';
        out << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned int>(bytes[i]);
    }
    out << std::dec << '\n';
}

inline InspectionResult inspect_file(const fs::path& path) {
    std::ostringstream out;
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return {2, "Error: file does not exist or cannot be accessed: " + path_to_utf8(path) + "\n"};
    }
    if (!fs::is_regular_file(path, ec) || ec) {
        return {2, "Error: path is not a regular file: " + path_to_utf8(path) + "\n"};
    }

    const auto size = fs::file_size(path, ec);
    if (ec) return {2, "Error: unable to read file size: " + ec.message() + "\n"};

    const auto prefix = read_bytes_at(path, 0, 16);
    out << "SomeBoxOne image inspector v" << version_string << '\n'
        << "File: " << path_to_utf8(fs::absolute(path, ec)) << '\n'
        << "Physical file size: " << size << " bytes (" << std::fixed << std::setprecision(2)
        << (static_cast<double>(size) / (1024.0 * 1024.0)) << " MiB)\n";
    append_hex_prefix(out, prefix);

    const bool vbi_magic = prefix.size() >= 4 &&
        has_vbi_magic({prefix[0], prefix[1], prefix[2], prefix[3]});
    if (vbi_magic) {
        std::string error;
        const auto vbi = read_vbi_header(path, size, error);
        if (!vbi) {
            out << "Classification: VBI signature detected, but header parsing failed.\n"
                << "Reason: " << error << "\n";
            return {1, out.str()};
        }
        out << "Classification: Virtual Boot Image (VBI) signature\n"
            << "Minimum version: " << vbi->minimum_version << '\n'
            << "Header size: " << hex_value(vbi->header_size) << '\n'
            << "Image size (header field): " << hex_value(vbi->image_size) << '\n'
            << "Base physical address: " << hex_value(vbi->base_physical_address, 16) << '\n'
            << "Trampoline virtual address: " << hex_value(vbi->trampoline_virtual_address, 16) << '\n'
            << "Data offset: " << hex_value(vbi->data_offset) << '\n'
            << "Flags: " << hex_value(vbi->flags) << '\n'
            << "Directory entries: " << vbi->directory_count << '\n';
        for (std::size_t i = 0; i < vbi->directories.size(); ++i) {
            out << "  [" << i << "] offset=" << hex_value(vbi->directories[i].offset)
                << ", size=" << hex_value(vbi->directories[i].size) << '\n';
        }
        for (const auto& warning : vbi->warnings) out << "Validation warning: " << warning << '\n';
        out << "\nMetadata only: this does not decrypt, authenticate, or boot the image.\n";
        return {vbi->warnings.empty() ? 0 : 1, out.str()};
    }

    const auto xvd_magic_bytes = read_bytes_at(path, xvd_header_start, 8);
    if (has_xvd_magic(xvd_magic_bytes)) {
        std::string error;
        const auto xvd = read_xvd_header(path, size, error);
        if (!xvd) {
            out << "Classification: XVD magic found at offset 0x200, but fixed-header parsing failed.\n"
                << "Reason: " << error << '\n';
            return {1, out.str()};
        }
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool xvc_candidate = extension == ".xvc" || xvc_content_type(xvd->content_type);
        out << "Classification: "
            << (xvc_candidate ? "XVC game-package candidate with XVD-compatible header ('msft-xvd')\n"
                              : "XVD-compatible container header ('msft-xvd')\n")
            << "Header magic offset: " << hex_value(xvd->header_offset) << '\n'
            << "Fixed header body size: 0xE00 bytes (file offsets 0x200 through 0xFFF)\n"
            << "Signature area non-zero bytes: " << xvd->signature_nonzero_bytes << "/512 (presence only; not verified)\n"
            << "Volume flags (raw): " << hex_value(xvd->volume_flags, 8) << '\n'
            << "Volume flags (decoded known bits): " << xvd_volume_flags_description(xvd->volume_flags) << '\n'
            << "Data encryption status: " << (((xvd->volume_flags & 0x02U) != 0) ? "Disabled (header flag set)" : "Enabled (header flag clear)") << '\n'
            << "Format version: " << xvd->format_version << '\n'
            << "Creation timestamp (raw FILETIME value): " << hex_value(xvd->file_time_created_raw, 16) << '\n'
            << "Declared virtual drive size: " << xvd->drive_size << " bytes ("
            << std::fixed << std::setprecision(2) << (static_cast<double>(xvd->drive_size) / (1024.0 * 1024.0)) << " MiB)\n"
            << "XVD type (raw): " << hex_value(xvd->type, 8) << '\n'
            << "XVD type (decoded): " << xvd_type_name(xvd->type) << '\n'
            << "Content type (raw): " << hex_value(xvd->content_type, 8) << '\n'
            << "Content type (decoded): " << xvd_content_type_name(xvd->content_type) << '\n'
            << "VDUID / drive ID: " << guid_from_xvd_bytes(xvd->vduid) << '\n'
            << "UDUID / user ID: " << guid_from_xvd_bytes(xvd->uduid) << '\n'
            << "Top hash-block hash: " << bytes_hex(xvd->top_hash_block_hash) << '\n'
            << "Original XVC-data hash: " << bytes_hex(xvd->original_xvc_data_hash) << '\n'
            << "Embedded XVD length: " << xvd->embedded_xvd_length << " bytes\n"
            << "User-data length: " << xvd->user_data_length << " bytes\n"
            << "XVC-data length: " << xvd->xvc_data_length << " bytes\n"
            << "Dynamic-header length: " << xvd->dynamic_header_length << " bytes\n"
            << "Block size: " << hex_value(xvd->block_size) << '\n';
        std::size_t printed_extensions = 0;
        for (std::size_t i = 0; i < xvd->extension_entries.size(); ++i) {
            const auto& entry = xvd->extension_entries[i];
            if (entry.code == 0 && entry.length == 0 && entry.offset == 0 && entry.data_length == 0 && entry.reserved == 0) continue;
            ++printed_extensions;
            out << "Extension entry [" << i << "]: code=" << hex_value(entry.code, 8)
                << ", length=" << hex_value(entry.length, 8)
                << ", offset=" << hex_value(entry.offset, 16)
                << ", data length=" << hex_value(entry.data_length, 8)
                << ", reserved=" << hex_value(entry.reserved, 8) << '\n';
        }
        out << "Non-empty extension entries: " << printed_extensions << '\n'
            << "Capabilities:";
        for (const auto capability : xvd->capabilities) out << ' ' << hex_value(capability, 4);
        out << '\n'
            << "PE catalog hash: " << bytes_hex(xvd->pe_catalog_hash) << '\n'
            << "Embedded XVD build ID: " << guid_from_xvd_bytes(xvd->embedded_xvd_pduid) << '\n'
            << "User-data hash: " << bytes_hex(xvd->user_data_hash) << '\n'
            << "Sandbox ID (printable bytes): \"" << fixed_text(xvd->sandbox_id_raw) << "\"\n"
            << "Product ID: " << guid_from_xvd_bytes(xvd->product_id) << '\n'
            << "Package/build ID (PDUID): " << guid_from_xvd_bytes(xvd->pduid) << '\n'
            << "Package version: " << version_quad(xvd->package_version) << '\n'
            << "Required system version: " << version_quad(xvd->required_system_version) << '\n'
            << "PE catalog capabilities:";
        for (const auto cap : xvd->pe_catalog_caps) out << ' ' << hex_value(cap, 4);
        out << '\n'
            << "PE catalogs field: " << (has_nonzero(xvd->pe_catalogs) ? "non-zero bytes present" : "all zero") << '\n'
            << "Writeable expiration date (raw): " << hex_value(xvd->writeable_expiration_date, 8) << '\n'
            << "Writeable policy flags (raw): " << hex_value(xvd->writeable_policy_flags, 8) << '\n'
            << "Persistent local storage size: " << xvd->persistent_local_storage_size << " bytes\n"
            << "Mutable-data page count: " << static_cast<unsigned int>(xvd->mutable_data_page_count) << '\n'
            << "Sequence number (raw): " << xvd->sequence_number_raw << '\n'
            << "ODK keyslot ID (raw): " << hex_value(xvd->odk_keyslot_id, 8) << '\n'
            << "Resilient-data offset: " << hex_value(xvd->resilient_data_offset, 16) << '\n'
            << "Resilient-data length: " << xvd->resilient_data_length << " bytes\n"
            << "Top hash-tree page SHA-256 check: " << xvd->top_hash_check_status << '\n';
        if (xvd->layout_estimated) {
            out << "\nCalculated XVD layout (public formula; estimate only):\n"
                << "Nominal payload base: " << hex_value(xvd->embedded_xvd_offset) << '\n'
                << "Hashed pages (calculated): " << xvd->hashed_page_count << '\n'
                << "Hash-tree pages (calculated): " << xvd->hash_tree_page_count << '\n'
                << "MDU offset: " << hex_value(xvd->mdu_offset) << '\n'
                << "Hash-tree offset: " << hex_value(xvd->hash_tree_offset) << '\n'
                << "User-data offset: " << hex_value(xvd->user_data_offset) << '\n'
                << "XVC-info offset: " << hex_value(xvd->xvc_info_offset) << '\n'
                << "Dynamic-header/BAT offset: " << hex_value(xvd->dynamic_header_offset) << '\n'
                << "Drive-data base offset: " << hex_value(xvd->drive_data_offset) << '\n';
            if (xvd->type == 1) {
                out << "Expected BAT entries: " << xvd->expected_bat_entries << '\n'
                    << "BAT entries read: " << xvd->bat_entries_read << " (32-bit words)\n";
                if (xvd->bat_values_plaintext) {
                    out << "BAT entries not equal to invalid sentinel: " << xvd->bat_not_invalid_count << '\n'
                        << "BAT entries equal to 0xFFFFFFFF unallocated sentinel: " << xvd->bat_invalid_count << '\n'
                        << "BAT entries pointing beyond physical file pages: " << xvd->bat_out_of_file_count << '\n'
                        << "Repeated in-range BAT page indices: " << xvd->bat_duplicate_count << '\n'
                        << "In-range BAT page indices: " << xvd->bat_in_range_count << '\n';
                } else {
                    out << "BAT interpretation: raw words only (encryption is enabled; values are not treated as page mappings)\n";
                }
                out << "Raw BAT words (first " << xvd->bat_sample.size() << "):";
                for (const auto entry : xvd->bat_sample) out << ' ' << hex_value(entry, 8);
                out << '\n';
            }
        }
        if (xvc_candidate) {
            out << "\nXVC descriptor parsing (read-only):\n";
            std::string xvc_error;
            const auto xvc = read_xvc_descriptor(path, size, *xvd, xvc_error);
            if (!xvc) {
                out << "Descriptor status: unavailable / not parsed\n"
                    << "Reason: " << xvc_error << '\n';
            } else {
                out << "Descriptor version: " << xvc->version << '\n'
                    << "Content ID: " << guid_from_xvd_bytes(xvc->content_id) << '\n'
                    << "Description: " << (xvc->description.empty() ? "(empty or non-ASCII text)" : xvc->description) << '\n'
                    << "Descriptor flags: " << hex_value(xvc->flags, 8) << '\n'
                    << "Key count (declared): " << xvc->key_count << '\n'
                    << "Non-zero key GUIDs read: " << xvc->key_ids.size()
                    << " (identifiers only; not decryption keys)\n";
                for (std::size_t i = 0; i < xvc->key_ids.size(); ++i)
                    out << "  Key GUID [" << i << "]: " << guid_from_xvd_bytes(xvc->key_ids[i]) << '\n';
                out << "Region count: " << xvc->regions.size() << '\n'
                    << "Update-segment count: " << xvc->update_segment_count << '\n'
                    << "Region-specifier count (declared): " << xvc->region_specifier_count << '\n'
                    << "Initial-play region ID / offset: " << xvc->initial_play_region_id
                    << " / " << hex_value(xvc->initial_play_offset) << '\n'
                    << "Preview region ID / offset: " << xvc->preview_region_id
                    << " / " << hex_value(xvc->preview_offset) << '\n';
                for (std::size_t i = 0; i < xvc->regions.size(); ++i) {
                    const auto& region = xvc->regions[i];
                    out << "  Region [" << i << "]: id=" << hex_value(region.id, 8)
                        << ", key-id=" << hex_value(region.key_id, 4)
                        << ", flags=" << hex_value(region.flags, 8)
                        << ", first-segment=" << region.first_segment_index
                        << ", description=\"" << region.description << "\""
                        << ", offset=" << hex_value(region.offset)
                        << ", length=" << hex_value(region.length)
                        << ", region-hash=" << hex_value(region.hash, 16)
                        << ", extent=" << (region.extent_in_file ? "within physical file" : "outside physical file / possibly not resident")
                        << '\n';
                }
                if (!xvc->update_page_numbers.empty()) {
                    out << "First update-segment page numbers:";
                    const auto sample_count = (std::min<std::size_t>)(xvc->update_page_numbers.size(), 16);
                    for (std::size_t i = 0; i < sample_count; ++i)
                        out << ' ' << hex_value(xvc->update_page_numbers[i], 8);
                    if (xvc->update_page_numbers.size() > sample_count) out << " ...";
                    out << '\n';
                }
                for (const auto& warning : xvc->warnings) out << "XVC validation warning: " << warning << '\n';
                out << "XVC note: region/key metadata is parsed only; encrypted region data is not decrypted, mounted, extracted, or executed.\n";
            }
        }
        for (const auto& warning : xvd->warnings) out << "Validation warning: " << warning << '\n';
        out << "\nInterpretation: physical file size and declared virtual drive size are different concepts; their difference alone is not a corruption warning.\n"
            << "The first hash-tree page check compares one SHA-256 digest with the header field only; it does not verify the signature or the entire tree.\n"
            << "BAT words are interpreted as page mappings only when the header says data encryption is disabled. No key extraction, decryption, filesystem mount, extraction, or boot is performed.\n";
        return {xvd->warnings.empty() ? 0 : 1, out.str()};
    }

    std::string unresolved_extension = path.extension().string();
    std::transform(unresolved_extension.begin(), unresolved_extension.end(), unresolved_extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (unresolved_extension == ".xvc") {
        out << "Classification: .xvc extension detected, but the shared XVD magic at offset 0x200 was not found.\n"
            << "The file was not parsed as an XVC; it may be truncated, wrapped, or use an unsupported variant.\n";
    } else if (prefix.size() >= 2 && prefix[0] == 'M' && prefix[1] == 'Z') {
        out << "Classification: PE/DOS executable signature (MZ).\n"
            << "This alone does not establish that it is an Xbox One system component.\n";
    } else {
        out << "Classification: unknown or container-wrapped image.\n"
            << "The image may be encrypted, wrapped, or use a format not yet recognised.\n";
    }
    out << "\nNo firmware has been executed or modified.\n";
    return {0, out.str()};
}

inline bool is_image_candidate(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::array<const char*, 8> supported = {".xvd", ".xvc", ".vbi", ".bin", ".img", ".dat", ".xvi", ".xct"};
    return std::any_of(supported.begin(), supported.end(), [&](const char* ext) { return extension == ext; });
}

inline std::string report_value(const std::string& report, const std::string& prefix) {
    std::istringstream input(report);
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
    }
    return "-";
}

inline InspectionResult scan_directory(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec) || ec || !fs::is_directory(root, ec) || ec) {
        return {2, "Error: folder does not exist or cannot be accessed: " + path_to_utf8(root) + "\n"};
    }

    std::vector<fs::path> files;
    constexpr std::size_t max_files = 5000;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        std::error_code item_ec;
        if (!it->is_regular_file(item_ec) || item_ec || !is_image_candidate(it->path())) continue;
        files.push_back(it->path());
        if (files.size() >= max_files) break;
    }
    std::sort(files.begin(), files.end());

    std::ostringstream out;
    out << "SomeBoxOne OSU/restore directory scan v" << version_string << '\n'
        << "Root: " << path_to_utf8(fs::absolute(root, ec)) << '\n'
        << "Scope: recursive, read-only signature and fixed-header reconnaissance\n"
        << "Candidate files checked: " << files.size() << (files.size() == max_files ? " (limit reached)" : "") << "\n\n"
        << "Relative path | Classification | Format | XVD type | Content type | Encryption | Hash sample | Package version | Physical MiB | Virtual MiB\n"
        << "--------------|----------------|--------|----------|--------------|------------|-------------|-----------------|--------------|-----------\n";

    std::size_t xvd_count = 0, vbi_count = 0, unknown_count = 0, warning_count = 0;
    std::map<std::string, std::size_t> content_type_counts;
    std::map<std::string, std::size_t> package_version_counts;
    for (const auto& file : files) {
        const auto inspected = inspect_file(file);
        const auto& report = inspected.report;
        std::error_code rel_ec;
        const auto relative = fs::relative(file, root, rel_ec);
        const auto path_label = path_to_utf8(rel_ec ? file.filename() : relative);
        std::string classification = report_value(report, "Classification: ");
        if (classification.find("XVD-compatible") != std::string::npos) ++xvd_count;
        else if (classification.find("Virtual Boot Image") != std::string::npos) ++vbi_count;
        else ++unknown_count;
        if (report.find("Validation warning:") != std::string::npos || inspected.exit_code != 0) ++warning_count;
        const auto size_value = report_value(report, "Physical file size: ");
        const auto format = report_value(report, "Format version: ");
        const auto type_raw = report_value(report, "XVD type (raw): ");
        const auto type_name = report_value(report, "XVD type (decoded): ");
        const auto content_raw = report_value(report, "Content type (raw): ");
        const auto content_name = report_value(report, "Content type (decoded): ");
        const auto type = type_name == "-" ? type_raw : type_name + " (" + type_raw + ")";
        const auto content = content_name == "-" ? content_raw : content_name + " (" + content_raw + ")";
        const auto drive = report_value(report, "Declared virtual drive size: ");
        const auto package_version = report_value(report, "Package version: ");
        const auto encryption = report_value(report, "Data encryption status: ");
        const auto hash_sample = report_value(report, "Top hash-tree page SHA-256 check: ");
        std::string content_label = content_name;
        if (content_label == "-") content_label = "Unknown";
        content_type_counts[content_label]++;
        if (package_version != "-") package_version_counts[package_version]++;
        std::string physical_mib = "-", virtual_mib = "-";
        const auto paren = size_value.find(" MiB)");
        if (paren != std::string::npos) {
            const auto start = size_value.rfind('(', paren);
            if (start != std::string::npos) physical_mib = size_value.substr(start + 1, paren - start - 1);
        }
        const auto drive_paren = drive.find(" MiB)");
        if (drive_paren != std::string::npos) {
            const auto start = drive.rfind('(', drive_paren);
            if (start != std::string::npos) virtual_mib = drive.substr(start + 1, drive_paren - start - 1);
        }
        out << path_label << " | " << classification << " | " << format << " | " << type << " | " << content
            << " | " << encryption << " | " << hash_sample << " | " << package_version
            << " | " << physical_mib << " | " << virtual_mib << '\n';
    }
    out << "\nSummary: XVD-compatible=" << xvd_count << ", VBI=" << vbi_count
        << ", unknown/other=" << unknown_count << ", files with warnings/errors=" << warning_count << "\n";
    out << "Encryption and hash-sample columns are copied from each individual inspection report. The hash column covers only the sampled top hash-tree page.\n";
    out << "Content-type counts:";
    for (const auto& [name, count] : content_type_counts) out << " " << name << "=" << count << ";";
    out << "\nPackage-version counts:";
    if (package_version_counts.empty()) out << " (none decoded)";
    for (const auto& [version, count] : package_version_counts) out << " " << version << "=" << count << ";";
    out << "\nAll processing is read-only. For full known metadata of an individual XVD, use `SomeBoxOne inspect <file>`.\n"
        << "The scan does not establish signature authenticity, successful decryption, filesystem validity, logical-read support, or bootability.\n";
    return {0, out.str()};
}

} // namespace someboxone

