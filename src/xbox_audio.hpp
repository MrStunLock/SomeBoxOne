#pragma once
// SomeBoxOne v0.32.0 - HLE XAudio2 (xaudio2_9) for Xbox One titles.
//
// The engine, mastering voice and source voices are guest-visible objects with the documented XAudio2.9 vtable
// layouts. Source voices are backed by the software mixer in audio_mixer.hpp: submitted buffers are copied, resampled to 48 kHz
// stereo, mixed and sent to the audio sink (Windows waveOut / WAV file) once per main-loop iteration, and the title's
// IXAudio2VoiceCallback (OnBufferStart/End, OnLoopEnd, OnStreamEnd, OnVoiceProcessingPassStart) is invoked as buffers play.

namespace someboxone {

inline std::uint64_t XboxTitleRuntime::make_raw_object(const std::string& cls, const std::vector<Handler>& methods, unsigned total_slots) {
    const unsigned slots = (std::max)(total_slots, static_cast<unsigned>(methods.size()));
    const std::uint64_t vt = static_alloc(slots * 8ULL);
    for (unsigned i = 0; i < slots; ++i) {
        const std::string full = "COM:" + cls + "::vslot" + std::to_string(i) + "#" + GuestCpu::hex(vt);
        if (i < methods.size()) handlers_[full] = methods[i];
        else handlers_[full] = [this, cls, i](GuestCpu& c) { last_discovery_ = cls + " method slot " + std::to_string(i); milestone("AUDIO DISCOVERY: " + cls + " method slot " + std::to_string(i) + " not implemented, called from " + locate(mem_.read<std::uint64_t>(c.r[4])) + " -> E_NOTIMPL"); c.r[0] = 0x80004001ULL; };
        mem_.write<std::uint64_t>(vt + i * 8ULL, trap_for_name(full));
    }
    const std::uint64_t obj = static_alloc(64);
    mem_.write<std::uint64_t>(obj, vt);
    return obj;
}

// Voice objects are raw COM vtables (IXAudio2Voice slots 0-18, source voices 19-28, as in XAudio2.9). Source voices are backed by a
// voice in the software mixer (audio_mixer.hpp); the mastering voice scales the whole mix.
inline std::uint64_t XboxTitleRuntime::audio_make_voice(const std::string& cls, bool source, bool master, std::uint32_t mix_id) {
    auto ok = [](GuestCpu& c) { c.r[0] = 0; };
    auto vd = [](GuestCpu&) {};
    auto arg_float = [](GuestCpu& c, unsigned n) { return c.xmm[n].f[0]; };      // float parameters of a Win64 call arrive in XMMn (n = 1-based position - 1)
    std::vector<Handler> m = {
        vd, ok, ok, ok, ok, vd, ok, ok, ok, vd, ok, vd,                                                // 0-11: GetVoiceDetails..GetOutputFilterParameters
        [this, master, mix_id, arg_float](GuestCpu& c) {                                                // 12 SetVolume(float volume, UINT32 operationSet)
            const float v = arg_float(c, 1);
            if (master) { audio_title_master_ = v; audio_mixer_.set_master_volume(v * opt_.audio_volume); } else audio_mixer_.set_volume(mix_id, v);
            c.r[0] = 0; },
        [this, master, mix_id](GuestCpu& c) { mem_.write<float>(c.arg(1), master ? audio_title_master_ : audio_mixer_.volume(mix_id)); },   // 13 GetVolume(float*)
        ok, vd, ok, vd,                                                                                  // 14-17 channel volumes / output matrix
        [this, source, mix_id](GuestCpu&) { ++audio_voices_destroyed_; if (source) { audio_mixer_.destroy_voice(mix_id); audio_callbacks_.erase(mix_id); } }};   // 18 DestroyVoice
    if (source) {
        m.push_back([this, mix_id](GuestCpu& c) { ++audio_start_calls_; audio_mixer_.start(mix_id); audio_log_once("start", "XAudio2 source voice Start() called (audio is mixed to " + std::string(audio_sink_ ? audio_sink_->name() : "no device") + ")"); c.r[0] = 0; });   // 19 Start
        m.push_back([this, mix_id](GuestCpu& c) { audio_mixer_.stop(mix_id); c.r[0] = 0; });             // 20 Stop
        m.push_back([this, mix_id](GuestCpu& c) { // 21 SubmitSourceBuffer(const XAUDIO2_BUFFER*, const XAUDIO2_BUFFER_WMA*)
            const std::uint64_t b = c.arg(1);
            if (!b || !mem_.is_mapped(b, 48)) { c.r[0] = 0x80070057ULL; return; }
            const std::uint32_t flags = mem_.read<std::uint32_t>(b), bytes = mem_.read<std::uint32_t>(b + 4);
            const std::uint64_t data = mem_.read<std::uint64_t>(b + 8), ctx = mem_.read<std::uint64_t>(b + 40);
            if (bytes > (96u << 20) || (bytes && !mem_.is_mapped(data, bytes))) { c.r[0] = 0x80070057ULL; return; }
            std::vector<std::uint8_t> pcm(bytes); if (bytes) mem_.read_bytes(data, pcm.data(), bytes);   // copied now: the title may reuse its buffer once OnBufferEnd fires
            ++audio_buffers_submitted_; audio_bytes_submitted_ += bytes;
            audio_mixer_.submit(mix_id, std::move(pcm), flags, mem_.read<std::uint32_t>(b + 16), mem_.read<std::uint32_t>(b + 20), mem_.read<std::uint32_t>(b + 24),
                                mem_.read<std::uint32_t>(b + 28), mem_.read<std::uint32_t>(b + 32), ctx);
            c.r[0] = 0; });
        m.push_back([this, mix_id](GuestCpu& c) { audio_mixer_.flush(mix_id); c.r[0] = 0; });            // 22 FlushSourceBuffers
        m.push_back(ok);                                                                                 // 23 Discontinuity
        m.push_back([this, mix_id](GuestCpu& c) { audio_mixer_.exit_loop(mix_id); c.r[0] = 0; });        // 24 ExitLoop
        m.push_back([this, mix_id](GuestCpu& c) { // 25 GetState(XAUDIO2_VOICE_STATE*, flags): {pCurrentBufferContext, BuffersQueued, SamplesPlayed}
            const auto st = audio_mixer_.state(mix_id); const std::uint64_t o = c.arg(1);
            if (o && mem_.is_mapped(o, 24)) { zero_fill(o, 24); mem_.write<std::uint64_t>(o, st.context); mem_.write<std::uint32_t>(o + 8, st.queued); mem_.write<std::uint64_t>(o + 16, st.samples_played); } });
        m.push_back([this, mix_id, arg_float](GuestCpu& c) { audio_mixer_.set_ratio(mix_id, arg_float(c, 1)); c.r[0] = 0; });   // 26 SetFrequencyRatio(float, operationSet)
        m.push_back([this, mix_id](GuestCpu& c) { mem_.write<float>(c.arg(1), audio_mixer_.ratio(mix_id)); });                 // 27 GetFrequencyRatio(float*)
        m.push_back([this, mix_id](GuestCpu& c) { if (audio_mixer_.queue_empty(mix_id)) audio_mixer_.set_source_rate(mix_id, static_cast<std::uint32_t>(c.arg(1))); c.r[0] = 0; });   // 28 SetSourceSampleRate
    } else {
        m.push_back([this](GuestCpu& c) { mem_.write<std::uint32_t>(c.arg(1), 3); });                   // 19 GetChannelMask (stereo)
    }
    return make_raw_object(cls, m, 32);
}

inline void XboxTitleRuntime::audio_log_once(const std::string& key, const std::string& msg) {
    if (com_log_.insert("audio:" + key).second) milestone(msg);
}

inline std::uint64_t XboxTitleRuntime::audio_make_engine() {
    ComMethods m; // IXAudio2: 3 RegisterForCallbacks, 4 UnregisterForCallbacks, 5 CreateSourceVoice, 6 CreateSubmixVoice, 7 CreateMasteringVoice, 8 StartEngine, ...
    m.emplace_back("RegisterForCallbacks", [](GuestCpu& c) { c.r[0] = 0; });
    m.emplace_back("UnregisterForCallbacks", [](GuestCpu&) {});
    m.emplace_back("CreateSourceVoice", [this](GuestCpu& c) { // (ppVoice, WAVEFORMATEX*, flags, maxFreqRatio, callback, sendList, effectChain)
        AudioFormat fmt; const std::uint64_t f = c.arg(2);
        if (!f || !mem_.is_mapped(f, 16)) { c.r[0] = 0x80070057ULL; return; }
        fmt.tag = mem_.read<std::uint16_t>(f); fmt.channels = mem_.read<std::uint16_t>(f + 2); fmt.rate = mem_.read<std::uint32_t>(f + 4);
        fmt.block_align = mem_.read<std::uint16_t>(f + 12); fmt.bits = mem_.read<std::uint16_t>(f + 14);
        if (fmt.tag == 0xFFFE && mem_.is_mapped(f, 40)) fmt.tag = mem_.read<std::uint16_t>(f + 24);   // WAVEFORMATEXTENSIBLE: the sub-format GUID's first word is the real tag
        audio_log_once("src", "XAudio2 CreateSourceVoice: format tag " + std::to_string(fmt.tag) + ", " + std::to_string(fmt.channels) + " ch, " + std::to_string(fmt.rate) + " Hz, " + std::to_string(fmt.bits) + " bit" +
                       ((fmt.tag == 1 || fmt.tag == 3) ? "" : " (compressed: not decoded, the voice stays silent)"));
        const auto id = audio_mixer_.create_voice(fmt);
        if (c.arg(5)) { audio_callbacks_[id] = c.arg(5); audio_callback_ = c.arg(5); }
        audio_start_service();
        mem_.write<std::uint64_t>(c.arg(1), audio_make_voice("IXAudio2SourceVoice", true, false, id)); c.r[0] = 0; });
    m.emplace_back("CreateSubmixVoice", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(1), audio_make_voice("IXAudio2SubmixVoice", false, false, 0)); c.r[0] = 0; });
    m.emplace_back("CreateMasteringVoice", [this](GuestCpu& c) {
        audio_log_once("master", "XAudio2 CreateMasteringVoice (category " + std::to_string(c.arg(7) & 0xFFFFFFFFULL) + ")");
        audio_start_service();
        mem_.write<std::uint64_t>(c.arg(1), audio_make_voice("IXAudio2MasteringVoice", false, true, 0)); c.r[0] = 0; });
    m.emplace_back("StartEngine", [](GuestCpu& c) { c.r[0] = 0; });
    m.emplace_back("StopEngine", [](GuestCpu&) {});
    m.emplace_back("CommitChanges", [](GuestCpu& c) { c.r[0] = 0; });
    m.emplace_back("GetPerformanceData", [this](GuestCpu& c) { zero_fill(c.arg(1), 40); });
    m.emplace_back("SetDebugConfiguration", [](GuestCpu&) {});
    return make_com_object("IXAudio2", m, {"*"}, false, 20);
}

