#pragma once
// SomeBoxOne v0.32.0 - software audio mixer behind the HLE XAudio2 engine.
//
// Models what XAudio2 source voices do: each voice has a queue of submitted buffers (with play range and loop points), a volume
// and a frequency ratio; started voices are resampled (linear interpolation) to the 48 kHz stereo output, down-mixed from
// mono / stereo / 5.1 / 7.1, summed and clipped. Buffer start/end, loop-end and end-of-stream notifications are queued as
// events so the title's IXAudio2VoiceCallback::OnBufferEnd refill logic keeps running at the right moments.
//
// Supported sample formats: PCM 8/16/24/32-bit and IEEE float (tag 1 / 3). Compressed formats (MS-ADPCM, XMA2, xWMA, Opus) are
// not decoded: those voices keep their timing and callbacks but are silent.
//
// All methods are called from the emulation thread; the mixer itself does no I/O.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

namespace someboxone {

struct AudioFormat {
    std::uint16_t tag{1};            // 1 = PCM, 3 = IEEE float; anything else is silent
    std::uint16_t channels{2};
    std::uint16_t bits{16};
    std::uint16_t block_align{4};
    std::uint32_t rate{48000};
};

class AudioMixer {
public:
    static constexpr unsigned kOutRate = 48000;
    using VoiceId = std::uint32_t;

    struct Event {
        enum Kind { BufferStart, BufferEnd, LoopEnd, StreamEnd } kind;
        VoiceId voice;
        std::uint64_t context;
    };
    struct VoiceState { std::uint64_t context{}; std::uint32_t queued{}; std::uint64_t samples_played{}; };

    VoiceId create_voice(const AudioFormat& fmt) {
        Voice v; v.fmt = fmt;
        if (v.fmt.channels == 0) v.fmt.channels = 1;
        if (v.fmt.block_align == 0) v.fmt.block_align = static_cast<std::uint16_t>(std::max<unsigned>(1, v.fmt.channels * ((v.fmt.bits + 7) / 8)));
        if (v.fmt.rate == 0) v.fmt.rate = kOutRate;
        voices_[next_id_] = std::move(v);
        return next_id_++;
    }
    void destroy_voice(VoiceId id) { voices_.erase(id); }
    void start(VoiceId id) { if (auto* v = find(id)) v->playing = true; }
    void stop(VoiceId id) { if (auto* v = find(id)) v->playing = false; }
    void set_volume(VoiceId id, float vol) { if (auto* v = find(id)) v->volume = vol; }
    void set_ratio(VoiceId id, float r) { if (auto* v = find(id)) v->ratio = std::clamp(r, 0.0009765625f, 1024.0f); }
    void set_source_rate(VoiceId id, std::uint32_t rate) { if (auto* v = find(id)) if (rate) v->fmt.rate = rate; }
    void set_master_volume(float vol) { master_ = vol; }
    float master_volume() const { return master_; }

