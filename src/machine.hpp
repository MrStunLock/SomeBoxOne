#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace someboxone {

// Phase 3 starting point: a bounded little-endian guest address space. This is
// an infrastructure layer only; it is not an x86-64 CPU or Xbox hardware model.
class GuestAddressSpace {
public:
    using MmioRead = std::function<std::optional<std::uint64_t>(std::uint64_t, std::uint8_t)>;
    using MmioWrite = std::function<bool(std::uint64_t, std::uint8_t, std::uint64_t)>;

    enum class RegionKind { Ram, Mmio };
    struct RegionInfo {
        std::uint64_t base{};
        std::uint64_t size{};
        RegionKind kind{RegionKind::Ram};
        std::string name;
    };

    explicit GuestAddressSpace(std::uint64_t max_ram_bytes = 512ULL * 1024ULL * 1024ULL)
        : max_ram_bytes_(max_ram_bytes) {}

    bool map_ram(std::uint64_t base, std::uint64_t size, std::string name) {
        if (size == 0 || size > max_ram_bytes_ - ram_bytes_mapped_ ||
            size > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) return false;
        if (!can_map(base, size)) return false;
        Region region;
        region.info = {base, size, RegionKind::Ram, std::move(name)};
        try {
            region.bytes.resize(static_cast<std::size_t>(size), 0);
        } catch (...) {
            return false;
        }
        try {
            regions_.push_back(std::move(region));
        } catch (...) {
            return false;
        }
        ram_bytes_mapped_ += size;
        return true;
    }

    bool map_mmio(std::uint64_t base, std::uint64_t size, std::string name,
                  MmioRead read, MmioWrite write) {
        if (size == 0 || !read || !write || !can_map(base, size)) return false;
        Region region;
        region.info = {base, size, RegionKind::Mmio, std::move(name)};
        region.read = std::move(read);
        region.write = std::move(write);
        regions_.push_back(std::move(region));
        return true;
    }

    std::optional<std::uint64_t> read(std::uint64_t address, std::uint8_t width) const {
        if (!valid_width(width)) return std::nullopt;
        const Region* region = find(address, width);
        if (!region) return std::nullopt;
        const auto offset = address - region->info.base;
        if (region->info.kind == RegionKind::Mmio) return region->read(offset, width);
        std::uint64_t value = 0;
        for (std::uint8_t i = 0; i < width; ++i) {
            value |= static_cast<std::uint64_t>(region->bytes[static_cast<std::size_t>(offset + i)]) << (static_cast<unsigned>(i) * 8U);
        }
        return value;
    }

    bool write(std::uint64_t address, std::uint8_t width, std::uint64_t value) {
        if (!valid_width(width)) return false;
        Region* region = find_mutable(address, width);
        if (!region) return false;
        const auto offset = address - region->info.base;
        if (region->info.kind == RegionKind::Mmio) return region->write(offset, width, value);
        for (std::uint8_t i = 0; i < width; ++i) {
            region->bytes[static_cast<std::size_t>(offset + i)] = static_cast<std::uint8_t>(value >> (static_cast<unsigned>(i) * 8U));
        }
        return true;
    }

    std::vector<RegionInfo> regions() const {
        std::vector<RegionInfo> result;
        result.reserve(regions_.size());
        for (const auto& region : regions_) result.push_back(region.info);
        return result;
    }

private:
    struct Region {
        RegionInfo info;
        std::vector<std::uint8_t> bytes;
        MmioRead read;
        MmioWrite write;
    };

    static bool valid_width(std::uint8_t width) {
        return width == 1 || width == 2 || width == 4 || width == 8;
    }

    bool can_map(std::uint64_t base, std::uint64_t size) const {
        if (size == 0 || base > (std::numeric_limits<std::uint64_t>::max)() - size) return false;
        const std::uint64_t end = base + size;
        for (const auto& region : regions_) {
            const std::uint64_t other_end = region.info.base + region.info.size;
            if (base < other_end && region.info.base < end) return false;
        }
        return true;
    }

    const Region* find(std::uint64_t address, std::uint8_t width) const {
        if (address > (std::numeric_limits<std::uint64_t>::max)() - width) return nullptr;
        const std::uint64_t end = address + width;
        for (const auto& region : regions_) {
            const auto region_end = region.info.base + region.info.size;
            if (address >= region.info.base && end <= region_end) return &region;
        }
        return nullptr;
    }

    Region* find_mutable(std::uint64_t address, std::uint8_t width) {
        if (address > (std::numeric_limits<std::uint64_t>::max)() - width) return nullptr;
        const std::uint64_t end = address + width;
        for (auto& region : regions_) {
            const auto region_end = region.info.base + region.info.size;
            if (address >= region.info.base && end <= region_end) return &region;
        }
        return nullptr;
    }

    std::uint64_t max_ram_bytes_;
    std::uint64_t ram_bytes_mapped_{};
    std::vector<Region> regions_;
};

} // namespace someboxone
