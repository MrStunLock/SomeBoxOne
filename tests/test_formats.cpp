#include "inspector.hpp"
#include "xvd.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <span>
#include <vector>

namespace {
void put16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}
void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned int i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU);
}
void put64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    put32(bytes, offset, static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
    put32(bytes, offset + 4, static_cast<std::uint32_t>(value >> 32U));
}
bool write_temp(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}
}

int main() {
    const std::array<std::uint8_t, 3> abc = {'a', 'b', 'c'};
    const auto abc_hash = someboxone::sha256(std::span<const std::uint8_t>(abc.data(), abc.size()));
    constexpr std::array<std::uint8_t, 32> abc_expected = {
        0xBA,0x78,0x16,0xBF,0x8F,0x01,0xCF,0xEA,0x41,0x41,0x40,0xDE,0x5D,0xAE,0x22,0x23,
        0xB0,0x03,0x61,0xA3,0x96,0x17,0x7A,0x9C,0xB4,0x10,0xFF,0x61,0xF2,0x00,0x15,0xAD
    };
    if (abc_hash != abc_expected) {
        std::cerr << "SHA-256 known-vector test failed\n";
        return 1;
    }
    const auto dir = std::filesystem::temp_directory_path();
    const auto xvd_path = dir / "someboxone_test_xvd.bin";
    const auto unknown_path = dir / "someboxone_test_unknown.bin";

    std::vector<std::uint8_t> xvd(0x3000, 0);
    constexpr char magic[] = "msft-xvd";
    for (std::size_t i = 0; i < 8; ++i) xvd[0x200 + i] = static_cast<std::uint8_t>(magic[i]);
    put32(xvd, 0x208, 1);
    put32(xvd, 0x20C, 3);
    put64(xvd, 0x210, 0x01D0000000000000ULL);
    put64(xvd, 0x218, 0x100000000ULL);
    put32(xvd, 0x280, 2);
    put32(xvd, 0x284, 0x12);
    put32(xvd, 0x288, 0x1000);
    put32(xvd, 0x28C, 0x2000);
    put32(xvd, 0x290, 0x3000);
    put32(xvd, 0x294, 0x4000);
    put32(xvd, 0x298, 0xAA000);
    // First XVD extension entry (header-relative offset 0x9C).
    put32(xvd, 0x29C, 0x2A);
    put32(xvd, 0x2A0, 0x18);
    put64(xvd, 0x2A4, 0x123400ULL);
    put32(xvd, 0x2AC, 0x18);
    put32(xvd, 0x2B0, 0);
    put32(xvd, 0x2FC, 0x1234); // first capability at header offset 0xFC
    // Full fixed-header fields at file-relative offsets (header body starts at 0x200).
    put32(xvd, 0x464, 0x55667788); // writeable expiration date at body offset 0x264
    put32(xvd, 0x468, 0xAABBCCDD); // writeable policy flags at body offset 0x268
    put32(xvd, 0x46C, 0x12345678); // persistent local storage size
    xvd[0x470] = 3;                // mutable-data page count
    put64(xvd, 0x48C, 0x123456789ULL); // sequence number at body offset 0x28C
    put32(xvd, 0xFFC, 0);          // resilient-data length at body offset 0xDFC

    if (!write_temp(xvd_path, xvd)) {
        std::cerr << "Could not create temporary XVD test file\n";
        return 1;
    }
    std::string error;
    auto parsed = someboxone::read_xvd_header(xvd_path, xvd.size(), error);
    if (!parsed || parsed->format_version != 3 || parsed->volume_flags != 1 ||
        parsed->drive_size != 0x100000000ULL || parsed->type != 2 ||
        parsed->content_type != 0x12 || parsed->embedded_xvd_length != 0x1000 ||
        parsed->user_data_length != 0x2000 || parsed->xvc_data_length != 0x3000 ||
        parsed->dynamic_header_length != 0x4000 || parsed->block_size != 0xAA000 ||
        parsed->extension_entries.size() != 4 || parsed->extension_entries[0].code != 0x2A ||
        parsed->extension_entries[0].length != 0x18 || parsed->extension_entries[0].offset != 0x123400ULL ||
        parsed->extension_entries[0].data_length != 0x18 || parsed->capabilities.size() != 8 ||
        parsed->capabilities[0] != 0x1234 || parsed->writeable_expiration_date != 0x55667788 ||
        parsed->writeable_policy_flags != 0xAABBCCDD || parsed->persistent_local_storage_size != 0x12345678 ||
        parsed->mutable_data_page_count != 3 || parsed->sequence_number_raw != 0x123456789ULL || !error.empty()) {
        std::cerr << "XVD metadata parser test failed: " << error << '\n';
        std::filesystem::remove(xvd_path);
        return 1;
    }
    const auto report = someboxone::inspect_file(xvd_path);
    if (report.report.find("Classification: XVD-compatible container header") == std::string::npos ||
        report.report.find("Format version: 3") == std::string::npos ||
        report.report.find("Extension entry [0]") == std::string::npos ||
        report.report.find("0x1234") == std::string::npos ||
        report.report.find("Persistent local storage size: 305419896 bytes") == std::string::npos ||
        report.report.find("Fixed header body size: 0xE00") == std::string::npos) {
        std::cerr << "XVD inspection report test failed\n";
        std::filesystem::remove(xvd_path);
        return 1;
    }

    // Layout regression checks use the exact sizes in the user's two reports.
    someboxone::XvdHeaderInfo system_layout{};
    system_layout.volume_flags = 1;
    system_layout.drive_size = 1610612736ULL;
    system_layout.type = 1;
    system_layout.user_data_length = 44634112;
    system_layout.dynamic_header_length = 9512;
    system_layout.block_size = 0xAA000;
    someboxone::xvd_calculate_layout(system_layout);
    someboxone::XvdHeaderInfo aux_layout{};
    aux_layout.volume_flags = 1;
    aux_layout.drive_size = 4294967296ULL;
    aux_layout.type = 1;
    aux_layout.dynamic_header_length = 24676;
    aux_layout.block_size = 0xAA000;
    someboxone::xvd_calculate_layout(aux_layout);
    if (!system_layout.layout_estimated || system_layout.hash_tree_page_count != 2393 ||
        system_layout.dynamic_header_offset != 0x33ED000 || system_layout.drive_data_offset != 0x33F0000 ||
        system_layout.expected_bat_entries != 2314 || system_layout.dynamic_header_length / 4 < system_layout.expected_bat_entries ||
        !aux_layout.layout_estimated || aux_layout.hash_tree_page_count != 6207 ||
        aux_layout.dynamic_header_offset != 0x1842000 || aux_layout.drive_data_offset != 0x1849000 ||
        aux_layout.expected_bat_entries != 6169 || aux_layout.dynamic_header_length / 4 != aux_layout.expected_bat_entries) {
        std::cerr << "XVD layout regression checks using the supplied report values failed\n";
        std::filesystem::remove(xvd_path);
        return 1;
    }

    // A compact dynamic-XVD fixture exercises computed offsets and the read-only BAT preview.
    const auto dynamic_path = dir / "someboxone_test_dynamic_xvd.bin";
    std::vector<std::uint8_t> dynamic_xvd(0x8000, 0);
    for (std::size_t i = 0; i < 8; ++i) dynamic_xvd[0x200 + i] = static_cast<std::uint8_t>(magic[i]);
    put32(dynamic_xvd, 0x208, 3);       // Read-only, encryption disabled, hash tree enabled
    put32(dynamic_xvd, 0x20C, 3);
    put64(dynamic_xvd, 0x218, 0xAA000); // Exactly one allocation block
    put32(dynamic_xvd, 0x280, 1);       // Dynamic XVD
    put32(dynamic_xvd, 0x284, 2);       // SystemOS content-type value
    put32(dynamic_xvd, 0x294, 4);       // One DWORD BAT
    put32(dynamic_xvd, 0x298, 0xAA000);
    put32(dynamic_xvd, 0x6000, 5);     // BAT-sized word at calculated dynamic-header offset
    for (std::size_t i = 0; i < 0x1000; ++i) dynamic_xvd[0x3000 + i] = static_cast<std::uint8_t>((i * 37U + 11U) & 0xFFU);
    const auto synthetic_tree_hash = someboxone::sha256(std::span<const std::uint8_t>(dynamic_xvd.data() + 0x3000, 0x1000));
    for (std::size_t i = 0; i < synthetic_tree_hash.size(); ++i) dynamic_xvd[0x240 + i] = synthetic_tree_hash[i];
    if (!write_temp(dynamic_path, dynamic_xvd)) {
        std::cerr << "Could not create dynamic XVD fixture\n";
        std::filesystem::remove(xvd_path);
        return 1;
    }
    auto dynamic_parsed = someboxone::read_xvd_header(dynamic_path, dynamic_xvd.size(), error);
    const auto dynamic_report = someboxone::inspect_file(dynamic_path);
    if (!dynamic_parsed || !dynamic_parsed->layout_estimated ||
        dynamic_parsed->expected_bat_entries != 1 || dynamic_parsed->bat_entries_read != 1 ||
        dynamic_parsed->bat_not_invalid_count != 1 || dynamic_parsed->bat_invalid_count != 0 ||
        dynamic_parsed->bat_in_range_count != 1 || dynamic_parsed->bat_out_of_file_count != 0 ||
        dynamic_parsed->bat_duplicate_count != 0 || dynamic_parsed->bat_sample.size() != 1 || dynamic_parsed->bat_sample[0] != 5 ||
        dynamic_parsed->dynamic_header_offset != 0x6000 || dynamic_parsed->drive_data_offset != 0x7000 ||
        dynamic_parsed->top_hash_check_status != "MATCH" || !dynamic_parsed->top_hash_check_matches ||
        dynamic_report.report.find("Top hash-tree page SHA-256 check: MATCH") == std::string::npos ||
        dynamic_report.report.find("Content type (decoded): SystemOS") == std::string::npos ||
        dynamic_report.report.find("Dynamic-header/BAT offset: 0x6000") == std::string::npos ||
        dynamic_report.report.find("BAT entries read: 1") == std::string::npos) {
        std::cerr << "XVD layout/BAT preview test failed: " << error << "\n" << dynamic_report.report;
        std::filesystem::remove(xvd_path);
        std::filesystem::remove(dynamic_path);
        return 1;
    }

    auto tampered_tree = dynamic_xvd;
    tampered_tree[0x3010] ^= 0x01U;
    const auto tampered_tree_path = dir / "someboxone_test_tampered_tree.bin";
    if (!write_temp(tampered_tree_path, tampered_tree)) {
        std::cerr << "Could not create tampered hash-tree fixture\n";
        std::filesystem::remove(xvd_path);
        std::filesystem::remove(dynamic_path);
        return 1;
    }
    const auto tampered_parsed = someboxone::read_xvd_header(tampered_tree_path, tampered_tree.size(), error);
    if (!tampered_parsed || tampered_parsed->top_hash_check_status != "MISMATCH" || tampered_parsed->warnings.empty()) {
        std::cerr << "Hash-tree mismatch detection test failed\n";
        std::filesystem::remove(xvd_path);
        std::filesystem::remove(dynamic_path);
        std::filesystem::remove(tampered_tree_path);
        return 1;
    }
    std::filesystem::remove(tampered_tree_path);

    // Retail-style encrypted headers retain only a raw BAT preview; ciphertext must not
    // be misreported as page numbers or sentinel counts.
    auto encrypted_dynamic = dynamic_xvd;
    put32(encrypted_dynamic, 0x208, 1); // Encryption-enabled: encryption-disabled flag bit remains clear.
    const auto encrypted_dynamic_path = dir / "someboxone_test_encrypted_dynamic_xvd.bin";
    if (!write_temp(encrypted_dynamic_path, encrypted_dynamic)) {
        std::cerr << "Could not create encrypted BAT fixture\n";
        std::filesystem::remove(xvd_path);
        std::filesystem::remove(dynamic_path);
        return 1;
    }
    const auto encrypted_parsed = someboxone::read_xvd_header(encrypted_dynamic_path, encrypted_dynamic.size(), error);
    if (!encrypted_parsed || encrypted_parsed->bat_values_plaintext || encrypted_parsed->bat_not_invalid_count != 0 ||
        someboxone::inspect_file(encrypted_dynamic_path).report.find("BAT interpretation: raw words only") == std::string::npos) {
        std::cerr << "Encrypted BAT interpretation guard test failed\n";
        std::filesystem::remove(xvd_path);
        std::filesystem::remove(dynamic_path);
        std::filesystem::remove(encrypted_dynamic_path);
        return 1;
    }
    std::filesystem::remove(encrypted_dynamic_path);

    // Plaintext BAT validation: repeated and out-of-file page indices must be called out.
    auto suspect_bat = dynamic_xvd;
    put64(suspect_bat, 0x218, 0x154000ULL); // Two allocation blocks.
    put32(suspect_bat, 0x294, 8);          // Two 32-bit BAT entries.
    suspect_bat.resize(0x9000, 0);
    for (std::size_t i = 0; i < 0x1000; ++i) suspect_bat[0x3000 + i] = static_cast<std::uint8_t>((i * 37U + 11U) & 0xFFU);
    const auto suspect_tree_hash = someboxone::sha256(std::span<const std::uint8_t>(suspect_bat.data() + 0x3000, 0x1000));
    for (std::size_t i = 0; i < suspect_tree_hash.size(); ++i) suspect_bat[0x240 + i] = suspect_tree_hash[i];
    put32(suspect_bat, 0x7000, 5);
    put32(suspect_bat, 0x7004, 5); // duplicate page index
    const auto suspect_path = dir / "someboxone_test_suspect_bat.bin";
    if (!write_temp(suspect_path, suspect_bat)) {
        std::cerr << "Could not create suspect BAT fixture\n";
        std::filesystem::remove(xvd_path);
        std::filesystem::remove(dynamic_path);
        return 1;
    }
    const auto suspect_parsed = someboxone::read_xvd_header(suspect_path, suspect_bat.size(), error);
    if (!suspect_parsed || suspect_parsed->expected_bat_entries != 2 || suspect_parsed->bat_entries_read != 2 ||
        suspect_parsed->bat_duplicate_count != 1 || suspect_parsed->bat_out_of_file_count != 0) {
        std::cerr << "Plaintext duplicate BAT test failed\n";
        std::filesystem::remove(xvd_path);
        std::filesystem::remove(dynamic_path);
        std::filesystem::remove(suspect_path);
        return 1;
    }
    put32(suspect_bat, 0x7004, 0xFFFFFFFEU); // not sentinel, but beyond the file page range
    if (!write_temp(suspect_path, suspect_bat)) {
        std::cerr << "Could not rewrite out-of-range BAT fixture\n";
        std::filesystem::remove(xvd_path);
        std::filesystem::remove(dynamic_path);
        std::filesystem::remove(suspect_path);
        return 1;
    }
    const auto out_of_range_parsed = someboxone::read_xvd_header(suspect_path, suspect_bat.size(), error);
    if (!out_of_range_parsed || out_of_range_parsed->bat_out_of_file_count != 1 ||
        out_of_range_parsed->warnings.empty()) {
        std::cerr << "Out-of-range BAT detection test failed\n";
        std::filesystem::remove(xvd_path);
        std::filesystem::remove(dynamic_path);
        std::filesystem::remove(suspect_path);
        return 1;
    }
    std::filesystem::remove(suspect_path);

    const auto truncated_path = dir / "someboxone_test_xvd_truncated.bin";
    std::vector<std::uint8_t> truncated(xvd.begin(), xvd.begin() + 0x400);
    if (!write_temp(truncated_path, truncated)) {
        std::cerr << "Could not create temporary truncated XVD test file\n";
        std::filesystem::remove(xvd_path);
        return 1;
    }
    const auto truncated_report = someboxone::inspect_file(truncated_path);
    if (truncated_report.report.find("complete known fixed XVD metadata fields") == std::string::npos) {
        std::cerr << "Truncated XVD warning test failed\n";
        std::filesystem::remove(xvd_path);
        std::filesystem::remove(truncated_path);
        return 1;
    }

    std::vector<std::uint8_t> unknown(16, 0x42);
    if (!write_temp(unknown_path, unknown)) {
        std::cerr << "Could not create temporary unknown test file\n";
        std::filesystem::remove(xvd_path);
        return 1;
    }
    const auto unknown_report = someboxone::inspect_file(unknown_path);
    if (unknown_report.report.find("unknown or container-wrapped") == std::string::npos) {
        std::cerr << "Unknown signature classification test failed\n";
        std::filesystem::remove(xvd_path);
        std::filesystem::remove(truncated_path);
        std::filesystem::remove(unknown_path);
        return 1;
    }

    const auto scan_dir = dir / "someboxone_test_scan_directory";
    std::filesystem::create_directories(scan_dir);
    const auto scan_xvd_path = scan_dir / "system.xvd";
    const auto ignored_path = scan_dir / "notes.txt";
    auto scan_xvd = xvd;
    put32(scan_xvd, 0x280, 1); // Dynamic, so the scan tests decoded type rendering.
    if (!write_temp(scan_xvd_path, scan_xvd) || !write_temp(ignored_path, unknown)) {
        std::cerr << "Could not create directory-scan fixtures\n";
        std::filesystem::remove_all(scan_dir);
        std::filesystem::remove(xvd_path);
        std::filesystem::remove(truncated_path);
        std::filesystem::remove(unknown_path);
        return 1;
    }
    const auto scan = someboxone::scan_directory(scan_dir);
    if (scan.exit_code != 0 || scan.report.find("Candidate files checked: 1") == std::string::npos ||
        scan.report.find("system.xvd") == std::string::npos ||
        scan.report.find("XVD-compatible") == std::string::npos ||
        scan.report.find("Dynamic (0x00000001)") == std::string::npos ||
        scan.report.find("EraTools (0x00000012)") == std::string::npos ||
        scan.report.find("Content-type counts: EraTools=1;") == std::string::npos ||
        scan.report.find("Package-version counts: 0.0.0.0=1;") == std::string::npos) {
        std::cerr << "Directory scan test failed\n" << scan.report;
        std::filesystem::remove_all(scan_dir);
        std::filesystem::remove(xvd_path);
        std::filesystem::remove(truncated_path);
        std::filesystem::remove(unknown_path);
        return 1;
    }

    // XVC descriptor regression fixture: a synthetic title package with one bounded region.
    const auto xvc_path = dir / "someboxone_test_title.xvc";
    std::vector<std::uint8_t> xvc_bytes(0x7000, 0);
    for (std::size_t i = 0; i < 8; ++i) xvc_bytes[0x200 + i] = static_cast<std::uint8_t>(magic[i]);
    put32(xvc_bytes, 0x208, 0x04);       // Synthetic data-integrity-disabled header; encryption remains enabled.
    put32(xvc_bytes, 0x20C, 3);
    put64(xvc_bytes, 0x218, 0xAA000ULL); // One allocation block.
    put32(xvc_bytes, 0x280, 1);          // Dynamic XVD-compatible layout.
    put32(xvc_bytes, 0x284, 1);          // Title is an XVC-enabled content type.
    put32(xvc_bytes, 0x290, 0x2000);     // XVC descriptor/table region size.
    put32(xvc_bytes, 0x294, 0x1000);     // Dynamic header size.
    put32(xvc_bytes, 0x298, 0xAA000);
    // XVC-info starts at 0x3000 for this fixture; only identifiers/metadata are synthetic.
    xvc_bytes[0x3000] = 0x78; xvc_bytes[0x3001] = 0x56;
    xvc_bytes[0x3002] = 0x34; xvc_bytes[0x3003] = 0x12;
    xvc_bytes[0x3010] = 0x11; // non-zero key GUID identifier, not key material
    const std::string xvc_description = "Test Game";
    for (std::size_t i = 0; i < xvc_description.size(); ++i) {
        xvc_bytes[0x3C10 + i * 2] = static_cast<std::uint8_t>(xvc_description[i]);
        xvc_bytes[0x3C11 + i * 2] = 0;
    }
    put32(xvc_bytes, 0x3D10, 1); // Descriptor version 1.
    put32(xvc_bytes, 0x3D14, 1); // One region.
    put16(xvc_bytes, 0x3D1E, 1); // One key identifier.
    put32(xvc_bytes, 0x3D3C, 0); // No update segments.
    const std::size_t region = 0x3DA8;
    put32(xvc_bytes, region, 1);         // Region ID.
    put16(xvc_bytes, region + 4, 0);     // References key ID 0.
    put32(xvc_bytes, region + 8, 0x10);  // Present flag.
    const std::string region_description = "XVC-HD";
    for (std::size_t i = 0; i < region_description.size(); ++i) {
        xvc_bytes[region + 0x10 + i * 2] = static_cast<std::uint8_t>(region_description[i]);
        xvc_bytes[region + 0x11 + i * 2] = 0;
    }
    put64(xvc_bytes, region + 0x50, 0x6000);
    put64(xvc_bytes, region + 0x58, 0x1000);
    if (!write_temp(xvc_path, xvc_bytes)) {
        std::cerr << "Could not create synthetic XVC fixture\n";
        std::filesystem::remove_all(scan_dir);
        std::filesystem::remove(xvd_path);
        return 1;
    }
    const auto xvc_header = someboxone::read_xvd_header(xvc_path, xvc_bytes.size(), error);
    const auto xvc_descriptor = xvc_header
        ? someboxone::read_xvc_descriptor(xvc_path, xvc_bytes.size(), *xvc_header, error)
        : std::nullopt;
    const auto xvc_report = someboxone::inspect_file(xvc_path);
    if (!xvc_header || !xvc_descriptor || xvc_descriptor->version != 1 ||
        xvc_descriptor->content_id[0] != 0x78 || xvc_descriptor->description != "Test Game" ||
        xvc_descriptor->regions.size() != 1 || xvc_descriptor->regions[0].description != "XVC-HD" ||
        xvc_descriptor->regions[0].offset != 0x6000 || !xvc_descriptor->regions[0].extent_in_file ||
        xvc_report.report.find("Classification: XVC game-package candidate") == std::string::npos ||
        xvc_report.report.find("Descriptor version: 1") == std::string::npos ||
        xvc_report.report.find("Description: Test Game") == std::string::npos ||
        xvc_report.report.find("Key GUID [0]") == std::string::npos ||
        xvc_report.report.find("region-hash=0x0000000000000000") == std::string::npos ||
        xvc_report.report.find("Region [0]") == std::string::npos ||
        xvc_report.report.find("not decrypted, mounted, extracted, or executed") == std::string::npos) {
        std::cerr << "XVC descriptor parsing / inspection test failed: " << error << "\n" << xvc_report.report;
        std::filesystem::remove(xvc_path);
        std::filesystem::remove_all(scan_dir);
        std::filesystem::remove(xvd_path);
        return 1;
    }

    const auto xvc_bad_magic_path = dir / "someboxone_test_bad_magic.xvc";
    if (!write_temp(xvc_bad_magic_path, std::vector<std::uint8_t>(0x1000, 0))) {
        std::cerr << "Could not create malformed-magic XVC fixture\n";
        std::filesystem::remove(xvc_path);
        std::filesystem::remove_all(scan_dir);
        std::filesystem::remove(xvd_path);
        return 1;
    }
    const auto xvc_bad_magic_report = someboxone::inspect_file(xvc_bad_magic_path);
    if (xvc_bad_magic_report.report.find(".xvc extension detected") == std::string::npos ||
        xvc_bad_magic_report.report.find("was not parsed as an XVC") == std::string::npos) {
        std::cerr << "XVC extension with unrecognized magic test failed\n" << xvc_bad_magic_report.report;
        std::filesystem::remove(xvc_bad_magic_path);
        std::filesystem::remove(xvc_path);
        std::filesystem::remove_all(scan_dir);
        std::filesystem::remove(xvd_path);
        return 1;
    }
    std::filesystem::remove(xvc_bad_magic_path);

    // Reject impossible region table counts before allocating or reading any table data.
    auto malformed_xvc = xvc_bytes;
    put32(malformed_xvc, 0x3D14, 0xFFFFFFFFU);
    const auto malformed_xvc_path = dir / "someboxone_test_malformed.xvc";
    if (!write_temp(malformed_xvc_path, malformed_xvc)) {
        std::cerr << "Could not create malformed XVC fixture\n";
        std::filesystem::remove(xvc_path);
        std::filesystem::remove_all(scan_dir);
        std::filesystem::remove(xvd_path);
        return 1;
    }
    const auto malformed_header = someboxone::read_xvd_header(malformed_xvc_path, malformed_xvc.size(), error);
    const auto malformed_descriptor = malformed_header
        ? someboxone::read_xvc_descriptor(malformed_xvc_path, malformed_xvc.size(), *malformed_header, error)
        : std::nullopt;
    if (malformed_descriptor || error.find("safety limits") == std::string::npos) {
        std::cerr << "Malformed XVC table bounds test failed: " << error << "\n";
        std::filesystem::remove(malformed_xvc_path);
        std::filesystem::remove(xvc_path);
        std::filesystem::remove_all(scan_dir);
        std::filesystem::remove(xvd_path);
        return 1;
    }
    std::filesystem::remove(malformed_xvc_path);
    std::filesystem::remove(xvc_path);

    std::filesystem::remove_all(scan_dir);
    std::filesystem::remove(xvd_path);
    std::filesystem::remove(dynamic_path);
    std::filesystem::remove(truncated_path);
    std::filesystem::remove(unknown_path);
    return 0;
}
