#pragma once

#include "framebuffer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace someboxone::graphics {

// Cross-process scan-out surface shared by SomeBoxOne and SomeBoxOneDisplay.
// A renderer publishes complete, top-down 32-bit frames; the display process snapshots
// them and presents through a D3D11/DXGI swap chain (with GDI fallback). `publish_bgra`
// accepts the runtime's byte-ordered BGRA capture directly to avoid an extra conversion.
// The transport does not rasterise guest GPU commands; the title runtime captures the
// CPU-backed texture that its existing software-capture D3D11 model observes.
class SharedFrameMapping {
public:
    static constexpr std::uint32_t kMagic = 0x53424652U; // "SBFR"
    static constexpr std::uint32_t kVersion = 3U;
#ifdef _WIN32
    static constexpr wchar_t kMappingName[] = L"Local\\SomeBoxOne.GuestFramebuffer.v3";
#endif

    SharedFrameMapping() = default;
    SharedFrameMapping(const SharedFrameMapping&) = delete;
    SharedFrameMapping& operator=(const SharedFrameMapping&) = delete;

    ~SharedFrameMapping() { close(); }

    bool open() noexcept {
#ifdef _WIN32
        if (header_) return true;
        mapping_bytes_ = offsetof(SharedHeader, pixels) + FrameBuffer::kMaxPixels * sizeof(std::uint32_t);
        const std::uint64_t total_bytes = static_cast<std::uint64_t>(mapping_bytes_);
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            static_cast<DWORD>((total_bytes >> 32U) & 0xFFFFFFFFULL),
            static_cast<DWORD>(total_bytes & 0xFFFFFFFFULL), kMappingName);
        if (!mapping_) return false;
        view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, mapping_bytes_);
        if (!view_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }
        header_ = static_cast<SharedHeader*>(view_);
        if (header_->magic != kMagic || header_->version != kVersion) {
            // The mapping is newly created (or from an incompatible previous
            // build). No published frame is valid until a writer commits one.
            header_->sequence = 0;
            header_->magic = kMagic;
            header_->version = kVersion;
            header_->width = 0;
            header_->height = 0;
            header_->pixel_count = 0;
            header_->frame_id = 0;
            header_->status_sequence = 0;
            std::memset(header_->status_text, 0, sizeof(header_->status_text));
            header_->game_info_sequence = 0;
            std::memset(header_->display_name, 0, sizeof(header_->display_name));
            std::memset(header_->splash_path, 0, sizeof(header_->splash_path));
            MemoryBarrier();
        }
        return true;
#else
        return false;
