#pragma once
// SomeBoxOne v0.32.0 - LZX codec for the Xbox xcompress.dll (XMemCompress / XMemDecompress) API.
//
// XMEMCODEC_LZX streams are standard Microsoft LZX (as in CAB/CHM) cut into frames of at most 32768 output bytes. Every frame is
// preceded by a chunk header: `FF uu uu cc cc` (big-endian uncompressed and compressed sizes) or just `cc cc` for a full
// 32768-byte frame. The LZX window and block state carry over from frame to frame; the bit stream restarts on a 16-bit
// boundary at the start of every frame.
//
// The decoder is a clean implementation of the published LZX format (verbatim, aligned-offset and uncompressed blocks, Intel E8
// preprocessing). The encoder only emits uncompressed blocks: it does not shrink data, but every conforming LZX decoder (including
// the title's own) reads it, which is all offline save/cache data needs.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace someboxone::lzx {

constexpr std::size_t kFrameSize = 32768;

class Decoder {
public:
    explicit Decoder(unsigned window_bits = 17) { reset(window_bits); }

    void reset(unsigned window_bits) {
        window_bits = std::clamp(window_bits, 15u, 21u);
        window_bits_ = window_bits;
        window_size_ = 1u << window_bits;
        window_.assign(window_size_, 0);
        wpos_ = 0;
        slots_ = window_bits == 20 ? 42 : window_bits == 21 ? 50 : window_bits * 2;
        std::uint32_t j = 0;
        for (unsigned i = 0, e = 0; i <= 50; i += 2) { extra_[i] = extra_[i + 1] = static_cast<std::uint8_t>(e); if (i != 0 && e < 17) ++e; }
        for (unsigned i = 0; i <= 50; ++i) { base_[i] = j; j += 1u << extra_[i]; }
        r0_ = r1_ = r2_ = 1; block_type_ = 0; block_remaining_ = 0; header_read_ = false;
        intel_filesize_ = 0; intel_pos_ = 0;
        main_len_.assign(256 + 50 * 8 + 8, 0); length_len_.assign(249, 0);
    }

    // Decodes one frame (`out_len` <= 32768 bytes) from `in` into `out`. Returns false on corrupt data.
    bool decode_frame(const std::uint8_t* in, std::size_t in_len, std::uint8_t* out, std::size_t out_len) {
        in_ = in; in_len_ = in_len; ip_ = 0; buf_ = 0; cnt_ = 0; error_ = false;
        if (!header_read_) {
            if (read_bits(1)) { const std::uint32_t hi = read_bits(16), lo = read_bits(16); intel_filesize_ = (hi << 16) | lo; }
            header_read_ = true;
        }
        const std::size_t frame_start = wpos_;
        std::size_t todo = out_len;
        while (todo > 0 && !error_) {
            if (block_remaining_ == 0) {
                if (block_type_ == 3 && (block_length_ & 1)) { /* the pad byte after an odd-length uncompressed block */ ++ip_; }
                if (!read_block_header()) return false;
            }
            std::size_t run = std::min<std::size_t>(block_remaining_, todo);
            todo -= run; block_remaining_ -= static_cast<std::uint32_t>(run);
            if (block_type_ == 3) {
                for (std::size_t i = 0; i < run; ++i) put(ip_ < in_len_ ? in_[ip_++] : 0);
                continue;
            }
            const bool aligned = block_type_ == 2;
            long long left = static_cast<long long>(run);
            while (left > 0 && !error_) {
                int sym = decode(main_);
                if (sym < 0) return false;
                if (sym < 256) { put(static_cast<std::uint8_t>(sym)); --left; continue; }
                sym -= 256;
                unsigned mlen = static_cast<unsigned>(sym & 7);
                if (mlen == 7) { const int f = decode(length_); if (f < 0) return false; mlen += static_cast<unsigned>(f); }
                mlen += 2;
                const unsigned slot = static_cast<unsigned>(sym >> 3);
                std::uint32_t off;
                if (slot == 0) off = r0_;
                else if (slot == 1) { off = r1_; r1_ = r0_; r0_ = off; }
                else if (slot == 2) { off = r2_; r2_ = r0_; r0_ = off; }
                else {
                    const unsigned extra = extra_[slot];
                    if (slot >= slots_) return false;
                    if (aligned && extra >= 3) {
                        std::uint32_t verbatim = extra > 3 ? read_bits(extra - 3) : 0;
                        const int al = decode(aligned_); if (al < 0) return false;
                        off = base_[slot] - 2 + (verbatim << 3) + static_cast<std::uint32_t>(al);
                    } else off = base_[slot] - 2 + (extra ? read_bits(extra) : 0);
                    r2_ = r1_; r1_ = r0_; r0_ = off;
                }
                if (off == 0 || off > window_size_) return false;
                for (unsigned i = 0; i < mlen; ++i) put(window_[(wpos_ - off) & (window_size_ - 1)]);
                left -= mlen;
            }
            if (left < 0) {   // a match ran past the end of the run: charge the overshoot to the block (encoders never cross a frame)
                if (static_cast<std::size_t>(-left) > block_remaining_) return false;
                block_remaining_ -= static_cast<std::uint32_t>(-left);
                if (static_cast<std::size_t>(-left) > todo) return false;
                todo -= static_cast<std::size_t>(-left);
            }
        }
        if (error_) return false;
        // copy the frame out of the window (the window position advanced by exactly out_len unless a match overshot)
        for (std::size_t i = 0; i < out_len; ++i) out[i] = window_[(frame_start + i) & (window_size_ - 1)];
        if (intel_filesize_) e8_translate(out, out_len);
        return true;
    }

private:
    struct Huff { std::array<std::uint16_t, 17> count{}; std::vector<std::uint16_t> symbol; };

