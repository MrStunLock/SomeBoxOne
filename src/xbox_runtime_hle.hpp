#pragma once
// Included at the end of xbox_runtime.hpp. Implements the import handlers.

namespace someboxone {

inline void XboxTitleRuntime::register_handlers() {
    register_crt_handlers();
    register_kernel_handlers();
    register_cpp_handlers();
    register_extra_handlers();
    register_crt_vs2012_handlers();
    register_winrt_classes();
    register_audio_handlers();
    register_kernel_baseline();   // overrides older stubs of the same name
    register_thread_handlers();   // last: real guest threads replace the single-threaded synchronisation approximations
}

inline void XboxTitleRuntime::register_crt_handlers() {
    // ---- memory ----
    H("malloc", [this](GuestCpu& c) { c.r[0] = heap_alloc(c.arg(0), false); });
    H("calloc", [this](GuestCpu& c) { c.r[0] = heap_alloc(c.arg(0) * c.arg(1), true); });
    // MSVC x64 scalar operator new(size_t), imported by Peggle 2 as
    // ??2@YAPEAX_K@Z. Returning a zero stub caused its very next dereference to fault.
    H("??2@YAPEAX_K@Z", [this](GuestCpu& c) {
        const auto requested = c.arg(0);
        const auto result = heap_alloc(requested, false);
        if (!result) milestone("operator new(size=" + std::to_string(requested) + ") failed: guest heap could not satisfy allocation");
        c.r[0] = result;
    });
    H("realloc", [this](GuestCpu& c) { c.r[0] = heap_realloc(c.arg(0), c.arg(1)); });
    H("free", [this](GuestCpu& c) { heap_free(c.arg(0)); });
    // MSVC scalar operator delete used by Peggle 2's VS2012 CRT.
    H("??3@YAXPEAX@Z", [this](GuestCpu& c) { heap_free(c.arg(0)); c.r[0] = 0; });
    H("??_V@YAXPEAX@Z", [this](GuestCpu& c) { heap_free(c.arg(0)); c.r[0] = 0; });
    auto bounded_import_copy = [this](GuestCpu& c, const char* api) -> bool {
        const std::uint64_t destination = c.arg(0), source = c.arg(1), length = c.arg(2);
        constexpr std::uint64_t max_import_copy = 16ULL * 1024ULL * 1024ULL;
        const std::uint64_t caller = mem_.read<std::uint64_t>(c.r[4]);
        auto reject = [&](const std::string& reason) {
            const std::string detail = std::string("CRT ") + api + " rejected: " + reason +
                "; dst=0x" + GuestCpu::hex(destination) + "; src=0x" + GuestCpu::hex(source) +
                "; bytes=0x" + GuestCpu::hex(length) + "; caller=" + locate(caller);
            milestone(detail);
            c.request_stop(GuestCpu::Stop::HostRequest, detail);
        };
        if (length == 0) return true;
        if (length > max_import_copy) { reject("copy exceeds the 16 MiB safety limit"); return false; }
        if (destination > (std::numeric_limits<std::uint64_t>::max)() - length ||
            source > (std::numeric_limits<std::uint64_t>::max)() - length) {
            reject("address arithmetic overflow"); return false;
        }
        if (!mem_.is_mapped(destination, length) || !mem_.is_mapped(source, length)) {
            reject("source or destination range is not fully mapped"); return false;
        }
        copy_guest(destination, source, length);
        return true;
    };
    H("memcpy", [bounded_import_copy](GuestCpu& c) { if (bounded_import_copy(c, "memcpy")) c.r[0] = c.arg(0); });
    H("memmove", [bounded_import_copy](GuestCpu& c) { if (bounded_import_copy(c, "memmove")) c.r[0] = c.arg(0); });
    H("_memcpy", [bounded_import_copy](GuestCpu& c) { if (bounded_import_copy(c, "_memcpy")) c.r[0] = c.arg(0); });
    H("_memmove", [bounded_import_copy](GuestCpu& c) { if (bounded_import_copy(c, "_memmove")) c.r[0] = c.arg(0); });
    H("memset", [this](GuestCpu& c) {
        const std::uint64_t d = c.arg(0), n = c.arg(2); const auto v = static_cast<std::uint8_t>(c.arg(1));
        std::vector<std::uint8_t> buf(static_cast<std::size_t>((std::min<std::uint64_t>)(n, 65536)), v);
        for (std::uint64_t off = 0; off < n; off += buf.size()) mem_.write_bytes(d + off, buf.data(), static_cast<std::size_t>((std::min<std::uint64_t>)(buf.size(), n - off)));
        c.r[0] = d;
    });
    H("memcmp", [this](GuestCpu& c) {
        const std::uint64_t a = c.arg(0), b = c.arg(1), n = c.arg(2); std::int32_t r = 0;
        for (std::uint64_t i = 0; i < n && !r; ++i) { const int x = mem_.read<std::uint8_t>(a + i), y = mem_.read<std::uint8_t>(b + i); r = x - y; }
        c.r[0] = static_cast<std::uint32_t>(r);
    });
    H("memchr", [this](GuestCpu& c) {
        const std::uint64_t a = c.arg(0), n = c.arg(2); const auto v = static_cast<std::uint8_t>(c.arg(1)); c.r[0] = 0;
        for (std::uint64_t i = 0; i < n; ++i) if (mem_.read<std::uint8_t>(a + i) == v) { c.r[0] = a + i; break; }
    });
    H("strnlen", [this](GuestCpu& c) { std::uint64_t n = 0; const auto a = c.arg(0), m = c.arg(1); while (n < m && mem_.read<std::uint8_t>(a + n)) ++n; c.r[0] = n; });
    H("wcslen", [this](GuestCpu& c) { std::uint64_t n = 0; const auto a = c.arg(0); while (mem_.read<std::uint16_t>(a + n * 2)) ++n; c.r[0] = n; });
    H("wcsnlen", [this](GuestCpu& c) { std::uint64_t n = 0; const auto a = c.arg(0), m = c.arg(1); while (n < m && mem_.read<std::uint16_t>(a + n * 2)) ++n; c.r[0] = n; });
    H("strcpy_s", [this](GuestCpu& c) { // (dst, size, src)
        const std::uint64_t dst = c.arg(0), size = c.arg(1); const std::string src = mem_.read_cstring(c.arg(2));
        if (!dst || src.size() + 1 > size) { c.r[0] = 34; return; }
        mem_.write_bytes(dst, src.c_str(), src.size() + 1); c.r[0] = 0;
    });
    H("strncpy", [this](GuestCpu& c) {
        const std::uint64_t d = c.arg(0), n = c.arg(2); const std::string s = mem_.read_cstring(c.arg(1), static_cast<std::size_t>((std::min<std::uint64_t>)(n, 1 << 20)));
        for (std::uint64_t i = 0; i < n; ++i) mem_.write<std::uint8_t>(d + i, i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0);
        c.r[0] = d;
    });
    H("_stricmp", [this](GuestCpu& c) {
        const std::string a = mem_.read_cstring(c.arg(0)), b = mem_.read_cstring(c.arg(1)); int r = 0;
        for (std::size_t i = 0; i <= (std::max)(a.size(), b.size()) && !r; ++i) r = std::tolower(i < a.size() ? static_cast<unsigned char>(a[i]) : 0) - std::tolower(i < b.size() ? static_cast<unsigned char>(b[i]) : 0);
        c.r[0] = static_cast<std::uint32_t>(r);
    });
    H("_wcsicmp", [this](GuestCpu& c) {
        const auto a = mem_.read_wstring(c.arg(0)), b = mem_.read_wstring(c.arg(1)); int r = 0;
        for (std::size_t i = 0; i <= (std::max)(a.size(), b.size()) && !r; ++i) r = std::tolower(i < a.size() ? a[i] & 0x7F : 0) - std::tolower(i < b.size() ? b[i] & 0x7F : 0);
        c.r[0] = static_cast<std::uint32_t>(r);
    });
    H("wcstok_s", [this](GuestCpu& c) { // (str, delims, context**)
        std::uint64_t s = c.arg(0); const auto delims = mem_.read_wstring(c.arg(1)); const std::uint64_t ctx = c.arg(2);
        if (!s) s = mem_.read<std::uint64_t>(ctx);
        auto isd = [&](std::uint16_t ch) { return delims.find(static_cast<char16_t>(ch)) != std::u16string::npos; };
        while (s && mem_.read<std::uint16_t>(s) && isd(mem_.read<std::uint16_t>(s))) s += 2;
        if (!s || !mem_.read<std::uint16_t>(s)) { mem_.write<std::uint64_t>(ctx, s); c.r[0] = 0; return; }
        const std::uint64_t tok = s;
        while (mem_.read<std::uint16_t>(s) && !isd(mem_.read<std::uint16_t>(s))) s += 2;
        if (mem_.read<std::uint16_t>(s)) { mem_.write<std::uint16_t>(s, 0); s += 2; }
        mem_.write<std::uint64_t>(ctx, s); c.r[0] = tok;
    });
    auto parse_int = [this](GuestCpu& c, bool wide, bool is_signed) {
        const std::string s = wide ? utf16_to_utf8(mem_.read_wstring(c.arg(0))) : mem_.read_cstring(c.arg(0));
        char* end = nullptr; const int base = static_cast<int>(c.arg(2));
        const long long v = is_signed ? std::strtoll(s.c_str(), &end, base) : static_cast<long long>(std::strtoull(s.c_str(), &end, base));
        if (c.arg(1)) mem_.write<std::uint64_t>(c.arg(1), c.arg(0) + static_cast<std::uint64_t>(end - s.c_str()) * (wide ? 2 : 1));
        c.r[0] = static_cast<std::uint64_t>(v) & 0xFFFFFFFFULL;
    };
    H("wcstol", [parse_int](GuestCpu& c) { parse_int(c, true, true); });
    H("wcstoul", [parse_int](GuestCpu& c) { parse_int(c, true, false); });
    H("_strtod_l", [this](GuestCpu& c) { const std::string s = mem_.read_cstring(c.arg(0)); char* e; const double v = std::strtod(s.c_str(), &e); if (c.arg(1)) mem_.write<std::uint64_t>(c.arg(1), c.arg(0) + static_cast<std::uint64_t>(e - s.c_str())); setd(c, v); });
    H("_wcstod_l", [this](GuestCpu& c) { const std::string s = utf16_to_utf8(mem_.read_wstring(c.arg(0))); char* e; const double v = std::strtod(s.c_str(), &e); if (c.arg(1)) mem_.write<std::uint64_t>(c.arg(1), c.arg(0) + static_cast<std::uint64_t>(e - s.c_str()) * 2); setd(c, v); });
    auto itoa_s = [this](GuestCpu& c, bool wide, bool is_signed) { // (value, buf, size, radix)
        const std::uint64_t v = c.arg(0), buf = c.arg(1), size = c.arg(2); const unsigned radix = static_cast<unsigned>(c.arg(3));
        std::string s; std::uint64_t u = v; const bool neg = is_signed && radix == 10 && static_cast<std::int64_t>(v) < 0;
        if (neg) u = static_cast<std::uint64_t>(-static_cast<std::int64_t>(v));
        if (u == 0) s = "0";
        while (u) { s.insert(s.begin(), "0123456789abcdefghijklmnopqrstuvwxyz"[u % radix]); u /= radix; }
        if (neg) s.insert(s.begin(), '-');
        if (s.size() + 1 > size) { c.r[0] = 34; return; }
        if (wide) write_wstr(buf, utf8_to_utf16(s)); else mem_.write_bytes(buf, s.c_str(), s.size() + 1);
        c.r[0] = 0;
    };
    H("_i64toa_s", [itoa_s](GuestCpu& c) { itoa_s(c, false, true); });
    H("_ui64toa_s", [itoa_s](GuestCpu& c) { itoa_s(c, false, false); });
    H("_i64tow_s", [itoa_s](GuestCpu& c) { itoa_s(c, true, true); });
    H("_ui64tow_s", [itoa_s](GuestCpu& c) { itoa_s(c, true, false); });

    // ---- ctype / locale ----
    H("isalpha", [](GuestCpu& c) { const auto ch = static_cast<int>(static_cast<std::int32_t>(c.arg(0))); c.r[0] = (ch >= 0 && ch < 128 && std::isalpha(ch)) ? 1 : 0; });
    H("isdigit", [](GuestCpu& c) { const auto ch = static_cast<int>(static_cast<std::int32_t>(c.arg(0))); c.r[0] = (ch >= 0 && ch < 128 && std::isdigit(ch)) ? 1 : 0; });
    H("isspace", [](GuestCpu& c) { const auto ch = static_cast<int>(static_cast<std::int32_t>(c.arg(0))); c.r[0] = (ch >= 0 && ch < 128 && std::isspace(ch)) ? 1 : 0; });
    H("iswspace", [](GuestCpu& c) { const auto ch = static_cast<int>(c.arg(0) & 0xFFFF); c.r[0] = (ch < 128 && std::isspace(ch)) ? 1 : 0; });
    H("tolower", [](GuestCpu& c) { const auto ch = static_cast<std::int32_t>(c.arg(0)); c.r[0] = static_cast<std::uint32_t>((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch); });
    H("__pctype_func", [this](GuestCpu& c) { c.r[0] = ctype_ptr_; });
    H("_isctype_l", [this](GuestCpu& c) { const auto ch = static_cast<std::int32_t>(c.arg(0)); c.r[0] = (ch >= 0 && ch < 256) ? (mem_.read<std::uint16_t>(ctype_ptr_ + static_cast<std::uint64_t>(ch) * 2) & c.arg(1)) : 0; });
    H("setlocale", [this](GuestCpu& c) { c.r[0] = c_locale_str_; });
    H("_create_locale", [this](GuestCpu& c) { c.r[0] = static_alloc(64); });
    H("_free_locale", [](GuestCpu&) {});
    H("_configthreadlocale", [](GuestCpu& c) { c.r[0] = 0; });
    H("_errno", [this](GuestCpu& c) { c.r[0] = errno_ptr_; });

    // ---- CRT startup ----
    H("_set_app_type", [](GuestCpu&) {});
    H("_set_fmode", [](GuestCpu& c) { c.r[0] = 0; });
    H("_set_new_mode", [](GuestCpu& c) { c.r[0] = 0; });
    H("__p__commode", [this](GuestCpu& c) { c.r[0] = commode_ptr_; });
    H("__setusermatherr", [](GuestCpu&) {});
    H("_configure_narrow_argv", [](GuestCpu& c) { c.r[0] = 0; });
    H("_initialize_narrow_environment", [](GuestCpu& c) { c.r[0] = 0; });
    H("_configure_wide_argv", [this](GuestCpu& c) { ensure_wide_crt_state(); c.r[0] = 0; });
    H("_initialize_wide_environment", [this](GuestCpu& c) { ensure_wide_crt_state(); c.r[0] = 0; });
    H("__set_app_type", [](GuestCpu& c) { c.r[0] = 0; });
    H("__wgetmainargs", [this](GuestCpu& c) {
        ensure_wide_crt_state();
        if (c.arg(0)) mem_.write<std::int32_t>(c.arg(0), 1);
        if (c.arg(1)) mem_.write<std::uint64_t>(c.arg(1), wide_argv_);
        if (c.arg(2)) mem_.write<std::uint64_t>(c.arg(2), wide_env_);
        c.r[0] = 0; // no wildcard expansion; argv[0] only
    });
    H("__p___argc", [this](GuestCpu& c) { ensure_wide_crt_state(); c.r[0] = wide_argc_slot_; });
    H("__p___wargv", [this](GuestCpu& c) { ensure_wide_crt_state(); c.r[0] = wide_argv_slot_; });
    H("__p___wenviron", [this](GuestCpu& c) { ensure_wide_crt_state(); c.r[0] = wide_env_slot_; });
    H("__p___winitenv", [this](GuestCpu& c) { ensure_wide_crt_state(); c.r[0] = wide_env_slot_; });
    H("__p__wcmdln", [this](GuestCpu& c) { ensure_wide_crt_state(); c.r[0] = wide_cmdline_slot_; });
    H("_get_initial_wide_environment", [this](GuestCpu& c) { ensure_wide_crt_state(); c.r[0] = wide_env_; });
    H("_get_wide_winmain_command_line", [this](GuestCpu& c) { ensure_wide_crt_state(); c.r[0] = wide_cmdline_; });
    H("__getmainargs", [this](GuestCpu& c) {
        // Minimal single-argument CRT startup state; wildcard expansion and a real
        // inherited host environment are intentionally not emulated.
        const std::uint64_t argv = static_alloc(16, 8);
        const std::uint64_t envp = static_alloc(8, 8);
        const std::uint64_t arg0 = static_cstr("SomeBoxOneTitle.exe");
        mem_.write<std::uint64_t>(argv, arg0); mem_.write<std::uint64_t>(argv + 8, 0);
        mem_.write<std::uint64_t>(envp, 0);
        if (c.arg(0)) mem_.write<std::int32_t>(c.arg(0), 1);
        if (c.arg(1)) mem_.write<std::uint64_t>(c.arg(1), argv);
        if (c.arg(2)) mem_.write<std::uint64_t>(c.arg(2), envp);
        c.r[0] = 0;
    });
    H("_onexit", [this](GuestCpu& c) { if (c.arg(0)) atexit_fns_.push_back(c.arg(0)); c.r[0] = c.arg(0); });
    H("_get_narrow_winmain_command_line", [this](GuestCpu& c) { c.r[0] = static_cstr(opt_.command_line); });
    H("_seh_filter_exe", [](GuestCpu& c) { c.r[0] = 0; });
    H("_initialize_onexit_table", [this](GuestCpu& c) { const auto t = c.arg(0); if (t) { mem_.write<std::uint64_t>(t, ~0ULL); mem_.write<std::uint64_t>(t + 8, ~0ULL); mem_.write<std::uint64_t>(t + 16, ~0ULL); } c.r[0] = 0; });
    H("_register_onexit_function", [this](GuestCpu& c) { atexit_fns_.push_back(c.arg(1)); c.r[0] = 0; });
    H("_crt_atexit", [this](GuestCpu& c) { atexit_fns_.push_back(c.arg(0)); c.r[0] = 0; });
    H("_register_thread_local_exe_atexit_callback", [](GuestCpu&) {});
    H("_initterm", [this](GuestCpu& c) {
        const std::uint64_t b = c.arg(0), e = c.arg(1);
        unsigned n = 0;
        for (std::uint64_t p = b; p < e; p += 8) {
            const std::uint64_t fn = mem_.read<std::uint64_t>(p);
            if (!fn) continue;
            ++n;
            call_guest(c, fn, {});
            if (c.stop != GuestCpu::Stop::None) return;
        }
        milestone("_initterm ran " + std::to_string(n) + " initializer(s) from [" + locate(b) + " .. " + locate(e) + ")");
        c.r[0] = 0;
    });
    H("_initterm_e", [this](GuestCpu& c) {
        const std::uint64_t b = c.arg(0), e = c.arg(1);
        unsigned n = 0;
        for (std::uint64_t p = b; p < e; p += 8) {
            const std::uint64_t fn = mem_.read<std::uint64_t>(p);
            if (!fn) continue;
            ++n;
            const std::uint64_t rv = call_guest(c, fn, {});
            if (c.stop != GuestCpu::Stop::None) return;
            if (static_cast<std::uint32_t>(rv) != 0) { c.r[0] = rv & 0xFFFFFFFFULL; return; }
        }
        milestone("_initterm_e ran " + std::to_string(n) + " initializer(s)");
        c.r[0] = 0;
    });
    auto do_exit = [this](GuestCpu& c) {
        exited_ = true; exit_code_ = static_cast<int>(static_cast<std::int32_t>(c.arg(0)));
        c.request_stop(GuestCpu::Stop::HostRequest, "title called exit(" + std::to_string(exit_code_) + ")");
    };
    H("exit", do_exit); H("_exit", do_exit);
    H("_cexit", [](GuestCpu&) {}); H("_c_exit", [](GuestCpu&) {});
    H("_callnewh", [](GuestCpu& c) { c.r[0] = 0; });

    // ---- math ----
    H("sinf", [](GuestCpu& c) { setf(c, std::sin(getf(c))); }); H("cosf", [](GuestCpu& c) { setf(c, std::cos(getf(c))); });
    H("acosf", [](GuestCpu& c) { setf(c, std::acos(getf(c))); }); H("asinf", [](GuestCpu& c) { setf(c, std::asin(getf(c))); });
    H("atan2f", [](GuestCpu& c) { setf(c, std::atan2(getf(c, 0), getf(c, 1))); }); H("logf", [](GuestCpu& c) { setf(c, std::log(getf(c))); });
    H("sin", [](GuestCpu& c) { setd(c, std::sin(getd(c))); }); H("exp", [](GuestCpu& c) { setd(c, std::exp(getd(c))); });
    H("sqrt", [](GuestCpu& c) { setd(c, std::sqrt(getd(c))); });
    H("sqrtf", [](GuestCpu& c) { setf(c, std::sqrt(getf(c))); });
    H("pow", [](GuestCpu& c) { setd(c, std::pow(getd(c, 0), getd(c, 1))); });
    H("ldexp", [](GuestCpu& c) { setd(c, std::ldexp(getd(c, 0), static_cast<int>(static_cast<std::int32_t>(c.r[2])))); });
    H("rand", [this](GuestCpu& c) { rand_state_ = rand_state_ * 214013ULL + 2531011ULL; c.r[0] = (rand_state_ >> 16) & 0x7FFF; });
    H("qsort", [this](GuestCpu& c) {
        const std::uint64_t base = c.arg(0), n = c.arg(1), sz = c.arg(2), cmp = c.arg(3);
        if (n < 2 || sz == 0 || sz > 4096) return;
        std::vector<std::vector<std::uint8_t>> items(static_cast<std::size_t>(n), std::vector<std::uint8_t>(static_cast<std::size_t>(sz)));
        for (std::uint64_t i = 0; i < n; ++i) mem_.read_bytes(base + i * sz, items[static_cast<std::size_t>(i)].data(), static_cast<std::size_t>(sz));
        const std::uint64_t ta = static_alloc(sz * 2 > 16 ? sz * 2 : 16), tb = ta + sz;
        // merge sort with the guest comparator (stable; avoids quadratic guest calls)
        std::vector<std::size_t> idx(items.size()); for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        bool aborted = false;
        std::stable_sort(idx.begin(), idx.end(), [&](std::size_t x, std::size_t y) {
            if (aborted || c.stop != GuestCpu::Stop::None) { aborted = true; return false; }
            mem_.write_bytes(ta, items[x].data(), static_cast<std::size_t>(sz)); mem_.write_bytes(tb, items[y].data(), static_cast<std::size_t>(sz));
            return static_cast<std::int32_t>(call_guest(c, cmp, {ta, tb})) < 0;
        });
        if (aborted) return;
        for (std::uint64_t i = 0; i < n; ++i) mem_.write_bytes(base + i * sz, items[idx[static_cast<std::size_t>(i)]].data(), static_cast<std::size_t>(sz));
    });

    // ---- time ----
    H("_time64", [this](GuestCpu& c) { const std::uint64_t t = 1767225600ULL + (++rand_state_ % 3); if (c.arg(0)) mem_.write<std::uint64_t>(c.arg(0), t); c.r[0] = t; });
    H("_time32", [this](GuestCpu& c) { const std::uint32_t t = 1767225600U; if (c.arg(0)) mem_.write<std::uint32_t>(c.arg(0), t); c.r[0] = t; });
    auto fill_tm = [this](std::uint64_t dst, std::int64_t t) {
        std::time_t tt = static_cast<std::time_t>(t); std::tm tmv{};
#if defined(_WIN32)
        gmtime_s(&tmv, &tt);
#else
        gmtime_r(&tt, &tmv);
#endif
        const std::int32_t f[9] = {tmv.tm_sec, tmv.tm_min, tmv.tm_hour, tmv.tm_mday, tmv.tm_mon, tmv.tm_year, tmv.tm_wday, tmv.tm_yday, 0};
        for (int i = 0; i < 9; ++i) mem_.write<std::int32_t>(dst + static_cast<std::uint64_t>(i) * 4, f[i]);
    };
    H("_localtime64_s", [this, fill_tm](GuestCpu& c) { fill_tm(c.arg(0), static_cast<std::int64_t>(mem_.read<std::uint64_t>(c.arg(1)))); c.r[0] = 0; });
    H("_localtime64", [this, fill_tm](GuestCpu& c) { const auto p = static_alloc(64); fill_tm(p, static_cast<std::int64_t>(mem_.read<std::uint64_t>(c.arg(0)))); c.r[0] = p; });
    H("_mktime64", [this](GuestCpu& c) {
        std::tm t{}; const auto a = c.arg(0);
        t.tm_sec = mem_.read<std::int32_t>(a); t.tm_min = mem_.read<std::int32_t>(a + 4); t.tm_hour = mem_.read<std::int32_t>(a + 8);
        t.tm_mday = mem_.read<std::int32_t>(a + 12); t.tm_mon = mem_.read<std::int32_t>(a + 16); t.tm_year = mem_.read<std::int32_t>(a + 20);
        c.r[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(
#if defined(_WIN32)
            _mkgmtime(&t)
#else
            timegm(&t)
#endif
        ));
    });
    H("strftime", [](GuestCpu& c) { c.r[0] = 0; });

    // ---- stdio (sandboxed) ----
    H("__acrt_iob_func", [this](GuestCpu& c) { c.r[0] = iob_base_ + (c.arg(0) & 3) * 0x100; });
    H("fopen", [this](GuestCpu& c) {
        const std::string path = mem_.read_cstring(c.arg(0)), mode = mem_.read_cstring(c.arg(1));
        const bool wr = mode.find_first_of("wa+") != std::string::npos;
        const auto host = resolve_path(path, wr);
        if (wr) { std::error_code ec; std::filesystem::create_directories(host.parent_path(), ec); }
        std::FILE* fp = open_host_file(host, mode);
        if (files_opened_.size() < 64) files_opened_.push_back((fp ? "opened " : "FAILED ") + path + " (" + mode + ") -> " + PeImage::path_utf8(host));
        if (!fp) { mem_.write<std::int32_t>(errno_ptr_, 2); c.r[0] = 0; return; }
        c.r[0] = make_file(fp, path);
    });
    H("fclose", [this](GuestCpu& c) { const auto it = files_.find(c.arg(0)); if (it != files_.end()) { std::fclose(it->second.fp); files_.erase(it); c.r[0] = 0; } else c.r[0] = 0xFFFFFFFFULL; });
    H("fread", [this](GuestCpu& c) {
        std::FILE* fp = file_of(c.arg(3)); const std::uint64_t buf = c.arg(0), size = c.arg(1), cnt = c.arg(2);
        if (!fp || !size || !cnt) { c.r[0] = 0; return; }
        std::vector<std::uint8_t> tmp(static_cast<std::size_t>((std::min<std::uint64_t>)(size * cnt, 1ULL << 22)));
        std::uint64_t total = 0, want = size * cnt;
        while (want) { const std::size_t n = std::fread(tmp.data(), 1, static_cast<std::size_t>((std::min<std::uint64_t>)(want, tmp.size())), fp); if (!n) break; mem_.write_bytes(buf + total, tmp.data(), n); total += n; want -= n; }
        c.r[0] = total / size;
    });
    H("fwrite", [this](GuestCpu& c) {
        const std::uint64_t buf = c.arg(0), size = c.arg(1), cnt = c.arg(2), f = c.arg(3);
        std::FILE* fp = file_of(f); const std::uint64_t total = size * cnt;
        std::vector<std::uint8_t> tmp(static_cast<std::size_t>((std::min<std::uint64_t>)(total, 1ULL << 22)));
        if (!fp) { // stdout/stderr captured
            if (total <= tmp.size()) { mem_.read_bytes(buf, tmp.data(), static_cast<std::size_t>(total)); if (stdout_log_.size() < 200) stdout_log_.emplace_back(reinterpret_cast<char*>(tmp.data()), static_cast<std::size_t>(total)); }
            c.r[0] = cnt; return;
        }
        std::uint64_t done = 0;
        while (done < total) { const auto n = (std::min<std::uint64_t>)(total - done, tmp.size()); mem_.read_bytes(buf + done, tmp.data(), static_cast<std::size_t>(n)); std::fwrite(tmp.data(), 1, static_cast<std::size_t>(n), fp); done += n; }
        c.r[0] = cnt;
    });
    H("fseek", [this](GuestCpu& c) { std::FILE* fp = file_of(c.arg(0)); c.r[0] = fp ? static_cast<std::uint64_t>(std::fseek(fp, static_cast<long>(static_cast<std::int32_t>(c.arg(1))), static_cast<int>(c.arg(2)))) & 0xFFFFFFFFULL : 0xFFFFFFFFULL; });
    H("_fseeki64", [this](GuestCpu& c) {
        std::FILE* fp = file_of(c.arg(0));
#if defined(_WIN32)
        c.r[0] = fp ? static_cast<std::uint64_t>(_fseeki64(fp, static_cast<long long>(c.arg(1)), static_cast<int>(c.arg(2)))) & 0xFFFFFFFFULL : 0xFFFFFFFFULL;
#else
        c.r[0] = fp ? static_cast<std::uint64_t>(fseeko(fp, static_cast<off_t>(c.arg(1)), static_cast<int>(c.arg(2)))) & 0xFFFFFFFFULL : 0xFFFFFFFFULL;
#endif
    });
    H("ftell", [this](GuestCpu& c) { std::FILE* fp = file_of(c.arg(0)); c.r[0] = fp ? static_cast<std::uint64_t>(std::ftell(fp)) & 0xFFFFFFFFULL : 0xFFFFFFFFULL; });
    H("fgetc", [this](GuestCpu& c) { std::FILE* fp = file_of(c.arg(0)); c.r[0] = fp ? static_cast<std::uint64_t>(static_cast<std::int64_t>(std::fgetc(fp))) & 0xFFFFFFFFULL : 0xFFFFFFFFULL; });
    H("fputc", [this](GuestCpu& c) { std::FILE* fp = file_of(c.arg(1)); if (fp) std::fputc(static_cast<int>(c.arg(0) & 0xFF), fp); c.r[0] = c.arg(0) & 0xFF; });
    H("ungetc", [this](GuestCpu& c) { std::FILE* fp = file_of(c.arg(1)); c.r[0] = fp ? static_cast<std::uint64_t>(std::ungetc(static_cast<int>(c.arg(0)), fp)) & 0xFFFFFFFFULL : 0xFFFFFFFFULL; });
    H("feof", [this](GuestCpu& c) { std::FILE* fp = file_of(c.arg(0)); c.r[0] = fp ? (std::feof(fp) ? 1 : 0) : 1; });
    H("ferror", [this](GuestCpu& c) { std::FILE* fp = file_of(c.arg(0)); c.r[0] = fp ? (std::ferror(fp) ? 1 : 0) : 0; });
    H("fflush", [this](GuestCpu& c) { std::FILE* fp = file_of(c.arg(0)); if (fp) std::fflush(fp); c.r[0] = 0; });
    H("setvbuf", [](GuestCpu& c) { c.r[0] = 0; });
    H("fgetpos", [this](GuestCpu& c) { std::FILE* fp = file_of(c.arg(0)); if (!fp) { c.r[0] = 1; return; } mem_.write<std::uint64_t>(c.arg(1), static_cast<std::uint64_t>(std::ftell(fp))); c.r[0] = 0; });
    H("fsetpos", [this](GuestCpu& c) { std::FILE* fp = file_of(c.arg(0)); if (!fp) { c.r[0] = 1; return; } c.r[0] = static_cast<std::uint64_t>(std::fseek(fp, static_cast<long>(mem_.read<std::uint64_t>(c.arg(1))), SEEK_SET)) & 0xFFFFFFFFULL; });
    H("remove", [this](GuestCpu& c) { std::error_code ec; const auto p = resolve_path(mem_.read_cstring(c.arg(0)), true); c.r[0] = std::filesystem::remove(p, ec) ? 0 : 0xFFFFFFFFULL; });
    H("_lock_file", [](GuestCpu&) {}); H("_unlock_file", [](GuestCpu&) {});
    H("_get_stream_buffer_pointers", [](GuestCpu& c) { c.r[0] = 0; });

    // printf family. Signatures (UCRT): vsprintf(opts, buf, count, fmt, locale, args); vsprintf_s same;
    // vsnprintf_s(opts, buf, bufcount, maxcount, fmt, locale, args); vfprintf(opts, FILE*, fmt, locale, args)
    auto emit_narrow = [this](GuestCpu& c, std::uint64_t buf, std::uint64_t cap, const std::string& s) {
        if (cap && buf) { const std::size_t n = static_cast<std::size_t>((std::min<std::uint64_t>)(s.size(), cap - 1)); mem_.write_bytes(buf, s.data(), n); mem_.write<std::uint8_t>(buf + n, 0); }
        c.r[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(s.size())) & 0xFFFFFFFFULL;
    };
    auto emit_wide = [this](GuestCpu& c, std::uint64_t buf, std::uint64_t cap, const std::string& s) {
        const std::u16string w = utf8_to_utf16(s);
        if (cap && buf) { const std::size_t n = static_cast<std::size_t>((std::min<std::uint64_t>)(w.size(), cap - 1)); for (std::size_t i = 0; i < n; ++i) mem_.write<std::uint16_t>(buf + i * 2, w[i]); mem_.write<std::uint16_t>(buf + n * 2, 0); }
        c.r[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(w.size())) & 0xFFFFFFFFULL;
    };
    H("__stdio_common_vsprintf", [this, emit_narrow](GuestCpu& c) { emit_narrow(c, c.arg(1), c.arg(2), guest_format(mem_.read_cstring(c.arg(3)), c.arg(5), false)); });
    H("__stdio_common_vsprintf_s", [this, emit_narrow](GuestCpu& c) { emit_narrow(c, c.arg(1), c.arg(2), guest_format(mem_.read_cstring(c.arg(3)), c.arg(5), false)); });
    H("__stdio_common_vsnprintf_s", [this, emit_narrow](GuestCpu& c) { emit_narrow(c, c.arg(1), (std::min)(c.arg(2), c.arg(3) + 1), guest_format(mem_.read_cstring(c.arg(4)), c.arg(6), false)); });
    H("__stdio_common_vswprintf_s", [this, emit_wide](GuestCpu& c) { emit_wide(c, c.arg(1), c.arg(2), guest_format(utf16_to_utf8(mem_.read_wstring(c.arg(3))), c.arg(5), true)); });
    H("__stdio_common_vsnwprintf_s", [this, emit_wide](GuestCpu& c) { emit_wide(c, c.arg(1), (std::min)(c.arg(2), c.arg(3) + 1), guest_format(utf16_to_utf8(mem_.read_wstring(c.arg(4))), c.arg(6), true)); });
    H("__stdio_common_vfprintf", [this](GuestCpu& c) {
        const std::string s = guest_format(mem_.read_cstring(c.arg(2)), c.arg(4), false);
        std::FILE* fp = file_of(c.arg(1));
        if (fp) std::fwrite(s.data(), 1, s.size(), fp); else if (stdout_log_.size() < 200) stdout_log_.push_back(s);
        c.r[0] = s.size();
    });
    H("__stdio_common_vswscanf", [](GuestCpu& c) { c.r[0] = 0xFFFFFFFFULL; });
}

inline void XboxTitleRuntime::register_kernel_handlers() {
    // ---- process / thread identity ----
    H("GetCurrentProcess", [](GuestCpu& c) { c.r[0] = ~0ULL; });
    H("GetCurrentThread", [](GuestCpu& c) { c.r[0] = ~1ULL; });
    H("GetCurrentProcessId", [](GuestCpu& c) { c.r[0] = 0x400; });
    H("GetCurrentThreadId", [](GuestCpu& c) { c.r[0] = 0x1000; });
    H("GetTickCount64", [this](GuestCpu& c) {
        c.r[0] = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time_).count());
    });
    H("GetTickCount", [this](GuestCpu& c) {
        c.r[0] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time_).count());
    });
    H("TlsAlloc", [this](GuestCpu& c) {
        constexpr std::uint32_t kMaxTlsSlots = 1088;
        std::uint32_t index = 0;
        if (!free_tls_indexes_.empty()) {
            index = free_tls_indexes_.back();
            free_tls_indexes_.pop_back();
        } else {
            if (next_tls_index_ >= kMaxTlsSlots) { c.r[0] = 0xFFFFFFFFULL; last_error_ = 8; return; }
            index = next_tls_index_++;
        }
        allocated_tls_indexes_.insert(index);
        tls_values_.erase(index);
        c.r[0] = index;
    });
    H("TlsGetValue", [this](GuestCpu& c) {
        const auto index = static_cast<std::uint32_t>(c.arg(0));
        if (!allocated_tls_indexes_.contains(index)) { c.r[0] = 0; last_error_ = 87; return; }
        const auto it = tls_values_.find(index);
        c.r[0] = it != tls_values_.end() ? it->second : 0;
        last_error_ = 0;
    });
    H("TlsSetValue", [this](GuestCpu& c) {
        const auto index = static_cast<std::uint32_t>(c.arg(0));
        if (!allocated_tls_indexes_.contains(index)) { last_error_ = 87; c.r[0] = 0; return; }
        tls_values_[index] = c.arg(1); c.r[0] = 1; last_error_ = 0;
    });
    H("TlsFree", [this](GuestCpu& c) {
        const auto index = static_cast<std::uint32_t>(c.arg(0));
        if (!allocated_tls_indexes_.erase(index)) { last_error_ = 87; c.r[0] = 0; return; }
        tls_values_.erase(index);
        free_tls_indexes_.push_back(index);
        c.r[0] = 1; last_error_ = 0;
    });
    H("GetLastError", [this](GuestCpu& c) { c.r[0] = last_error_; });
    H("CreateMutexExW", [this](GuestCpu& c) {
        (void)c;
        const auto handle = next_fake_handle_; next_fake_handle_ += 8;
        mutex_handles_.insert(handle);
        c.r[0] = handle;
        milestone("CreateMutexExW -> synthetic mutex handle (single-threaded synchronization only)");
    });
    H("CreateMutexW", [this](GuestCpu& c) {
        (void)c; const auto handle = next_fake_handle_; next_fake_handle_ += 8;
        mutex_handles_.insert(handle); c.r[0] = handle;
    });
    H("OpenMutexW", [this](GuestCpu& c) {
        (void)c; const auto handle = next_fake_handle_; next_fake_handle_ += 8;
        mutex_handles_.insert(handle); c.r[0] = handle;
    });
    H("ReleaseMutex", [this](GuestCpu& c) {
        if (mutex_handles_.contains(c.arg(0))) { c.r[0] = 1; last_error_ = 0; }
        else { c.r[0] = 0; last_error_ = 6; }
    });
    auto create_semaphore = [this](GuestCpu& c) {
        const std::uint64_t initial = c.arg(1), maximum = c.arg(2);
        if (maximum == 0 || initial > maximum || maximum > 0x7FFFFFFFULL) {
            c.r[0] = 0; last_error_ = 87; return;
        }
        const std::uint64_t handle = next_fake_handle_; next_fake_handle_ += 8;
        semaphores_[handle] = {static_cast<std::uint32_t>(initial), static_cast<std::uint32_t>(maximum)};
        c.r[0] = handle; last_error_ = 0;
        if (com_log_.insert("semaphore-create").second)
            milestone("CreateSemaphoreExW/CreateSemaphoreW -> synthetic counting semaphore (single-threaded approximation)");
    };
    H("CreateSemaphoreExW", create_semaphore);
    H("CreateSemaphoreW", create_semaphore);
    H("CreateSemaphoreA", create_semaphore);
    H("CreateSemaphoreExA", create_semaphore);
    H("ReleaseSemaphore", [this](GuestCpu& c) {
        const auto handle = c.arg(0); const auto release = c.arg(1);
        const auto it = semaphores_.find(handle);
        if (it == semaphores_.end() || release == 0 || release > it->second.maximum - it->second.count) {
            c.r[0] = 0; last_error_ = (it == semaphores_.end()) ? 6 : 298; return;
        }
        if (c.arg(2)) mem_.write<std::uint32_t>(c.arg(2), it->second.count);
        it->second.count += static_cast<std::uint32_t>(release);
        c.r[0] = 1; last_error_ = 0;
    });
    H("IsDebuggerPresent", [](GuestCpu& c) { c.r[0] = 0; });
    H("SetThreadAffinityMask", [](GuestCpu& c) { c.r[0] = 0xF; });
    H("DecodePointer", [](GuestCpu& c) { c.r[0] = c.arg(0); });
    H("?_Orphan_all@_Container_base0@std@@QEAAXXZ", [](GuestCpu& c) { (void)c; });
    H("toupper", [](GuestCpu& c) {
        const unsigned char ch = static_cast<unsigned char>(c.arg(0) & 0xFFU);
        c.r[0] = static_cast<std::uint64_t>(std::toupper(ch));
    });
    H("tolower", [](GuestCpu& c) {
        const unsigned char ch = static_cast<unsigned char>(c.arg(0) & 0xFFU);
        c.r[0] = static_cast<std::uint64_t>(std::tolower(ch));
    });
    H("IsProcessorFeaturePresent", [](GuestCpu& c) {
        const auto f = c.arg(0); c.r[0] = (f == 10 || f == 6 || f == 12 || f == 17) ? 1 : 0; // SSE, SSE2, NX, SLAT only; no fastfail/AVX
    });
    H("TerminateProcess", [this](GuestCpu& c) { exited_ = true; exit_code_ = static_cast<int>(c.arg(1)); c.request_stop(GuestCpu::Stop::HostRequest, "TerminateProcess(exit code " + std::to_string(exit_code_) + ")"); });
    H("SetUnhandledExceptionFilter", [](GuestCpu& c) { c.r[0] = 0; });
    H("UnhandledExceptionFilter", [](GuestCpu& c) { c.r[0] = 0; });
    H("RtlLookupFunctionEntry", [this](GuestCpu& c) {
        const std::uint64_t pc = c.arg(0);
        std::uint64_t found = 0, found_base = 0;
        auto scan_image = [this, pc](std::uint64_t base, std::uint32_t image_size,
                                     std::uint32_t exception_rva, std::uint32_t exception_size) -> std::uint64_t {
            if (pc < base || pc - base >= image_size || !exception_rva || exception_size < 12) return 0;
            const std::uint64_t rva = pc - base;
            const std::uint32_t count = exception_size / 12;
            for (std::uint32_t i = 0; i < count && i < 1000000; ++i) {
                const std::uint64_t entry = base + exception_rva + static_cast<std::uint64_t>(i) * 12;
                const auto begin = mem_.read<std::uint32_t>(entry);
                const auto end = mem_.read<std::uint32_t>(entry + 4);
                if (begin <= rva && rva < end) return entry;
            }
            return 0;
        };
        found = scan_image(image_.image_base, image_.size_of_image, image_.exception_rva, image_.exception_size);
        if (found) found_base = image_.image_base;
        if (!found) {
            for (const auto& module : modules_) {
                found = scan_image(module->base, module->img.size_of_image, module->img.exception_rva, module->img.exception_size);
                if (found) { found_base = module->base; break; }
            }
        }
        if (c.arg(1)) mem_.write<std::uint64_t>(c.arg(1), found_base);
        c.r[0] = found;
    });
    H("RtlVirtualUnwind", [](GuestCpu& c) { c.r[0] = 0; });
    H("RtlCaptureContext", [this](GuestCpu& c) {
        // AMD64 CONTEXT offsets are fixed by the Windows ABI. The previous zero-fill
        // made Rip=0 and caused the CRT's own exception path to look up function data
        // for address zero, obscuring the original startup fault.
        const std::uint64_t dst = c.arg(0);
        constexpr std::uint64_t kContextBytes = 0x4D0;
        if (!dst || !mem_.is_mapped(dst, kContextBytes)) { c.r[0] = 0; return; }
        zero_fill(dst, kContextBytes);
        mem_.write<std::uint32_t>(dst + 0x30, 0x0010000FULL); // CONTEXT_AMD64 | control/integer/segments/fp/debug flags
        mem_.write<std::uint32_t>(dst + 0x34, 0x1F80);        // conventional MXCSR default
        mem_.write<std::uint16_t>(dst + 0x38, 0x33);          // guest 64-bit code selector
        mem_.write<std::uint16_t>(dst + 0x42, 0x2B);          // guest 64-bit stack selector
        mem_.write<std::uint32_t>(dst + 0x44, static_cast<std::uint32_t>(c.rflags));
        constexpr std::uint64_t gpr_offsets[16] = {
            0x78, 0x80, 0x88, 0x90, 0x98, 0xA0, 0xA8, 0xB0,
            0xB8, 0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0
        };
        for (unsigned i = 0; i < 16; ++i) {
            std::uint64_t value = c.r[i];
            if (i == 4) value += 8; // API returns to caller immediately after RtlCaptureContext.
            mem_.write<std::uint64_t>(dst + gpr_offsets[i], value);
        }
        const std::uint64_t ret_pc = mem_.is_mapped(c.r[4], 8) ? mem_.read<std::uint64_t>(c.r[4]) : c.rip;
        mem_.write<std::uint64_t>(dst + 0xF8, ret_pc);
        for (unsigned i = 0; i < 16; ++i) {
            mem_.write<std::uint64_t>(dst + 0x1A0 + i * 16, c.xmm[i].q[0]);
            mem_.write<std::uint64_t>(dst + 0x1A8 + i * 16, c.xmm[i].q[1]);
        }
        c.r[0] = 0;
    });
    H("InitializeSListHead", [this](GuestCpu& c) { zero_fill(c.arg(0), 16); });
    H("GetStartupInfoW", [this](GuestCpu& c) { zero_fill(c.arg(0), 104); mem_.write<std::uint32_t>(c.arg(0), 104); });
    H("OutputDebugStringA", [this](GuestCpu& c) { if (stdout_log_.size() < 200) stdout_log_.push_back("[OutputDebugStringA] " + mem_.read_cstring(c.arg(0))); });
    H("OutputDebugStringW", [this](GuestCpu& c) { if (stdout_log_.size() < 200) stdout_log_.push_back("[OutputDebugStringW] " + utf16_to_utf8(mem_.read_wstring(c.arg(0)))); });
    H("GetModuleHandleW", [this](GuestCpu& c) {
        if (!c.arg(0)) { c.r[0] = image_.image_base; return; }
        const std::string n = utf16_to_utf8(mem_.read_wstring(c.arg(0)));
        const std::uint64_t h = module_handle_for(n);
        if (!h) { last_error_ = 126; if (com_log_.insert("gmh:" + n).second) milestone("GetModuleHandleW(\"" + n + "\") -> NULL (not a known system or loaded module)"); }
        c.r[0] = h;
    });
    H("LoadLibraryExW", [this](GuestCpu& c) {
        const std::string n = utf16_to_utf8(mem_.read_wstring(c.arg(0)));
        const std::uint64_t h = module_handle_for(n);
        if (h) { if (com_log_.insert("ll:" + n).second) milestone("LoadLibraryExW(\"" + n + "\") -> " + (is_os_module_handle(h) || h == 0x00007FF4A0D10000ULL ? "HLE module handle" : "loaded real DLL")); c.r[0] = h; return; }
        milestone("LoadLibraryExW(\"" + n + "\") -> not available (ERROR_MOD_NOT_FOUND)");
        last_error_ = 126; c.r[0] = 0;
    });
    H("FreeLibrary", [](GuestCpu& c) { c.r[0] = 1; });
    H("GetProcAddress", [this](GuestCpu& c) {
        const bool by_ordinal = !(c.arg(1) >> 16);
        const std::string n = by_ordinal ? ("#" + std::to_string(c.arg(1))) : mem_.read_cstring(c.arg(1));
        const std::uint64_t h = c.arg(0);
        for (const auto& m : modules_) if (m->base == h) { // a real loaded DLL: resolve against its own export table
            std::uint64_t a = 0;
            if (by_ordinal) { const auto it = m->img.exports_by_ordinal.find(static_cast<std::uint16_t>(c.arg(1))); if (it != m->img.exports_by_ordinal.end()) a = m->base + it->second; }
            else { const auto it = m->img.exports_by_name.find(n); if (it != m->img.exports_by_name.end()) a = m->base + it->second; }
            if (!a) { last_error_ = 127; if (com_log_.insert("gpa:" + m->name + n).second) milestone("GetProcAddress(" + m->name + ", \"" + n + "\") -> NULL (not exported)"); }
            c.r[0] = a; return;
        }
        if (h == 0x00007FF4A0D10000ULL) { // the HLE xaudio2_9 module: every export resolves; only CreateXAudio2Object is real
            if (!handlers_.count(n)) { milestone("xaudio2_9 export \"" + n + "\" resolved to a no-op stub"); handlers_[n] = [](GuestCpu& cc) { cc.r[0] = 0; }; }
            c.r[0] = trap_for_name(n); return;
        }
        if (handlers_.count(n)) { c.r[0] = trap_for_name(n); return; }
        if (com_log_.insert("gpa:" + n).second) milestone("GetProcAddress(\"" + n + "\") -> NULL (no HLE implementation)");
        last_error_ = 127; c.r[0] = 0;
    });
    H("GetUserDefaultLocaleName", [this](GuestCpu& c) { write_wstr(c.arg(0), u"en-US"); c.r[0] = 6; });
    H("FormatMessageW", [](GuestCpu& c) { c.r[0] = 0; });
    H("GetTimeFormatEx", [](GuestCpu& c) { c.r[0] = 0; });
    H("GetDateFormatEx", [](GuestCpu& c) { c.r[0] = 0; });
    H("DeleteFileW", [this](GuestCpu& c) { std::error_code ec; c.r[0] = std::filesystem::remove(resolve_path(utf16_to_utf8(mem_.read_wstring(c.arg(0))), true), ec) ? 1 : 0; });

    // ---- time ----
    H("QueryPerformanceCounter", [this](GuestCpu& c) { qpc_ += 1000 + (c.instructions >> 6); mem_.write<std::uint64_t>(c.arg(0), qpc_); c.r[0] = 1; });
    H("GetSystemTimeAsFileTime", [this](GuestCpu& c) { filetime_ += 10000 + c.instructions; mem_.write<std::uint64_t>(c.arg(0), filetime_); });
    auto ft_to_st = [this](std::uint64_t ft, std::uint64_t dst) {
        const std::uint64_t secs = (ft / 10000000ULL) - 11644473600ULL; std::time_t tt = static_cast<std::time_t>(secs); std::tm t{};
#if defined(_WIN32)
        gmtime_s(&t, &tt);
#else
        gmtime_r(&tt, &t);
#endif
        const std::uint16_t f[8] = {static_cast<std::uint16_t>(t.tm_year + 1900), static_cast<std::uint16_t>(t.tm_mon + 1), static_cast<std::uint16_t>(t.tm_wday), static_cast<std::uint16_t>(t.tm_mday),
            static_cast<std::uint16_t>(t.tm_hour), static_cast<std::uint16_t>(t.tm_min), static_cast<std::uint16_t>(t.tm_sec), static_cast<std::uint16_t>((ft / 10000) % 1000)};
        for (int i = 0; i < 8; ++i) mem_.write<std::uint16_t>(dst + static_cast<std::uint64_t>(i) * 2, f[i]);
    };
    H("GetSystemTime", [this, ft_to_st](GuestCpu& c) { ft_to_st(filetime_, c.arg(0)); });
    H("GetLocalTime", [this, ft_to_st](GuestCpu& c) { ft_to_st(filetime_, c.arg(0)); });
    H("FileTimeToSystemTime", [this, ft_to_st](GuestCpu& c) { ft_to_st(mem_.read<std::uint64_t>(c.arg(0)), c.arg(1)); c.r[0] = 1; });
    H("SystemTimeToFileTime", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(1), filetime_); c.r[0] = 1; });

    // ---- synchronization (single-threaded execution model) ----
    // Windows x64 CRITICAL_SECTION-shaped layout: DebugInfo +0, LockCount +8,
    // RecursionCount +12, OwningThread +16, LockSemaphore +24, SpinCount +32.
    auto init_critical_section = [this](std::uint64_t address, std::uint64_t spin_count) {
        if (!address || !mem_.is_mapped(address, 40)) return;
        mem_.write<std::uint64_t>(address + 0, 0);
        mem_.write<std::int32_t>(address + 8, -1);
        mem_.write<std::int32_t>(address + 12, 0);
        mem_.write<std::uint64_t>(address + 16, 0);
        mem_.write<std::uint64_t>(address + 24, 0);
        mem_.write<std::uint64_t>(address + 32, spin_count);
    };
    H("InitializeCriticalSection", [init_critical_section](GuestCpu& c) { init_critical_section(c.arg(0), 0); });
    H("InitializeCriticalSectionAndSpinCount", [init_critical_section](GuestCpu& c) { init_critical_section(c.arg(0), c.arg(1)); c.r[0] = 1; });
    H("InitializeCriticalSectionEx", [init_critical_section](GuestCpu& c) { init_critical_section(c.arg(0), c.arg(2)); c.r[0] = 1; });
    H("DeleteCriticalSection", [](GuestCpu&) {});
    H("EnterCriticalSection", [this](GuestCpu& c) {
        const auto a = c.arg(0); if (!a || !mem_.is_mapped(a, 40)) return;
        const auto owner = mem_.read<std::uint64_t>(a + 16);
        const auto recursion = mem_.read<std::int32_t>(a + 12);
        mem_.write<std::int32_t>(a + 8, mem_.read<std::int32_t>(a + 8) + 1);
        mem_.write<std::uint64_t>(a + 16, owner ? owner : 1ULL);
        mem_.write<std::int32_t>(a + 12, recursion + 1);
    });
    H("LeaveCriticalSection", [this](GuestCpu& c) {
        const auto a = c.arg(0); if (!a || !mem_.is_mapped(a, 40)) return;
        const auto recursion = mem_.read<std::int32_t>(a + 12);
        if (recursion <= 1) {
            mem_.write<std::int32_t>(a + 12, 0); mem_.write<std::uint64_t>(a + 16, 0);
            mem_.write<std::int32_t>(a + 8, -1);
        } else {
            mem_.write<std::int32_t>(a + 12, recursion - 1);
            mem_.write<std::int32_t>(a + 8, mem_.read<std::int32_t>(a + 8) - 1);
        }
    });
    H("TryEnterCriticalSection", [this](GuestCpu& c) {
        const auto a = c.arg(0); if (!a || !mem_.is_mapped(a, 40)) { c.r[0] = 0; return; }
        const auto owner = mem_.read<std::uint64_t>(a + 16);
        if (owner && owner != 1) { c.r[0] = 0; return; }
        mem_.write<std::uint64_t>(a + 16, 1);
        mem_.write<std::int32_t>(a + 12, mem_.read<std::int32_t>(a + 12) + 1);
        mem_.write<std::int32_t>(a + 8, mem_.read<std::int32_t>(a + 8) + 1);
        c.r[0] = 1;
    });
    for (const char* n : {"AcquireSRWLockExclusive", "ReleaseSRWLockExclusive", "WakeAllConditionVariable"}) H(n, [](GuestCpu&) {});
    H("InitializeConditionVariable", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(0), 0); });
    H("SleepConditionVariableCS", [this](GuestCpu& c) {
        milestone("SleepConditionVariableCS called: would block forever without other threads (returned timeout)");
        last_error_ = 1460; c.r[0] = 0;
    });
    H("SetEvent", [](GuestCpu& c) { c.r[0] = 1; }); H("ResetEvent", [](GuestCpu& c) { c.r[0] = 1; });
    H("CloseHandle", [this](GuestCpu& c) {
        const auto handle = c.arg(0);
        const bool known = semaphores_.erase(handle) != 0 || mutex_handles_.erase(handle) != 0;
        c.r[0] = (known || handle >= 0xDEAD1000ULL) ? 1 : 0;
        last_error_ = c.r[0] ? 0 : 6;
    });
    auto wait_single_object = [this](GuestCpu& c) {
        const auto handle = c.arg(0);
        const auto it = semaphores_.find(handle);
        if (it != semaphores_.end() && it->second.count > 0) {
            --it->second.count;
            c.r[0] = 0; // WAIT_OBJECT_0
        } else if (it != semaphores_.end() && c.arg(1) == 0) {
            c.r[0] = 0x102; // WAIT_TIMEOUT for a zero-time poll on an unsignaled semaphore
        } else {
            // Other guest threads are not scheduled yet; treating waits as satisfied
            // avoids deadlocking legacy single-threaded paths on synchronization they
            // expect a background thread to signal.
            c.r[0] = 0;
        }
        if (com_log_.insert("wait:" + GuestCpu::hex(handle)).second)
            milestone("WaitForSingleObject[Ex](0x" + GuestCpu::hex(handle) + ") handled by the single-threaded synchronization model");
    };
    H("WaitForSingleObject", wait_single_object);
    H("WaitForSingleObjectEx", wait_single_object);
    H("CreateThread", [this](GuestCpu& c) {
        ++threads_created_;
        milestone("CreateThread requested: start=" + locate(c.arg(2)) + " (thread NOT run: this runner is single-threaded)");
        if (c.arg(5)) mem_.write<std::uint32_t>(c.arg(5), 0x1000 + threads_created_);
        c.r[0] = 0x8000 + threads_created_;
    });

    // ---- ETW ----
    for (const char* n : {"EventRegister", "EventUnregister", "EventWrite", "EtxRegister", "EtxEventWrite", "EtxFillCommonFields_v7"}) H(n, [](GuestCpu& c) { c.r[0] = 0; });

    // ---- COM / WinRT string helpers (combase) ----
    H("CoTaskMemAlloc", [this](GuestCpu& c) { c.r[0] = heap_alloc(c.arg(0), false); });
    H("CoTaskMemFree", [this](GuestCpu& c) { heap_free(c.arg(0)); });
    H("CoCreateGuid", [this](GuestCpu& c) { for (int i = 0; i < 4; ++i) mem_.write<std::uint32_t>(c.arg(0) + static_cast<std::uint64_t>(i) * 4, static_cast<std::uint32_t>(0x1234567 * (i + 1) + rand_state_++)); c.r[0] = 0; });
    // HSTRING = pointer to {u32 len; u32 flags; u16 data[]} block (a simplified, self-consistent layout).
    H("WindowsCreateString", [this](GuestCpu& c) {
        const std::uint64_t src = c.arg(0), len = c.arg(1) & 0xFFFFFFFFULL, out = c.arg(2);
        if (len == 0) { mem_.write<std::uint64_t>(out, 0); c.r[0] = 0; return; }
        const std::uint64_t h = heap_alloc(8 + (len + 1) * 2, true);
        if (!h) { c.r[0] = 0x8007000EULL; return; }
        mem_.write<std::uint32_t>(h, static_cast<std::uint32_t>(len));
        for (std::uint64_t i = 0; i < len; ++i) mem_.write<std::uint16_t>(h + 8 + i * 2, mem_.read<std::uint16_t>(src + i * 2));
        mem_.write<std::uint64_t>(out, h); c.r[0] = 0;
    });
    H("WindowsCreateStringReference", [this](GuestCpu& c) { // (src, len, header*, out*)
        const std::uint64_t src = c.arg(0), len = c.arg(1) & 0xFFFFFFFFULL, hdr = c.arg(2), out = c.arg(3);
        if (len == 0) { mem_.write<std::uint64_t>(out, 0); c.r[0] = 0; return; }
        // HSTRING_HEADER is 24 bytes: store the length and a pointer to the caller-owned buffer.
        mem_.write<std::uint32_t>(hdr, 1); mem_.write<std::uint32_t>(hdr + 4, static_cast<std::uint32_t>(len)); mem_.write<std::uint64_t>(hdr + 8, src); mem_.write<std::uint64_t>(hdr + 16, src);
        mem_.write<std::uint64_t>(out, hdr); c.r[0] = 0;
    });
    H("WindowsDeleteString", [](GuestCpu& c) { c.r[0] = 0; });
    H("WindowsDuplicateString", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(1), c.arg(0)); c.r[0] = 0; });
    H("WindowsGetStringRawBuffer", [this](GuestCpu& c) {
        const std::uint64_t h = c.arg(0), lenp = c.arg(1);
        if (!h) { if (lenp) mem_.write<std::uint32_t>(lenp, 0); c.r[0] = 0; return; }
        const bool is_ref = mem_.read<std::uint32_t>(h) == 1 && h >= xrt::kStackTop - xrt::kStackSize;
        if (is_ref) { if (lenp) mem_.write<std::uint32_t>(lenp, mem_.read<std::uint32_t>(h + 4)); c.r[0] = mem_.read<std::uint64_t>(h + 8); }
        else { if (lenp) mem_.write<std::uint32_t>(lenp, mem_.read<std::uint32_t>(h)); c.r[0] = h + 8; }
    });
    H("WindowsCompareStringOrdinal", [this](GuestCpu& c) {
        auto get = [this](std::uint64_t h) { if (!h) return std::u16string(); const bool is_ref = mem_.read<std::uint32_t>(h) == 1 && h >= xrt::kStackTop - xrt::kStackSize;
            if (is_ref) { std::u16string s; const auto n = mem_.read<std::uint32_t>(h + 4); const auto p = mem_.read<std::uint64_t>(h + 8); for (std::uint32_t i = 0; i < n; ++i) s.push_back(static_cast<char16_t>(mem_.read<std::uint16_t>(p + i * 2))); return s; }
            return mem_.read_wstring(h + 8, mem_.read<std::uint32_t>(h)); };
        const auto a = get(c.arg(0)), b = get(c.arg(1));
        mem_.write<std::int32_t>(c.arg(2), a < b ? -1 : (a == b ? 0 : 1)); c.r[0] = 0;
    });
    H("StringFromGUID2", [this](GuestCpu& c) { write_wstr(c.arg(1), u"{00000000-0000-0000-0000-000000000000}"); c.r[0] = 39; });
    H("RoReportUnhandledError", [](GuestCpu& c) { c.r[0] = 0; });
    H("RoFailFastWithErrorContext", [](GuestCpu& c) { c.request_stop(GuestCpu::Stop::HostRequest, "RoFailFastWithErrorContext(hr=0x" + GuestCpu::hex(c.arg(0)) + ") - WinRT fail-fast"); });
    H("SetRestrictedErrorInfo", [](GuestCpu& c) { c.r[0] = 0; });

    // ---- graphics boundary: explicit, honest stop ----
    H("D3D11CreateDevice", [this](GuestCpu& c) { // (adapter, driver, software, flags, levels*, nlevels, sdk, ppDevice, pFeatureLevel, ppContext)
        milestone("D3D11CreateDevice(driver=" + std::to_string(c.arg(1)) + ", flags=0x" + GuestCpu::hex(c.arg(3)) + ") -> software-capture device (no GPU: draws are counted, not rasterised)");
        if (!d3d_device_) { d3d_context_ = 0; d3d_device_ = d3d_create_device(); d3d_context_ = d3d_create_context(); d3d_create_dxgi(); }
        for (auto o : {d3d_device_, d3d_context_}) mem_.write<std::uint64_t>(o + 8, mem_.read<std::uint64_t>(o + 8) + 1);
        if (c.arg(7)) mem_.write<std::uint64_t>(c.arg(7), d3d_device_);
        if (c.arg(8)) mem_.write<std::uint32_t>(c.arg(8), 0xB100);
        if (c.arg(9)) mem_.write<std::uint64_t>(c.arg(9), d3d_context_);
        c.r[0] = 0;
    });
}