    void submit(VoiceId id, std::vector<std::uint8_t>&& data, std::uint32_t flags, std::uint32_t play_begin, std::uint32_t play_length,
                std::uint32_t loop_begin, std::uint32_t loop_length, std::uint32_t loop_count, std::uint64_t context) {
        Voice* v = find(id); if (!v) return;
        Buffer b; b.data = std::move(data); b.flags = flags; b.context = context;
        b.total = total_frames(*v, b);
        b.begin = std::min<std::uint64_t>(play_begin, b.total);
        b.end = play_length ? std::min<std::uint64_t>(b.begin + play_length, b.total) : b.total;
        b.loop_count = loop_count;
        if (loop_count && loop_length) { b.loop_begin = loop_begin; b.loop_end = std::min<std::uint64_t>(static_cast<std::uint64_t>(loop_begin) + loop_length, b.total); }
        else if (loop_count) { b.loop_begin = b.begin; b.loop_end = b.end; }     // LoopLength 0 = loop the whole play range
        if (b.loop_count && b.loop_end <= b.loop_begin) b.loop_count = 0;
        v->queue.push_back(std::move(b));
    }
    void flush(VoiceId id) {   // XAudio2: drops every queued buffer except the one already playing; each dropped buffer still gets OnBufferEnd
        Voice* v = find(id); if (!v || v->queue.empty()) return;
        const std::size_t keep = v->current_started ? 1 : 0;
        for (std::size_t i = keep; i < v->queue.size(); ++i) events_.push_back({Event::BufferEnd, id, v->queue[i].context});
        v->queue.erase(v->queue.begin() + static_cast<std::ptrdiff_t>(keep), v->queue.end());
    }
    float volume(VoiceId id) const { const auto it = voices_.find(id); return it == voices_.end() ? 1.0f : it->second.volume; }
    float ratio(VoiceId id) const { const auto it = voices_.find(id); return it == voices_.end() ? 1.0f : it->second.ratio; }
    bool queue_empty(VoiceId id) const { const auto it = voices_.find(id); return it == voices_.end() || it->second.queue.empty(); }
    std::uint32_t source_rate(VoiceId id) const { const auto it = voices_.find(id); return it == voices_.end() ? kOutRate : it->second.fmt.rate; }
    unsigned block_align(VoiceId id) const { const auto it = voices_.find(id); return it == voices_.end() ? 4 : it->second.fmt.block_align; }
    void exit_loop(VoiceId id) { if (auto* v = find(id)) if (!v->queue.empty()) v->queue.front().loop_count = 0; }

    VoiceState state(VoiceId id) const {
        VoiceState s; const auto it = voices_.find(id); if (it == voices_.end()) return s;
        const Voice& v = it->second; s.queued = static_cast<std::uint32_t>(v.queue.size()); s.samples_played = v.samples_played;
        if (!v.queue.empty()) s.context = v.queue.front().context;
        return s;
    }
    bool any_playing() const { for (const auto& kv : voices_) if (kv.second.playing && !kv.second.queue.empty()) return true; return false; }
    std::size_t voice_count() const { return voices_.size(); }

    std::vector<Event> take_events() { std::vector<Event> out; out.swap(events_); return out; }

