// v0.32.0 regression tests: LZX/XMem codec, software audio mixer, frame pacer, FPS overlay, runtime settings,
// ALU flag computation and the VEX destination/source aliasing fix. Self-contained (no game files needed).
#include "guest_cpu.hpp"
#include "audio_mixer.hpp"
#include "fps_overlay.hpp"
#include "frame_pacer.hpp"
#include "lzx.hpp"
#include "runtime_config.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>

using namespace someboxone;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// The first XMemDecompress call Minecraft: Xbox One Edition 1.2.0.0 makes (codec 1 = LZX, 128 KB window): 1706 bytes -> 10058 bytes of UTF-16 XML.
static const unsigned char kMinecraftXmemChunk[] = {
255,39,74,6,160,2,32,164,116,65,16,0,2,6,0,70,34,0,70,85,0,184,111,128,135,76,243,41,94,255,149,237,
119,9,221,172,82,32,107,134,101,4,153,61,1,187,253,230,80,150,88,77,123,255,157,212,127,32,138,1,0,102,38,242,
138,13,0,189,1,191,73,237,253,211,196,79,243,221,153,27,97,57,4,114,80,228,192,231,82,59,33,71,184,154,152,22,
246,196,42,115,171,185,253,134,165,235,109,59,94,252,216,74,118,76,56,166,119,189,152,142,107,172,238,213,105,45,141,38,
152,225,155,253,75,128,255,0,32,0,0,136,8,0,102,160,10,21,217,110,37,148,221,67,205,50,177,35,231,43,6,104,
144,119,174,50,129,204,179,127,231,255,255,232,255,63,124,232,6,152,94,162,185,21,187,173,208,204,133,2,19,10,229,92,
244,94,129,146,11,68,47,128,94,22,141,70,224,186,72,18,123,177,165,254,149,36,209,69,227,138,229,153,24,188,21,57,
200,81,71,33,125,194,121,194,255,79,221,200,59,125,232,218,38,175,36,153,207,228,109,2,151,69,245,39,189,122,2,75,
50,111,105,59,220,40,138,149,93,250,242,123,219,66,140,108,159,174,72,28,100,128,220,19,89,236,219,15,119,94,179,94,
100,189,24,214,222,156,228,234,41,163,180,42,177,25,95,136,135,113,147,124,5,121,47,153,201,190,15,6,186,178,251,168,
107,104,201,44,220,97,145,36,33,173,24,199,220,115,54,99,72,226,9,164,8,59,165,214,246,204,152,78,18,163,36,33,
152,53,226,147,199,180,190,239,138,154,241,119,26,111,29,228,26,81,198,255,91,132,211,247,230,127,4,57,93,77,191,204,
184,21,37,190,50,86,78,117,79,13,107,196,124,211,45,81,58,59,159,63,36,23,0,227,37,223,112,122,105,225,32,138,
155,148,220,160,93,146,60,136,27,187,121,97,78,179,89,112,8,195,145,57,54,170,72,151,52,159,215,42,48,17,211,74,
198,124,81,100,84,204,139,189,107,232,46,30,22,77,61,48,7,224,16,207,185,131,54,97,204,208,153,156,161,56,186,251,
140,240,221,136,222,142,134,121,11,201,196,115,192,200,32,136,132,136,254,89,119,122,211,28,218,112,6,160,57,133,135,6,
207,187,136,210,105,222,207,239,176,119,172,95,121,98,71,12,176,218,50,101,146,27,26,167,224,201,28,122,25,30,218,120,
6,192,57,5,136,6,208,57,70,2,151,8,93,10,207,240,161,232,15,238,241,137,222,56,7,37,42,57,223,32,105,235,
224,111,158,68,91,208,0,60,7,16,17,32,186,70,203,71,17,78,90,180,0,26,160,157,87,23,68,162,126,233,32,171,
113,10,21,219,49,131,149,102,61,122,152,34,218,136,6,128,58,11,131,29,239,50,174,175,234,239,110,101,37,19,140,219,
180,219,218,177,200,62,207,13,34,67,40,199,32,42,62,0,218,158,65,224,180,83,118,232,216,13,81,245,237,195,10,67,
35,20,79,222,203,166,131,144,8,104,22,132,16,173,0,250,196,9,190,56,189,68,120,55,166,162,63,205,57,230,128,148,
152,133,40,63,40,74,24,66,168,36,182,84,41,42,70,91,133,159,226,12,219,19,155,19,9,60,15,254,26,22,75,174,
251,164,217,45,153,118,104,1,86,254,124,247,32,76,74,68,189,144,94,10,220,194,127,148,2,207,64,180,237,175,27,87,
200,8,246,1,89,187,66,158,122,65,112,4,210,45,80,158,195,140,188,202,121,62,251,205,75,114,245,206,202,168,192,138,
153,142,110,49,187,235,179,219,239,15,78,27,70,238,165,182,20,121,192,207,240,19,213,13,69,83,58,128,3,152,14,35,
37,148,250,228,63,158,99,182,59,107,223,94,12,171,110,205,150,25,19,29,79,215,69,66,121,135,204,202,68,244,206,18,
105,21,129,210,135,244,81,130,214,86,165,151,222,186,70,181,101,214,1,232,189,192,219,250,205,169,65,125,175,169,127,162,
199,73,194,93,180,61,185,241,62,151,200,205,253,254,116,181,84,119,88,230,245,25,242,213,252,177,17,89,252,254,233,230,
119,45,172,255,245,199,147,21,71,215,0,169,252,26,12,21,198,78,218,146,105,226,39,247,151,169,191,109,206,58,92,201,
134,125,205,141,9,237,100,26,52,156,182,12,140,154,97,222,73,212,251,75,253,111,121,152,243,236,148,189,58,86,175,184,
222,153,217,31,93,218,11,64,75,136,75,210,50,208,149,215,93,137,97,122,171,136,199,55,232,210,17,117,149,86,252,87,
40,191,146,76,92,79,229,38,142,45,39,38,136,113,22,197,23,119,51,147,225,120,132,128,106,184,121,172,112,144,211,214,
158,111,97,112,43,63,0,115,233,200,53,209,54,74,195,94,168,53,11,254,228,106,59,19,213,229,41,63,35,9,49,213,
66,245,70,237,71,177,64,74,150,241,36,91,101,85,22,31,222,166,0,198,37,178,203,6,167,235,166,251,198,52,244,23,
188,204,189,179,127,116,62,119,187,6,215,64,14,167,153,118,212,9,38,34,15,167,32,98,15,249,45,101,41,50,61,190,
203,240,225,91,45,205,213,150,240,165,190,251,148,180,36,55,27,167,20,11,129,214,97,68,127,116,178,69,161,233,2,169,
69,188,185,239,70,38,11,97,198,123,79,38,127,101,102,104,227,101,47,47,253,238,184,215,65,99,61,8,157,113,175,191,
220,26,103,243,37,169,135,101,119,21,203,75,213,62,70,233,112,211,229,62,111,203,211,156,20,102,86,198,214,74,250,152,
74,154,170,104,163,20,70,161,88,22,28,204,137,198,126,160,19,136,8,45,83,30,127,236,202,48,87,94,117,40,23,238,
154,37,163,214,16,22,35,169,2,75,69,3,199,164,81,102,112,56,178,215,232,134,229,210,83,163,238,197,46,238,51,191,
116,140,208,97,16,228,234,120,44,44,118,226,209,142,77,91,46,141,149,152,65,41,200,247,54,118,245,101,150,33,178,81,
118,236,115,148,6,68,248,84,71,111,108,20,253,119,151,190,227,106,93,84,84,175,160,164,183,210,66,172,22,67,76,183,
9,87,220,144,205,33,30,75,53,196,137,140,106,68,88,156,82,34,239,133,21,173,78,123,234,237,135,244,199,44,21,228,
165,143,170,181,136,193,220,89,98,169,29,88,21,26,204,73,174,83,81,107,20,104,71,5,15,247,129,163,250,205,207,23,
0,142,98,212,15,120,68,237,84,111,79,32,209,128,42,223,175,55,165,47,228,39,38,239,126,134,113,76,249,34,203,236,
159,30,209,60,213,127,133,107,179,189,248,83,134,252,32,77,172,145,101,162,153,236,34,224,113,5,167,192,79,87,219,61,
231,77,248,128,195,248,14,153,188,222,93,184,139,218,168,91,187,255,34,56,81,18,51,150,94,215,14,118,154,11,179,170,
158,127,0,65,152,104,30,187,0,128,20,161,41,48,84,214,54,235,15,112,91,208,203,240,253,26,91,243,95,174,110,202,
29,241,218,172,117,96,170,231,216,210,28,70,126,92,250,82,149,164,163,22,8,229,99,184,116,16,17,2,84,224,168,172,
109,131,63,110,10,66,47,103,215,101,45,68,170,51,90,31,102,123,26,16,48,84,7,17,194,33,44,196,72,64,139,11,
117,114,33,92,252,48,12,2,83,192,163,88,219,46,198,188,57,173,140,233,208,217,248,67,162,55,254,207,132,141,217,35,
53,36,62,44,236,109,151,27,43,63,197,200,88,232,206,138,234,54,199,173,32,180,129,81,129,164,10,41,31,49,4,238,
77,208,124,192,110,0,0,0,0,0};

