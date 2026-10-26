#pragma once
// SomeBoxOne v0.32.0 - cooperative guest threads.
//
// Before this file CreateThread only *recorded* the request, so any title whose game logic lives on a worker thread
// (Minecraft: Xbox One Edition, most engines) sat in an idle main loop forever. Now every CreateThread gets its own
// GuestCpu (registers, TEB, TLS block) over the shared guest memory and the threads are scheduled cooperatively:
//   * the main thread runs all other threads once per main-loop iteration (CoreDispatcher::ProcessEvents) and whenever
//     it blocks (WaitFor*, Sleep, critical section / SRW lock contention, condition variables);
//   * a worker yields whenever it blocks: its HLE call is left un-returned on the trap and re-evaluated the next time
//     it is scheduled, so no host thread, no preemption inside HLE code and no data races are possible;
//   * time is virtual: Sleep/timeouts advance the QPC counter, so a deadlocked wait cannot hang the emulator.
// Synchronisation objects (events, semaphores, mutexes, critical sections, SRW locks, condition variables, WaitOnAddress)
// follow real Windows semantics instead of the old "treat every wait as satisfied" approximation.

namespace someboxone {

inline void XboxTitleRuntime::run_threads(std::uint64_t slice) {
    if (cur_thread_ || threads_.empty()) return;   // only the main thread schedules
    for (std::size_t i = 0; i < threads_.size(); ++i) {
        GuestThread& t = *threads_[i];
        if (t.finished || t.suspended) continue;
        GuestCpu& tc = *t.cpu;
        tc.stop = GuestCpu::Stop::None; tc.stop_detail.clear();
        cur_thread_ = &t;
        tc.run(slice);
        cur_thread_ = nullptr;
        ++t.slices;
        switch (tc.stop) {
        case GuestCpu::Stop::None: case GuestCpu::Stop::StepLimit: break;   // slice used up or yielded: resume next pass
        case GuestCpu::Stop::HostRequest:
            if (t.finished) break;                                          // returned from its entry point / ExitThread
            [[fallthrough]];
        default: {
            t.finished = true;
            const std::string why = "guest thread 0x" + GuestCpu::hex(t.tid) + " (start " + locate(t.start) + ") stopped: " +
                (tc.stop_detail.empty() ? std::string("fault") : tc.stop_detail) + " at " + locate(tc.fault_rip ? tc.fault_rip : tc.rip);
            milestone(why);
            if (cpu_.stop == GuestCpu::Stop::None) cpu_.request_stop(GuestCpu::Stop::HostRequest, why);
            return;
        }
        }
        if (cpu_.stop != GuestCpu::Stop::None) return;
    }
}

// Blocks the calling guest thread until ready() (which also performs the acquisition) returns true.
//  Ready   - the caller completes the call.   Timeout - the caller returns WAIT_TIMEOUT.
//  Retry   - (worker threads only) the HLE call was parked on its trap; the handler must return without touching registers.
inline XboxTitleRuntime::WaitResult XboxTitleRuntime::wait_for(GuestCpu& c, const std::function<bool()>& ready, std::uint64_t timeout_ms) {
    constexpr std::uint64_t infinite = 0xFFFFFFFFULL;
    if (ready()) { if (cur_thread_) cur_thread_->waiting = false; return WaitResult::Ready; }
    if (timeout_ms == 0) { if (cur_thread_) cur_thread_->waiting = false; return WaitResult::Timeout; }
    if (cur_thread_) {
        GuestThread& t = *cur_thread_;
        if (!(t.waiting && t.wait_rsp == c.r[4])) { t.waiting = true; t.wait_rsp = c.r[4]; t.deadline = timeout_ms == infinite ? ~0ULL : qpc_ + timeout_ms * 10000ULL; }
        else if (qpc_ >= t.deadline) { t.waiting = false; return WaitResult::Timeout; }
        hle_retry_ = true; pending_yield_ = true;
        return WaitResult::Retry;
    }
    // Main thread: run the other threads until the condition holds. Virtual time advances 2 ms per idle pass.
    const std::uint64_t deadline = timeout_ms == infinite ? ~0ULL : qpc_ + timeout_ms * 10000ULL;
    constexpr unsigned kMaxPasses = 300;                               // 300 passes = 600 ms of virtual time for an INFINITE wait
    for (unsigned pass = 0; pass < kMaxPasses; ++pass) {
        run_threads((std::min<std::uint64_t>)(thread_slice_, 300000));   // short slices while the main thread waits: it only needs the others to make progress
        if (c.stop != GuestCpu::Stop::None) return WaitResult::Timeout;
        if (ready()) return WaitResult::Ready;
        if (qpc_ >= deadline) return WaitResult::Timeout;
        qpc_ += 20000;
        if (opt_.cancel && opt_.cancel->load()) { c.request_stop(GuestCpu::Stop::HostRequest, "stopped by the user while waiting"); return WaitResult::Timeout; }
    }
    if (thread_deadlocks_logged_++ < 6) milestone("a main-thread wait (called from " + locate(mem_.read<std::uint64_t>(c.r[4])) + ") made no progress after " + std::to_string(kMaxPasses) + " scheduling passes (deadlock suspected); it was released so the emulator keeps running");
    return WaitResult::Ready;
}

// Try to acquire/observe one kernel object handle (non-blocking). Unknown handle types count as signalled.
inline bool XboxTitleRuntime::wait_try_handle(std::uint64_t h) {
    const std::uint32_t me = current_tid();
    if (const auto sem = semaphores_.find(h); sem != semaphores_.end()) { if (sem->second.count > 0) { --sem->second.count; return true; } return false; }
    const auto it = win_handles_.find(h);
    if (it == win_handles_.end()) return true;
    WinHandle& w = it->second;
    switch (w.kind) {
    case WinHandle::Event: if (w.signaled) { if (!w.manual) w.signaled = false; return true; } return false;
    case WinHandle::Semaphore: if (w.count > 0) { --w.count; return true; } return false;
    case WinHandle::Mutex: if (w.owner == 0 || w.owner == me) { w.owner = me; ++w.count; return true; } return false;
    case WinHandle::Thread: return w.thread_index < threads_.size() ? threads_[w.thread_index]->finished : true;
    default: return true;
    }
}

inline std::uint64_t XboxTitleRuntime::create_guest_thread(GuestCpu& c, std::uint64_t stack, std::uint64_t start, std::uint64_t param, std::uint64_t flags, std::uint64_t tid_out) {
    (void)c;
    if (threads_.size() >= 64) { milestone("CreateThread refused: more than 64 guest threads"); last_error_ = 8; return 0; }
    auto t = std::make_unique<GuestThread>();
    t->cpu = std::make_unique<GuestCpu>(mem_, 1u << 14);
    GuestCpu& tc = *t->cpu;
    tc.use_fast = cpu_.use_fast; tc.trap_lo = cpu_.trap_lo; tc.trap_hi = cpu_.trap_hi; tc.trap_handler = cpu_.trap_handler;
    // stack
    std::uint64_t size = stack ? stack : 0x100000ULL;
    size = (std::min<std::uint64_t>)((std::max<std::uint64_t>)(size, 0x40000ULL), 0x200000ULL);
    const std::uint64_t base = heap_alloc(size + 0x1000, false);
    if (!base) { last_error_ = 8; return 0; }
    const std::uint64_t top = (base + size + 0x800) & ~0xFULL;
    if (!thread_exit_trap_) thread_exit_trap_ = trap_for_name("SboThreadExit");
    std::uint64_t sp = (top - 0x200) & ~0xFULL; sp -= 8;               // entry convention: RSP == 8 (mod 16) with the return address on top
    mem_.write<std::uint64_t>(sp, thread_exit_trap_);
    for (auto& r : tc.r) r = 0;
    tc.r[4] = sp; tc.r[1] = param; tc.rip = start;
    tc.rflags = cpu_.rflags;
    // TEB + a private copy of every module's static TLS block
    t->teb = static_alloc(0x1000, 0x1000);
    t->tid = (next_tid_ += 4);
    mem_.write<std::uint64_t>(t->teb + 0x08, top);
    mem_.write<std::uint64_t>(t->teb + 0x10, base);
    mem_.write<std::uint64_t>(t->teb + 0x30, t->teb);
    mem_.write<std::uint64_t>(t->teb + 0x40, 0x400);
    mem_.write<std::uint64_t>(t->teb + 0x48, t->tid);
    mem_.write<std::uint64_t>(t->teb + 0x60, xrt::kPebBase);
    const std::uint64_t arr = static_alloc(8 * 64);
    for (const auto& tpl : tls_templates_) {
        const std::uint64_t block = static_alloc(tpl.len + tpl.zero_fill + 64);
        if (tpl.len) copy_guest(block, tpl.start_va, tpl.len);
        mem_.write<std::uint64_t>(arr + tpl.index * 8ULL, block);
    }
    mem_.write<std::uint64_t>(t->teb + 0x58, arr);
    tc.gs_base = t->teb;
    t->start = start; t->suspended = (flags & 4) != 0;
    WinHandle wh; wh.kind = WinHandle::Thread; wh.thread_index = threads_.size();
    const std::uint64_t handle = next_win_handle_(); win_handles_[handle] = wh; t->handle = handle;
    if (tid_out && mem_.is_mapped(tid_out, 4)) mem_.write<std::uint32_t>(tid_out, t->tid);
    milestone("CreateThread: guest thread 0x" + GuestCpu::hex(t->tid) + " starts at " + locate(start) + (t->suspended ? " (suspended)" : "") + "; it is scheduled cooperatively");
    threads_.push_back(std::move(t));
    return handle;
}

inline void XboxTitleRuntime::register_thread_handlers() {
    auto ok = [](GuestCpu& c) { c.r[0] = 1; };
    H("SboThreadExit", [this](GuestCpu& c) {
        if (cur_thread_) { cur_thread_->finished = true; cur_thread_->exit_code = static_cast<std::uint32_t>(c.r[0]); }
        c.request_stop(GuestCpu::Stop::HostRequest, "thread exit"); });
    H("CreateThread", [this](GuestCpu& c) { ++threads_created_; c.r[0] = create_guest_thread(c, c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5)); });
    H("_beginthreadex", [this](GuestCpu& c) { ++threads_created_; c.r[0] = create_guest_thread(c, c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5)); });
    H("ExitThread", [this](GuestCpu& c) {
        if (cur_thread_) { cur_thread_->finished = true; cur_thread_->exit_code = static_cast<std::uint32_t>(c.arg(0)); c.request_stop(GuestCpu::Stop::HostRequest, "thread exit"); }
        else c.request_stop(GuestCpu::Stop::HostRequest, "ExitThread on the main guest thread"); });
    H("GetCurrentThreadId", [this](GuestCpu& c) { c.r[0] = current_tid(); });
    H("ResumeThread", [this](GuestCpu& c) {
        const auto it = win_handles_.find(c.arg(0)); c.r[0] = 0;
        if (it != win_handles_.end() && it->second.kind == WinHandle::Thread && it->second.thread_index < threads_.size()) { auto& t = *threads_[it->second.thread_index]; c.r[0] = t.suspended ? 1 : 0; t.suspended = false; } });
    H("GetExitCodeThread", [this](GuestCpu& c) {
        std::uint32_t code = 259; const auto it = win_handles_.find(c.arg(0));
        if (it != win_handles_.end() && it->second.kind == WinHandle::Thread && it->second.thread_index < threads_.size() && threads_[it->second.thread_index]->finished) code = threads_[it->second.thread_index]->exit_code;
        if (c.arg(1)) mem_.write<std::uint32_t>(c.arg(1), code);
        c.r[0] = 1; });

    // ---- per-thread TLS / FLS (slot numbers are process-wide, values are per thread) ----
    auto slot_vec = [this](bool fls) -> std::vector<std::uint64_t>& {
        auto& global = fls ? fls_slots_ : tls_slots_;
        if (!cur_thread_) return global;
        auto& mine = fls ? cur_thread_->fls : cur_thread_->tls;
        if (mine.size() < global.size()) mine.resize(global.size(), 0);
        return mine; };
    for (bool fls : {false, true}) {
        const char* get = fls ? "FlsGetValue" : "TlsGetValue"; const char* set = fls ? "FlsSetValue" : "TlsSetValue";
        H(get, [this, slot_vec, fls](GuestCpu& c) { auto& v = slot_vec(fls); const auto i = c.arg(0); last_error_ = 0; c.r[0] = i < v.size() ? v[static_cast<std::size_t>(i)] : 0; });
        H(set, [this, slot_vec, fls](GuestCpu& c) { auto& v = slot_vec(fls); const auto i = c.arg(0); if (i < v.size()) { v[static_cast<std::size_t>(i)] = c.arg(1); c.r[0] = 1; } else { last_error_ = 87; c.r[0] = 0; } });
    }

    // ---- sleeping / yielding ----
    auto do_sleep = [this](GuestCpu& c, std::uint64_t ms) {
        if (ms == 0) { if (cur_thread_) pending_yield_ = true; else run_threads(thread_slice_); return; }
        if (!cur_thread_ && threads_.empty()) { qpc_ += ms * 10000ULL; return; }   // single-threaded title: just advance virtual time
        const auto r = wait_for(c, []() { return false; }, ms);
        (void)r; };                                                             // Timeout (the sleep elapsed) and Retry both leave r[0] alone
    H("Sleep", [do_sleep](GuestCpu& c) { do_sleep(c, c.arg(0) & 0xFFFFFFFFULL); });
    H("SleepEx", [do_sleep](GuestCpu& c) { do_sleep(c, c.arg(0) & 0xFFFFFFFFULL); c.r[0] = 0; });
    H("__crtSleep", [do_sleep](GuestCpu& c) { do_sleep(c, c.arg(0) & 0xFFFFFFFFULL); });
    H("SwitchToThread", [this](GuestCpu& c) { if (cur_thread_) pending_yield_ = true; else run_threads(thread_slice_); c.r[0] = 1; });

    // ---- kernel object waits ----
    H("WaitForSingleObject", [this](GuestCpu& c) {
        const auto h = c.arg(0); const auto r = wait_for(c, [this, h]() { return wait_try_handle(h); }, c.arg(1) & 0xFFFFFFFFULL);
        if (r != WaitResult::Retry) c.r[0] = r == WaitResult::Ready ? 0 : 0x102; });
    H("WaitForSingleObjectEx", [this](GuestCpu& c) {
        const auto h = c.arg(0); const auto r = wait_for(c, [this, h]() { return wait_try_handle(h); }, c.arg(1) & 0xFFFFFFFFULL);
        if (r != WaitResult::Retry) c.r[0] = r == WaitResult::Ready ? 0 : 0x102; });
    auto wait_multiple = [this](GuestCpu& c, bool ex) {
        const std::uint64_t n = c.arg(0) & 0xFFFFFFFFULL, handles = c.arg(1); const bool all = (c.arg(2) & 0xFFFFFFFFULL) != 0;
        const std::uint64_t ms = c.arg(3) & 0xFFFFFFFFULL; (void)ex;
        if (n == 0 || n > 64) { c.r[0] = 0xFFFFFFFFULL; return; }
        std::uint64_t index = 0;
        const auto r = wait_for(c, [&]() {
            if (all) { // all-or-nothing: only consume when every object is available
                for (std::uint64_t i = 0; i < n; ++i) { const auto h = mem_.read<std::uint64_t>(handles + i * 8); const auto it = win_handles_.find(h);
                    if (it == win_handles_.end() && !semaphores_.count(h)) continue;
                    if (it != win_handles_.end()) { const auto& w = it->second;
                        if ((w.kind == WinHandle::Event && !w.signaled) || (w.kind == WinHandle::Semaphore && w.count <= 0) || (w.kind == WinHandle::Mutex && w.owner && w.owner != current_tid()) ||
                            (w.kind == WinHandle::Thread && w.thread_index < threads_.size() && !threads_[w.thread_index]->finished)) return false; }
                    else if (semaphores_[h].count == 0) return false; }
                for (std::uint64_t i = 0; i < n; ++i) wait_try_handle(mem_.read<std::uint64_t>(handles + i * 8));
                index = 0; return true; }
            for (std::uint64_t i = 0; i < n; ++i) if (wait_try_handle(mem_.read<std::uint64_t>(handles + i * 8))) { index = i; return true; }
            return false; }, ms);
        if (r != WaitResult::Retry) c.r[0] = r == WaitResult::Ready ? index : 0x102; };
    H("WaitForMultipleObjects", [wait_multiple](GuestCpu& c) { wait_multiple(c, false); });
    H("WaitForMultipleObjectsEx", [wait_multiple](GuestCpu& c) { wait_multiple(c, true); });
    H("SignalObjectAndWait", [this](GuestCpu& c) { // (signal, wait, ms, alertable)
        const auto sig = win_handles_.find(c.arg(0));
        if (sig != win_handles_.end() && c.arg(0)) { if (sig->second.kind == WinHandle::Event) sig->second.signaled = true; else if (sig->second.kind == WinHandle::Semaphore) ++sig->second.count; }
        const auto h = c.arg(1); const auto r = wait_for(c, [this, h]() { return wait_try_handle(h); }, c.arg(2) & 0xFFFFFFFFULL);
        if (r != WaitResult::Retry) c.r[0] = r == WaitResult::Ready ? 0 : 0x102; });
    // mutexes with ownership
    H("CreateMutexExW", [this](GuestCpu& c) { WinHandle h; h.kind = WinHandle::Mutex; if (c.arg(2) & 1) { h.owner = current_tid(); h.count = 1; } const auto id = next_win_handle_(); win_handles_[id] = h; c.r[0] = id; });
    H("CreateMutexW", [this](GuestCpu& c) { WinHandle h; h.kind = WinHandle::Mutex; if (c.arg(1)) { h.owner = current_tid(); h.count = 1; } const auto id = next_win_handle_(); win_handles_[id] = h; c.r[0] = id; });
    H("ReleaseMutex", [this](GuestCpu& c) {
        const auto it = win_handles_.find(c.arg(0));
        if (it == win_handles_.end() || it->second.kind != WinHandle::Mutex || it->second.owner != current_tid()) { last_error_ = 288; c.r[0] = 0; return; }
        if (--it->second.count <= 0) { it->second.count = 0; it->second.owner = 0; }
        c.r[0] = 1; });
    H("CloseHandle", [this](GuestCpu& c) {
        const auto h = c.arg(0); bool known = semaphores_.erase(h) != 0 || mutex_handles_.erase(h) != 0;
        const auto it = win_handles_.find(h);
        if (it != win_handles_.end()) { known = true; if (it->second.fp) std::fclose(it->second.fp); win_handles_.erase(it); }   // a running thread keeps running after its handle is closed
        c.r[0] = (known || h >= 0xDEAD1000ULL) ? 1 : 0; last_error_ = c.r[0] ? 0 : 6; });

    // ---- critical sections (CRITICAL_SECTION: LockCount +8, RecursionCount +12, OwningThread +16) ----
    auto cs_try = [this](std::uint64_t a) -> bool {
        const std::uint32_t me = current_tid(); const auto owner = static_cast<std::uint32_t>(mem_.read<std::uint64_t>(a + 16));
        if (owner == 0) { mem_.write<std::uint64_t>(a + 16, me); mem_.write<std::int32_t>(a + 12, 1); mem_.write<std::int32_t>(a + 8, 0); return true; }
        if (owner == me) { const auto rec = mem_.read<std::int32_t>(a + 12) + 1; mem_.write<std::int32_t>(a + 12, rec); mem_.write<std::int32_t>(a + 8, rec - 1); return true; }
        return false; };
    H("EnterCriticalSection", [this, cs_try](GuestCpu& c) {
        const auto a = c.arg(0); if (!a || !mem_.is_mapped(a, 40)) return;
        const auto r = wait_for(c, [&]() { return cs_try(a); }, 0xFFFFFFFFULL); (void)r; });
    H("TryEnterCriticalSection", [this, cs_try](GuestCpu& c) { const auto a = c.arg(0); c.r[0] = (a && mem_.is_mapped(a, 40) && cs_try(a)) ? 1 : 0; });
    H("LeaveCriticalSection", [this](GuestCpu& c) {
        const auto a = c.arg(0); if (!a || !mem_.is_mapped(a, 40)) return;
        const auto rec = mem_.read<std::int32_t>(a + 12);
        if (rec <= 1) { mem_.write<std::int32_t>(a + 12, 0); mem_.write<std::uint64_t>(a + 16, 0); mem_.write<std::int32_t>(a + 8, -1); }
        else { mem_.write<std::int32_t>(a + 12, rec - 1); mem_.write<std::int32_t>(a + 8, rec - 2); } });

    // ---- slim reader/writer locks ----
    auto srw_try = [this](std::uint64_t a, bool shared) -> bool {
        auto& s = srw_[a];
        if (shared) { if (s.excl) return false; ++s.shared; return true; }
        if (s.excl || s.shared) return false; s.excl = current_tid(); return true; };
    for (bool shared : {false, true}) {
        const char* acq = shared ? "AcquireSRWLockShared" : "AcquireSRWLockExclusive"; const char* rel = shared ? "ReleaseSRWLockShared" : "ReleaseSRWLockExclusive";
        const char* tryacq = shared ? "TryAcquireSRWLockShared" : "TryAcquireSRWLockExclusive";
        H(acq, [this, srw_try, shared](GuestCpu& c) { const auto a = c.arg(0); const auto r = wait_for(c, [&]() { return srw_try(a, shared); }, 0xFFFFFFFFULL); (void)r; });
        H(tryacq, [srw_try, shared](GuestCpu& c) { c.r[0] = srw_try(c.arg(0), shared) ? 1 : 0; });
        H(rel, [this, shared](GuestCpu& c) { auto& s = srw_[c.arg(0)]; if (shared) { if (s.shared) --s.shared; } else s.excl = 0; });
    }
    H("InitializeSRWLock", [this](GuestCpu& c) { srw_.erase(c.arg(0)); mem_.write<std::uint64_t>(c.arg(0), 0); });

    // ---- condition variables ----
    H("InitializeConditionVariable", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(0), 0); cv_gen_[c.arg(0)] = 0; });
    H("WakeConditionVariable", [this](GuestCpu& c) { ++cv_gen_[c.arg(0)]; });
    H("WakeAllConditionVariable", [this](GuestCpu& c) { ++cv_gen_[c.arg(0)]; });
    // phase 0: release the lock and remember the wake generation; phase 1: wait for a wake or the timeout; phase 2: re-acquire the lock.
    auto cv_sleep = [this, cs_try, srw_try](GuestCpu& c, bool srw, std::uint64_t cv, std::uint64_t lock, std::uint64_t ms, bool shared) {
        auto release = [&]() -> std::uint64_t {
            if (srw) { auto& s = srw_[lock]; if (shared) { if (s.shared) --s.shared; } else s.excl = 0; return 1; }
            const auto rec = static_cast<std::uint32_t>(mem_.read<std::int32_t>(lock + 12));
            mem_.write<std::int32_t>(lock + 12, 0); mem_.write<std::uint64_t>(lock + 16, 0); mem_.write<std::int32_t>(lock + 8, -1); return rec; };
        auto reacquire = [&](std::uint64_t rec) -> bool {
            if (srw) return srw_try(lock, shared);
            if (static_cast<std::uint32_t>(mem_.read<std::uint64_t>(lock + 16)) != 0) return false;
            mem_.write<std::uint64_t>(lock + 16, current_tid()); mem_.write<std::int32_t>(lock + 12, static_cast<std::int32_t>(rec)); mem_.write<std::int32_t>(lock + 8, static_cast<std::int32_t>(rec) - 1); return true; };
        (void)cs_try;
        if (cur_thread_) {
            GuestThread& t = *cur_thread_;
            const bool fresh = !(t.waiting && t.wait_rsp == c.r[4] && t.phase != 0);
            if (fresh) { t.waiting = true; t.wait_rsp = c.r[4]; t.aux = cv_gen_[cv]; t.aux2 = release(); t.phase = 1; t.timed_out = false;
                         t.deadline = ms == 0xFFFFFFFFULL ? ~0ULL : qpc_ + ms * 10000ULL; }
            if (t.phase == 1) { if (cv_gen_[cv] != t.aux) t.phase = 2; else if (qpc_ >= t.deadline) { t.phase = 2; t.timed_out = true; } }
            if (t.phase == 2 && reacquire(t.aux2)) { t.waiting = false; t.phase = 0; if (t.timed_out) { last_error_ = 1460; c.r[0] = 0; } else c.r[0] = 1; return; }
            hle_retry_ = true; pending_yield_ = true; return;
        }
        const std::uint64_t gen = cv_gen_[cv]; const std::uint64_t rec = release();
        const std::uint64_t deadline = ms == 0xFFFFFFFFULL ? ~0ULL : qpc_ + ms * 10000ULL;
        bool woken = false;
        for (unsigned pass = 0; pass < 300 && c.stop == GuestCpu::Stop::None; ++pass) {
            run_threads((std::min<std::uint64_t>)(thread_slice_, 300000));
            if (cv_gen_[cv] != gen) { woken = true; break; }
            if (qpc_ >= deadline) break;
            qpc_ += 20000;
        }
        if (!woken && ms == 0xFFFFFFFFULL && !thread_deadlock_logged_) { thread_deadlock_logged_ = true; milestone("the main thread waited on a condition variable that nobody signalled (deadlock suspected); it was released as a spurious wake-up"); woken = true; }
        for (unsigned pass = 0; pass < 300 && c.stop == GuestCpu::Stop::None && !reacquire(rec); ++pass) { run_threads((std::min<std::uint64_t>)(thread_slice_, 300000)); qpc_ += 20000; }
        if (woken) c.r[0] = 1; else { last_error_ = 1460; c.r[0] = 0; } };
    H("SleepConditionVariableCS", [cv_sleep](GuestCpu& c) { cv_sleep(c, false, c.arg(0), c.arg(1), c.arg(2) & 0xFFFFFFFFULL, false); });
    H("SleepConditionVariableSRW", [cv_sleep](GuestCpu& c) { cv_sleep(c, true, c.arg(0), c.arg(1), c.arg(2) & 0xFFFFFFFFULL, (c.arg(3) & 1) != 0); });

    // ---- WaitOnAddress / WakeByAddress (polling: a woken thread simply re-evaluates the comparison) ----
    H("WaitOnAddress", [this](GuestCpu& c) { // (address, compare*, size, ms)
        const auto a = c.arg(0), cmp = c.arg(1), size = c.arg(2);
        auto differs = [&]() { std::uint64_t x = 0, y = 0; for (std::uint64_t i = 0; i < size && i < 8; ++i) { x |= static_cast<std::uint64_t>(mem_.read<std::uint8_t>(a + i)) << (8 * i); y |= static_cast<std::uint64_t>(mem_.read<std::uint8_t>(cmp + i)) << (8 * i); } return x != y; };
        const auto r = wait_for(c, differs, c.arg(3) & 0xFFFFFFFFULL);
        if (r != WaitResult::Retry) { if (r == WaitResult::Ready) c.r[0] = 1; else { last_error_ = 1460; c.r[0] = 0; } } });
    H("WakeByAddressSingle", [](GuestCpu&) {}); H("WakeByAddressAll", [](GuestCpu&) {});
    (void)ok;
}

} // namespace someboxone