    // Writes `frames` interleaved stereo float frames (overwrites `out`).
    void mix(float* out, std::size_t frames) {
        std::fill(out, out + frames * 2, 0.0f);
        for (auto& kv : voices_) {
            Voice& v = kv.second;
            if (!v.playing) continue;
            const double step = static_cast<double>(v.fmt.rate) * v.ratio / kOutRate;
            for (std::size_t i = 0; i < frames; ++i) {
                if (v.queue.empty()) break;
                Buffer& b = v.queue.front();
                if (!v.current_started) { v.current_started = true; v.pos = static_cast<double>(b.begin); events_.push_back({Event::BufferStart, kv.first, b.context}); }
                float l = 0.0f, r = 0.0f;
                if (b.end > b.begin) {
                    const std::uint64_t i0 = static_cast<std::uint64_t>(v.pos), i1 = std::min<std::uint64_t>(i0 + 1, b.end - 1);
                    const float frac = static_cast<float>(v.pos - static_cast<double>(i0));
                    float l0, r0, l1, r1;
                    frame_lr(v, b, std::min<std::uint64_t>(i0, b.end - 1), l0, r0); frame_lr(v, b, i1, l1, r1);
                    l = l0 + (l1 - l0) * frac; r = r0 + (r1 - r0) * frac;
                }
                const float g = v.volume * master_;
                out[i * 2] += l * g; out[i * 2 + 1] += r * g;
                const double before = v.pos; v.pos += step;
                v.samples_played += static_cast<std::uint64_t>(std::floor(v.pos)) - static_cast<std::uint64_t>(std::floor(before));
                const std::uint64_t region_end = b.loop_count ? b.loop_end : b.end;
                if (v.pos >= static_cast<double>(region_end)) {
                    if (b.loop_count) {
                        if (b.loop_count != 255) --b.loop_count;                      // 255 = XAUDIO2_LOOP_INFINITE
                        v.pos = static_cast<double>(b.loop_begin) + (v.pos - static_cast<double>(b.loop_end));
                        events_.push_back({Event::LoopEnd, kv.first, b.context});
                    } else {
                        events_.push_back({Event::BufferEnd, kv.first, b.context});
                        if (b.flags & 0x40) events_.push_back({Event::StreamEnd, kv.first, b.context});   // XAUDIO2_END_OF_STREAM
                        v.queue.pop_front(); v.current_started = false;
                    }
                }
            }
        }
        for (std::size_t i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
    }

private:
    struct Buffer {
        std::vector<std::uint8_t> data; std::uint32_t flags{}; std::uint64_t context{};
        std::uint64_t total{}, begin{}, end{}, loop_begin{}, loop_end{}; std::uint32_t loop_count{};
    };
    struct Voice {
        AudioFormat fmt; std::deque<Buffer> queue; float volume{1.0f}, ratio{1.0f};
        bool playing{false}, current_started{false}; double pos{0}; std::uint64_t samples_played{0};
    };

    Voice* find(VoiceId id) { const auto it = voices_.find(id); return it == voices_.end() ? nullptr : &it->second; }

    static bool decodable(const AudioFormat& f) { return (f.tag == 1 && (f.bits == 8 || f.bits == 16 || f.bits == 24 || f.bits == 32)) || (f.tag == 3 && f.bits == 32); }

    static std::uint64_t total_frames(const Voice& v, const Buffer& b) {
        if (decodable(v.fmt)) return b.data.size() / v.fmt.block_align;
        if (v.fmt.tag == 2) {   // MS-ADPCM: ((block_align - 7*ch) * 2) / ch + 2 samples per block (timing only; the audio stays silent)
            const std::uint64_t per_block = static_cast<std::uint64_t>((std::max<int>(0, v.fmt.block_align - 7 * v.fmt.channels)) * 2 / v.fmt.channels + 2);
            return (b.data.size() / v.fmt.block_align) * per_block;
        }
        return b.data.size() / v.fmt.block_align * 128;   // unknown codec: assume 128 samples per block so queued buffers still drain
    }

    static float sample(const Voice& v, const Buffer& b, std::uint64_t frame, unsigned ch) {
        const std::size_t bytes = (v.fmt.bits + 7) / 8, off = static_cast<std::size_t>(frame) * v.fmt.block_align + ch * bytes;
        if (off + bytes > b.data.size()) return 0.0f;
        const std::uint8_t* p = &b.data[off];
        if (v.fmt.tag == 3) { float f; std::memcpy(&f, p, 4); return std::isfinite(f) ? f : 0.0f; }
        switch (v.fmt.bits) {
        case 8: return (static_cast<int>(p[0]) - 128) / 128.0f;
        case 16: return static_cast<std::int16_t>(p[0] | (p[1] << 8)) / 32768.0f;
        case 24: { std::int32_t s = (p[0] << 8) | (p[1] << 16) | (static_cast<std::int32_t>(p[2]) << 24); return static_cast<float>(s >> 8) / 8388608.0f; }
        default: { std::int32_t s; std::memcpy(&s, p, 4); return static_cast<float>(s / 2147483648.0); }
        }
    }

    // Down-mix one source frame to stereo. Channel order follows WAVEFORMATEXTENSIBLE: FL FR FC LFE BL BR (SL SR).
    static void frame_lr(const Voice& v, const Buffer& b, std::uint64_t frame, float& l, float& r) {
        l = r = 0.0f;
        if (!decodable(v.fmt)) return;
        const unsigned n = v.fmt.channels;
        if (n == 1) { l = r = sample(v, b, frame, 0); return; }
        l = sample(v, b, frame, 0); r = sample(v, b, frame, 1);
        if (n >= 3) { const float c = 0.7071f * sample(v, b, frame, 2); l += c; r += c; }
        if (n >= 6) { l += 0.5f * sample(v, b, frame, 4); r += 0.5f * sample(v, b, frame, 5); }
        if (n >= 8) { l += 0.5f * sample(v, b, frame, 6); r += 0.5f * sample(v, b, frame, 7); }
    }

    std::map<VoiceId, Voice> voices_;
    std::vector<Event> events_;
    VoiceId next_id_{1};
    float master_{1.0f};
};

} // namespace someboxone