#endif
    }

    // Status is shared separately from the frame sequence so the display can report
    // a guest startup/fault even when no renderer has ever published pixels.
    bool publish_status(const std::string& status) noexcept {
#ifdef _WIN32
        if (!header_ && !open()) return false;
        LONG seq = InterlockedCompareExchange(&header_->status_sequence, 0, 0);
        if ((seq & 1L) != 0) InterlockedIncrement(&header_->status_sequence);
        InterlockedIncrement(&header_->status_sequence); // odd: status update in progress
        std::memset(header_->status_text, 0, sizeof(header_->status_text));
        const std::size_t count = (std::min)(status.size(), sizeof(header_->status_text) - 1U);
        if (count != 0) std::memcpy(header_->status_text, status.data(), count);
        MemoryBarrier();
        InterlockedIncrement(&header_->status_sequence); // even: committed status
        return true;
#else
        (void)status;
        return false;
#endif
    }

    bool read_status(std::string& status) const {
#ifdef _WIN32
        if (!header_) return false;
        const LONG before = InterlockedCompareExchange(&header_->status_sequence, 0, 0);
        if (before == 0 || (before & 1L) != 0) return false;
        char snapshot[256]{};
        std::memcpy(snapshot, header_->status_text, sizeof(snapshot));
        MemoryBarrier();
        if (InterlockedCompareExchange(&header_->status_sequence, 0, 0) != before) return false;
        std::size_t length = 0;
        while (length < sizeof(snapshot) && snapshot[length] != '\0') ++length;
        status.assign(snapshot, length);
        return true;
#else
        (void)status;
        return false;
#endif
    }

    // Publish the active title's display name and package splash path. Strings are UTF-8;
    // the receiver converts them to UTF-16 before calling Windows APIs.
    bool publish_game_info(const std::string& display_name, const std::string& splash_path) noexcept {
#ifdef _WIN32
        if (!header_ && !open()) return false;
        LONG seq = InterlockedCompareExchange(&header_->game_info_sequence, 0, 0);
        if ((seq & 1L) != 0) InterlockedIncrement(&header_->game_info_sequence);
        InterlockedIncrement(&header_->game_info_sequence); // odd: title update in progress
        std::memset(header_->display_name, 0, sizeof(header_->display_name));
        std::memset(header_->splash_path, 0, sizeof(header_->splash_path));
        const std::size_t title_count = (std::min)(display_name.size(), sizeof(header_->display_name) - 1U);
        const std::size_t splash_count = (std::min)(splash_path.size(), sizeof(header_->splash_path) - 1U);
        if (title_count) std::memcpy(header_->display_name, display_name.data(), title_count);
        if (splash_count) std::memcpy(header_->splash_path, splash_path.data(), splash_count);
        MemoryBarrier();
        InterlockedIncrement(&header_->game_info_sequence); // even: committed title update
        return true;
#else
        (void)display_name;
        (void)splash_path;
        return false;
#endif
    }

    bool read_game_info(std::string& display_name, std::string& splash_path, std::uint32_t& generation) const {
#ifdef _WIN32
        if (!header_) return false;
        const LONG before = InterlockedCompareExchange(&header_->game_info_sequence, 0, 0);
        if (before == 0 || (before & 1L) != 0) return false;
        char title_snapshot[256]{};
        char splash_snapshot[1024]{};
        std::memcpy(title_snapshot, header_->display_name, sizeof(title_snapshot));
        std::memcpy(splash_snapshot, header_->splash_path, sizeof(splash_snapshot));
        MemoryBarrier();
        if (InterlockedCompareExchange(&header_->game_info_sequence, 0, 0) != before) return false;
        const auto title_len = std::find(title_snapshot, title_snapshot + sizeof(title_snapshot), '\0') - title_snapshot;
        const auto splash_len = std::find(splash_snapshot, splash_snapshot + sizeof(splash_snapshot), '\0') - splash_snapshot;
        display_name.assign(title_snapshot, static_cast<std::size_t>(title_len));
        splash_path.assign(splash_snapshot, static_cast<std::size_t>(splash_len));
        generation = static_cast<std::uint32_t>(before);
        return true;
#else
        (void)display_name;
        (void)splash_path;
        (void)generation;
        return false;
#endif
    }

    // Prevent a previous game's last frame from covering a new title's splash while the
    // guest is still booting. The first frame after this ID is displayed normally.
    std::uint64_t current_frame_id() const noexcept {
#ifdef _WIN32
        if (!header_) return 0;
        const LONG before = InterlockedCompareExchange(&header_->sequence, 0, 0);
        if (before == 0 || (before & 1L) != 0) return 0;
        const std::uint64_t frame_id = header_->frame_id;
        MemoryBarrier();
        return InterlockedCompareExchange(&header_->sequence, 0, 0) == before ? frame_id : 0;
#else
        return 0;
#endif
    }

    void close() noexcept {
#ifdef _WIN32
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        view_ = nullptr;
        mapping_ = nullptr;
        header_ = nullptr;
        mapping_bytes_ = 0;
#endif
    }

    [[nodiscard]] bool is_open() const noexcept {
#ifdef _WIN32
        return header_ != nullptr;
#else
        return false;
#endif
    }

    // Direct publisher for the title runtime's existing top-down BGRA frame sink.
    // It avoids allocating a second FrameBuffer and avoids per-pixel repacking on the
    // emulation thread. The display treats the shared words as BGRA bytes (alpha ignored).
    bool publish_bgra(std::uint32_t width, std::uint32_t height,
                      const std::uint8_t* bgra, std::size_t byte_count) noexcept {
#ifdef _WIN32
        if (width == 0 || height == 0 || width > FrameBuffer::kMaxDimension ||
            height > FrameBuffer::kMaxDimension || !bgra) return false;
        const std::uint64_t count64 = static_cast<std::uint64_t>(width) * height;
        if (count64 == 0 || count64 > FrameBuffer::kMaxPixels ||
            count64 > (std::numeric_limits<std::size_t>::max)() / 4U ||
            byte_count != static_cast<std::size_t>(count64) * 4U) return false;
        if (!header_ && !open()) return false;

        LONG seq = InterlockedCompareExchange(&header_->sequence, 0, 0);
        if ((seq & 1L) != 0) InterlockedIncrement(&header_->sequence);
        InterlockedIncrement(&header_->sequence); // odd: frame write in progress
        header_->width = width;
        header_->height = height;
        header_->pixel_count = static_cast<std::uint32_t>(count64);
        std::memcpy(header_->pixels, bgra, byte_count);
        ++header_->frame_id;
        MemoryBarrier();
        InterlockedIncrement(&header_->sequence); // even: committed frame
        return true;
#else
        (void)width; (void)height; (void)bgra; (void)byte_count;
        return false;
#endif
    }

    // Called by other renderer integrations that already hold a packed FrameBuffer.
    // Concurrent readers never consume a partially written frame.
    bool publish(const FrameBuffer& frame) noexcept {
#ifdef _WIN32
        if (!header_ && !open()) return false;
        if (frame.empty() || frame.width() == 0 || frame.height() == 0 ||
            frame.pixels().size() > FrameBuffer::kMaxPixels) return false;

        LONG seq = InterlockedCompareExchange(&header_->sequence, 0, 0);
        if ((seq & 1L) != 0) InterlockedIncrement(&header_->sequence); // recover stale writer state
        InterlockedIncrement(&header_->sequence); // odd: frame write in progress
        header_->width = frame.width();
        header_->height = frame.height();
        header_->pixel_count = static_cast<std::uint32_t>(frame.pixels().size());
        std::memcpy(header_->pixels, frame.pixels().data(), frame.pixels().size_bytes());
        ++header_->frame_id;
        MemoryBarrier();
        InterlockedIncrement(&header_->sequence); // even: committed frame
        return true;
#else
        (void)frame;
        return false;
#endif
    }

    // Copies only a new, stable frame. The caller can skip invalidation when false
    // is returned (no frame yet, no new frame, or a writer was mid-present).
    bool read_latest(FrameBuffer& destination, std::uint64_t& last_frame_id) {
#ifdef _WIN32
        if (!header_ && !open()) return false;
        const LONG before = InterlockedCompareExchange(&header_->sequence, 0, 0);
        if (before == 0 || (before & 1L) != 0) return false;
        const std::uint32_t width = header_->width;
        const std::uint32_t height = header_->height;
        const std::uint32_t count = header_->pixel_count;
        const std::uint64_t frame_id = header_->frame_id;
        if (frame_id == 0 || frame_id == last_frame_id || width == 0 || height == 0 ||
            width > FrameBuffer::kMaxDimension || height > FrameBuffer::kMaxDimension ||
            static_cast<std::uint64_t>(width) * height != count || count > FrameBuffer::kMaxPixels)
            return false;

        try {
            scratch_.resize(count);
        } catch (const std::bad_alloc&) {
            return false;
        }
        std::memcpy(scratch_.data(), header_->pixels, static_cast<std::size_t>(count) * sizeof(std::uint32_t));
        MemoryBarrier();
        if (InterlockedCompareExchange(&header_->sequence, 0, 0) != before) return false;

        if (destination.width() != width || destination.height() != height || destination.empty()) {
            if (!destination.resize(width, height)) return false;
        }
        std::copy(scratch_.begin(), scratch_.end(), destination.writable_pixels().begin());
        last_frame_id = frame_id;
        return true;
#else
        (void)destination;
        (void)last_frame_id;
        return false;
#endif
    }

private:
#ifdef _WIN32
    struct SharedHeader {
        volatile LONG sequence;
        std::uint32_t magic;
        std::uint32_t version;
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t pixel_count;
        std::uint32_t reserved;
        std::uint64_t frame_id;
        volatile LONG status_sequence;
        char status_text[256];
        volatile LONG game_info_sequence;
        char display_name[256];
        char splash_path[1024];
        std::uint32_t pixels[1];
    };
    static_assert(offsetof(SharedHeader, pixels) % alignof(std::uint32_t) == 0);
    HANDLE mapping_{};
    void* view_{};
    SharedHeader* header_{};
    std::size_t mapping_bytes_{};
#endif
    std::vector<std::uint32_t> scratch_;
};

} // namespace someboxone::graphics