static void test_lzx() {
    lzx::Decoder dec(17); std::vector<std::uint8_t> out(10058);
    const long long n = lzx::decompress(dec, kMinecraftXmemChunk, sizeof kMinecraftXmemChunk, out.data(), out.size());
    CHECK(n == 10058, "real XMem chunk decodes to 10058 bytes (got %lld)", n);
    const char* head = "<\0?\0x\0m\0l\0 \0v\0e\0r\0s\0i\0o\0n\0"; // "<?xml version" in UTF-16LE
    CHECK(n == 10058 && out[0] == 0xFF && out[1] == 0xFE && std::memcmp(out.data() + 2, head, 26) == 0, "decoded data is UTF-16 text (BOM) starting with the XML declaration");
    CHECK(n == 10058 && out[10056] == '>' && out[10057] == 0, "decoded data ends with '>'");
    for (std::size_t len : {0ul, 1ul, 5ul, 32767ul, 32768ul, 32769ul, 100000ul}) {   // the stored-block encoder round-trips at every frame boundary
        std::vector<std::uint8_t> in(len); for (std::size_t i = 0; i < len; ++i) in[i] = static_cast<std::uint8_t>(i * 31 + (i >> 8));
        const auto packed = lzx::compress_stored(in.data(), in.size()); lzx::Decoder d2(17); std::vector<std::uint8_t> o(len + 1);
        const long long m = lzx::decompress(d2, packed.data(), packed.size(), o.data(), len);
        CHECK(m == static_cast<long long>(len) && std::equal(in.begin(), in.end(), o.begin()), "LZX stored round trip, %zu bytes", len);
    }
    std::vector<std::uint8_t> bad(kMinecraftXmemChunk, kMinecraftXmemChunk + sizeof kMinecraftXmemChunk); bad[40] ^= 0xFF; bad[41] ^= 0x55;
    lzx::Decoder d3(17); std::vector<std::uint8_t> o3(10058); (void)lzx::decompress(d3, bad.data(), bad.size(), o3.data(), o3.size());   // corrupt input must not crash
}

