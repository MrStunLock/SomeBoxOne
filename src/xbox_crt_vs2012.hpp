#pragma once
// SomeBoxOne v0.32.0 - VS2012 (MSVCR110) CRT gaps + Windows.Xbox.Media.GameTransportControls.
//
// Minecraft: Xbox One Edition 1.2.0.0 is built with Visual Studio 2012. Its real msvcp110.dll imports internal CRT
// allocation/locking helpers (_malloc_crt, _calloc_crt, _lock, ...) from msvcr110.dll. Those used to be unimplemented
// stubs that returned 0, so DllMain(msvcp110.dll) failed and the title later hit E_NOTIMPL from
// GameTransportControls (which C++/CX turns into a C++ exception SomeBoxOne cannot unwind).
//
// This file only adds handlers; it does not change any existing handler.

namespace someboxone {

inline void XboxTitleRuntime::register_crt_vs2012_handlers() {
    // ---- CRT-internal allocation / locking (used by msvcp110.dll's DllMain and static initialisers) ----
    H("_malloc_crt", [this](GuestCpu& c) { c.r[0] = heap_alloc(c.arg(0), false); });
    H("_calloc_crt", [this](GuestCpu& c) { c.r[0] = heap_alloc(c.arg(0) * c.arg(1), true); });
    H("_realloc_crt", [this](GuestCpu& c) { c.r[0] = heap_realloc(c.arg(0), c.arg(1)); });
    H("_lock", [](GuestCpu&) {});     // single guest thread: locks are never contended
    H("_unlock", [](GuestCpu&) {});
    H("__crtSleep", [this](GuestCpu& c) { qpc_ += c.arg(0) * 10000; });
    H("__dllonexit", [](GuestCpu& c) { c.r[0] = c.arg(0); });
    H("_XcptFilter", [](GuestCpu& c) { c.r[0] = 0; });       // EXCEPTION_CONTINUE_SEARCH
    H("__CppXcptFilter", [](GuestCpu& c) { c.r[0] = 0; });
    H("_amsg_exit", [this](GuestCpu& c) {
        const std::string m = "CRT _amsg_exit(" + std::to_string(c.arg(0) & 0xFFFFFFFFULL) + ") called from " + locate(mem_.read<std::uint64_t>(c.r[4]));
        milestone(m); c.request_stop(GuestCpu::Stop::HostRequest, m); });
    H("?terminate@@YAXXZ", [this](GuestCpu& c) {
        const std::string m = "std::terminate() called from " + locate(mem_.read<std::uint64_t>(c.r[4]));
        milestone(m); c.request_stop(GuestCpu::Stop::HostRequest, m); });
    H("_wassert", [this](GuestCpu& c) {
        const std::string m = "assertion failed: " + utf16_to_utf8(mem_.read_wstring(c.arg(0))) + " (" + utf16_to_utf8(mem_.read_wstring(c.arg(1))) + ":" + std::to_string(c.arg(2) & 0xFFFFFFFFULL) + ")";
        milestone(m); c.request_stop(GuestCpu::Stop::HostRequest, m); });
    H("__iob_func", [this](GuestCpu& c) {
        if (!crt_iob_) crt_iob_ = static_alloc(3 * 0x30, 16);   // stdin/stdout/stderr FILE records (opaque to the title)
        c.r[0] = crt_iob_; });
    H("__clean_type_info_names_internal", [](GuestCpu&) {});
    H("?_type_info_dtor_internal_method@type_info@@QEAAXXZ", [](GuestCpu&) {});
    auto type_name_equal = [this](GuestCpu& c) { return mem_.read_cstring(c.arg(0) + 16) == mem_.read_cstring(c.arg(1) + 16); };
    H("??8type_info@@QEBA_NAEBV0@@Z", [type_name_equal](GuestCpu& c) { c.r[0] = type_name_equal(c) ? 1 : 0; });
    H("??9type_info@@QEBA_NAEBV0@@Z", [type_name_equal](GuestCpu& c) { c.r[0] = type_name_equal(c) ? 0 : 1; });
    H("__RTtypeid", [](GuestCpu& c) { c.r[0] = 0; });
    H("__RTDynamicCast", [](GuestCpu& c) { c.r[0] = 0; });    // dynamic_cast fails (no RTTI walk yet)

    // ---- strings ----
    H("strlen", [this](GuestCpu& c) { std::uint64_t n = 0; const auto a = c.arg(0); while (mem_.read<std::uint8_t>(a + n)) ++n; c.r[0] = n; });
    H("strcmp", [this](GuestCpu& c) {
        const auto a = c.arg(0), b = c.arg(1); int r = 0;
        for (std::uint64_t i = 0; !r; ++i) { const int x = mem_.read<std::uint8_t>(a + i), y = mem_.read<std::uint8_t>(b + i); r = x - y; if (!x) break; }
        c.r[0] = static_cast<std::uint32_t>(r < 0 ? -1 : (r > 0 ? 1 : 0)); });
    H("strncmp", [this](GuestCpu& c) {
        const auto a = c.arg(0), b = c.arg(1), n = c.arg(2); int r = 0;
        for (std::uint64_t i = 0; i < n && !r; ++i) { const int x = mem_.read<std::uint8_t>(a + i), y = mem_.read<std::uint8_t>(b + i); r = x - y; if (!x) break; }
        c.r[0] = static_cast<std::uint32_t>(r < 0 ? -1 : (r > 0 ? 1 : 0)); });
    H("wcsncmp", [this](GuestCpu& c) {
        const auto a = c.arg(0), b = c.arg(1), n = c.arg(2); int r = 0;
        for (std::uint64_t i = 0; i < n && !r; ++i) { const int x = mem_.read<std::uint16_t>(a + i * 2), y = mem_.read<std::uint16_t>(b + i * 2); r = x - y; if (!x) break; }
        c.r[0] = static_cast<std::uint32_t>(r < 0 ? -1 : (r > 0 ? 1 : 0)); });
    H("strchr", [this](GuestCpu& c) {
        const auto s = c.arg(0); const auto ch = static_cast<std::uint8_t>(c.arg(1)); c.r[0] = 0;
        for (std::uint64_t i = 0;; ++i) { const auto x = mem_.read<std::uint8_t>(s + i); if (x == ch) { c.r[0] = s + i; return; } if (!x) return; } });
    H("strrchr", [this](GuestCpu& c) {
        const auto s = c.arg(0); const auto ch = static_cast<std::uint8_t>(c.arg(1)); std::uint64_t last = 0;
        for (std::uint64_t i = 0;; ++i) { const auto x = mem_.read<std::uint8_t>(s + i); if (x == ch) last = s + i; if (!x) break; }
        c.r[0] = last; });
    H("strstr", [this](GuestCpu& c) {
        const std::string h = mem_.read_cstring(c.arg(0)), n = mem_.read_cstring(c.arg(1)); const auto p = h.find(n);
        c.r[0] = p == std::string::npos ? 0 : c.arg(0) + p; });
    H("strerror", [this](GuestCpu& c) { if (!crt_strerror_) { crt_strerror_ = static_alloc(32); mem_.write_bytes(crt_strerror_, "error", 6); } c.r[0] = crt_strerror_; });
    H("atof", [this](GuestCpu& c) { setd(c, std::strtod(mem_.read_cstring(c.arg(0)).c_str(), nullptr)); });
    H("isalnum", [](GuestCpu& c) { const int ch = static_cast<int>(c.arg(0) & 0xFFFFFFFFULL); c.r[0] = (ch < 128 && std::isalnum(ch)) ? 1 : 0; });
    H("towupper", [](GuestCpu& c) { const auto ch = static_cast<std::uint16_t>(c.arg(0)); c.r[0] = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch; });
    H("towlower", [](GuestCpu& c) { const auto ch = static_cast<std::uint16_t>(c.arg(0)); c.r[0] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; });
    H("memcpy_s", [this](GuestCpu& c) { // (dst, dstsize, src, count)
        const auto dst = c.arg(0), dsz = c.arg(1), src = c.arg(2), n = c.arg(3);
        if (!n) { c.r[0] = 0; return; }
        if (!dst || !src || n > dsz || n > (16ULL << 20)) { c.r[0] = 22; return; }
        copy_guest(dst, src, n); c.r[0] = 0; });
    H("wcsncpy", [this](GuestCpu& c) { // (dst, src, n)
        const auto d = c.arg(0), s = c.arg(1), n = c.arg(2); bool end = false;
        for (std::uint64_t i = 0; i < n; ++i) { std::uint16_t ch = 0; if (!end) { ch = mem_.read<std::uint16_t>(s + i * 2); if (!ch) end = true; } mem_.write<std::uint16_t>(d + i * 2, ch); }
        c.r[0] = d; });
    H("wcsncpy_s", [this](GuestCpu& c) { // (dst, dstsize, src, count)
        const auto d = c.arg(0), dsz = c.arg(1), s = c.arg(2); auto n = c.arg(3);
        if (!d || !dsz) { c.r[0] = 22; return; }
        std::u16string w = mem_.read_wstring(s); if (n != ~0ULL && w.size() > n) w.resize(static_cast<std::size_t>(n));
        if (w.size() + 1 > dsz) { mem_.write<std::uint16_t>(d, 0); c.r[0] = 34; return; }
        write_wstr(d, w); c.r[0] = 0; });
    H("wcstombs", [this](GuestCpu& c) { // (dst, src, max): narrow with UTF-8
        const std::string s = utf16_to_utf8(mem_.read_wstring(c.arg(1)));
        if (!c.arg(0)) { c.r[0] = s.size(); return; }
        const std::size_t n = static_cast<std::size_t>((std::min<std::uint64_t>)(s.size(), c.arg(2)));
        mem_.write_bytes(c.arg(0), s.data(), n); if (n < c.arg(2)) mem_.write<std::uint8_t>(c.arg(0) + n, 0); c.r[0] = n; });
    H("_itoa_s", [this](GuestCpu& c) { // (int value, char* buf, size_t size, int radix)
        const auto v = static_cast<std::int32_t>(c.arg(0)); const unsigned radix = static_cast<unsigned>(c.arg(3));
        if (radix < 2 || radix > 36) { c.r[0] = 22; return; }
        std::string s; std::uint32_t u = (radix == 10 && v < 0) ? static_cast<std::uint32_t>(-static_cast<std::int64_t>(v)) : static_cast<std::uint32_t>(v);
        if (!u) s = "0"; while (u) { s.insert(s.begin(), "0123456789abcdefghijklmnopqrstuvwxyz"[u % radix]); u /= radix; }
        if (radix == 10 && v < 0) s.insert(s.begin(), '-');
        if (s.size() + 1 > c.arg(2)) { c.r[0] = 34; return; }
        mem_.write_bytes(c.arg(1), s.c_str(), s.size() + 1); c.r[0] = 0; });
    H("_itow", [this](GuestCpu& c) { // (int value, wchar_t* buf, int radix)
        const auto v = static_cast<std::int32_t>(c.arg(0)); const unsigned radix = static_cast<unsigned>(c.arg(2));
        std::string s; std::uint32_t u = (radix == 10 && v < 0) ? static_cast<std::uint32_t>(-static_cast<std::int64_t>(v)) : static_cast<std::uint32_t>(v);
        if (radix < 2 || radix > 36) { c.r[0] = 0; return; }
        if (!u) s = "0"; while (u) { s.insert(s.begin(), "0123456789abcdefghijklmnopqrstuvwxyz"[u % radix]); u /= radix; }
        if (radix == 10 && v < 0) s.insert(s.begin(), '-');
        write_wstr(c.arg(1), utf8_to_utf16(s)); c.r[0] = c.arg(1); });

    // ---- math (x64: first double/float argument in XMM0, second in XMM1) ----
    H("cos", [](GuestCpu& c) { setd(c, std::cos(getd(c))); });
    H("tan", [](GuestCpu& c) { setd(c, std::tan(getd(c))); });
    H("atan", [](GuestCpu& c) { setd(c, std::atan(getd(c))); });
    H("acos", [](GuestCpu& c) { setd(c, std::acos(getd(c))); });
    H("asin", [](GuestCpu& c) { setd(c, std::asin(getd(c))); });
    H("atan2", [](GuestCpu& c) { setd(c, std::atan2(getd(c, 0), getd(c, 1))); });
    H("ceil", [](GuestCpu& c) { setd(c, std::ceil(getd(c))); });
    H("floor", [](GuestCpu& c) { setd(c, std::floor(getd(c))); });
    H("fabs", [](GuestCpu& c) { setd(c, std::fabs(getd(c))); });
    H("fmod", [](GuestCpu& c) { setd(c, std::fmod(getd(c, 0), getd(c, 1))); });
    H("modf", [this](GuestCpu& c) { double ip = 0; const double f = std::modf(getd(c), &ip); mem_.write<double>(c.arg(1), ip); setd(c, f); });
    H("tanf", [](GuestCpu& c) { setf(c, std::tan(getf(c))); });
    H("atanf", [](GuestCpu& c) { setf(c, std::atan(getf(c))); });
    H("fmodf", [](GuestCpu& c) { setf(c, std::fmod(getf(c, 0), getf(c, 1))); });
    H("log10f", [](GuestCpu& c) { setf(c, std::log10(getf(c))); });

    // ---- printf family (VS2012 exports real functions instead of UCRT's __stdio_common_*) ----
    // Win64 varargs: the callee sees the extra arguments in RCX/RDX/R8/R9 and on the stack. We copy them into a
    // scratch va_list in guest memory so the existing guest_format() engine can walk them.
    auto va_from = [this](GuestCpu& c, unsigned first) -> std::uint64_t {
        if (!crt_va_scratch_) crt_va_scratch_ = static_alloc(16 * 8, 16);
        for (unsigned i = 0; i < 16; ++i) { std::uint64_t v = 0; try { v = c.arg(first + i); } catch (...) {} mem_.write<std::uint64_t>(crt_va_scratch_ + i * 8ULL, v); }
        return crt_va_scratch_;
    };
    auto put_narrow = [this](GuestCpu& c, std::uint64_t buf, std::uint64_t cap, const std::string& s, bool truncate_ok) {
        if (buf && cap) {
            const std::size_t n = static_cast<std::size_t>((std::min<std::uint64_t>)(s.size(), cap - (truncate_ok ? 0 : 1)));
            mem_.write_bytes(buf, s.data(), n); if (n < cap) mem_.write<std::uint8_t>(buf + n, 0);
        }
        c.r[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(s.size())) & 0xFFFFFFFFULL;
    };
    auto put_wide = [this](GuestCpu& c, std::uint64_t buf, std::uint64_t cap, const std::string& s) {
        const std::u16string w = utf8_to_utf16(s);
        if (buf && cap) { const std::size_t n = static_cast<std::size_t>((std::min<std::uint64_t>)(w.size(), cap - 1)); for (std::size_t i = 0; i < n; ++i) mem_.write<std::uint16_t>(buf + i * 2, w[i]); mem_.write<std::uint16_t>(buf + n * 2, 0); }
        c.r[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(w.size())) & 0xFFFFFFFFULL;
    };
    H("sprintf_s", [this, va_from, put_narrow](GuestCpu& c) { put_narrow(c, c.arg(0), c.arg(1), guest_format(mem_.read_cstring(c.arg(2)), va_from(c, 3), false), false); });
    H("sprintf", [this, va_from, put_narrow](GuestCpu& c) { put_narrow(c, c.arg(0), 1ULL << 20, guest_format(mem_.read_cstring(c.arg(1)), va_from(c, 2), false), false); });
    H("_snprintf", [this, va_from, put_narrow](GuestCpu& c) { put_narrow(c, c.arg(0), c.arg(1), guest_format(mem_.read_cstring(c.arg(2)), va_from(c, 3), false), true); });
    H("swprintf_s", [this, va_from, put_wide](GuestCpu& c) { put_wide(c, c.arg(0), c.arg(1), guest_format(utf16_to_utf8(mem_.read_wstring(c.arg(2))), va_from(c, 3), true)); });
    H("vsprintf", [this, put_narrow](GuestCpu& c) { put_narrow(c, c.arg(0), 1ULL << 20, guest_format(mem_.read_cstring(c.arg(1)), c.arg(2), false), false); });
    H("_vsnprintf", [this, put_narrow](GuestCpu& c) { put_narrow(c, c.arg(0), c.arg(1), guest_format(mem_.read_cstring(c.arg(2)), c.arg(3), false), true); });
    H("vswprintf_s", [this, put_wide](GuestCpu& c) { put_wide(c, c.arg(0), c.arg(1), guest_format(utf16_to_utf8(mem_.read_wstring(c.arg(2))), c.arg(3), true)); });
    H("_vswprintf", [this, put_wide](GuestCpu& c) { put_wide(c, c.arg(0), 1ULL << 20, guest_format(utf16_to_utf8(mem_.read_wstring(c.arg(1))), c.arg(2), true)); });
    H("_vswprintf_c_l", [this, put_wide](GuestCpu& c) { put_wide(c, c.arg(0), c.arg(1), guest_format(utf16_to_utf8(mem_.read_wstring(c.arg(2))), c.arg(4), true)); });
    H("_vsnwprintf_s", [this, put_wide](GuestCpu& c) { put_wide(c, c.arg(0), (std::min)(c.arg(1), c.arg(2) + 1), guest_format(utf16_to_utf8(mem_.read_wstring(c.arg(3))), c.arg(4), true)); });
    H("printf", [this, va_from](GuestCpu& c) {
        const std::string s = guest_format(mem_.read_cstring(c.arg(0)), va_from(c, 1), false);
        if (stdout_log_.size() < 200) stdout_log_.push_back(s);
        c.r[0] = s.size(); });
    H("fprintf", [this, va_from](GuestCpu& c) {
        const std::string s = guest_format(mem_.read_cstring(c.arg(1)), va_from(c, 2), false);
        std::FILE* fp = file_of(c.arg(0));
        if (fp) std::fwrite(s.data(), 1, s.size(), fp); else if (stdout_log_.size() < 200) stdout_log_.push_back(s);
        c.r[0] = s.size(); });
    H("sscanf_s", [](GuestCpu& c) { c.r[0] = 0xFFFFFFFFULL; });   // EOF: no scanf engine yet
    H("swscanf_s", [](GuestCpu& c) { c.r[0] = 0xFFFFFFFFULL; });
    H("swscanf", [](GuestCpu& c) { c.r[0] = 0xFFFFFFFFULL; });

    // ---- std::exception / std::bad_cast (C++ exception object layout: vptr, const char* what, int owns) ----
    auto make_exc_vtable = [this]() {
        if (crt_exc_vtable_) return crt_exc_vtable_;
        crt_exc_vtable_ = static_alloc(2 * 8, 16);
        handlers_["std::exception::vdtor"] = [](GuestCpu& c) { c.r[0] = c.arg(0); };
        handlers_["std::exception::what"] = [this](GuestCpu& c) {
            const std::uint64_t w = c.arg(0) ? mem_.read<std::uint64_t>(c.arg(0) + 8) : 0;
            if (w) { c.r[0] = w; return; }
            if (!crt_unknown_exc_) { crt_unknown_exc_ = static_alloc(32); mem_.write_bytes(crt_unknown_exc_, "Unknown exception", 18); }
            c.r[0] = crt_unknown_exc_; };
        mem_.write<std::uint64_t>(crt_exc_vtable_, trap_for_name("std::exception::vdtor"));
        mem_.write<std::uint64_t>(crt_exc_vtable_ + 8, trap_for_name("std::exception::what"));
        return crt_exc_vtable_;
    };
    auto exc_init = [this, make_exc_vtable](GuestCpu& c, std::uint64_t what) {
        const std::uint64_t self = c.arg(0); mem_.write<std::uint64_t>(self, make_exc_vtable());
        mem_.write<std::uint64_t>(self + 8, what); mem_.write<std::uint64_t>(self + 16, 0); c.r[0] = self; };
    for (const char* n : {"??0exception@std@@QEAA@XZ", "??0bad_cast@std@@QEAA@PEBD@Z", "??0invalid_operation@Concurrency@@QEAA@XZ",
                          "??0task_canceled@Concurrency@@QEAA@XZ", "??0bad_target@Concurrency@@QEAA@XZ", "??0message_not_found@Concurrency@@QEAA@XZ",
                          "??0operation_timed_out@Concurrency@@QEAA@XZ"})
        H(n, [exc_init](GuestCpu& c) { exc_init(c, 0); });
    for (const char* n : {"??0exception@std@@QEAA@AEBQEBD@Z", "??0exception@std@@QEAA@AEBQEBDH@Z"})   // (this, const char* const& what[, int])
        H(n, [this, exc_init](GuestCpu& c) { exc_init(c, c.arg(1) ? mem_.read<std::uint64_t>(c.arg(1)) : 0); });
    for (const char* n : {"??0invalid_operation@Concurrency@@QEAA@PEBD@Z", "??0invalid_link_target@Concurrency@@QEAA@PEBD@Z"})
        H(n, [exc_init](GuestCpu& c) { exc_init(c, c.arg(1)); });
    for (const char* n : {"??0exception@std@@QEAA@AEBV01@@Z", "??0bad_cast@std@@QEAA@AEBV01@@Z"})
        H(n, [this, make_exc_vtable](GuestCpu& c) { const auto self = c.arg(0), o = c.arg(1);
            mem_.write<std::uint64_t>(self, make_exc_vtable()); mem_.write<std::uint64_t>(self + 8, o ? mem_.read<std::uint64_t>(o + 8) : 0);
            mem_.write<std::uint64_t>(self + 16, 0); c.r[0] = self; });
    for (const char* n : {"??1exception@std@@UEAA@XZ", "??1bad_cast@std@@UEAA@XZ"}) H(n, [](GuestCpu&) {});
    H("?what@exception@std@@UEBAPEBDXZ", [this](GuestCpu& c) { handlers_["std::exception::what"](c); });
    H("?__ExceptionPtrCreate@@YAXPEAX@Z", [this](GuestCpu& c) { if (c.arg(0)) mem_.write<std::uint64_t>(c.arg(0), 0); });
    H("?__ExceptionPtrDestroy@@YAXPEAX@Z", [](GuestCpu&) {});
    H("?__ExceptionPtrCopy@@YAXPEAXPEBX@Z", [this](GuestCpu& c) { if (c.arg(0)) mem_.write<std::uint64_t>(c.arg(0), c.arg(1) ? mem_.read<std::uint64_t>(c.arg(1)) : 0); });
    H("?__ExceptionPtrCurrentException@@YAXPEAX@Z", [this](GuestCpu& c) { if (c.arg(0)) mem_.write<std::uint64_t>(c.arg(0), 0); });

    // ---- Concurrency runtime primitives (single guest thread: nothing can contend) ----
    for (const char* n : {"??0critical_section@Concurrency@@QEAA@XZ", "??0event@Concurrency@@QEAA@XZ", "??0_SpinLock@details@Concurrency@@QEAA@AECJ@Z",
                          "??0_NonReentrantPPLLock@details@Concurrency@@QEAA@XZ", "??0_ReentrantPPLLock@details@Concurrency@@QEAA@XZ",
                          "??0_Condition_variable@details@Concurrency@@QEAA@XZ", "??0scoped_lock@critical_section@Concurrency@@QEAA@AEAV12@@Z",
                          "??0_Scoped_lock@_NonReentrantPPLLock@details@Concurrency@@QEAA@AEAV123@@Z", "??0_Scoped_lock@_ReentrantPPLLock@details@Concurrency@@QEAA@AEAV123@@Z"})
        H(n, [this](GuestCpu& c) { if (c.arg(0)) { mem_.write<std::uint64_t>(c.arg(0), 0); } c.r[0] = c.arg(0); });
    for (const char* n : {"??1critical_section@Concurrency@@QEAA@XZ", "??1event@Concurrency@@QEAA@XZ", "??1_SpinLock@details@Concurrency@@QEAA@XZ",
                          "??1_Condition_variable@details@Concurrency@@QEAA@XZ", "??1scoped_lock@critical_section@Concurrency@@QEAA@XZ",
                          "??1_Scoped_lock@_NonReentrantPPLLock@details@Concurrency@@QEAA@XZ", "??1_Scoped_lock@_ReentrantPPLLock@details@Concurrency@@QEAA@XZ",
                          "?lock@critical_section@Concurrency@@QEAAXXZ", "?unlock@critical_section@Concurrency@@QEAAXXZ",
                          "?notify_all@_Condition_variable@details@Concurrency@@QEAAXXZ", "?notify_one@_Condition_variable@details@Concurrency@@QEAAXXZ",
                          "?_UnderlyingYield@details@Concurrency@@YAXXZ", "?_Yield@_Context@details@Concurrency@@SAXXZ",
                          "?_ReportUnobservedException@details@Concurrency@@YAXXZ"})
        H(n, [](GuestCpu&) {});
    H("?try_lock@critical_section@Concurrency@@QEAA_NXZ", [](GuestCpu& c) { c.r[0] = 1; });
    H("?try_lock_for@critical_section@Concurrency@@QEAA_NI@Z", [](GuestCpu& c) { c.r[0] = 1; });
    H("?_SpinOnce@?$_SpinWait@$00@details@Concurrency@@QEAA_NXZ", [](GuestCpu& c) { c.r[0] = 1; });
    H("?set@event@Concurrency@@QEAAXXZ", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(0), 1); });
    H("?wait@event@Concurrency@@QEAA_KI@Z", [](GuestCpu& c) { c.r[0] = 0; });   // signalled (a single thread cannot block)
    H("?wait@_Condition_variable@details@Concurrency@@QEAAXAEAVcritical_section@3@@Z", [](GuestCpu&) {});
    H("?wait_for@_Condition_variable@details@Concurrency@@QEAA_NAEAVcritical_section@3@I@Z", [](GuestCpu& c) { c.r[0] = 0; });
    H("?Alloc@Concurrency@@YAPEAX_K@Z", [this](GuestCpu& c) { c.r[0] = heap_alloc(c.arg(0), false); });
    H("?Free@Concurrency@@YAXPEAX@Z", [this](GuestCpu& c) { heap_free(c.arg(0)); });
    H("?GetNumberOfVirtualProcessors@CurrentScheduler@Concurrency@@SAIXZ", [](GuestCpu& c) { c.r[0] = 1; });
    H("?IsCurrentTaskCollectionCanceling@Context@Concurrency@@SA_NXZ", [](GuestCpu& c) { c.r[0] = 0; });
    H("?Log2@details@Concurrency@@YAK_K@Z", [](GuestCpu& c) { std::uint64_t v = c.arg(0); unsigned r = 0; while (v >>= 1) ++r; c.r[0] = r; });

    // ---- time ----
    H("_gmtime64", [this](GuestCpu& c) { // (const __time64_t*) -> struct tm* (9 ints)
        if (!crt_tm_) crt_tm_ = static_alloc(64, 16);
        const std::time_t t = static_cast<std::time_t>(static_cast<std::int64_t>(mem_.read<std::uint64_t>(c.arg(0))));
        std::tm tmv{}; 
#ifdef _WIN32
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        const std::int32_t f[9] = {tmv.tm_sec, tmv.tm_min, tmv.tm_hour, tmv.tm_mday, tmv.tm_mon, tmv.tm_year, tmv.tm_wday, tmv.tm_yday, tmv.tm_isdst};
        for (int i = 0; i < 9; ++i) mem_.write<std::int32_t>(crt_tm_ + static_cast<std::uint64_t>(i) * 4, f[i]);
        c.r[0] = crt_tm_; });

    // ---- Xbox-specific kernelx/combase gaps seen in the Minecraft 1.2 import list ----
    H("ExitProcess", [this](GuestCpu& c) { exited_ = true; exit_code_ = static_cast<int>(c.arg(0)); milestone("ExitProcess(" + std::to_string(exit_code_) + ") called from " + locate(mem_.read<std::uint64_t>(c.r[4])));
        c.request_stop(GuestCpu::Stop::HostRequest, "the title called ExitProcess(" + std::to_string(exit_code_) + ")"); });
    H("CreateEventA", [this](GuestCpu& c) { c.r[0] = handlers_.count("CreateEventW") ? (handlers_["CreateEventW"](c), c.r[0]) : 0x7100; });
    H("CreateMutexA", [this](GuestCpu& c) { c.r[0] = handlers_.count("CreateMutexW") ? (handlers_["CreateMutexW"](c), c.r[0]) : 0x7200; });
    H("GetThreadPriority", [](GuestCpu& c) { c.r[0] = 0; });
    H("GetUserGeoID", [](GuestCpu& c) { c.r[0] = 0xF4; });   // GEOID_NATION: United States
    H("SetThreadIdealProcessorEx", [](GuestCpu& c) { c.r[0] = 1; });
    H("SignalObjectAndWait", [](GuestCpu& c) { c.r[0] = 0; });
    H("WaitForMultipleObjects", [](GuestCpu& c) { c.r[0] = 0; });   // WAIT_OBJECT_0
    H("XMemAllocDefault", [this](GuestCpu& c) { c.r[0] = heap_alloc(c.arg(0), false); });
    H("XMemFreeDefault", [this](GuestCpu& c) { heap_free(c.arg(0)); });
    H("SysStringLen", [this](GuestCpu& c) { std::uint64_t n = 0; const auto a = c.arg(0); if (a) while (mem_.read<std::uint16_t>(a + n * 2)) ++n; c.r[0] = n; });
    H("EtxUnregister", [](GuestCpu& c) { c.r[0] = 0; });

    // ---- file attributes: titles probe optional assets with these; returning 0 ("exists") for everything made Minecraft open files that are not in the package ----
    auto file_attributes = [this](const std::string& path) -> std::uint32_t {
        std::error_code ec; const auto host = resolve_path(path, false);
        if (std::filesystem::is_directory(host, ec)) { last_error_ = 0; return 0x10; }                    // FILE_ATTRIBUTE_DIRECTORY
        if (std::filesystem::exists(host, ec)) { last_error_ = 0; return 0x80; }                           // FILE_ATTRIBUTE_NORMAL
        last_error_ = std::filesystem::exists(host.parent_path(), ec) ? 2 : 3;                             // ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND
        return 0xFFFFFFFFU; };                                                                             // INVALID_FILE_ATTRIBUTES
    H("GetFileAttributesW", [this, file_attributes](GuestCpu& c) { c.r[0] = file_attributes(utf16_to_utf8(mem_.read_wstring(c.arg(0)))); });
    H("GetFileAttributesA", [this, file_attributes](GuestCpu& c) { c.r[0] = file_attributes(mem_.read_cstring(c.arg(0))); });
    H("_setjmp", [](GuestCpu& c) { c.r[0] = 0; });   // returns 0 on the direct path; longjmp is not supported yet

    // ---- xcompress.dll: XMemCompress / XMemDecompress (codec 1 = LZX; see lzx.hpp) ----
    // Contexts only remember the LZX window size. Each call starts a fresh LZX stream: titles compress and decompress whole,
    // independent buffers. Compression emits valid LZX made of stored blocks (no size reduction, readable by any LZX decoder).
    auto xmem_create = [this](GuestCpu& c) { // (codec, params*, flags, ctx**)
        unsigned window = 0x20000;
        if (c.arg(1) && mem_.is_mapped(c.arg(1), 8)) { const auto w = mem_.read<std::uint32_t>(c.arg(1) + 4); if (w >= 0x8000 && w <= 0x200000 && (w & (w - 1)) == 0) window = w; }
        const std::uint64_t id = 0x9000 + 0x10 * (xmem_contexts_.size() + 1);
        xmem_contexts_[id] = window;
        if (c.arg(3) && mem_.is_mapped(c.arg(3), 8)) mem_.write<std::uint64_t>(c.arg(3), id);
        c.r[0] = 0; };
    H("XMemCreateDecompressionContext", xmem_create);
    H("XMemCreateCompressionContext", xmem_create);
    for (const char* n : {"XMemDestroyDecompressionContext", "XMemDestroyCompressionContext", "XMemResetDecompressionContext", "XMemResetCompressionContext"})
        H(n, [](GuestCpu& c) { c.r[0] = 0; });
    H("XMemDecompress", [this](GuestCpu& c) { // (ctx, dst, uint32* dstLen, src, srcLen)
        const auto it = xmem_contexts_.find(c.arg(0));
        const unsigned window = it != xmem_contexts_.end() ? it->second : 0x20000;
        const std::uint64_t dst = c.arg(1), lenp = c.arg(2), src = c.arg(3), src_len = c.arg(4) & 0xFFFFFFFFULL;
        if (!dst || !lenp || !src || src_len > (256ULL << 20)) { c.r[0] = 0x80070057ULL; return; }
        const std::uint64_t cap = mem_.read<std::uint32_t>(lenp);
        std::vector<std::uint8_t> in(static_cast<std::size_t>(src_len)), out(static_cast<std::size_t>(cap));
        for (std::size_t k = 0; k < in.size(); ++k) in[k] = mem_.read<std::uint8_t>(src + k);
        unsigned bits = 15; while ((1u << bits) < window && bits < 21) ++bits;
        lzx::Decoder dec(bits);
        const long long n = lzx::decompress(dec, in.data(), in.size(), out.data(), out.size());
        if (n < 0) { milestone("XMemDecompress failed: corrupt or unsupported LZX data (" + std::to_string(src_len) + " bytes in, " + std::to_string(cap) + " expected)"); c.r[0] = 0x80004005ULL; return; }
        for (long long k = 0; k < n; ++k) mem_.write<std::uint8_t>(dst + static_cast<std::uint64_t>(k), out[static_cast<std::size_t>(k)]);
        mem_.write<std::uint32_t>(lenp, static_cast<std::uint32_t>(n)); ++xmem_calls_; c.r[0] = 0; });
    H("XMemCompress", [this](GuestCpu& c) { // (ctx, dst, uint32* dstLen, src, srcLen)
        const std::uint64_t dst = c.arg(1), lenp = c.arg(2), src = c.arg(3), src_len = c.arg(4) & 0xFFFFFFFFULL;
        if (!dst || !lenp || (!src && src_len) || src_len > (256ULL << 20)) { c.r[0] = 0x80070057ULL; return; }
        std::vector<std::uint8_t> in(static_cast<std::size_t>(src_len)); for (std::size_t k = 0; k < in.size(); ++k) in[k] = mem_.read<std::uint8_t>(src + k);
        const auto packed = lzx::compress_stored(in.data(), in.size());
        if (packed.size() > mem_.read<std::uint32_t>(lenp)) { c.r[0] = 0x8007007AULL; return; }   // ERROR_INSUFFICIENT_BUFFER
        for (std::size_t k = 0; k < packed.size(); ++k) mem_.write<std::uint8_t>(dst + k, packed[k]);
        mem_.write<std::uint32_t>(lenp, static_cast<std::uint32_t>(packed.size())); c.r[0] = 0; });

    // FindFirstFileW(name, WIN32_FIND_DATAW*) is FindFirstFileExW(name, level, data, ...) without the level argument. It was an
    // unimplemented stub returning 0, which a title reads as a valid search handle.
    H("FindFirstFileW", [this](GuestCpu& c) { c.r[8] = c.arg(1); handlers_["FindFirstFileExW"](c); });

    winrt_classes_["Windows.Xbox.Storage.ConnectedStorageSpace"] = [this]() { return make_permissive_object("Windows.Xbox.Storage.ConnectedStorageSpace"); };
    // GetFileInformationByHandleEx(handle, class, buffer, size): FileBasicInfo (0) and FileStandardInfo (1) - the title sizes its read buffers from EndOfFile.
    H("GetFileInformationByHandleEx", [this](GuestCpu& c) {
        const auto it = win_handles_.find(c.arg(0)); const std::uint64_t cls = c.arg(1) & 0xFFFFFFFFULL, buf = c.arg(2), size = c.arg(3) & 0xFFFFFFFFULL;
        if (it == win_handles_.end() || !buf) { last_error_ = 6; c.r[0] = 0; return; }
        std::uint64_t len = 0;
        if (it->second.fp) { const long cur = std::ftell(it->second.fp); std::fseek(it->second.fp, 0, SEEK_END); len = static_cast<std::uint64_t>(std::ftell(it->second.fp)); std::fseek(it->second.fp, cur, SEEK_SET); }
        if (cls == 1 && size >= 24 && mem_.is_mapped(buf, 24)) {          // FILE_STANDARD_INFO
            zero_fill(buf, 24); mem_.write<std::uint64_t>(buf, (len + 4095) & ~4095ULL); mem_.write<std::uint64_t>(buf + 8, len); mem_.write<std::uint32_t>(buf + 16, 1); c.r[0] = 1; return; }
        if (cls == 0 && size >= 40 && mem_.is_mapped(buf, 40)) {          // FILE_BASIC_INFO: four FILETIMEs + attributes
            zero_fill(buf, 40); mem_.write<std::uint32_t>(buf + 32, 0x80); c.r[0] = 1; return; }
        last_error_ = 87; c.r[0] = 0; });

    // ---- Windows.Xbox.Media.GameTransportControls ----
    // Minecraft activates this class, calls slot 6 on it (`HRESULT get_X(GameTransportControls** out)`: the title
    // stores *out and uses it) and then calls slot 14 (a bool setter, dl=1) and slot 25 (an event add taking
    // (handler, EventRegistrationToken*)) on the returned object. The Xbox shell's media transport (guide-button
    // play/pause/skip) does not exist in SomeBoxOne, so the instance accepts every call with a zero result and event
    // registrations are accepted but never fire. Returning E_NOTIMPL threw a C++/CX exception; returning null crashed.
    winrt_classes_["Windows.Xbox.Media.GameTransportControls"] = [this]() {
        const std::string cls = "Windows.Xbox.Media.GameTransportControls";
        auto placeholder = [this, cls](unsigned slot) { return [this, cls, slot](GuestCpu& c) {
            if (com_log_.insert("offline:" + cls + ":" + std::to_string(slot)).second)
                milestone("WinRT " + cls + " slot " + std::to_string(slot) + " served by offline placeholder (S_OK; no Xbox media transport available)");
            const std::uint64_t first = c.arg(1), second = c.arg(2);
            const std::uint64_t stack_lo = xrt::kStackTop - 0x400000ULL;
            if (slot == 25 || slot == 26 || slot == 27) {                    // add_<event>(handler, EventRegistrationToken*)
                if (second >= stack_lo && second < xrt::kStackTop && mem_.is_mapped(second, 8)) mem_.write<std::uint64_t>(second, 0x6000 + slot);
            } else if (first >= stack_lo && first < xrt::kStackTop && first != 1 && mem_.is_mapped(first, 8)) {
                mem_.write<std::uint64_t>(first, 0);                         // generic `get_X(T* out)`: zero result
            }
            c.r[0] = 0; }; };
        ComMethods inst;
        for (unsigned slot = 6; slot <= 40; ++slot) inst.emplace_back("slot" + std::to_string(slot), placeholder(slot));
        const std::uint64_t instance = make_com_object(cls + ".Instance", inst, {"*"}, true, 48);
        ComMethods m;
        for (unsigned slot = 6; slot <= 20; ++slot) {
            if (slot == 6) m.emplace_back("get_Instance", [this, cls, instance](GuestCpu& c) {
                if (com_log_.insert("offline:" + cls + ":6").second)
                    milestone("WinRT " + cls + " slot 6 returned an offline placeholder instance (no Xbox media transport available)");
                if (c.arg(1) && mem_.is_mapped(c.arg(1), 8)) { mem_.write<std::uint64_t>(c.arg(1), instance); mem_.write<std::uint64_t>(instance + 8, mem_.read<std::uint64_t>(instance + 8) + 1); }
                c.r[0] = 0; });
            else m.emplace_back("slot" + std::to_string(slot), placeholder(slot));
        }
        return make_com_object(cls, m, {"*"}, true, 40);
    };

    // ---- Windows.ApplicationModel.Store.CurrentApp (ICurrentApp, IID d52dc065-da3f-4685-995e-9b482eb5b603) ----
    // Slot 6 = get_LicenseInformation(ILicenseInformation**). The licence object reports an active, non-trial licence
    // (a purchased title). ILicenseInformation: 6 ProductLicenses, 7 IsActive, 8 IsTrial, 9 ExpirationDate, 10 add_LicenseChanged, 11 remove.
    winrt_classes_["Windows.ApplicationModel.Store.CurrentApp"] = [this]() {
        const std::string cls = "Windows.ApplicationModel.Store.CurrentApp";
        const std::uint64_t stack_lo = xrt::kStackTop - 0x400000ULL;
        auto on_stack = [this, stack_lo](std::uint64_t p, unsigned n) { return p >= stack_lo && p < xrt::kStackTop && mem_.is_mapped(p, n); };
        ComMethods lic;
        lic.emplace_back("get_ProductLicenses", [this, on_stack](GuestCpu& c) { if (on_stack(c.arg(1), 8)) mem_.write<std::uint64_t>(c.arg(1), 0); c.r[0] = 0; });
        lic.emplace_back("get_IsActive", [this, on_stack](GuestCpu& c) { if (on_stack(c.arg(1), 1)) mem_.write<std::uint8_t>(c.arg(1), 1); c.r[0] = 0; });
        lic.emplace_back("get_IsTrial", [this, on_stack](GuestCpu& c) { if (on_stack(c.arg(1), 1)) mem_.write<std::uint8_t>(c.arg(1), 0); c.r[0] = 0; });
        lic.emplace_back("get_ExpirationDate", [this, on_stack](GuestCpu& c) { if (on_stack(c.arg(1), 8)) mem_.write<std::uint64_t>(c.arg(1), 0x7FFFFFFFFFFFFFFFULL); c.r[0] = 0; });
        lic.emplace_back("add_LicenseChanged", [this, on_stack](GuestCpu& c) { if (on_stack(c.arg(2), 8)) mem_.write<std::uint64_t>(c.arg(2), 0x6100); c.r[0] = 0; });
        lic.emplace_back("remove_LicenseChanged", [](GuestCpu& c) { c.r[0] = 0; });
        const std::uint64_t license = make_com_object(cls + ".LicenseInformation", lic, {"*"}, true, 20);
        ComMethods m;
        m.emplace_back("get_LicenseInformation", [this, cls, on_stack, license](GuestCpu& c) {
            if (com_log_.insert("offline:" + cls + ":6").second)
                milestone("WinRT " + cls + " slot 6 (LicenseInformation) served with an active, non-trial offline licence");
            if (on_stack(c.arg(1), 8)) { mem_.write<std::uint64_t>(c.arg(1), license); mem_.write<std::uint64_t>(license + 8, mem_.read<std::uint64_t>(license + 8) + 1); }
            c.r[0] = 0; });
        return make_com_object(cls, m, {"*"}, true, 24);
    };

    // ---- Windows.Storage.Streams.Buffer (IBufferFactory, slot 6 = Create(uint32 capacity, IBuffer** out)) ----
    // IBuffer: 6 get_Capacity, 7 get_Length, 8 put_Length. The title obtains the raw pointer through IBufferByteAccess
    // (IID 905a0fef-bc53-11df-8c49-001e4fc686da; slot 3 = Buffer(byte**)), which is a different vtable on the same logical
    // object, so QueryInterface is routed to a second COM object through obj_qi_.
    winrt_classes_["Windows.Storage.Streams.Buffer"] = [this]() {
        const std::string cls = "Windows.Storage.Streams.Buffer";
        ComMethods m;
        m.emplace_back("Create", [this, cls](GuestCpu& c) {
            const std::uint64_t capacity = c.arg(1) & 0xFFFFFFFFULL, out = c.arg(2);
            if (capacity > (256ULL << 20) || !out || !mem_.is_mapped(out, 8)) { c.r[0] = 0x80070057ULL; return; }   // E_INVALIDARG
            const std::uint64_t data = heap_alloc(capacity ? capacity : 1, true);
            if (!data) { c.r[0] = 0x8007000EULL; return; }                                                          // E_OUTOFMEMORY
            const std::uint64_t state = static_alloc(16, 8);   // [0] = capacity, [8] = length
            mem_.write<std::uint64_t>(state, capacity); mem_.write<std::uint64_t>(state + 8, 0);
            ComMethods bm;
            bm.emplace_back("get_Capacity", [this, state](GuestCpu& c2) { mem_.write<std::uint32_t>(c2.arg(1), static_cast<std::uint32_t>(mem_.read<std::uint64_t>(state))); c2.r[0] = 0; });
            bm.emplace_back("get_Length", [this, state](GuestCpu& c2) { mem_.write<std::uint32_t>(c2.arg(1), static_cast<std::uint32_t>(mem_.read<std::uint64_t>(state + 8))); c2.r[0] = 0; });
            bm.emplace_back("put_Length", [this, state](GuestCpu& c2) {
                const std::uint64_t len = c2.arg(1) & 0xFFFFFFFFULL;
                if (len > mem_.read<std::uint64_t>(state)) { c2.r[0] = 0x8007000EULL; return; }                    // longer than the capacity
                mem_.write<std::uint64_t>(state + 8, len); c2.r[0] = 0; });
            const std::uint64_t buffer = make_com_object(cls + ".Instance", bm, {"*"}, true, 12);
            ComMethods am; // IBufferByteAccess is a plain IUnknown interface: slot 3 = Buffer(byte** value)
            am.emplace_back("Buffer", [this, data](GuestCpu& c2) { mem_.write<std::uint64_t>(c2.arg(1), data); c2.r[0] = 0; });
            const std::uint64_t byte_access = make_com_object(cls + ".ByteAccess", am, {}, false, 4);
            obj_qi_[buffer]["905a0fef-bc53-11df-8c49-001e4fc686da"] = byte_access;
            if (com_log_.insert("offline:" + cls + ":create").second) milestone("WinRT " + cls + ".Create(" + std::to_string(capacity) + ") -> guest-heap backed IBuffer (+ IBufferByteAccess)");
            mem_.write<std::uint64_t>(out, buffer);
            c.r[0] = 0; });
        return make_com_object(cls, m, {"*"}, true, 16);
    };
}