    void put(std::uint8_t b) { window_[wpos_ & (window_size_ - 1)] = b; ++wpos_; }

    std::uint32_t read_bits(unsigned n) {
        if (n == 0) return 0;
        while (cnt_ < n) {
            std::uint32_t w = 0;
            if (ip_ + 1 < in_len_) w = static_cast<std::uint32_t>(in_[ip_]) | (static_cast<std::uint32_t>(in_[ip_ + 1]) << 8);
            else if (ip_ < in_len_) w = in_[ip_];
            else if (ip_ > in_len_ + 8) error_ = true;
            ip_ += 2; buf_ = (buf_ << 16) | w; cnt_ += 16;
        }
        const std::uint32_t v = static_cast<std::uint32_t>((buf_ >> (cnt_ - n)) & ((1ULL << n) - 1));
        cnt_ -= n; return v;
    }

    bool build(Huff& h, const std::uint8_t* lens, std::size_t n) {
        h.count.fill(0); h.symbol.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i) ++h.count[lens[i]];
        h.count[0] = 0;
        std::array<std::uint16_t, 18> offs{}; offs[1] = 0;
        for (unsigned l = 1; l < 17; ++l) offs[l + 1] = static_cast<std::uint16_t>(offs[l] + h.count[l]);
        for (std::size_t i = 0; i < n; ++i) if (lens[i]) h.symbol[offs[lens[i]]++] = static_cast<std::uint16_t>(i);
        return true;
    }

    int decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (unsigned len = 1; len <= 16; ++len) {
            code |= static_cast<int>(read_bits(1));
            const int count = h.count[len];
            if (code - count < first) return h.symbol[static_cast<std::size_t>(index + (code - first))];
            index += count; first += count; first <<= 1; code <<= 1;
        }
        error_ = true; return -1;
    }

    bool read_lengths(std::uint8_t* lens, unsigned first, unsigned last) {
        std::uint8_t pre_lens[20]; Huff pre;
        for (unsigned i = 0; i < 20; ++i) pre_lens[i] = static_cast<std::uint8_t>(read_bits(4));
        build(pre, pre_lens, 20);
        for (unsigned i = first; i < last;) {
            const int z = decode(pre); if (z < 0) return false;
            if (z == 17) { unsigned run = read_bits(4) + 4; while (run-- && i < last) lens[i++] = 0; }
            else if (z == 18) { unsigned run = read_bits(5) + 20; while (run-- && i < last) lens[i++] = 0; }
            else if (z == 19) {
                unsigned run = read_bits(1) + 4; const int y = decode(pre); if (y < 0) return false;
                const std::uint8_t v = static_cast<std::uint8_t>((lens[i] + 17 - y) % 17);
                while (run-- && i < last) lens[i++] = v;
            } else { lens[i] = static_cast<std::uint8_t>((lens[i] + 17 - z) % 17); ++i; }
        }
        return !error_;
    }

    bool read_block_header() {
        // an uncompressed block's header is followed by a 16-bit realignment; everything else starts right at the bit position
        block_type_ = read_bits(3);
        block_length_ = read_bits(16) << 8; block_length_ |= read_bits(8);
        block_remaining_ = block_length_;
        switch (block_type_) {
        case 2: {
            std::uint8_t al[8]; for (auto& l : al) l = static_cast<std::uint8_t>(read_bits(3));
            build(aligned_, al, 8);
        } [[fallthrough]];
        case 1:
            if (!read_lengths(main_len_.data(), 0, 256)) return false;
            if (!read_lengths(main_len_.data(), 256, 256 + slots_ * 8)) return false;
            if (!read_lengths(length_len_.data(), 0, 249)) return false;
            build(main_, main_len_.data(), 256 + slots_ * 8); build(length_, length_len_.data(), 249);
            return !error_;
        case 3: {
            // discard the rest of the current 16-bit word, then three little-endian 32-bit match offsets follow
            cnt_ -= cnt_ % 16; // drop partial-word bits; whole buffered words are rewound to the byte position
            ip_ -= cnt_ / 8; cnt_ = 0; buf_ = 0;
            if (ip_ + 12 > in_len_) return false;
            auto rd = [&](std::size_t o) { return static_cast<std::uint32_t>(in_[o]) | (static_cast<std::uint32_t>(in_[o + 1]) << 8) | (static_cast<std::uint32_t>(in_[o + 2]) << 16) | (static_cast<std::uint32_t>(in_[o + 3]) << 24); };
            r0_ = rd(ip_); r1_ = rd(ip_ + 4); r2_ = rd(ip_ + 8); ip_ += 12;
            return true;
        }
        default: return false;
        }
    }

    void e8_translate(std::uint8_t* d, std::size_t len) {
        if (len > 10) {
            const std::int32_t filesize = static_cast<std::int32_t>(intel_filesize_);
            for (std::size_t i = 0; i < len - 10;) {
                if (d[i++] != 0xE8) continue;
                const std::int32_t abs = static_cast<std::int32_t>(d[i] | (d[i + 1] << 8) | (d[i + 2] << 16) | (static_cast<std::uint32_t>(d[i + 3]) << 24));
                const std::int32_t cur = static_cast<std::int32_t>(intel_pos_ + i - 1);
                if (abs >= -cur && abs < filesize) {
                    const std::int32_t rel = abs >= 0 ? abs - cur : abs + filesize;
                    d[i] = static_cast<std::uint8_t>(rel); d[i + 1] = static_cast<std::uint8_t>(rel >> 8);
                    d[i + 2] = static_cast<std::uint8_t>(rel >> 16); d[i + 3] = static_cast<std::uint8_t>(rel >> 24);
                }
                i += 4;
            }
        }
        intel_pos_ += static_cast<std::uint32_t>(len);
    }

    unsigned window_bits_{17}; std::uint32_t window_size_{}; std::vector<std::uint8_t> window_; std::size_t wpos_{};
    unsigned slots_{34}; std::array<std::uint8_t, 52> extra_{}; std::array<std::uint32_t, 52> base_{};
    std::uint32_t r0_{1}, r1_{1}, r2_{1}, block_type_{}, block_remaining_{}, block_length_{};
    bool header_read_{false}, error_{false}; std::uint32_t intel_filesize_{}, intel_pos_{};
    std::vector<std::uint8_t> main_len_, length_len_; Huff main_, length_, aligned_;
    const std::uint8_t* in_{}; std::size_t in_len_{}, ip_{}; std::uint64_t buf_{}; unsigned cnt_{};
};