static std::vector<std::uint8_t> sine16(unsigned rate, double hz, double secs, double amp = 0.5) {
    std::vector<std::uint8_t> d; const std::size_t n = static_cast<std::size_t>(rate * secs);
    for (std::size_t i = 0; i < n; ++i) { const auto s = static_cast<std::int16_t>(std::sin(2 * 3.14159265358979 * hz * static_cast<double>(i) / rate) * amp * 32767); d.push_back(static_cast<std::uint8_t>(s & 0xFF)); d.push_back(static_cast<std::uint8_t>((s >> 8) & 0xFF)); }
    return d;
}
static void mix_all(AudioMixer& m, std::size_t frames, std::vector<float>& l, std::vector<float>& r) {
    std::vector<float> buf(frames * 2); m.mix(buf.data(), frames); l.resize(frames); r.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) { l[i] = buf[2 * i]; r[i] = buf[2 * i + 1]; }
}
static int zero_crossings(const std::vector<float>& ch, std::size_t from, std::size_t to) { int z = 0; for (std::size_t i = from + 1; i < to; ++i) if ((ch[i - 1] < 0) != (ch[i] < 0)) ++z; return z; }

static void test_mixer() {
    { AudioMixer m; const auto v = m.create_voice({1, 1, 16, 2, 22050}); m.submit(v, sine16(22050, 440, 1.0), 0x40, 0, 0, 0, 0, 0, 0xABCD);
      std::vector<float> l, r; mix_all(m, 1000, l, r); float peak = 0; for (float x : l) peak = std::max(peak, std::fabs(x)); CHECK(peak == 0.0f, "a voice that was not started is silent");
      m.start(v); mix_all(m, 48100, l, r); peak = 0; for (float x : l) peak = std::max(peak, std::fabs(x));
      CHECK(std::fabs(peak - 0.5f) < 0.02f, "peak %f", peak);
      const double hz = zero_crossings(l, 100, 47000) / 2.0 / (46900.0 / 48000.0); CHECK(std::fabs(hz - 440.0) < 8.0, "22.05 kHz -> 48 kHz keeps the pitch (%.1f Hz)", hz);
      int bs = 0, be = 0, se = 0; for (auto& e : m.take_events()) { if (e.kind == AudioMixer::Event::BufferStart) ++bs; if (e.kind == AudioMixer::Event::BufferEnd) { ++be; CHECK(e.context == 0xABCD, "buffer context"); } if (e.kind == AudioMixer::Event::StreamEnd) ++se; }
      CHECK(bs == 1 && be == 1 && se == 1, "events %d %d %d", bs, be, se); }
    { AudioMixer m; const auto v = m.create_voice({1, 1, 16, 2, 48000}); m.set_ratio(v, 2.0f); m.submit(v, sine16(48000, 440, 1.0), 0, 0, 0, 0, 0, 0, 1); m.start(v);
      std::vector<float> l, r; mix_all(m, 24000, l, r); const double hz = zero_crossings(l, 100, 23000) / 2.0 / (22900.0 / 48000.0); CHECK(std::fabs(hz - 880.0) < 15.0, "frequency ratio 2 -> %.1f Hz", hz); }
    { AudioMixer m; const auto v = m.create_voice({1, 1, 16, 2, 48000}); m.submit(v, sine16(48000, 1000, 0.1), 0, 0, 0, 0, 0, 2, 7); m.start(v);
      std::vector<float> l, r; mix_all(m, 19220, l, r); int loops = 0, ends = 0; for (auto& e : m.take_events()) { if (e.kind == AudioMixer::Event::LoopEnd) ++loops; if (e.kind == AudioMixer::Event::BufferEnd) ++ends; }
      CHECK(loops == 2 && ends == 1, "loop count 2 -> 2 LoopEnd + 1 BufferEnd (%d, %d)", loops, ends); }
    { AudioMixer m; const auto v = m.create_voice({3, 2, 32, 8, 48000}); std::vector<std::uint8_t> d(48000 * 8); for (std::size_t i = 0; i < 48000; ++i) { const float s[2] = {0.8f, -0.4f}; std::memcpy(&d[i * 8], s, 8); }
      m.submit(v, std::move(d), 0, 0, 0, 0, 0, 0, 0); m.start(v); m.set_volume(v, 0.5f); std::vector<float> l, r; mix_all(m, 100, l, r);
      CHECK(std::fabs(l[50] - 0.4f) < 1e-4f && std::fabs(r[50] + 0.2f) < 1e-4f, "float stereo + voice volume"); m.set_master_volume(0.5f); mix_all(m, 100, l, r); CHECK(std::fabs(l[50] - 0.2f) < 1e-4f, "master volume"); }
    { AudioMixer m; const auto v = m.create_voice({1, 1, 16, 2, 48000});   // flush signals OnBufferEnd for every buffer it drops
      for (int i = 0; i < 3; ++i) m.submit(v, sine16(48000, 300, 0.1), 0, 0, 0, 0, 0, 0, 11 + static_cast<std::uint64_t>(i));
      m.flush(v); int dropped = 0; for (auto& e : m.take_events()) if (e.kind == AudioMixer::Event::BufferEnd) ++dropped; CHECK(dropped == 3 && m.state(v).queued == 0, "flush drops %d buffers", dropped); }
    { AudioMixer m; const auto v = m.create_voice({1, 6, 16, 12, 48000}); std::vector<std::uint8_t> d(4800 * 12, 0); for (std::size_t i = 0; i < 4800; ++i) { const std::int16_t fc = 16384; std::memcpy(&d[i * 12 + 4], &fc, 2); }
      m.submit(v, std::move(d), 0, 0, 0, 0, 0, 0, 3); m.start(v); std::vector<float> l, r; mix_all(m, 100, l, r); CHECK(std::fabs(l[50] - 0.5f * 0.7071f) < 0.01f && std::fabs(r[50] - l[50]) < 1e-6f, "5.1 centre channel down-mix"); }
    { AudioMixer m; const auto v = m.create_voice({0x166, 2, 16, 2048, 48000}); m.submit(v, std::vector<std::uint8_t>(4096, 1), 0x40, 0, 0, 0, 0, 0, 4); m.start(v);
      std::vector<float> l, r; mix_all(m, 48000, l, r); bool done = false; for (auto& e : m.take_events()) if (e.kind == AudioMixer::Event::StreamEnd) done = true; CHECK(done, "an undecodable (XMA) voice still drains and signals"); }
}

