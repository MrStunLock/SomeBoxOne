#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace someboxone {

// Small dependency-free SHA-256 implementation used only to compare the first
// XVD hash-tree page with the digest stored in its header. It does not perform
// signature verification or authenticate the signing key.
inline std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> input) {
    static constexpr std::array<std::uint32_t, 64> k = {
        0x428A2F98U,0x71374491U,0xB5C0FBCFU,0xE9B5DBA5U,0x3956C25BU,0x59F111F1U,0x923F82A4U,0xAB1C5ED5U,
        0xD807AA98U,0x12835B01U,0x243185BEU,0x550C7DC3U,0x72BE5D74U,0x80DEB1FEU,0x9BDC06A7U,0xC19BF174U,
        0xE49B69C1U,0xEFBE4786U,0x0FC19DC6U,0x240CA1CCU,0x2DE92C6FU,0x4A7484AAU,0x5CB0A9DCU,0x76F988DAU,
        0x983E5152U,0xA831C66DU,0xB00327C8U,0xBF597FC7U,0xC6E00BF3U,0xD5A79147U,0x06CA6351U,0x14292967U,
        0x27B70A85U,0x2E1B2138U,0x4D2C6DFCU,0x53380D13U,0x650A7354U,0x766A0ABBU,0x81C2C92EU,0x92722C85U,
        0xA2BFE8A1U,0xA81A664BU,0xC24B8B70U,0xC76C51A3U,0xD192E819U,0xD6990624U,0xF40E3585U,0x106AA070U,
        0x19A4C116U,0x1E376C08U,0x2748774CU,0x34B0BCB5U,0x391C0CB3U,0x4ED8AA4AU,0x5B9CCA4FU,0x682E6FF3U,
        0x748F82EEU,0x78A5636FU,0x84C87814U,0x8CC70208U,0x90BEFFFAU,0xA4506CEBU,0xBEF9A3F7U,0xC67178F2U
    };
    std::array<std::uint32_t, 8> h = {
        0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
        0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U
    };
    auto rotr = [](std::uint32_t x, unsigned n) { return (x >> n) | (x << (32U - n)); };

    std::vector<std::uint8_t> data(input.begin(), input.end());
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8ULL;
    data.push_back(0x80U);
    while ((data.size() % 64U) != 56U) data.push_back(0U);
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<std::uint8_t>((bit_length >> (static_cast<unsigned>(i) * 8U)) & 0xFFU));

    for (std::size_t block = 0; block < data.size(); block += 64U) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t p = block + i * 4U;
            w[i] = (static_cast<std::uint32_t>(data[p]) << 24U) |
                   (static_cast<std::uint32_t>(data[p + 1]) << 16U) |
                   (static_cast<std::uint32_t>(data[p + 2]) << 8U) |
                   static_cast<std::uint32_t>(data[p + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7U) ^ rotr(w[i - 15], 18U) ^ (w[i - 15] >> 3U);
            const std::uint32_t s1 = rotr(w[i - 2], 17U) ^ rotr(w[i - 2], 19U) ^ (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
            const std::uint32_t choice = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = hh + s1 + choice + k[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;
            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < h.size(); ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(h[i] >> 24U);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16U);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8U);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return digest;
}

} // namespace someboxone