inline void XboxTitleRuntime::register_cpp_handlers() {
    // C++ runtime pieces that must not silently "succeed".
    H("_CxxThrowException", [this](GuestCpu& c) {
        std::string why = "guest threw a C++ exception (object=" + locate(c.arg(0)) + ", throwinfo=" + locate(c.arg(1)) + "); C++/SEH unwinding is not implemented";
        if (!last_discovery_.empty())
            why += ". Most likely cause: WinRT " + last_discovery_ + " is not implemented (returned E_NOTIMPL, which C++/CX turns into an exception)";
        else if (!winrt_requests_.empty() && last_winrt_hr_ != 0)
            why += ". Most likely cause: the Windows Runtime class \"" + last_winrt_class_ + "\" is not emulated (RoGetActivationFactory returned 0x" + GuestCpu::hex(last_winrt_hr_) + ")";
        c.request_stop(GuestCpu::Stop::HostRequest, why);
    });
    for (const char* n : {"__std_terminate", "terminate", "_purecall", "_invalid_parameter_noinfo_noreturn"})
        H(n, [this, n](GuestCpu& c) { c.request_stop(GuestCpu::Stop::HostRequest, std::string("title called ") + n + " (fatal runtime condition)"); });
    H("_invalid_parameter_noinfo", [](GuestCpu&) {});
    H("__CxxFrameHandler3", [](GuestCpu& c) { c.r[0] = 1; });
    H("__C_specific_handler", [](GuestCpu& c) { c.r[0] = 1; });
    H("__std_exception_copy", [this](GuestCpu& c) { // (src {const char*, bool}, dst)
        const std::uint64_t src = c.arg(0), dst = c.arg(1);
        mem_.write<std::uint64_t>(dst, mem_.read<std::uint64_t>(src)); mem_.write<std::uint64_t>(dst + 8, 0);
    });
    H("__std_exception_destroy", [](GuestCpu&) {});
    H("__std_type_info_compare", [this](GuestCpu& c) {
        const auto a = mem_.read_cstring(c.arg(0) + 16), b = mem_.read_cstring(c.arg(1) + 16);
        c.r[0] = static_cast<std::uint32_t>(a < b ? -1 : (a == b ? 0 : 1));
    });
    // MSVC STL: locking and failure helpers
    H("??0_Lockit@std@@QEAA@H@Z", [](GuestCpu& c) { c.r[0] = c.arg(0); });
    H("??1_Lockit@std@@QEAA@XZ", [](GuestCpu&) {});
    for (const char* n : {"?_Xlength_error@std@@YAXPEBD@Z", "?_Xout_of_range@std@@YAXPEBD@Z", "?_Xbad_alloc@std@@YAXXZ",
                          "?_Xinvalid_argument@std@@YAXPEBD@Z", "?_Xruntime_error@std@@YAXPEBD@Z", "?_Xbad_function_call@std@@YAXXZ"})
        H(n, [this, n](GuestCpu& c) { c.request_stop(GuestCpu::Stop::HostRequest, std::string("STL failure helper called: ") + n); });
    // Platform::Details heap (C++/CX runtime in vccorlib140)
    H("?Allocate@Heap@Details@Platform@@SAPEAX_K@Z", [this](GuestCpu& c) { c.r[0] = heap_alloc(c.arg(0), true); });
    H("?Allocate@Heap@Details@Platform@@SAPEAX_K0@Z", [this](GuestCpu& c) { c.r[0] = heap_alloc(c.arg(0), true); });
    H("?Free@Heap@Details@Platform@@SAXPEAX@Z", [this](GuestCpu& c) { heap_free(c.arg(0)); });
    H("?AlignedFree@Heap@Details@Platform@@SAXPEAX@Z", [this](GuestCpu& c) { heap_free(c.arg(0)); });
    H("?InitializeData@Details@Platform@@YAJH@Z", [](GuestCpu& c) { c.r[0] = 0; });
    H("?UninitializeData@Details@Platform@@YAXH@Z", [](GuestCpu&) {});
    H("?GetCmdArguments@Details@Platform@@YAPEAPEA_WPEAH@Z", [this](GuestCpu& c) { if (c.arg(0)) mem_.write<std::int32_t>(c.arg(0), 0); c.r[0] = 0; });
    H("?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@PEAPEAX@Z", [this](GuestCpu& c) {
        const std::string n = utf16_to_utf8(mem_.read_wstring(c.arg(0)));
        milestone("WinRT activation factory requested: \"" + n + "\" - WinRT (CoreApplication/CoreWindow) is not emulated; stopping here");
        c.request_stop(GuestCpu::Stop::HostRequest, "WinRT boundary reached: activation factory \"" + n + "\" requested (CoreApplication runtime is not emulated yet)");
    });
}