static void test_pacer() {
    for (unsigned limit : {60u, 30u}) {
        FramePacer p; p.set_limit(limit); const int frames = static_cast<int>(limit) * 1;   // one second
        const auto t0 = FramePacer::Clock::now();
        for (int i = 0; i < frames; ++i) p.end_frame();
        const double secs = std::chrono::duration<double>(FramePacer::Clock::now() - t0).count();
        CHECK(secs > 0.93 && secs < 1.10, "%u FPS lock: %d frames took %.3f s", limit, frames, secs);
    }
    FramePacer slow; slow.set_limit(60); const auto t0 = FramePacer::Clock::now();
    for (int i = 0; i < 5; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(40)); slow.end_frame(); }
    const double secs = std::chrono::duration<double>(FramePacer::Clock::now() - t0).count();
    CHECK(secs < 0.30, "a slow emulator must not be slowed further or burst-corrected (%.3f s)", secs);
}

static void test_overlay() {
    const unsigned w = 424, h = 240; std::vector<std::uint8_t> px(w * h * 4, 128), before = px;
    draw_fps_overlay(px, w, h, 59.8, 60);
    CHECK(px != before, "overlay changed the frame");
    bool far_untouched = true; for (unsigned y = 100; y < h; ++y) for (unsigned x = 100; x < w; ++x) if (px[(y * w + x) * 4] != 128) far_untouched = false;
    CHECK(far_untouched, "overlay stays in the top-left corner");
    bool has_green = false; for (unsigned y = 0; y < 40; ++y) for (unsigned x = 0; x < 100; ++x) { const auto* p = &px[(y * w + x) * 4]; if (p[1] == 255 && p[0] == 80 && p[2] == 80) has_green = true; }
    CHECK(has_green, "the FPS digits are drawn green when the lock is held");
    bool compact_panel = true;
    for (unsigned y = 30; y < h; ++y) for (unsigned x = 0; x < 100; ++x)
        if (px[(y * w + x) * 4] != 128) compact_panel = false;
    CHECK(compact_panel, "the compact FPS panel stays within the first 30 pixels on a 240p frame");
    std::vector<std::uint8_t> tiny(8 * 8 * 4, 7); draw_fps_overlay(tiny, 8, 8, 60, 60); CHECK(tiny == std::vector<std::uint8_t>(8 * 8 * 4, 7), "frames too small for text are left alone");
}

