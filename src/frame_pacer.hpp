#pragma once
// SomeBoxOne v0.32.0 - frame pacing and FPS measurement.
//
// A "frame" is one iteration of the title's main loop (CoreDispatcher::ProcessEvents). end_frame() is called once per frame:
//   * it measures the real frame rate over a sliding half-second window (this is the number the FPS counter shows), and
//   * when a limit is set (30 or 60) it sleeps until the next frame deadline, so the emulator never runs faster than the lock.
// If the emulator is slower than the limit nothing sleeps. A deadline that has fallen more than four frames behind is
// re-based on "now" instead of being caught up, so a slow stretch is not followed by a burst of unthrottled frames.
//
// Windows' default timer granularity is ~15.6 ms, which would make a 16.7 ms frame deadline wobble badly. The pacer asks
// for 1 ms resolution (timeBeginPeriod, loaded dynamically so no extra import library is needed) and finishes each wait with a
// short spin, so the lock holds within about a millisecond.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace someboxone {

class FramePacer {
public:
    using Clock = std::chrono::steady_clock;

    FramePacer() { request_fine_timer(); }
    ~FramePacer() { release_fine_timer(); }
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    // 0 = unlimited; otherwise a frames-per-second cap (the UI offers 30 and 60).
    void set_limit(unsigned fps) {
        limit_ = fps > 240 ? 240 : fps;
        period_ = limit_ ? std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / limit_)) : Clock::duration::zero();
        have_deadline_ = false;
    }
    unsigned limit() const { return limit_; }

    // Call once per frame. Returns the time spent waiting for the limiter (zero when the emulator is behind the limit).
    Clock::duration end_frame(const std::atomic<bool>* cancel = nullptr) {
        Clock::duration waited = Clock::duration::zero();
        Clock::time_point now = Clock::now();
        if (limit_) {
            if (!have_deadline_) { deadline_ = now + period_; have_deadline_ = true; }
            else {
                deadline_ += period_;
                if (now > deadline_ + period_ * 4) deadline_ = now + period_;     // far behind: re-base, do not burst to catch up
            }
            if (now < deadline_) {
                const Clock::time_point start = now;
                constexpr auto spin_window = std::chrono::microseconds(1500);
                while (true) {
                    now = Clock::now();
                    const auto left = deadline_ - now;
                    if (left <= Clock::duration::zero()) break;
                    if (cancel && cancel->load(std::memory_order_relaxed)) break;
                    if (left > spin_window + std::chrono::milliseconds(1)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    else std::this_thread::yield();
                }
                waited = now - start;
            }
        }
        // sliding measurement window (real time, independent of the limiter)
        ++window_frames_;
        if (!window_started_) { window_start_ = now; window_started_ = true; window_frames_ = 0; }
        else {
            const double elapsed = std::chrono::duration<double>(now - window_start_).count();
            if (elapsed >= 0.5) {
                fps_ = static_cast<double>(window_frames_) / elapsed;
                window_start_ = now; window_frames_ = 0;
            }
        }
        last_frame_end_ = now;
        return waited;
    }

    double fps() const { return fps_; }                       // smoothed frames per second over the last ~0.5 s (0 until measured)
    void reset_measurement() { window_started_ = false; fps_ = 0.0; have_deadline_ = false; }

private:
    static void request_fine_timer() {
#if defined(_WIN32)
        if (timer_users_.fetch_add(1) == 0) {
            if (HMODULE h = LoadLibraryA("winmm.dll")) {
                using Fn = UINT(WINAPI*)(UINT);
                if (auto f = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(h, "timeBeginPeriod")))) f(1);
            }
        }
#endif
    }
    static void release_fine_timer() {
#if defined(_WIN32)
        if (timer_users_.fetch_sub(1) == 1) {
            if (HMODULE h = GetModuleHandleA("winmm.dll")) {
                using Fn = UINT(WINAPI*)(UINT);
                if (auto f = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(h, "timeEndPeriod")))) f(1);
            }
        }
#endif
    }
#if defined(_WIN32)
    static inline std::atomic<int> timer_users_{0};
#endif

    unsigned limit_{0};
    Clock::duration period_{Clock::duration::zero()};
    Clock::time_point deadline_{}, window_start_{}, last_frame_end_{};
    bool have_deadline_{false}, window_started_{false};
    unsigned window_frames_{0};
    double fps_{0.0};
};

} // namespace someboxone