// Decodes a full XMem stream (a sequence of framed chunks) into `out`. Returns the number of bytes produced, or -1 on error.
inline long long decompress(Decoder& dec, const std::uint8_t* src, std::size_t src_len, std::uint8_t* out, std::size_t out_cap) {
    std::size_t sp = 0, produced = 0;
    while (sp < src_len && produced < out_cap) {
        std::size_t uncomp, comp, hdr;
        if (src[sp] == 0xFF) {
            if (sp + 5 > src_len) return -1;
            uncomp = (static_cast<std::size_t>(src[sp + 1]) << 8) | src[sp + 2]; comp = (static_cast<std::size_t>(src[sp + 3]) << 8) | src[sp + 4]; hdr = 5;
        } else {
            if (sp + 2 > src_len) return -1;
            comp = (static_cast<std::size_t>(src[sp]) << 8) | src[sp + 1]; uncomp = kFrameSize; hdr = 2;
        }
        if (comp == 0 || sp + hdr + comp > src_len) break;
        uncomp = std::min(uncomp, out_cap - produced);
        if (!dec.decode_frame(src + sp + hdr, comp, out + produced, uncomp)) return -1;
        produced += uncomp; sp += hdr + comp;
    }
    return static_cast<long long>(produced);
}

// Emits an LZX stream made of uncompressed blocks (one block per frame). Always valid LZX; it does not shrink the data.
inline std::vector<std::uint8_t> compress_stored(const std::uint8_t* src, std::size_t len) {
    std::vector<std::uint8_t> out;
    bool first = true;
    for (std::size_t pos = 0; pos < len || (len == 0 && first);) {
        const std::size_t n = std::min(kFrameSize, len - pos);
        std::vector<std::uint8_t> f;
        std::uint64_t bits = 0; unsigned nb = 0;
        auto put = [&](std::uint32_t v, unsigned k) { bits = (bits << k) | v; nb += k; while (nb >= 16) { const std::uint32_t w = static_cast<std::uint32_t>(bits >> (nb - 16)) & 0xFFFF; f.push_back(static_cast<std::uint8_t>(w)); f.push_back(static_cast<std::uint8_t>(w >> 8)); nb -= 16; } };
        if (first) put(0, 1);                                   // no Intel E8 translation
        put(3, 3); put(static_cast<std::uint32_t>(n >> 8) & 0xFFFF, 16); put(static_cast<std::uint32_t>(n) & 0xFF, 8);
        if (nb) put(0, 16 - nb);                                // align to 16 bits
        for (int r = 0; r < 3; ++r) { f.push_back(1); f.push_back(0); f.push_back(0); f.push_back(0); }   // R0 = R1 = R2 = 1
        f.insert(f.end(), src + pos, src + pos + n);
        if (n & 1) f.push_back(0);
        if (n == kFrameSize) { out.push_back(static_cast<std::uint8_t>(f.size() >> 8)); out.push_back(static_cast<std::uint8_t>(f.size())); }
        else { out.push_back(0xFF); out.push_back(static_cast<std::uint8_t>(n >> 8)); out.push_back(static_cast<std::uint8_t>(n)); out.push_back(static_cast<std::uint8_t>(f.size() >> 8)); out.push_back(static_cast<std::uint8_t>(f.size())); }
        out.insert(out.end(), f.begin(), f.end());
        first = false; pos += n; if (n == 0) break;
    }
    return out;
}

} // namespace someboxone::lzx