static void test_config() {
    CHECK(parse_runtime_config(default_runtime_config_text()).frame_rate_limit == 60, "default frame-rate lock is 60");
    CHECK(parse_runtime_config("FrameRateLimit=30\n").frame_rate_limit == 30, "30 accepted");
    for (const char* bad : {"FrameRateLimit=45\n", "FrameRateLimit=0\n", "FrameRateLimit=abc\n", "FrameRateLimit=120\n", "FrameRateLimit=\n"}) CHECK(parse_runtime_config(bad).frame_rate_limit == 60, "'%s' falls back to 60", bad);
    RuntimeConfig c; c.frame_rate_limit = 30; c.show_fps_counter = false; c.audio_enabled = false; const auto r = parse_runtime_config(serialize_runtime_config(c));
    CHECK(r.frame_rate_limit == 30 && !r.show_fps_counter && !r.audio_enabled, "settings round trip");
    CHECK(parse_runtime_config("GraphicsWidth=1280\n").show_fps_counter, "settings files from older versions keep working (FPS counter defaults on)");
}

// reference ALU (the pre-0.32.0 implementation)
struct RefAlu {
    std::uint64_t rflags{};
    static std::uint64_t mask(unsigned s) { return s >= 8 ? ~0ULL : ((1ULL << (s * 8)) - 1); }
    static std::uint64_t sbit(unsigned s) { return 1ULL << (s * 8 - 1); }
    void setf(std::uint64_t f, bool on) { if (on) rflags |= f; else rflags &= ~f; }
    bool getf(std::uint64_t f) const { return (rflags & f) != 0; }
    void set_szp(std::uint64_t res, unsigned size) { res &= mask(size); setf(GuestCpu::ZF, res == 0); setf(GuestCpu::SF, (res & sbit(size)) != 0); setf(GuestCpu::PF, (std::popcount(static_cast<std::uint8_t>(res & 0xFF)) & 1) == 0); }
    void logic(std::uint64_t res, unsigned size) { setf(GuestCpu::CF, false); setf(GuestCpu::OF, false); setf(GuestCpu::AF, false); set_szp(res, size); }
    std::uint64_t alu(unsigned op, std::uint64_t a, std::uint64_t b, unsigned size) {
        const std::uint64_t m = mask(size), sb = sbit(size); a &= m; b &= m; std::uint64_t res = 0;
        switch (op) {
        case 0: case 2: { const std::uint64_t c = (op == 2 && getf(GuestCpu::CF)) ? 1 : 0; const std::uint64_t full = a + b + c; res = full & m;
            setf(GuestCpu::CF, size == 8 ? (full < a || (c && full == a)) : (full > m)); setf(GuestCpu::OF, ((a ^ res) & (b ^ res) & sb) != 0); setf(GuestCpu::AF, ((a ^ b ^ res) & 0x10) != 0); set_szp(res, size); break; }
        case 3: case 5: case 7: { const std::uint64_t c = (op == 3 && getf(GuestCpu::CF)) ? 1 : 0; res = (a - b - c) & m;
            setf(GuestCpu::CF, a < b || (a == b && c)); setf(GuestCpu::OF, ((a ^ b) & (a ^ res) & sb) != 0); setf(GuestCpu::AF, ((a ^ b ^ res) & 0x10) != 0); set_szp(res, size); break; }
        case 1: res = a | b; logic(res, size); break; case 4: res = a & b; logic(res, size); break; case 6: res = a ^ b; logic(res, size); break; }
        return res;
    }
};

