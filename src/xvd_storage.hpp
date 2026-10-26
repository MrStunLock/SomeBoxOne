#pragma once

#include "xvd.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace someboxone {

// Conservative, read-only XVD logical-volume access.
// Supported initially: fixed (non-dynamic) XVDs with plaintext data and a
// fully in-file extent derived from the documented layout formula.
// Dynamic BAT translation, encrypted payloads, writes and filesystem parsing
// are deliberately not supported by this layer.
struct XvdStorageProbe {
    bool header_recognized{};
    bool logical_read_supported{};
    bool data_encrypted{};
    bool data_integrity_enabled{};
    std::uint32_t xvd_type{};
    std::uint64_t file_size{};
    std::uint64_t drive_size{};
    std::uint64_t data_offset{};
    std::string detail;
};

class ReadOnlyXvdVolume {
public:
    static constexpr std::uint64_t max_read_bytes = 16ULL * 1024ULL * 1024ULL;

    static XvdStorageProbe probe(const std::filesystem::path& path) {
        XvdStorageProbe result;
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            result.detail = "Cannot determine physical file size: " + ec.message();
            return result;
        }
        result.file_size = size;
        std::string error;
        const auto header = read_xvd_header(path, size, error);
        if (!header) {
            result.detail = "XVD header could not be parsed: " + error;
            return result;
        }

        result.header_recognized = true;
        result.data_encrypted = (header->volume_flags & encryption_disabled_flag) == 0;
        result.data_integrity_enabled = (header->volume_flags & data_integrity_disabled_flag) == 0;
        result.xvd_type = header->type;
        result.drive_size = header->drive_size;
        result.data_offset = header->drive_data_offset;

        if (result.data_encrypted) {
            result.detail = "Logical reads are disabled: payload encryption is enabled and this build has no key/decryption implementation.";
            return result;
        }
        if (header->type == 1) {
            result.detail = "Logical reads are disabled: dynamic XVD BAT-to-file mapping is not implemented. BAT words are not treated as offsets.";
            return result;
        }
        if (header->type != 0) {
            result.detail = "Logical reads are disabled: unknown XVD type " + std::to_string(header->type) + ".";
            return result;
        }
        if (!header->layout_estimated) {
            result.detail = "Logical reads are disabled: the documented layout formula could not produce a data offset.";
            return result;
        }
        if (header->drive_data_offset > size || header->drive_size > size - header->drive_data_offset) {
            result.detail = "Logical reads are disabled: computed fixed-volume extent falls outside the physical file.";
            return result;
        }

        result.logical_read_supported = true;
        result.detail = "Read-only fixed-volume mapping is available using the calculated data base.";
        if (result.data_integrity_enabled) {
            result.detail += " Per-block hash-tree verification is not implemented; reads are not authenticated.";
        } else {
            result.detail += " Data-integrity verification is disabled in the header.";
        }
        return result;
    }

    static std::optional<ReadOnlyXvdVolume> open(const std::filesystem::path& path,
                                                 std::string* error = nullptr) {
        if (error) error->clear();
        const auto state = probe(path);
        if (!state.logical_read_supported) {
            if (error) *error = state.detail;
            return std::nullopt;
        }
        ReadOnlyXvdVolume volume;
        volume.path_ = path;
        volume.file_size_ = state.file_size;
        volume.drive_size_ = state.drive_size;
        volume.data_offset_ = state.data_offset;
        volume.data_integrity_enabled_ = state.data_integrity_enabled;
        return volume;
    }

    std::uint64_t size() const { return drive_size_; }
    std::uint64_t physical_data_offset() const { return data_offset_; }
    bool data_integrity_enabled() const { return data_integrity_enabled_; }

    std::optional<std::vector<std::uint8_t>> read_at(std::uint64_t logical_offset,
                                                      std::uint64_t count,
                                                      std::string* error = nullptr) const {
        if (error) error->clear();
        if (count > max_read_bytes) {
            if (error) *error = "Read exceeds the 16 MiB per-call safety limit.";
            return std::nullopt;
        }
        if (logical_offset > drive_size_ || count > drive_size_ - logical_offset) {
            if (error) *error = "Logical read extends beyond the declared virtual drive size.";
            return std::nullopt;
        }
        if (data_offset_ > (std::numeric_limits<std::uint64_t>::max)() - logical_offset) {
            if (error) *error = "Physical read offset overflows 64-bit arithmetic.";
            return std::nullopt;
        }
        const auto physical_offset = data_offset_ + logical_offset;
        if (physical_offset > file_size_ || count > file_size_ - physical_offset) {
            if (error) *error = "Physical read extends beyond the file.";
            return std::nullopt;
        }
        if (physical_offset > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)()) ||
            count > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            if (error) *error = "Read cannot be represented safely on this build.";
            return std::nullopt;
        }

        std::ifstream in(path_, std::ios::binary);
        if (!in) {
            if (error) *error = "Could not open XVD source for read-only access.";
            return std::nullopt;
        }
        in.seekg(static_cast<std::streamoff>(physical_offset), std::ios::beg);
        if (!in) {
            if (error) *error = "Could not seek to the calculated fixed-volume data offset.";
            return std::nullopt;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
        if (count != 0) {
            in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
            if (in.gcount() != static_cast<std::streamsize>(count)) {
                if (error) *error = "Short read from fixed-volume data.";
                return std::nullopt;
            }
        }
        return bytes;
    }

private:
    static constexpr std::uint32_t encryption_disabled_flag = 0x02U;
    static constexpr std::uint32_t data_integrity_disabled_flag = 0x04U;

    std::filesystem::path path_;
    std::uint64_t file_size_{};
    std::uint64_t drive_size_{};
    std::uint64_t data_offset_{};
    bool data_integrity_enabled_{};
};

} // namespace someboxone