inline void XboxTitleRuntime::finish(TitleLaunchReport& rep) {
    if (d3d_device_) d3d_save_final_frame();
    rep.instructions = cpu_.instructions;
    rep.exited = exited_; rep.exit_code = exit_code_;
    rep.stop_rip = cpu_.fault_rip ? cpu_.fault_rip : cpu_.rip;
    rep.stop_location = locate(rep.stop_rip);
    if (cpu_.stop == GuestCpu::Stop::MemoryFault) {
        rep.has_fault_address = true;
        rep.fault_address = cpu_.last_fault.address;
    }
    {
        std::ostringstream byte_text;
        for (std::uint64_t i = 0; i < 16; ++i) {
            const std::uint64_t at = rep.stop_rip + i;
            if (at < rep.stop_rip || !mem_.is_mapped(at, 1)) break;
            if (i) byte_text << ' ';
            byte_text << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(mem_.read<std::uint8_t>(at));
        }
        rep.stop_instruction_bytes = byte_text.str();
    }
    switch (cpu_.stop) {
    case GuestCpu::Stop::HostRequest: rep.stop_kind = exited_ ? "exit" : "host"; break;
    case GuestCpu::Stop::MemoryFault: rep.stop_kind = "fault"; break;
    case GuestCpu::Stop::Unsupported: rep.stop_kind = "unsupported"; break;
    case GuestCpu::Stop::StepLimit: rep.stop_kind = "limit"; break;
    case GuestCpu::Stop::None: rep.stop_kind = exited_ ? "exit" : "none"; break;
    default: rep.stop_kind = "fault"; break;
    }
    rep.stop_detail = cpu_.stop == GuestCpu::Stop::None ? "entry point returned" : cpu_.stop_detail;
    rep.milestones = milestones_;
    rep.files_opened = files_opened_;
    for (const auto& s : stubs_hit_) rep.stubbed_imports_called.push_back(s);
    for (const auto& b : bindings_) if (b.calls) call_counts_[b.name] += b.calls;   // per-import counters are kept on the binding; aggregate by name for the report
    std::vector<std::pair<std::string, std::uint64_t>> calls(call_counts_.begin(), call_counts_.end());
    std::sort(calls.begin(), calls.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    rep.hle_calls = calls;
    rep.last_calls = ring_lines();
    rep.heap_bytes = heap_live_;
    rep.guest_memory_bytes = mem_.used_bytes();
    rep.frames = frames_; rep.frames_saved = d3d_frames_saved_;
    if (d3d_device_) {
        rep.graphics.push_back("main-loop iterations: " + std::to_string(frames_) + ", presents: " + std::to_string(d3d_present_count_) + ", draw calls (counted, not rasterised): " + std::to_string(d3d_draws_) + ", frames saved: " + std::to_string(d3d_frames_saved_));
        for (const auto& kv : d3d_res_) {
            const auto& r = kv.second;
            if (r.kind != "texture2d") continue;
            std::uint64_t nonzero = 0, total = 0;
            if (r.data && r.size && r.size < (1ULL << 26)) { std::vector<std::uint8_t> buf(static_cast<std::size_t>(r.size)); try { mem_.read_bytes(r.data, buf.data(), buf.size()); } catch (...) {} total = buf.size(); for (auto b : buf) if (b) ++nonzero; }
            rep.graphics.push_back("texture " + std::to_string(r.width) + "x" + std::to_string(r.height) + " fmt " + std::to_string(r.format) + " usage " + std::to_string(r.usage) + ": written " + std::to_string(r.writes) + "x, " + (total ? std::to_string(nonzero * 100 / total) + "% non-zero bytes" : std::string("no CPU backing (GPU-only)")));
        }
        std::string calls; for (const auto& kv : d3d_calls_) calls += kv.first + "=" + std::to_string(kv.second) + " "; rep.graphics.push_back("D3D calls: " + calls);
        std::string fast; unsigned n = 0; std::vector<std::pair<std::uint64_t, std::uint64_t>> fv(d3d_fast_calls_.begin(), d3d_fast_calls_.end());
        std::sort(fv.begin(), fv.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& kv : fv) { if (n++ >= 14) break; fast += "+0x" + GuestCpu::hex(kv.first) + "=" + std::to_string(kv.second) + " "; }
        rep.graphics.push_back("D3D11.x fast-context call counts (offset=calls, top 14): " + fast);
    }
    if (audio_engine_) rep.graphics.push_back("audio: Start() calls " + std::to_string(audio_start_calls_) + ", source buffers submitted " + std::to_string(audio_buffers_submitted_) + " (" + std::to_string(audio_bytes_submitted_) + " bytes, mixed)");
    if (!caller_hist_.empty()) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> top(caller_hist_.begin(), caller_hist_.end());
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (std::size_t i = 0; i < top.size() && i < 8; ++i) rep.graphics.push_back("callers of " + caller_watch_ + ": " + std::to_string(top[i].second) + "x from " + locate(top[i].first));
    }
    for (const auto& kv : d3d_shader_formats_) rep.graphics.push_back("shader bytecode format '" + kv.first + "': " + std::to_string(kv.second) + " shaders");
    if (audio_started_) rep.graphics.push_back("audio: mixed " + std::to_string(audio_frames_mixed_ / AudioMixer::kOutRate) + "." + std::to_string((audio_frames_mixed_ % AudioMixer::kOutRate) / 4800) + " s of 48 kHz stereo to " + std::string(audio_sink_ ? audio_sink_->name() : "none") + (audio_sink_ && audio_sink_->frames_dropped() ? " (" + std::to_string(audio_sink_->frames_dropped()) + " frames dropped: the emulator is ahead of the sound device)" : ""));
    if (pacer_configured_) rep.graphics.push_back("frame pacing: lock " + (opt_.fps_limit ? std::to_string(opt_.fps_limit) + " FPS" : std::string("off")) + ", measured " + std::to_string(static_cast<int>(pacer_.fps() + 0.5)) + " FPS over the last half second");
    // Count only real PE imports (not COM vtable traps, fast-context traps or GetProcAddress-created traps).
    rep.imports_total = 0; rep.imports_implemented = 0;
    for (const auto& b : bindings_) {
        if (b.dll == "(GetProcAddress)" || b.name.rfind("COM:", 0) == 0 || b.name.rfind("CtxFast", 0) == 0) continue;
        ++rep.imports_total;
        if (b.handler) ++rep.imports_implemented;
    }
    rep.real_dll_imports = real_bound_;
    static const char* names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    for (int i = 0; i < 16; ++i) rep.registers.push_back(std::string(names[i]) + "=" + GuestCpu::hex(cpu_.r[static_cast<std::size_t>(i)]));
    rep.registers.push_back("rip=" + GuestCpu::hex(cpu_.rip));
    rep.registers.push_back("rflags=" + GuestCpu::hex(cpu_.rflags));
    // Heuristic backtrace: stack words that look like return addresses after a CALL.
    const std::uint64_t base = image_.image_base;
    for (std::uint64_t i = 0; i < 256 && rep.backtrace.size() < 14; ++i) {
        std::uint64_t v;
        try { v = mem_.read<std::uint64_t>(cpu_.r[4] + i * 8); } catch (...) { break; }
        if (v < base + 0x1000 || v >= base + image_.size_of_image) continue;
        try {
            const bool direct = mem_.read<std::uint8_t>(v - 5) == 0xE8;
            const bool ind = mem_.read<std::uint8_t>(v - 6) == 0xFF && mem_.read<std::uint8_t>(v - 5) == 0x15;
            const bool reg = mem_.read<std::uint8_t>(v - 2) == 0xFF && (mem_.read<std::uint8_t>(v - 1) & 0x38) == 0x10;
            if (direct || ind || reg) rep.backtrace.push_back("[rsp+0x" + GuestCpu::hex(i * 8) + "] returns to " + locate(v));
        } catch (...) {}
    }
    for (const auto& s : stdout_log_) { if (rep.last_calls.size() < 100) rep.last_calls.push_back("stdout/debug: " + s); }
    rep.text = render(rep);
}