// Executes real ALU instructions on the guest CPU (add/or/adc/sbb/and/sub/xor/cmp r/m, r at 8/16/32/64 bits) and compares the result and every
// arithmetic flag with the reference implementation above. No access to private members is needed.
static void test_alu_flags() {
    GuestMemory mem; mem.map(0x1000, 0x2000, "t"); mem.map(0x7000, 0x1000, "s"); RefAlu ref; std::mt19937_64 rng(12345); long bad = 0, n = 0;
    static const std::uint8_t base_opcode[8] = {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38};   // add or adc sbb and sub xor cmp (r/m8, r8 form; +1 for 16/32/64-bit)
    const unsigned sizes[] = {1, 2, 4, 8};
    for (int it = 0; it < 200000; ++it) {
        const unsigned op = static_cast<unsigned>(rng() % 8), size = sizes[rng() % 4];
        std::uint64_t a = rng(), b = rng();
        if (rng() % 4 == 0) a = b;
        if (rng() % 5 == 0) b = ~0ULL >> (rng() % 64);
        if (rng() % 7 == 0) a = 0;
        const std::uint64_t fl = rng() & (0x8D5 | 0x200 | 0x400);
        std::vector<std::uint8_t> code;
        if (size == 2) code.push_back(0x66);
        if (size == 8) code.push_back(0x48);
        code.push_back(static_cast<std::uint8_t>(base_opcode[op] + (size == 1 ? 0 : 1))); code.push_back(0xC8);   // <op> rax, rcx
        for (std::size_t k = 0; k < code.size(); ++k) mem.write<std::uint8_t>(0x1000 + k, code[k]);
        GuestCpu cpu(mem, 1u << 6); cpu.rip = 0x1000; cpu.r[4] = 0x7f00; cpu.r[0] = a; cpu.r[1] = b; cpu.rflags = fl; cpu.run(1);
        ref.rflags = fl; const std::uint64_t want = ref.alu(op, a, b, size), m = RefAlu::mask(size);
        std::uint64_t expect_rax = a;                                           // cmp writes nothing
        if (op != 7) expect_rax = size >= 4 ? (want & m) : ((a & ~m) | (want & m));   // 32-bit writes zero-extend; 8/16-bit writes keep the upper bits
        const std::uint64_t arith = GuestCpu::CF | GuestCpu::PF | GuestCpu::AF | GuestCpu::ZF | GuestCpu::SF | GuestCpu::OF;
        ++n; if (cpu.r[0] != expect_rax || (cpu.rflags & arith) != (ref.rflags & arith)) ++bad;
    }
    CHECK(bad == 0, "ALU instructions match the reference results and flags (%ld of %ld differ)", bad, n);
}

