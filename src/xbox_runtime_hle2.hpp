#pragma once
// Additional HLE handlers: CRT locale/ctype internals, conversion APIs and the kernel
// services needed by the real MSVC redistributable DLLs (msvcp140, concrt140).

namespace someboxone {

inline void XboxTitleRuntime::register_extra_handlers() {
    // ---- allocator aliases ----
    H("_malloc_base", [this](GuestCpu& c) { c.r[0] = heap_alloc(c.arg(0), false); });
    H("_calloc_base", [this](GuestCpu& c) { c.r[0] = heap_alloc(c.arg(0) * c.arg(1), true); });
    H("_realloc_base", [this](GuestCpu& c) { c.r[0] = heap_realloc(c.arg(0), c.arg(1)); });
    H("_free_base", [this](GuestCpu& c) { heap_free(c.arg(0)); });
    H("_wcsdup", [this](GuestCpu& c) { const auto w = mem_.read_wstring(c.arg(0)); const auto p = heap_alloc((w.size() + 1) * 2, false); write_wstr(p, w); c.r[0] = p; });
    H("wcscpy_s", [this](GuestCpu& c) { const auto w = mem_.read_wstring(c.arg(2)); if (w.size() + 1 > c.arg(1)) { c.r[0] = 34; return; } write_wstr(c.arg(0), w); c.r[0] = 0; });
    H("__strncnt", [this](GuestCpu& c) { std::uint64_t n = 0; while (n < c.arg(1) && mem_.read<std::uint8_t>(c.arg(0) + n)) ++n; c.r[0] = n; });
    H("strcspn", [this](GuestCpu& c) { const auto a = mem_.read_cstring(c.arg(0)), b = mem_.read_cstring(c.arg(1)); const auto p = a.find_first_of(b); c.r[0] = p == std::string::npos ? a.size() : p; });
    H("strtod", [this](GuestCpu& c) { const auto s = mem_.read_cstring(c.arg(0)); char* e; const double v = std::strtod(s.c_str(), &e); if (c.arg(1)) mem_.write<std::uint64_t>(c.arg(1), c.arg(0) + static_cast<std::uint64_t>(e - s.c_str())); setd(c, v); });
    H("strtof", [this](GuestCpu& c) { const auto s = mem_.read_cstring(c.arg(0)); char* e; const float v = std::strtof(s.c_str(), &e); if (c.arg(1)) mem_.write<std::uint64_t>(c.arg(1), c.arg(0) + static_cast<std::uint64_t>(e - s.c_str())); setf(c, v); });
    H("frexp", [this](GuestCpu& c) { int e = 0; const double v = std::frexp(getd(c), &e); mem_.write<std::int32_t>(c.r[2], e); setd(c, v); });
    H("log", [](GuestCpu& c) { setd(c, std::log(getd(c))); });
    H("powf", [](GuestCpu& c) { setf(c, std::pow(getf(c, 0), getf(c, 1))); });
    H("abort", [](GuestCpu& c) { c.request_stop(GuestCpu::Stop::HostRequest, "title called abort()"); });
    H("fputs", [this](GuestCpu& c) { const auto s = mem_.read_cstring(c.arg(0)); std::FILE* fp = file_of(c.arg(1)); if (fp) std::fputs(s.c_str(), fp); else if (stdout_log_.size() < 200) stdout_log_.push_back(s); c.r[0] = 1; });
    H("rand_s", [this](GuestCpu& c) { rand_state_ = rand_state_ * 6364136223846793005ULL + 1442695040888963407ULL; mem_.write<std::uint32_t>(c.arg(0), static_cast<std::uint32_t>(rand_state_ >> 32)); c.r[0] = 0; });
    H("_set_new_handler", [](GuestCpu& c) { c.r[0] = 0; });
    H("_seh_filter_dll", [](GuestCpu& c) { c.r[0] = 0; });
    H("_execute_onexit_table", [](GuestCpu& c) { c.r[0] = 0; });
    H("_Strftime", [](GuestCpu& c) { c.r[0] = 0; }); H("_Wcsftime", [](GuestCpu& c) { c.r[0] = 0; });
    H("_beginthreadex", [this](GuestCpu& c) { ++threads_created_; milestone("_beginthreadex requested: start=" + locate(c.arg(2)) + " (thread NOT run: single-threaded runner)"); if (c.arg(5)) mem_.write<std::uint32_t>(c.arg(5), 0x1000 + threads_created_); c.r[0] = 0x8000 + threads_created_; });
    H("_endthreadex", [](GuestCpu& c) { c.request_stop(GuestCpu::Stop::HostRequest, "_endthreadex called on the main guest thread"); });


    // ---- vcruntime exception-support helpers (no unwinding; just consistent bookkeeping) ----
    H("__AdjustPointer", [this](GuestCpu& c) { // (obj, const PMD& {i32 mdisp, pdisp, vdisp})
        std::uint64_t p = c.arg(0); const std::uint64_t pmd = c.arg(1);
        p += static_cast<std::uint64_t>(static_cast<std::int64_t>(mem_.read<std::int32_t>(pmd)));
        const std::int32_t pdisp = mem_.read<std::int32_t>(pmd + 4), vdisp = mem_.read<std::int32_t>(pmd + 8);
        if (pdisp >= 0) {
            const std::uint64_t vbptr = mem_.read<std::uint64_t>(c.arg(0) + static_cast<std::uint64_t>(pdisp));
            p += static_cast<std::uint64_t>(pdisp) + static_cast<std::uint64_t>(static_cast<std::int64_t>(mem_.read<std::int32_t>(vbptr + static_cast<std::uint64_t>(static_cast<std::int64_t>(vdisp)))));
        }
        c.r[0] = p;
    });
    H("__current_exception", [this](GuestCpu& c) { if (!cur_exc_) cur_exc_ = static_alloc(16); c.r[0] = cur_exc_; });
    H("__current_exception_context", [this](GuestCpu& c) { if (!cur_exc_) cur_exc_ = static_alloc(16); c.r[0] = cur_exc_ + 8; });
    H("__processing_throw", [this](GuestCpu& c) { if (!proc_throw_) proc_throw_ = static_alloc(8); c.r[0] = proc_throw_; });
    H("__uncaught_exceptions", [](GuestCpu& c) { c.r[0] = 0; });
    H("__uncaught_exception", [](GuestCpu& c) { c.r[0] = 0; });
    H("__std_type_info_destroy_list", [](GuestCpu&) {});


    // ---- heap API ----
    H("GetProcessHeap", [](GuestCpu& c) { c.r[0] = 0x00000000DEAD0000ULL; });
    H("HeapAlloc", [this](GuestCpu& c) { c.r[0] = heap_alloc(c.arg(2), (c.arg(1) & 8) != 0); });
    H("HeapFree", [this](GuestCpu& c) { heap_free(c.arg(2)); c.r[0] = 1; });
    H("_aligned_offset_malloc", [this](GuestCpu& c) { c.r[0] = heap_alloc(c.arg(0), false); }); // 16-byte-aligned heap covers common requests
    H("_aligned_free", [this](GuestCpu& c) { heap_free(c.arg(0)); });
    H("_invoke_watson", [](GuestCpu& c) { c.request_stop(GuestCpu::Stop::HostRequest, "_invoke_watson: CRT fatal error (invalid parameter)"); });
    H("AcquireSRWLockShared", [](GuestCpu&) {}); H("ReleaseSRWLockShared", [](GuestCpu&) {});
    H("TryAcquireSRWLockShared", [](GuestCpu& c) { c.r[0] = 1; });
    H("DisableThreadLibraryCalls", [](GuestCpu& c) { c.r[0] = 1; });
    H("GetThreadTimes", [this](GuestCpu& c) { for (unsigned i = 2; i < 6; ++i) { const auto p = c.arg(i); if (p) mem_.write<std::uint64_t>(p, i == 2 ? 0 : c.instructions); } c.r[0] = 1; });
    H("_SetWinRTOutOfMemoryExceptionCallback", [](GuestCpu&) {});
    H("__GetPlatformExceptionInfo", [](GuestCpu& c) { c.r[0] = 0; });

    // ---- combase / WinRT (this is the emulation boundary for the Windows Runtime) ----
    H("RoInitialize", [](GuestCpu& c) { c.r[0] = 0; });
    H("RoUninitialize", [](GuestCpu&) {});
    H("RoRegisterForApartmentShutdown", [this](GuestCpu& c) { if (c.arg(2)) mem_.write<std::uint64_t>(c.arg(2), 1); c.r[0] = 0; });
    H("RoUnregisterForApartmentShutdown", [](GuestCpu& c) { c.r[0] = 0; });
    H("RoGetApartmentIdentifier", [this](GuestCpu& c) { if (c.arg(0)) mem_.write<std::uint64_t>(c.arg(0), 1); c.r[0] = 0; });
    H("CoGetApartmentType", [this](GuestCpu& c) { if (c.arg(0)) mem_.write<std::uint32_t>(c.arg(0), 1); if (c.arg(1)) mem_.write<std::uint32_t>(c.arg(1), 0); c.r[0] = 0; });
    H("CoGetObjectContext", [](GuestCpu& c) { c.r[0] = 0x80004002ULL; });
    H("GetRestrictedErrorInfo", [this](GuestCpu& c) { if (c.arg(0)) mem_.write<std::uint64_t>(c.arg(0), 0); c.r[0] = 1; });
    H("RoOriginateError", [](GuestCpu& c) { c.r[0] = 1; });
    H("RoTransformError", [](GuestCpu& c) { c.r[0] = 1; });
    H("WindowsGetStringLen", [this](GuestCpu& c) { c.r[0] = hstring_len(c.arg(0)); });
    H("WindowsIsStringEmpty", [this](GuestCpu& c) { c.r[0] = hstring_len(c.arg(0)) == 0 ? 1 : 0; });
    H("WindowsStringHasEmbeddedNull", [this](GuestCpu& c) { mem_.write<std::int32_t>(c.arg(1), 0); c.r[0] = 0; });
    H("SysAllocStringLen", [this](GuestCpu& c) { const auto n = c.arg(1) & 0xFFFFFFFFULL; const auto p = heap_alloc(n * 2 + 8, true) + 4; mem_.write<std::uint32_t>(p - 4, static_cast<std::uint32_t>(n * 2)); if (c.arg(0)) for (std::uint64_t i = 0; i < n; ++i) mem_.write<std::uint16_t>(p + i * 2, mem_.read<std::uint16_t>(c.arg(0) + i * 2)); c.r[0] = p; });
    H("SysFreeString", [](GuestCpu&) {});
    H("RoGetActivationFactory", [this](GuestCpu& c) { // (HSTRING classId, REFIID iid, void** factory)
        const std::string cls = utf16_to_utf8(hstring_text(c.arg(0)));
        const std::string iid = guid_text(c.arg(1));
        const auto it = winrt_classes_.find(cls);
        if (it != winrt_classes_.end()) {
            auto& obj = winrt_objects_[cls];
            if (!obj) obj = it->second();
            mem_.write<std::uint64_t>(obj + 8, mem_.read<std::uint64_t>(obj + 8) + 1);
            if (c.arg(2)) mem_.write<std::uint64_t>(c.arg(2), obj);
            if (winrt_requests_.insert(cls).second) milestone("WinRT class \"" + cls + "\" activated (IID " + iid + ") -> HLE implementation");
            c.r[0] = 0; return;
        }
        if (cls.rfind("Windows.", 0) == 0 || cls.rfind("Microsoft.", 0) == 0) { // discovery object: accepts any IID, logs the first method slot used
            if (winrt_requests_.insert(cls).second) milestone("WinRT class \"" + cls + "\" (IID " + iid + ") is NOT implemented -> discovery object (every method returns E_NOTIMPL and is logged)");
            auto& obj = winrt_objects_[cls];
            if (!obj) obj = make_com_object(cls, {}, {"*"}, true, 64);
            mem_.write<std::uint64_t>(obj + 8, mem_.read<std::uint64_t>(obj + 8) + 1);
            if (c.arg(2)) mem_.write<std::uint64_t>(c.arg(2), obj);
            last_winrt_class_ = cls;
            c.r[0] = 0; return;
        }
        if (winrt_requests_.insert(cls).second) milestone("WinRT RoGetActivationFactory(\"" + cls + "\", IID " + iid + ") -> REGDB_E_CLASSNOTREG");
        if (c.arg(2)) mem_.write<std::uint64_t>(c.arg(2), 0);
        last_winrt_class_ = cls; last_winrt_hr_ = 0x80040154ULL;
        c.r[0] = 0x80040154ULL;
    });
    H("CoCreateFreeThreadedMarshaler", [this](GuestCpu& c) { // (IUnknown* outer, IUnknown** marshaler)
        if (!ftm_) ftm_ = make_com_object("FreeThreadedMarshaler", {}, {"00000003-0000-0000-c000-000000000046"});
        mem_.write<std::uint64_t>(ftm_ + 8, mem_.read<std::uint64_t>(ftm_ + 8) + 1);
        mem_.write<std::uint64_t>(c.arg(1), ftm_); c.r[0] = 0;
    });
    H("CoGetInterfaceAndReleaseStream", [](GuestCpu& c) { c.r[0] = 0x80004001ULL; });
    H("CoMarshalInterThreadInterfaceInStream", [](GuestCpu& c) { c.r[0] = 0x80004001ULL; });

    // ---- locale / ctype ----
    H("_lock_locales", [](GuestCpu&) {}); H("_unlock_locales", [](GuestCpu&) {});
    H("___lc_codepage_func", [](GuestCpu& c) { c.r[0] = 1252; });
    H("___lc_collate_cp_func", [](GuestCpu& c) { c.r[0] = 1252; });
    H("___lc_locale_name_func", [this](GuestCpu& c) { if (!locale_names_) locale_names_ = static_alloc(8 * 8); c.r[0] = locale_names_; });
    H("___mb_cur_max_func", [this](GuestCpu& c) { if (!mb_cur_max_) { mb_cur_max_ = static_alloc(8); mem_.write<std::int32_t>(mb_cur_max_, 1); } c.r[0] = mb_cur_max_; });
    H("_ismbblead", [](GuestCpu& c) { c.r[0] = 0; });
    H("localeconv", [this](GuestCpu& c) {
        if (!lconv_) {
            lconv_ = static_alloc(160);
            const auto empty = static_cstr(""); const auto dot = static_cstr(".");
            const auto wempty = static_alloc(4, 2); const auto wdot = static_alloc(4, 2); mem_.write<std::uint16_t>(wdot, '.');
            for (int i = 0; i < 10; ++i) mem_.write<std::uint64_t>(lconv_ + static_cast<std::uint64_t>(i) * 8, i == 0 ? dot : empty);
            for (int i = 0; i < 8; ++i) mem_.write<std::uint8_t>(lconv_ + 80 + static_cast<std::uint64_t>(i), 127);
            mem_.write<std::uint64_t>(lconv_ + 88, wdot);
            for (int i = 1; i < 7; ++i) mem_.write<std::uint64_t>(lconv_ + 88 + static_cast<std::uint64_t>(i) * 8, wempty);
        }
        c.r[0] = lconv_;
    });
    auto ctype_pred = [this](const char* n, std::uint16_t mask) {
        H(n, [this, mask](GuestCpu& c) { const auto ch = static_cast<std::int32_t>(c.arg(0)); c.r[0] = (ch >= 0 && ch < 128 && (mem_.read<std::uint16_t>(ctype_ptr_ + static_cast<std::uint64_t>(ch) * 2) & mask)) ? 1 : 0; });
    };
    ctype_pred("isalnum", 0x1 | 0x2 | 0x4); ctype_pred("isxdigit", 0x80); ctype_pred("islower", 0x2); ctype_pred("isupper", 0x1);
    H("btowc", [](GuestCpu& c) { const auto ch = static_cast<std::int32_t>(c.arg(0)); c.r[0] = (ch >= 0 && ch < 128) ? static_cast<std::uint64_t>(ch) : 0xFFFFULL; });
    H("iswctype", [this](GuestCpu& c) { const auto ch = static_cast<std::uint32_t>(c.arg(0) & 0xFFFF); c.r[0] = (ch < 128 && (mem_.read<std::uint16_t>(ctype_ptr_ + static_cast<std::uint64_t>(ch) * 2) & c.arg(1))) ? 1 : 0; });
    auto names = [this](const char* n, const std::string& s, bool wide) {
        H(n, [this, s, wide](GuestCpu& c) {
            if (wide) { const auto w = utf8_to_utf16(s); const auto p = heap_alloc((w.size() + 1) * 2, false); write_wstr(p, w); c.r[0] = p; }
            else { const auto p = heap_alloc(s.size() + 1, false); mem_.write_bytes(p, s.c_str(), s.size() + 1); c.r[0] = p; }
        });
    };
    const std::string days = ":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:Thu:Thursday:Fri:Friday:Sat:Saturday";
    const std::string months = ":Jan:January:Feb:February:Mar:March:Apr:April:May:May:Jun:June:Jul:July:Aug:August:Sep:September:Oct:October:Nov:November:Dec:December";
    const std::string tnames = "AM:PM:M/d/yyyy:dddd, MMMM d, yyyy:h:mm:ss tt";
    names("_Getdays", days, false); names("_W_Getdays", days, true); names("_Getmonths", months, false); names("_W_Getmonths", months, true);
    names("_Gettnames", tnames, false); names("_W_Gettnames", tnames, true);

    // ---- kernel services used by the MSVC runtime DLLs ----
    H("EncodePointer", [](GuestCpu& c) { c.r[0] = c.arg(0); });
    H("QueryPerformanceFrequency", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(0), 10000000); c.r[0] = 1; });
    H("GetSystemTimePreciseAsFileTime", [this](GuestCpu& c) { filetime_ += 10000 + c.instructions; mem_.write<std::uint64_t>(c.arg(0), filetime_); });
    H("Sleep", [this](GuestCpu& c) { qpc_ += c.arg(0) * 10000; });
    H("InitOnceExecuteOnce", [this](GuestCpu& c) { // (once*, fn(once, param, ctx**), param, ctx**)
        const std::uint64_t once = c.arg(0), fn = c.arg(1), param = c.arg(2), ctx = c.arg(3);
        if (mem_.read<std::uint64_t>(once) == 0) {
            const std::uint64_t slot = static_alloc(16);
            const std::uint64_t ok = call_guest(c, fn, {once, param, slot});
            if (c.stop != GuestCpu::Stop::None) return;
            if (ok & 0xFFFFFFFFULL) { mem_.write<std::uint64_t>(once, 2); if (ctx) mem_.write<std::uint64_t>(ctx, mem_.read<std::uint64_t>(slot)); c.r[0] = 1; return; }
            c.r[0] = 0; return;
        }
        if (ctx) mem_.write<std::uint64_t>(ctx, 0);
        c.r[0] = 1;
    });
    H("InitializeSRWLock", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(0), 0); });
    H("TryAcquireSRWLockExclusive", [](GuestCpu& c) { c.r[0] = 1; });
    H("TryEnterCriticalSection", [](GuestCpu& c) { c.r[0] = 1; });
    H("SleepConditionVariableSRW", [this](GuestCpu& c) { last_error_ = 1460; c.r[0] = 0; });
    H("WakeConditionVariable", [](GuestCpu&) {});
    H("RtlCaptureStackBackTrace", [](GuestCpu& c) { c.r[0] = 0; });
    H("RtlPcToFileHeader", [this](GuestCpu& c) {
        const std::uint64_t pc = c.arg(0); std::uint64_t base = 0;
        if (pc >= image_.image_base && pc < image_.image_base + image_.size_of_image) base = image_.image_base;
        for (const auto& m : modules_) if (pc >= m->base && pc < m->base + m->img.size_of_image) base = m->base;
        if (c.arg(1)) mem_.write<std::uint64_t>(c.arg(1), base);
        c.r[0] = base;
    });
    H("GetModuleHandleExW", [this](GuestCpu& c) { // (flags, name-or-address, HMODULE*)
        const std::uint64_t flags = c.arg(0), out = c.arg(2);
        std::uint64_t h = 0;
        if (flags & 4) { // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            const std::uint64_t pc = c.arg(1);
            if (pc >= image_.image_base && pc < image_.image_base + image_.size_of_image) h = image_.image_base;
            for (const auto& m : modules_) if (pc >= m->base && pc < m->base + m->img.size_of_image) h = m->base;
        } else if (!c.arg(1)) h = image_.image_base;
        else h = module_handle_for(utf16_to_utf8(mem_.read_wstring(c.arg(1))));
        if (out) mem_.write<std::uint64_t>(out, h);
        if (!h) last_error_ = 126;
        c.r[0] = h ? 1 : 0;
    });
    H("RaiseException", [](GuestCpu& c) {
        if ((c.arg(0) & 0xFFFFFFFFULL) == 0x406D1388ULL) return;   // MS_VC_EXCEPTION: the SetThreadName debugger convention; with no debugger the title's __except swallows it
        c.request_stop(GuestCpu::Stop::HostRequest, "RaiseException(code=0x" + GuestCpu::hex(c.arg(0)) + ") - SEH dispatch is not implemented"); });
    H("GetNativeSystemInfo", [this](GuestCpu& c) { zero_fill(c.arg(0), 48); mem_.write<std::uint16_t>(c.arg(0), 9); mem_.write<std::uint32_t>(c.arg(0) + 4, 4096); mem_.write<std::uint32_t>(c.arg(0) + 32, 6); mem_.write<std::uint32_t>(c.arg(0) + 28, 4); });
    H("GetExitCodeThread", [this](GuestCpu& c) { if (c.arg(1)) mem_.write<std::uint32_t>(c.arg(1), 259); c.r[0] = 1; });
    H("DuplicateHandle", [this](GuestCpu& c) { if (c.arg(3)) mem_.write<std::uint64_t>(c.arg(3), c.arg(1)); c.r[0] = 1; });
    H("CreateThreadpoolWork", [this](GuestCpu& c) { c.r[0] = heap_alloc(32, true); });
    H("SubmitThreadpoolWork", [this](GuestCpu&) { milestone("SubmitThreadpoolWork (work item NOT run: no thread pool)"); });
    H("CloseThreadpoolWork", [](GuestCpu&) {});
    H("FreeLibraryWhenCallbackReturns", [](GuestCpu&) {});
    H("GetLocaleInfoEx", [](GuestCpu& c) { c.r[0] = 0; });
    H("GetCPInfo", [this](GuestCpu& c) { zero_fill(c.arg(1), 20); mem_.write<std::uint32_t>(c.arg(1), 1); c.r[0] = 1; });
    H("WideCharToMultiByte", [this](GuestCpu& c) { // (cp, flags, wstr, cch, buf, cb, def, used)
        const std::uint64_t src = c.arg(2), buf = c.arg(4), cb = c.arg(5); const std::int32_t cch = static_cast<std::int32_t>(c.arg(3));
        std::u16string w = cch < 0 ? mem_.read_wstring(src) : mem_.read_wstring(src, static_cast<std::size_t>(cch)); if (cch >= 0 && w.size() > static_cast<std::size_t>(cch)) w.resize(static_cast<std::size_t>(cch));
        std::string s = utf16_to_utf8(w); if (cch < 0) s.push_back('\0');
        if (cb == 0) { c.r[0] = s.size(); return; }
        if (s.size() > cb) { last_error_ = 122; c.r[0] = 0; return; }
        mem_.write_bytes(buf, s.data(), s.size()); c.r[0] = s.size();
    });
    H("MultiByteToWideChar", [this](GuestCpu& c) { // (cp, flags, str, cb, wbuf, cch)
        const std::uint64_t src = c.arg(2), wbuf = c.arg(4), cchw = c.arg(5); const std::int32_t cb = static_cast<std::int32_t>(c.arg(3));
        std::string s; if (cb < 0) s = mem_.read_cstring(src); else for (std::int32_t i = 0; i < cb; ++i) s.push_back(static_cast<char>(mem_.read<std::uint8_t>(src + static_cast<std::uint64_t>(i))));
        std::u16string w = utf8_to_utf16(s); if (cb < 0) w.push_back(0);
        if (cchw == 0) { c.r[0] = w.size(); return; }
        if (w.size() > cchw) { last_error_ = 122; c.r[0] = 0; return; }
        for (std::size_t i = 0; i < w.size(); ++i) mem_.write<std::uint16_t>(wbuf + i * 2, w[i]);
        c.r[0] = w.size();
    });
    H("GetStringTypeW", [this](GuestCpu& c) { // (type, src, cch, out)
        std::int32_t n = static_cast<std::int32_t>(c.arg(2)); const std::uint64_t src = c.arg(1), out = c.arg(3);
        if (n < 0) n = static_cast<std::int32_t>(mem_.read_wstring(src).size()) + 1;
        for (std::int32_t i = 0; i < n; ++i) { const auto ch = mem_.read<std::uint16_t>(src + static_cast<std::uint64_t>(i) * 2); mem_.write<std::uint16_t>(out + static_cast<std::uint64_t>(i) * 2, ch < 128 ? mem_.read<std::uint16_t>(ctype_ptr_ + static_cast<std::uint64_t>(ch) * 2) : 0); }
        c.r[0] = 1;
    });
    H("LCMapStringEx", [this](GuestCpu& c) { // (locale, flags, src, cchsrc, dst, cchdst, ...)
        const std::uint64_t src = c.arg(2), dst = c.arg(4); std::int32_t n = static_cast<std::int32_t>(c.arg(3)); const std::uint64_t cap = c.arg(5); const auto flags = c.arg(1);
        if (n < 0) n = static_cast<std::int32_t>(mem_.read_wstring(src).size()) + 1;
        if (cap == 0) { c.r[0] = static_cast<std::uint64_t>(n); return; }
        if (static_cast<std::uint64_t>(n) > cap) { last_error_ = 122; c.r[0] = 0; return; }
        for (std::int32_t i = 0; i < n; ++i) { auto ch = mem_.read<std::uint16_t>(src + static_cast<std::uint64_t>(i) * 2); if (ch < 128) { if ((flags & 0x200) && ch >= 'a' && ch <= 'z') ch = static_cast<std::uint16_t>(ch - 32); else if ((flags & 0x100) && ch >= 'A' && ch <= 'Z') ch = static_cast<std::uint16_t>(ch + 32); } mem_.write<std::uint16_t>(dst + static_cast<std::uint64_t>(i) * 2, ch); }
        c.r[0] = static_cast<std::uint64_t>(n);
    });
    H("CompareStringEx", [this](GuestCpu& c) { // (locale, flags, s1, c1, s2, c2, ...)
        auto get = [this](std::uint64_t p, std::int32_t n) { return n < 0 ? mem_.read_wstring(p) : mem_.read_wstring(p, static_cast<std::size_t>(n)); };
        std::u16string a = get(c.arg(2), static_cast<std::int32_t>(c.arg(3))), b = get(c.arg(4), static_cast<std::int32_t>(c.arg(5)));
        if (c.arg(1) & 1) { for (auto& ch : a) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char16_t>(ch + 32); for (auto& ch : b) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char16_t>(ch + 32); }
        c.r[0] = a < b ? 1 : (a == b ? 2 : 3);
    });
}

} // namespace someboxone
