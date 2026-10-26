#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <vector>

namespace someboxone::graphics {

// Portable 32-bit framebuffer. The packed API uses 0x00RRGGBB, whose little-endian
// bytes are B,G,R,0 and match the D3D11 BGRA texture's colour channels (alpha is ignored).
// The IPC fast path may carry BGRA bytes from the title capture sink directly. The buffer
// stays backend-neutral and does not attempt to translate guest graphics commands.
class FrameBuffer {
public:
    static constexpr std::uint32_t kMaxDimension = 8192;
    static constexpr std::size_t kMaxPixels = 16U * 1024U * 1024U;

    FrameBuffer() = default;
    FrameBuffer(std::uint32_t width, std::uint32_t height) { (void)resize(width, height); }

    bool resize(std::uint32_t width, std::uint32_t height) {
        if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) return false;
        const auto w = static_cast<std::size_t>(width);
        const auto h = static_cast<std::size_t>(height);
        if (w > std::numeric_limits<std::size_t>::max() / h || w * h > kMaxPixels) return false;
        try {
            std::vector<std::uint32_t> replacement(w * h, 0U);
            pixels_.swap(replacement);
            width_ = width;
            height_ = height;
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] bool empty() const noexcept { return pixels_.empty(); }
    [[nodiscard]] std::span<const std::uint32_t> pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::span<std::uint32_t> writable_pixels() noexcept { return pixels_; }

    void clear(std::uint32_t rgb) noexcept { std::fill(pixels_.begin(), pixels_.end(), rgb & 0x00FFFFFFU); }

    bool put_pixel(std::uint32_t x, std::uint32_t y, std::uint32_t rgb) noexcept {
        if (x >= width_ || y >= height_) return false;
        pixels_[static_cast<std::size_t>(y) * width_ + x] = rgb & 0x00FFFFFFU;
        return true;
    }

    [[nodiscard]] bool get_pixel(std::uint32_t x, std::uint32_t y, std::uint32_t& rgb) const noexcept {
        if (x >= width_ || y >= height_) return false;
        rgb = pixels_[static_cast<std::size_t>(y) * width_ + x];
        return true;
    }

    // Generates obvious host-side color bars/checker tiles to validate the display
    // path before guest rendering APIs are implemented. This is not game output.
    void draw_test_pattern() noexcept {
        if (empty()) return;
        constexpr std::uint32_t colors[] = {
            0xE53935U, 0xFB8C00U, 0xFDD835U, 0x43A047U,
            0x00ACC1U, 0x1E88E5U, 0x5E35B1U, 0xD81B60U
        };
        const std::uint32_t band_width = (width_ + 7U) / 8U;
        for (std::uint32_t y = 0; y < height_; ++y) {
            for (std::uint32_t x = 0; x < width_; ++x) {
                const auto band = std::min<std::uint32_t>(x / band_width, 7U);
                std::uint32_t color = colors[band];
                if (((x / 24U) + (y / 24U)) % 2U != 0U) {
                    const auto r = (color >> 16U) & 0xFFU;
                    const auto g = (color >> 8U) & 0xFFU;
                    const auto b = color & 0xFFU;
                    color = ((r * 3U / 5U) << 16U) | ((g * 3U / 5U) << 8U) | (b * 3U / 5U);
                }
                pixels_[static_cast<std::size_t>(y) * width_ + x] = color;
            }
        }
    }

private:
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::vector<std::uint32_t> pixels_;
};

} // namespace someboxone::graphics
