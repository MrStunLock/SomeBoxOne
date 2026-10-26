#pragma once
// SomeBoxOne v0.32.0 - FPS counter overlay.
//
// Draws "NN FPS" (and "LOCK 30/60" when a frame-rate limit is active) into the top-left corner of a BGRA frame with a built-in
// 5x7 bitmap font and a translucent backing box. It is applied to the frame just before it is handed to the presenter, so it
// appears identically in the GUI window, the standalone display and the saved screenshots of a live run, with no dependency
// on fonts, GDI or Direct3D text.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace someboxone {

namespace fps_overlay_detail {
// 5 columns x 7 rows, one byte per row, bit 4 = leftmost pixel.
struct Glyph { char ch; std::uint8_t rows[7]; };
inline constexpr Glyph kFont[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}}, {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}, {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
};
inline const std::uint8_t* glyph(char ch) {
    for (const auto& g : kFont) if (g.ch == ch) return g.rows;
    return kFont[sizeof(kFont) / sizeof(kFont[0]) - 1].rows;   // space
}
inline void blend(std::vector<std::uint8_t>& px, unsigned w, unsigned h, int x, int y, std::uint8_t b, std::uint8_t g, std::uint8_t r, unsigned alpha /*0..255*/) {
    if (x < 0 || y < 0 || static_cast<unsigned>(x) >= w || static_cast<unsigned>(y) >= h) return;
    std::uint8_t* p = &px[(static_cast<std::size_t>(y) * w + static_cast<unsigned>(x)) * 4];
    p[0] = static_cast<std::uint8_t>((p[0] * (255 - alpha) + b * alpha) / 255);
    p[1] = static_cast<std::uint8_t>((p[1] * (255 - alpha) + g * alpha) / 255);
    p[2] = static_cast<std::uint8_t>((p[2] * (255 - alpha) + r * alpha) / 255);
    p[3] = 255;
}
inline unsigned text_width(const std::string& s, unsigned scale) { return static_cast<unsigned>(s.size()) * 6 * scale - scale; }
inline void draw_text(std::vector<std::uint8_t>& px, unsigned w, unsigned h, int x, int y, const std::string& s, unsigned scale, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        const std::uint8_t* rows = glyph(s[i]);
        for (unsigned row = 0; row < 7; ++row)
            for (unsigned col = 0; col < 5; ++col)
                if (rows[row] & (0x10 >> col))
                    for (unsigned dy = 0; dy < scale; ++dy) for (unsigned dx = 0; dx < scale; ++dx)
                        blend(px, w, h, x + static_cast<int>((i * 6 + col) * scale + dx), y + static_cast<int>(row * scale + dy), b, g, r, 255);
    }
}
} // namespace fps_overlay_detail

// `bgra` holds width*height*4 bytes. `fps` is the measured rate; `limit` is the active lock (0 = unlimited).
inline void draw_fps_overlay(std::vector<std::uint8_t>& bgra, unsigned width, unsigned height, double fps, unsigned limit) {
    using namespace fps_overlay_detail;
    if (width < 64 || height < 32 || bgra.size() < static_cast<std::size_t>(width) * height * 4) return;
    // Keep the diagnostics legible without covering a large portion of the game.
    // The previous 3x/4x scaling made the panel disproportionately large on HD frames.
    const unsigned scale = height >= 720 ? 2U : 1U;
    char num[32]; std::snprintf(num, sizeof num, "%.0f FPS", fps < 0 ? 0.0 : fps);
    const std::string line1 = fps > 0.0 ? num : "-- FPS";
    const std::string line2 = limit ? "LOCK " + std::to_string(limit) : std::string("NO LOCK");
    const unsigned pad = 2 * scale, line_h = 7 * scale, gap = 2 * scale;
    const unsigned box_w = (std::max)(text_width(line1, scale), text_width(line2, scale)) + 2 * pad, box_h = 2 * line_h + gap + 2 * pad;
    const int ox = static_cast<int>(2 * scale), oy = static_cast<int>(2 * scale);
    for (unsigned yy = 0; yy < box_h; ++yy) for (unsigned xx = 0; xx < box_w; ++xx)
        blend(bgra, width, height, ox + static_cast<int>(xx), oy + static_cast<int>(yy), 0, 0, 0, 150);       // translucent black backing
    const bool at_limit = limit && fps >= limit * 0.97;
    // line 1: green when holding the lock (or unlimited and fast), amber when below it
    const std::uint8_t g1 = 255, r1 = at_limit || !limit ? 80 : 255, b1 = 80;
    draw_text(bgra, width, height, ox + static_cast<int>(pad), oy + static_cast<int>(pad), line1, scale, b1, g1, r1);
    draw_text(bgra, width, height, ox + static_cast<int>(pad), oy + static_cast<int>(pad + line_h + gap), line2, scale, 200, 200, 200);
}

} // namespace someboxone