inline std::string XboxTitleRuntime::render(const TitleLaunchReport& r) const {
    std::ostringstream o;
    o << "=== SomeBoxOne title launch report ===\n";
    if (!r.image_loaded) { o << "Image NOT loaded: " << r.load_error << "\n"; return o.str(); }
    o << "Executable      : AMD64 PE, base 0x" << GuestCpu::hex(image_.image_base) << ", entry RVA 0x" << GuestCpu::hex(image_.entry_rva)
      << ", image size 0x" << GuestCpu::hex(image_.size_of_image) << ", " << image_.sections.size() << " sections\n";
    o << "Imports         : " << r.imports_total << " bound to HLE (" << r.imports_implemented << " with real handlers, "
      << (r.imports_total - r.imports_implemented) << " unimplemented stubs) + " << r.real_dll_imports << " bound to real DLL exports\n";
    o << "Guest execution : " << r.instructions << " x86-64 instructions retired in the SomeBoxOne interpreter\n";
    o << "Stopped because : [" << r.stop_kind << "] " << r.stop_detail << "\n";
    o << "Stopped at      : " << r.stop_location << "\n";
    if (!r.stop_instruction_bytes.empty())
        o << "Bytes at stop   : " << r.stop_instruction_bytes << " (up to 16 bytes at the stopped RIP)\n";
    if (r.has_fault_address)
        o << "Fault address   : 0x" << GuestCpu::hex(r.fault_address) << " (unmapped guest memory access)\n";
    if (r.exited) o << "Exit code       : " << r.exit_code << "\n";
    o << "Memory          : " << (r.guest_memory_bytes >> 20) << " MiB guest memory mapped, " << (r.heap_bytes >> 10) << " KiB live guest heap\n";
    o << "\nMilestones:\n";
    for (const auto& m : r.milestones) o << "  - " << m << "\n";
    if (!r.files_opened.empty()) { o << "\nFile access (sandboxed to the package folder):\n"; for (const auto& f : r.files_opened) o << "  - " << f << "\n"; }
    if (!r.stubbed_imports_called.empty()) {
        o << "\nUnimplemented imports that WERE called (returned 0 - behavior not emulated):\n";
        for (const auto& s : r.stubbed_imports_called) o << "  - " << s << "\n";
    }
    o << "\nMost-called HLE imports:\n";
    for (std::size_t i = 0; i < r.hle_calls.size() && i < 12; ++i) o << "  " << r.hle_calls[i].second << "x " << r.hle_calls[i].first << "\n";
    o << "\nLast HLE calls before stop:\n";
    for (std::size_t i = r.last_calls.size() > 12 ? r.last_calls.size() - 12 : 0; i < r.last_calls.size(); ++i) o << "  " << r.last_calls[i] << "\n";
    if (!r.graphics.empty()) { o << "\nGraphics / audio / frames:\n"; for (const auto& g : r.graphics) o << "  - " << g << "\n"; }
    if (!r.backtrace.empty()) { o << "\nApproximate call stack (return-address scan):\n"; for (const auto& b : r.backtrace) o << "  " << b << "\n"; }
    o << "\nRegisters: ";
    for (const auto& s : r.registers) o << s << " ";
    o << "\n";
    return o.str();
}

} // namespace someboxone