// Windows raises CoreApplicationView.Activated(IActivatedEventArgs{Kind=Launch, PreviousExecutionState=NotRunning}) when the
// application's event loop first runs. Titles such as Minecraft 1.2 start their game initialisation from that handler and
// otherwise idle in the main loop forever (no presents, no draws), so SomeBoxOne delivers it once.
inline void XboxTitleRuntime::deliver_launch_activation(GuestCpu& c) {
    if (activation_delivered_) return;
    activation_delivered_ = true;
    std::vector<std::uint64_t> handlers;
    for (const auto& e : event_subs_) if (e.source == "CoreApplicationView.Activated") handlers.push_back(e.handler);
    if (handlers.empty()) return;
    const std::uint64_t stack_lo = xrt::kStackTop - 0x400000ULL;
    ComMethods m;   // IActivatedEventArgs: 6 Kind (Launch = 0), 7 PreviousExecutionState (NotRunning = 0), 8 SplashScreen (none); further slots (ILaunchActivatedEventArgs ...) return empty results
    for (unsigned slot = 6; slot <= 16; ++slot)
        m.emplace_back("slot" + std::to_string(slot), [this, stack_lo](GuestCpu& c2) {
            const std::uint64_t out = c2.arg(1);
            if (out >= stack_lo && out < xrt::kStackTop && mem_.is_mapped(out, 4)) mem_.write<std::uint32_t>(out, 0);
            c2.r[0] = 0; });
    const std::uint64_t args = make_com_object("Windows.ApplicationModel.Activation.LaunchActivatedEventArgs", m, {"*"}, true, 20);
    const std::uint64_t view = make_core_application_view();
    for (const auto h : handlers) {
        milestone("delivering CoreApplicationView.Activated(Launch) to the title's handler (Invoke at " + handler_invoke_location(h) + ")");
        guest_vcall(c, h, 3, {view, args});
        if (c.stop != GuestCpu::Stop::None) return;
    }
    milestone("CoreApplicationView.Activated handler returned");
}