static std::uint64_t run_code(GuestMemory& mem, const std::vector<std::uint8_t>& code, GuestCpu& c) {
    for (std::size_t i = 0; i < code.size(); ++i) mem.write<std::uint8_t>(0x1000 + i, code[i]);
    c.rip = 0x1000; c.r[4] = 0x7f00; c.run(100); return c.rip;
}
static void test_vex() {
    { // vdivss xmm0, xmm1, xmm0: destination == r/m operand; the copy of vvvv into the destination used to destroy the divisor
      GuestMemory mem; mem.map(0x1000, 0x2000, "t"); mem.map(0x7000, 0x1000, "s"); GuestCpu c(mem, 1u << 8);
      c.xmm[1].f[0] = 1000.0f; c.xmm[0].f[0] = 8.0f;
      run_code(mem, {0xc5, 0xf2, 0x5e, 0xc0, 0xf4}, c); CHECK(c.xmm[0].f[0] == 125.0f, "vdivss with dest == source: %f", c.xmm[0].f[0]); }
    { // vcvtdq2pd xmm1, xmm1 (two-operand form, vvvv unused)
      GuestMemory mem; mem.map(0x1000, 0x2000, "t"); mem.map(0x7000, 0x1000, "s"); GuestCpu c(mem, 1u << 8); c.r[1] = 5; c.xmm[0].df[0] = 99.0;
      run_code(mem, {0xc5, 0xf9, 0x6e, 0xc9, 0xc5, 0xfa, 0xe6, 0xc9, 0xf4}, c); CHECK(c.xmm[1].df[0] == 5.0, "vmovd + vcvtdq2pd xmm1,xmm1: %f", c.xmm[1].df[0]); }
    { // vcvtsi2ss xmm0, xmm0, rdx must read RDX (a GPR in r/m is not an XMM operand)
      GuestMemory mem; mem.map(0x1000, 0x2000, "t"); mem.map(0x7000, 0x1000, "s"); GuestCpu c(mem, 1u << 8); c.r[0] = 1000; c.r[2] = 8;
      run_code(mem, {0xc4, 0xe1, 0xfa, 0x2a, 0xc2, 0xf4}, c); CHECK(c.xmm[0].f[0] == 8.0f, "vcvtsi2ss reads RDX: %f", c.xmm[0].f[0]); }
    { // vinsertps xmm3, xmm3, xmm1, 0 (was an unsupported opcode)
      GuestMemory mem; mem.map(0x1000, 0x2000, "t"); mem.map(0x7000, 0x1000, "s"); GuestCpu c(mem, 1u << 8);
      c.xmm[3].f[0] = 1; c.xmm[3].f[1] = 2; c.xmm[3].f[2] = 3; c.xmm[3].f[3] = 4; c.xmm[1].f[0] = 9;
      run_code(mem, {0xc4, 0xe3, 0x61, 0x21, 0xd9, 0x10, 0xf4}, c); CHECK(c.xmm[3].f[0] == 1 && c.xmm[3].f[1] == 9 && c.xmm[3].f[2] == 3 && c.xmm[3].f[3] == 4, "vinsertps: %f %f %f %f", c.xmm[3].f[0], c.xmm[3].f[1], c.xmm[3].f[2], c.xmm[3].f[3]); }
}