inline void XboxTitleRuntime::register_audio_handlers() {
    // XAudio2Create is inlined in the title: LoadLibraryExW("xaudio2_9.dll") + GetProcAddress("CreateXAudio2Object").
    // CreateXAudio2Object(IXAudio2** out, ...) -> S_OK with a new engine.
    H("CreateXAudio2Object", [this](GuestCpu& c) {
        if (!audio_engine_) audio_engine_ = audio_make_engine();
        mem_.write<std::uint64_t>(audio_engine_ + 8, mem_.read<std::uint64_t>(audio_engine_ + 8) + 1);
        mem_.write<std::uint64_t>(c.arg(0), audio_engine_);
        audio_log_once("engine", "XAudio2 engine created (HLE; software mixer)");
        c.r[0] = 0;
    });
    H("XAudio2Create", handlers_["CreateXAudio2Object"]);
    H("XAudio2CreateWithVersionInfo", handlers_["CreateXAudio2Object"]);
}


// ---- audio service: runs once per main-loop iteration ----
inline void XboxTitleRuntime::audio_start_service() {
    if (audio_started_) return;
    audio_started_ = true;
    audio_sink_ = make_audio_sink(opt_.audio_enabled, opt_.audio_wav_path);
    audio_mixer_.set_master_volume(audio_title_master_ * opt_.audio_volume);
    audio_last_ = std::chrono::steady_clock::now();
    milestone(std::string("audio output: ") + audio_sink_->name() + (opt_.audio_enabled ? "" : " (audio disabled in settings; voices still run so the title's callbacks keep firing)"));
}