// ---- permissive Xbox services (offline, nothing persisted) ----
// A Windows.Xbox.* method whose layout is unknown succeeds. If one of its trailing arguments points at a zero-initialised slot on the
// caller's stack (C++/CX pre-zeroes `T** out` before every call), that slot receives another permissive object, so a title that walks
// an object graph (Connected Storage: space -> container -> blob) keeps running instead of dereferencing null.
inline void XboxTitleRuntime::permissive_result(GuestCpu& c, const std::string& cls) {
    const std::uint64_t lo = xrt::kStackTop - 0x400000ULL;
    std::uint64_t out = 0;
    for (const std::uint64_t cand : {c.arg(1), c.arg(2), c.arg(3)})
        if (cand >= lo && cand < xrt::kStackTop && (cand & 7) == 0 && mem_.is_mapped(cand, 8) && mem_.read<std::uint64_t>(cand) == 0) out = cand;
    if (out) mem_.write<std::uint64_t>(out, make_permissive_object(cls + ".Result"));
}

inline std::uint64_t XboxTitleRuntime::make_permissive_object(const std::string& cls) {
    ComMethods m;
    for (unsigned slot = 6; slot <= 45; ++slot)
        m.emplace_back("slot" + std::to_string(slot), [this, cls, slot](GuestCpu& c) {
            if (com_log_.insert("permissive:" + cls + ":" + std::to_string(slot)).second)
                milestone("WinRT " + cls + " slot " + std::to_string(slot) + " served as an offline no-op (called from " + locate(mem_.read<std::uint64_t>(c.r[4])) + ")");
            permissive_result(c, cls); c.r[0] = 0; });
    return make_com_object(cls, m, {"*"}, true, 48);
}

} // namespace someboxone