static void test_lock_cmpxchg() {
    // The title's compare-and-swap helper: mov rax,r8 ; lock cmpxchg [rcx],rdx ; ret. A suspected spin in Minecraft's clock turned out NOT to be a CPU bug.
    for (int success = 1; success >= 0; --success) {
        GuestMemory mem; mem.map(0x1000, 0x2000, "t"); mem.map(0x7000, 0x1000, "s"); GuestCpu c(mem, 1u << 6);
        mem.write<std::uint64_t>(0x2000, 0x1122334455667788ULL);
        c.r[1] = 0x2000; c.r[2] = 0xAAAABBBBCCCCDDDDULL; c.r[8] = success ? 0x1122334455667788ULL : 1ULL;
        run_code(mem, {0x49, 0x8b, 0xc0, 0xf0, 0x48, 0x0f, 0xb1, 0x11, 0xf4}, c);
        CHECK(c.r[0] == 0x1122334455667788ULL, "cmpxchg returns the value that was in memory (%s)", success ? "success" : "failure");
        CHECK(mem.read<std::uint64_t>(0x2000) == (success ? 0xAAAABBBBCCCCDDDDULL : 0x1122334455667788ULL), "cmpxchg %s memory", success ? "stores the new value" : "leaves");
        CHECK(((c.rflags & GuestCpu::ZF) != 0) == (success == 1), "cmpxchg ZF");
    }
}

static void test_vpalignr() {
    GuestMemory mem; mem.map(0x1000, 0x2000, "t"); mem.map(0x7000, 0x1000, "s"); GuestCpu c(mem, 1u << 8);
    std::uint8_t lo[16], hi[16]; for (int i = 0; i < 16; ++i) { lo[i] = static_cast<std::uint8_t>(i); hi[i] = static_cast<std::uint8_t>(16 + i); }
    std::memcpy(&c.xmm[2], lo, 16); std::memcpy(&c.xmm[1], hi, 16);                       // vpalignr xmm0, xmm1(high), xmm2(low), 4
    run_code(mem, {0xc4, 0xe3, 0x71, 0x0f, 0xc2, 0x04, 0xf4}, c);
    std::uint8_t out[16]; std::memcpy(out, &c.xmm[0], 16); bool ok = true; for (int i = 0; i < 16; ++i) if (out[i] != 4 + i) ok = false;
    CHECK(ok, "vpalignr xmm0, xmm1, xmm2, 4 yields bytes 4..19");
}

int main() {
    test_lzx(); test_mixer(); test_pacer(); test_overlay(); test_config(); test_alu_flags(); test_vex(); test_lock_cmpxchg(); test_vpalignr();
    if (g_fail) { std::printf("%d v0.32.0 check(s) FAILED\n", g_fail); return 1; }
    std::printf("v0.32.0 tests passed (LZX/XMem, audio mixer, frame pacer, FPS overlay, settings, ALU flags, VEX aliasing, lock cmpxchg, vpalignr)\n");
    return 0;
}
