#pragma once
// SomeBoxOne v0.32.0 - audio output sinks for the software mixer.
//
//   * WaveOutSink (Windows): plays 48 kHz stereo 16-bit through the default output device with the classic winmm waveOut API
//     (no COM, no extra runtime): eight 20 ms buffers (160 ms of buffering). It never blocks the emulation thread: when every
//     buffer is still queued, newly produced audio is dropped instead of piling up latency.
//   * WavFileSink: writes the mix to a .wav file (used by `--audio-wav FILE`, handy for checking audio without a sound device).
//   * NullSink: discards everything (audio disabled, or no output device).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <mmsystem.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "winmm.lib")
#  endif
#endif

namespace someboxone {

class AudioSink {
public:
    virtual ~AudioSink() = default;
    // `frames` interleaved stereo float frames in [-1, 1] at 48 kHz.
    virtual void push(const float* stereo, std::size_t frames) = 0;
    virtual const char* name() const = 0;
    virtual std::uint64_t frames_dropped() const { return 0; }
};

class NullSink final : public AudioSink {
public:
    void push(const float*, std::size_t) override {}
    const char* name() const override { return "none"; }
};

namespace audio_detail {
inline std::int16_t to_s16(float x) {
    const float c = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
    return static_cast<std::int16_t>(c * 32767.0f + (c >= 0 ? 0.5f : -0.5f));
}
} // namespace audio_detail

class WavFileSink final : public AudioSink {
public:
    explicit WavFileSink(const std::string& path) {
#if defined(_MSC_VER)
        if (fopen_s(&fp_, path.c_str(), "wb") != 0) fp_ = nullptr;
#else
        fp_ = std::fopen(path.c_str(), "wb");
#endif
        if (fp_) write_header(0);
    }
    ~WavFileSink() override { if (fp_) { write_header(data_bytes_); std::fclose(fp_); } }
    bool ok() const { return fp_ != nullptr; }
    void push(const float* s, std::size_t frames) override {
        if (!fp_) return;
        for (std::size_t i = 0; i < frames * 2; ++i) { const std::int16_t v = audio_detail::to_s16(s[i]); std::fputc(v & 0xFF, fp_); std::fputc((v >> 8) & 0xFF, fp_); }
        data_bytes_ += static_cast<std::uint32_t>(frames * 4);
    }
    const char* name() const override { return "wav file"; }
private:
    void write_header(std::uint32_t data_bytes) {
        std::fseek(fp_, 0, SEEK_SET);
        auto u32 = [&](std::uint32_t v) { for (int k = 0; k < 4; ++k) std::fputc((v >> (8 * k)) & 0xFF, fp_); };
        auto u16 = [&](std::uint16_t v) { std::fputc(v & 0xFF, fp_); std::fputc(v >> 8, fp_); };
        std::fwrite("RIFF", 1, 4, fp_); u32(36 + data_bytes); std::fwrite("WAVEfmt ", 1, 8, fp_);
        u32(16); u16(1); u16(2); u32(48000); u32(48000 * 4); u16(4); u16(16);
        std::fwrite("data", 1, 4, fp_); u32(data_bytes);
        std::fseek(fp_, 0, SEEK_END);
    }
    std::FILE* fp_{};
    std::uint32_t data_bytes_{0};
};

#if defined(_WIN32)
class WaveOutSink final : public AudioSink {
public:
    WaveOutSink() {
        WAVEFORMATEX fx{};
        fx.wFormatTag = WAVE_FORMAT_PCM; fx.nChannels = 2; fx.nSamplesPerSec = 48000; fx.wBitsPerSample = 16;
        fx.nBlockAlign = static_cast<WORD>(fx.nChannels * fx.wBitsPerSample / 8); fx.nAvgBytesPerSec = fx.nSamplesPerSec * fx.nBlockAlign; fx.cbSize = 0;
        if (waveOutOpen(&dev_, WAVE_MAPPER, &fx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { dev_ = nullptr; return; }
        for (unsigned i = 0; i < kBuffers; ++i) {
            data_[i].assign(static_cast<std::size_t>(kFrames) * 2, 0);
            std::memset(&hdr_[i], 0, sizeof(WAVEHDR));
            hdr_[i].lpData = reinterpret_cast<LPSTR>(data_[i].data());
            hdr_[i].dwBufferLength = kFrames * 4;
            if (waveOutPrepareHeader(dev_, &hdr_[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) { shutdown(); return; }
            hdr_[i].dwFlags |= WHDR_DONE;       // a prepared header that is not queued counts as free
        }
        prepared_ = true;
    }
    ~WaveOutSink() override { shutdown(); }
    bool ok() const { return dev_ != nullptr && prepared_; }

    void push(const float* s, std::size_t frames) override {
        if (!ok()) return;
        for (std::size_t f = 0; f < frames; ++f) {
            if (dropping_) { if (!advance()) { ++dropped_; continue; } }
            data_[cur_][filled_ * 2] = audio_detail::to_s16(s[f * 2]);
            data_[cur_][filled_ * 2 + 1] = audio_detail::to_s16(s[f * 2 + 1]);
            if (++filled_ == kFrames) {
                waveOutWrite(dev_, &hdr_[cur_], sizeof(WAVEHDR));
                filled_ = 0;
                if (!advance()) dropping_ = true;
            }
        }
    }
    const char* name() const override { return "Windows waveOut"; }
    std::uint64_t frames_dropped() const override { return dropped_; }

private:
    static constexpr unsigned kBuffers = 8, kFrames = 960;          // 8 x 20 ms
    // Move to the next free buffer; false when all of them are still queued (the emulator is ahead of the device).
    bool advance() {
        const unsigned next = (cur_ + 1) % kBuffers;
        if (hdr_[next].dwFlags & WHDR_DONE) { cur_ = next; dropping_ = false; return true; }
        dropping_ = true; return false;
    }
    void shutdown() {
        if (!dev_) return;
        waveOutReset(dev_);
        for (unsigned i = 0; i < kBuffers; ++i) if (hdr_[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(dev_, &hdr_[i], sizeof(WAVEHDR));
        waveOutClose(dev_); dev_ = nullptr; prepared_ = false;
    }
    HWAVEOUT dev_{};
    WAVEHDR hdr_[kBuffers]{};
    std::vector<std::int16_t> data_[kBuffers];
    unsigned cur_{0}, filled_{0};
    bool dropping_{false}, prepared_{false};
    std::uint64_t dropped_{0};
};
#endif

// `wav_path` non-empty: write a file. Otherwise the platform output device (Windows) or nothing (other platforms).
inline std::unique_ptr<AudioSink> make_audio_sink(bool enabled, const std::string& wav_path) {
    if (!enabled) return std::make_unique<NullSink>();
    if (!wav_path.empty()) { auto w = std::make_unique<WavFileSink>(wav_path); if (w->ok()) return w; }
#if defined(_WIN32)
    auto w = std::make_unique<WaveOutSink>();
    if (w->ok()) return w;
#endif
    return std::make_unique<NullSink>();
}

} // namespace someboxone
