#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace someboxone {

// Bounded, read-only access to physical container-file bytes. This deliberately
// does not interpret XVD regions, decrypt payloads, or expose a logical disk.
class ReadOnlyImageSource {
public:
    static constexpr std::uint64_t max_read_bytes = 16ULL * 1024ULL * 1024ULL;

    static std::optional<ReadOnlyImageSource> open(const std::filesystem::path& path,
                                                   std::string* error = nullptr) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec) {
            set_error(error, "Image source is not an accessible regular file.");
            return std::nullopt;
        }
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            set_error(error, "Could not determine image file size: " + ec.message());
            return std::nullopt;
        }
        ReadOnlyImageSource source;
        source.path_ = path;
        source.size_ = size;
        return source;
    }

    std::uint64_t size() const { return size_; }
    const std::filesystem::path& path() const { return path_; }

    std::optional<std::vector<std::uint8_t>> read_at(std::uint64_t offset, std::uint64_t count,
                                                      std::string* error = nullptr) const {
        if (count > max_read_bytes) {
            set_error(error, "Requested read exceeds the 16 MiB safety limit.");
            return std::nullopt;
        }
        if (offset > size_ || count > size_ - offset) {
            set_error(error, "Requested raw read falls outside the physical file bounds.");
            return std::nullopt;
        }
        if (offset > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)())) {
            set_error(error, "Requested file offset exceeds the host stream offset range.");
            return std::nullopt;
        }
        std::vector<std::uint8_t> result;
        try {
            result.resize(static_cast<std::size_t>(count));
        } catch (...) {
            set_error(error, "Could not allocate the requested read buffer.");
            return std::nullopt;
        }
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            set_error(error, "Could not open image source for read-only access.");
            return std::nullopt;
        }
        input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!input) {
            set_error(error, "Could not seek to the requested physical file offset.");
            return std::nullopt;
        }
        if (count != 0) {
            input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(count));
            if (input.gcount() != static_cast<std::streamsize>(count)) {
                set_error(error, "Short read: source changed or ended before the requested byte count.");
                return std::nullopt;
            }
        }
        if (error) error->clear();
        return result;
    }

private:
    std::filesystem::path path_;
    std::uint64_t size_{};

    static void set_error(std::string* destination, std::string message) {
        if (destination) *destination = std::move(message);
    }
};

} // namespace someboxone