inline void XboxTitleRuntime::audio_pump(GuestCpu& c) {
    if (!audio_started_) return;
    const auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - audio_last_).count();
    audio_last_ = now;
    dt = dt < 0.0 ? 0.0 : (dt > 0.1 ? 0.1 : dt);                  // never synthesise more than 100 ms at once (breakpoints, long stalls)
    audio_carry_ += dt * AudioMixer::kOutRate;
    const std::size_t frames = static_cast<std::size_t>(audio_carry_);
    audio_carry_ -= static_cast<double>(frames);
    if (frames) {
        audio_scratch_.resize(frames * 2);
        audio_mixer_.mix(audio_scratch_.data(), frames);
        if (audio_sink_) audio_sink_->push(audio_scratch_.data(), frames);
        audio_frames_mixed_ += frames;
    }
    // voice callbacks (IXAudio2VoiceCallback vtable: 0 PassStart(bytes), 1 PassEnd, 2 StreamEnd, 3 BufferStart(ctx), 4 BufferEnd(ctx), 5 LoopEnd(ctx))
    for (const auto& e : audio_mixer_.take_events()) {
        const auto it = audio_callbacks_.find(e.voice); if (it == audio_callbacks_.end() || !it->second) continue;
        unsigned slot = 3; std::uint64_t arg = e.context;
        switch (e.kind) { case AudioMixer::Event::BufferStart: slot = 3; break; case AudioMixer::Event::BufferEnd: slot = 4; break; case AudioMixer::Event::LoopEnd: slot = 5; break; case AudioMixer::Event::StreamEnd: slot = 2; arg = 0; break; }
        if (slot == 2) guest_vcall(c, it->second, 2, {}); else guest_vcall(c, it->second, slot, {arg});
        if (c.stop != GuestCpu::Stop::None) return;
    }
    // OnVoiceProcessingPassStart(BytesRequired): XAudio2 asks a starved streaming voice for one 10 ms pass of data
    for (const auto& kv : audio_callbacks_) {
        if (!kv.second || !audio_mixer_.queue_empty(kv.first)) continue;
        const std::uint32_t bytes = static_cast<std::uint32_t>(audio_mixer_.source_rate(kv.first) / 100) * audio_mixer_.block_align(kv.first);
        guest_vcall(c, kv.second, 0, {bytes});
        if (c.stop != GuestCpu::Stop::None) return;
    }
}

// Everything that must happen at the title's frame boundary after the frame has been published: mix + play audio for the elapsed
// real time, run the title's audio callbacks, then hold the frame-rate lock.
inline void XboxTitleRuntime::frame_services(GuestCpu& c) {
    audio_pump(c);
    if (c.stop != GuestCpu::Stop::None) return;
    if (!pacer_configured_) { pacer_.set_limit(opt_.fps_limit); pacer_configured_ = true; milestone(opt_.fps_limit ? "frame-rate lock: " + std::to_string(opt_.fps_limit) + " FPS" : std::string("frame-rate lock: off (unlimited)")); }
    pacer_.end_frame(opt_.cancel);
}

} // namespace someboxone
