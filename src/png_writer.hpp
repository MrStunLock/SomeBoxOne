#pragma once
// Minimal dependency-free PNG writer (8-bit RGB, uncompressed "stored" deflate blocks).
// Files are larger than a compressed PNG but open everywhere, including the Windows photo viewer.

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace someboxone {

inline std::uint32_t png_crc32(const std::uint8_t* d, std::size_t n, std::uint32_t crc = 0xFFFFFFFFu) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) { std::uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1); t[i] = c; }
        return t;
    }();
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

inline void png_put32(std::vector<std::uint8_t>& o, std::uint32_t v) { o.push_back(static_cast<std::uint8_t>(v >> 24)); o.push_back(static_cast<std::uint8_t>(v >> 16)); o.push_back(static_cast<std::uint8_t>(v >> 8)); o.push_back(static_cast<std::uint8_t>(v)); }

inline void png_chunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
    png_put32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    png_put32(out, png_crc32(body.data(), body.size()) ^ 0xFFFFFFFFu);
}

// `rgb` is width*height*3 bytes. `scale` > 1 replicates pixels (nearest neighbour).
inline bool write_png_rgb(const std::filesystem::path& path, const std::vector<std::uint8_t>& rgb, unsigned width, unsigned height, unsigned scale = 1) {
    if (!width || !height || rgb.size() < static_cast<std::size_t>(width) * height * 3 || scale == 0) return false;
    const unsigned W = width * scale, H = height * scale;
    std::vector<std::uint8_t> raw; raw.reserve(static_cast<std::size_t>(H) * (W * 3 + 1));
    for (unsigned y = 0; y < height; ++y) {
        std::vector<std::uint8_t> line; line.push_back(0);
        for (unsigned x = 0; x < width; ++x) for (unsigned s = 0; s < scale; ++s) { const auto* p = &rgb[(static_cast<std::size_t>(y) * width + x) * 3]; line.push_back(p[0]); line.push_back(p[1]); line.push_back(p[2]); }
        for (unsigned s = 0; s < scale; ++s) raw.insert(raw.end(), line.begin(), line.end());
    }
    std::vector<std::uint8_t> z = {0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t byte : raw) { a = (a + byte) % 65521; b = (b + a) % 65521; }
    for (std::size_t off = 0; off < raw.size();) {
        const std::size_t n = (std::min<std::size_t>)(65535, raw.size() - off);
        z.push_back(off + n >= raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n)); z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n)); z.push_back(static_cast<std::uint8_t>((~n) >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off), raw.begin() + static_cast<std::ptrdiff_t>(off + n));
        off += n;
    }
    png_put32(z, (b << 16) | a);
    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr; png_put32(ihdr, W); png_put32(ihdr, H);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    png_chunk(out, "IHDR", ihdr); png_chunk(out, "IDAT", z); png_chunk(out, "IEND", {});
    std::error_code ec; std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

} // namespace someboxone
