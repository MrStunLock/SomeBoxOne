#pragma once
// SomeBoxOne v0.31.0 - kernel32-level baseline HLE.
//
// Titles with a statically linked CRT (Terraria) or a managed/native runtime (and the real VS2012+ redistributable DLLs, e.g.
// Peggle 2's msvcr110/msvcp110) call a large set of ordinary Win32 services that earlier builds left as "return 0" stubs:
// SetLastError, FLS/TLS, VirtualAlloc/VirtualQuery, the Heap*/Global*/Local* families, the file API, GetSystemInfo,
// events/semaphores/waits, locale basics, SList, version/package queries and similar.
//
// Everything is single-threaded and deterministic. Waits are optimistic (they return WAIT_OBJECT_0 for anything that cannot be
// signalled by a thread we are not running, and are reported once so the report shows where a missing thread would matter).
// File access stays sandboxed: reads come from the package folder (then the save sandbox), writes go to the save sandbox only.
// Registered AFTER the other handler groups, so these definitions override any older stub of the same name.

namespace someboxone {

inline void XboxTitleRuntime::register_kernel_baseline() {
    auto ok = [](GuestCpu& c) { c.r[0] = 1; };
    auto zero = [](GuestCpu& c) { c.r[0] = 0; };

    // ---------------- last error / fast-fail -----------------
    H("SetLastError", [this](GuestCpu& c) { last_error_ = static_cast<std::uint32_t>(c.arg(0)); });
    H("SetLastErrorEx", [this](GuestCpu& c) { last_error_ = static_cast<std::uint32_t>(c.arg(0)); });
    H("DebugBreak", [](GuestCpu& c) { c.request_stop(GuestCpu::Stop::Breakpoint, "DebugBreak() called by the title"); });
    H("RaiseFailFastException", [](GuestCpu& c) { c.request_stop(GuestCpu::Stop::HostRequest, "RaiseFailFastException: the title requested an immediate fail-fast"); });
    H("AddVectoredExceptionHandler", [this](GuestCpu& c) { milestone("AddVectoredExceptionHandler registered (never invoked: SEH dispatch is not implemented)"); c.r[0] = 0x00007FF4C0000001ULL; });
    H("RemoveVectoredExceptionHandler", ok);
    H("RtlUnwind", [](GuestCpu& c) { c.request_stop(GuestCpu::Stop::HostRequest, "RtlUnwind: structured-exception unwinding is not implemented"); });
    H("RtlUnwindEx", [](GuestCpu& c) { c.request_stop(GuestCpu::Stop::HostRequest, "RtlUnwindEx: structured-exception unwinding is not implemented"); });
    H("SetErrorMode", zero); H("GetErrorMode", zero);
    H("EncodeSystemPointer", [](GuestCpu& c) { c.r[0] = c.arg(0); });
    H("DecodeSystemPointer", [](GuestCpu& c) { c.r[0] = c.arg(0); });

    // ---------------- TLS / FLS -----------------
    // Slots are per-process (one guest thread runs). FlsAlloc's destructor callback is recorded but not invoked.
    H("TlsAlloc", [this](GuestCpu& c) { tls_slots_.push_back(0); last_error_ = 0; c.r[0] = tls_slots_.size() - 1; });
    H("TlsFree", ok);
    H("TlsGetValue", [this](GuestCpu& c) { const auto i = c.arg(0); last_error_ = 0; c.r[0] = i < tls_slots_.size() ? tls_slots_[static_cast<std::size_t>(i)] : 0; });
    H("TlsSetValue", [this](GuestCpu& c) { const auto i = c.arg(0); if (i < tls_slots_.size()) { tls_slots_[static_cast<std::size_t>(i)] = c.arg(1); c.r[0] = 1; } else { last_error_ = 87; c.r[0] = 0; } });
    H("FlsAlloc", [this](GuestCpu& c) { fls_slots_.push_back(0); last_error_ = 0; c.r[0] = fls_slots_.size() - 1; });
    H("FlsFree", ok);
    H("FlsGetValue", [this](GuestCpu& c) { const auto i = c.arg(0); last_error_ = 0; c.r[0] = i < fls_slots_.size() ? fls_slots_[static_cast<std::size_t>(i)] : 0; });
    H("FlsSetValue", [this](GuestCpu& c) { const auto i = c.arg(0); if (i < fls_slots_.size()) { fls_slots_[static_cast<std::size_t>(i)] = c.arg(1); c.r[0] = 1; } else { last_error_ = 87; c.r[0] = 0; } });

    // ---------------- virtual memory -----------------
    // Reservations are tracked; pages are mapped (zero-filled) only when committed, so a managed runtime may reserve a huge
    // range without exhausting the guest memory budget.
    H("VirtualAlloc", [this](GuestCpu& c) { c.r[0] = virtual_alloc(c.arg(0), c.arg(1), static_cast<std::uint32_t>(c.arg(2))); });
    H("VirtualAllocFromApp", [this](GuestCpu& c) { c.r[0] = virtual_alloc(c.arg(0), c.arg(1), static_cast<std::uint32_t>(c.arg(2))); });
    H("VirtualFree", [this](GuestCpu& c) { // (addr, size, type)
        const std::uint64_t a = c.arg(0), sz = c.arg(1); const auto type = static_cast<std::uint32_t>(c.arg(2));
        if (type & 0x8000) { // MEM_RELEASE: the whole reservation
            const auto it = virt_regions_.find(a);
            if (it != virt_regions_.end()) { mem_.unmap(a, it->second.size); virt_regions_.erase(it); }
        } else if ((type & 0x4000) && sz) mem_.unmap(a & ~0xFFFULL, (sz + 0xFFF) & ~0xFFFULL); // MEM_DECOMMIT
        c.r[0] = 1; });
    H("VirtualProtect", [this](GuestCpu& c) { if (c.arg(3)) mem_.write<std::uint32_t>(c.arg(3), 4); c.r[0] = 1; });
    H("VirtualProtectFromApp", [this](GuestCpu& c) { if (c.arg(3)) mem_.write<std::uint32_t>(c.arg(3), 4); c.r[0] = 1; });
    H("VirtualQuery", [this](GuestCpu& c) { // (addr, MEMORY_BASIC_INFORMATION*, len)
        const std::uint64_t a = c.arg(0) & ~0xFFFULL, out = c.arg(1);
        if (!out || c.arg(2) < 48) { last_error_ = 24; c.r[0] = 0; return; }
        zero_fill(out, 48);
        const bool mapped = mem_.is_mapped(a);
        std::uint64_t base = a, end = a + 0x1000;
        // extent of the run of pages with the same mapped/unmapped state (bounded so a scan is cheap)
        for (unsigned i = 0; i < 4096 && base > 0x10000 && mem_.is_mapped(base - 0x1000) == mapped; ++i) base -= 0x1000;
        for (unsigned i = 0; i < 65536 && mem_.is_mapped(end) == mapped; ++i) end += 0x1000;
        std::uint64_t alloc_base = base;
        // A region never spans two allocations: bound it to the VirtualAlloc reservation that contains the address (AllocationBase).
        auto rit = virt_regions_.upper_bound(a);
        if (rit != virt_regions_.begin()) { --rit; if (a >= rit->first && a < rit->first + rit->second.size) { base = (std::max)(base, rit->first); end = (std::min)(end, rit->first + rit->second.size); alloc_base = rit->first; } }
        std::uint32_t type = 0x20000; // MEM_PRIVATE
        if (a >= image_.image_base && a < image_.image_base + image_.size_of_image) type = 0x1000000; // MEM_IMAGE
        mem_.write<std::uint64_t>(out, base); mem_.write<std::uint64_t>(out + 8, alloc_base);
        mem_.write<std::uint32_t>(out + 16, mapped ? 4 : 0); mem_.write<std::uint64_t>(out + 24, end - base);
        mem_.write<std::uint32_t>(out + 32, mapped ? 0x1000 : 0x10000); mem_.write<std::uint32_t>(out + 36, mapped ? 4 : 1);
        mem_.write<std::uint32_t>(out + 40, mapped ? type : 0);
        c.r[0] = 48; });

    // ---------------- heaps -----------------
    H("HeapCreate", [this](GuestCpu& c) { c.r[0] = 0x00000000DEAD0000ULL + 0x10000 * (++heap_handles_); });
    H("HeapDestroy", ok);
    H("HeapReAlloc", [this](GuestCpu& c) { c.r[0] = heap_realloc(c.arg(2), c.arg(3)); });
    H("HeapSize", [this](GuestCpu& c) { c.r[0] = heap_size_of(c.arg(2)); });
    H("HeapValidate", ok); H("HeapSetInformation", ok); H("HeapCompact", zero);
    H("GlobalAlloc", [this](GuestCpu& c) { c.r[0] = heap_alloc(c.arg(1), (c.arg(0) & 0x40) != 0); });
    H("GlobalFree", [this](GuestCpu& c) { heap_free(c.arg(0)); c.r[0] = 0; });
    H("GlobalLock", [](GuestCpu& c) { c.r[0] = c.arg(0); }); H("GlobalUnlock", ok);
    H("LocalAlloc", [this](GuestCpu& c) { c.r[0] = heap_alloc(c.arg(1), (c.arg(0) & 0x40) != 0); });
    H("LocalFree", [this](GuestCpu& c) { heap_free(c.arg(0)); c.r[0] = 0; });
    H("LocalReAlloc", [this](GuestCpu& c) { c.r[0] = heap_realloc(c.arg(0), c.arg(1)); });

    // ---------------- system information -----------------
    H("GetSystemInfo", [this](GuestCpu& c) { fill_system_info(c.arg(0)); });
    H("GetNativeSystemInfo", [this](GuestCpu& c) { fill_system_info(c.arg(0)); });
    H("GlobalMemoryStatusEx", [this](GuestCpu& c) {
        const std::uint64_t o = c.arg(0); zero_fill(o + 4, 60);
        mem_.write<std::uint32_t>(o + 4, 20); mem_.write<std::uint64_t>(o + 8, 5ULL << 30); mem_.write<std::uint64_t>(o + 16, 4ULL << 30);
        mem_.write<std::uint64_t>(o + 24, 8ULL << 30); mem_.write<std::uint64_t>(o + 32, 7ULL << 30);
        mem_.write<std::uint64_t>(o + 40, 0x00007FFFFFFE0000ULL); mem_.write<std::uint64_t>(o + 48, 0x00007FFFFFF00000ULL);
        c.r[0] = 1; });
    H("GetTickCount64", [this](GuestCpu& c) { c.r[0] = tick_ms(); });
    H("GetTickCount", [this](GuestCpu& c) { c.r[0] = tick_ms() & 0xFFFFFFFFULL; });
    H("GetActiveProcessorCount", [](GuestCpu& c) { c.r[0] = 6; });
    H("GetNumaHighestNodeNumber", [this](GuestCpu& c) { if (c.arg(0)) mem_.write<std::uint32_t>(c.arg(0), 0); c.r[0] = 1; });
    H("GetProcessAffinityMask", [this](GuestCpu& c) { if (c.arg(1)) mem_.write<std::uint64_t>(c.arg(1), 0x3F); if (c.arg(2)) mem_.write<std::uint64_t>(c.arg(2), 0x3F); c.r[0] = 1; });
    H("GetLogicalProcessorInformation", [this](GuestCpu& c) { // (SYSTEM_LOGICAL_PROCESSOR_INFORMATION*, DWORD* len): 6 single-thread cores
        const std::uint32_t need = 6 * 32;
        if (!c.arg(1)) { last_error_ = 87; c.r[0] = 0; return; }
        const std::uint32_t have = mem_.read<std::uint32_t>(c.arg(1));
        mem_.write<std::uint32_t>(c.arg(1), need);
        if (!c.arg(0) || have < need) { last_error_ = 122; c.r[0] = 0; return; }
        zero_fill(c.arg(0), need);
        for (std::uint32_t i = 0; i < 6; ++i) { mem_.write<std::uint64_t>(c.arg(0) + i * 32ULL, 1ULL << i); mem_.write<std::uint32_t>(c.arg(0) + i * 32ULL + 8, 0); }
        c.r[0] = 1; });
    H("GetLogicalProcessorInformationEx", [this](GuestCpu& c) { last_error_ = 120; c.r[0] = 0; });
    H("GetSystemTimes", [this](GuestCpu& c) { for (unsigned i = 0; i < 3; ++i) if (c.arg(i)) mem_.write<std::uint64_t>(c.arg(i), c.instructions * 4ULL); c.r[0] = 1; });
    H("GetProcessTimes", [this](GuestCpu& c) { for (unsigned i = 1; i < 5; ++i) if (c.arg(i)) mem_.write<std::uint64_t>(c.arg(i), i == 1 ? filetime_ : c.instructions * 4ULL); c.r[0] = 1; });
    H("GetLargePageMinimum", zero);
    H("IsWow64Process", [this](GuestCpu& c) { if (c.arg(1)) mem_.write<std::uint32_t>(c.arg(1), 0); c.r[0] = 1; });
    H("GetProcessId", [](GuestCpu& c) { c.r[0] = 0x400; });
    H("GetThreadId", [](GuestCpu& c) { c.r[0] = 0x1000; });
    H("GetVersion", [](GuestCpu& c) { c.r[0] = (14393ULL << 16) | 10ULL; });
    auto version_ex = [this](GuestCpu& c) { // OSVERSIONINFO[EX]W: size@0 major@4 minor@8 build@12 platform@16
        const std::uint64_t o = c.arg(0); if (!o) { c.r[0] = 0; return; }
        mem_.write<std::uint32_t>(o + 4, 10); mem_.write<std::uint32_t>(o + 8, 0); mem_.write<std::uint32_t>(o + 12, 14393); mem_.write<std::uint32_t>(o + 16, 2);
        c.r[0] = 1; };
    H("GetVersionExW", version_ex);
    H("RtlGetVersion", [version_ex](GuestCpu& c) { version_ex(c); c.r[0] = 0; });
    H("GetTimeZoneInformation", [this](GuestCpu& c) { zero_fill(c.arg(0), 172); c.r[0] = 0; });
    H("GetDynamicTimeZoneInformation", [this](GuestCpu& c) { zero_fill(c.arg(0), 432); c.r[0] = 0; });
    H("GetCurrentThreadStackLimits", [this](GuestCpu& c) { if (c.arg(0)) mem_.write<std::uint64_t>(c.arg(0), xrt::kStackTop - xrt::kStackSize); if (c.arg(1)) mem_.write<std::uint64_t>(c.arg(1), xrt::kStackTop); });
    H("SetThreadStackGuarantee", ok);

    // ---------------- packaged-app (UWP) queries -----------------
    H("GetCurrentPackageFullName", [this](GuestCpu& c) { // (UINT32* len, PWSTR buf)
        const std::u16string name = u"SomeBoxOne.Title_1.0.0.0_x64__0000000000000";
        if (!c.arg(0)) { c.r[0] = 87; return; }
        const std::uint32_t cap = mem_.read<std::uint32_t>(c.arg(0)); mem_.write<std::uint32_t>(c.arg(0), static_cast<std::uint32_t>(name.size() + 1));
        if (!c.arg(1) || cap < name.size() + 1) { c.r[0] = 122; return; }
        write_wstr(c.arg(1), name); c.r[0] = 0; });
    H("GetCurrentPackageId", [](GuestCpu& c) { c.r[0] = 15700; }); // APPMODEL_ERROR_NO_PACKAGE: callers fall back to non-packaged behaviour
    H("AppPolicyGetProcessTerminationMethod", [this](GuestCpu& c) { if (c.arg(1)) mem_.write<std::uint32_t>(c.arg(1), 0); c.r[0] = 0; });
    H("AppPolicyGetThreadInitializationType", [this](GuestCpu& c) { if (c.arg(1)) mem_.write<std::uint32_t>(c.arg(1), 0); c.r[0] = 0; });
    H("AppPolicyGetWindowingModel", [this](GuestCpu& c) { if (c.arg(1)) mem_.write<std::uint32_t>(c.arg(1), 1); c.r[0] = 0; });

    // ---------------- command line / environment / paths -----------------
    H("GetCommandLineW", [this](GuestCpu& c) { if (!cmdline_w_) { cmdline_w_ = static_alloc((opt_.command_line.size() + 1) * 2, 2); write_wstr(cmdline_w_, utf8_to_utf16(opt_.command_line)); } c.r[0] = cmdline_w_; });
    H("GetCommandLineA", [this](GuestCpu& c) { if (!cmdline_a_) cmdline_a_ = static_cstr(opt_.command_line); c.r[0] = cmdline_a_; });
    H("GetEnvironmentStringsW", [this](GuestCpu& c) { if (!env_block_) env_block_ = static_alloc(8, 2); c.r[0] = env_block_; });
    H("FreeEnvironmentStringsW", ok);
    H("GetEnvironmentVariableW", [this](GuestCpu& c) { last_error_ = 203; c.r[0] = 0; });
    H("SetEnvironmentVariableW", ok);
    H("GetModuleFileNameW", [this](GuestCpu& c) { // (module, buf, size)
        std::string name = "C:\\Mount\\" + PeImage::path_utf8(std::filesystem::path(opt_.exe_name.empty() ? std::string("title.exe") : opt_.exe_name));
        if (c.arg(0) && c.arg(0) != image_.image_base) for (const auto& m : modules_) if (m->base == c.arg(0)) name = "C:\\Windows\\System32\\" + m->name;
        const auto w = utf8_to_utf16(name); const std::uint64_t cap = c.arg(2);
        if (!c.arg(1) || !cap) { c.r[0] = 0; return; }
        const std::size_t n = static_cast<std::size_t>((std::min<std::uint64_t>)(w.size(), cap - 1));
        for (std::size_t i = 0; i < n; ++i) mem_.write<std::uint16_t>(c.arg(1) + i * 2, w[i]);
        mem_.write<std::uint16_t>(c.arg(1) + n * 2, 0);
        if (n < w.size()) last_error_ = 122;
        c.r[0] = n; });
    H("GetModuleFileNameA", [this](GuestCpu& c) {
        const std::string name = "C:\\Mount\\" + (opt_.exe_name.empty() ? std::string("title.exe") : opt_.exe_name); const std::uint64_t cap = c.arg(2);
        if (!c.arg(1) || !cap) { c.r[0] = 0; return; }
        const std::size_t n = static_cast<std::size_t>((std::min<std::uint64_t>)(name.size(), cap - 1));
        mem_.write_bytes(c.arg(1), name.data(), n); mem_.write<std::uint8_t>(c.arg(1) + n, 0); c.r[0] = n; });
    H("GetCurrentDirectoryW", [this](GuestCpu& c) { // (size, buf)
        const std::u16string w = u"C:\\Mount"; const std::uint64_t cap = c.arg(0);
        if (!c.arg(1) || w.size() + 1 > cap) { c.r[0] = w.size() + 1; return; }
        write_wstr(c.arg(1), w); c.r[0] = w.size(); });
    H("GetSystemDirectoryW", [this](GuestCpu& c) { const std::u16string w = u"C:\\Windows\\System32"; if (!c.arg(0) || w.size() + 1 > c.arg(1)) { c.r[0] = w.size() + 1; return; } write_wstr(c.arg(0), w); c.r[0] = w.size(); });
    H("GetWindowsDirectoryW", [this](GuestCpu& c) { const std::u16string w = u"C:\\Windows"; if (!c.arg(0) || w.size() + 1 > c.arg(1)) { c.r[0] = w.size() + 1; return; } write_wstr(c.arg(0), w); c.r[0] = w.size(); });
    H("GetTempPathW", [this](GuestCpu& c) { const std::u16string w = u"C:\\Temp\\"; if (!c.arg(1) || w.size() + 1 > c.arg(0)) { c.r[0] = w.size() + 1; return; } write_wstr(c.arg(1), w); c.r[0] = w.size(); });
    H("GetFullPathNameW", [this](GuestCpu& c) { // (name, size, buf, filepart**)
        std::u16string w = mem_.read_wstring(c.arg(0));
        if (w.size() < 2 || w[1] != u':') w = u"C:\\Mount\\" + w;
        if (!c.arg(2) || w.size() + 1 > c.arg(1)) { c.r[0] = w.size() + 1; return; }
        write_wstr(c.arg(2), w); if (c.arg(3)) { const auto p = w.find_last_of(u'\\'); mem_.write<std::uint64_t>(c.arg(3), p == std::u16string::npos ? c.arg(2) : c.arg(2) + (p + 1) * 2); }
        c.r[0] = w.size(); });

    // ---------------- code pages / locale -----------------
    H("GetACP", [](GuestCpu& c) { c.r[0] = 1252; }); H("GetOEMCP", [](GuestCpu& c) { c.r[0] = 437; });
    H("IsValidCodePage", ok); H("IsValidLocaleName", ok); H("EnumSystemLocalesW", ok);
    H("GetUserDefaultLCID", [](GuestCpu& c) { c.r[0] = 0x409; }); H("GetSystemDefaultLCID", [](GuestCpu& c) { c.r[0] = 0x409; });
    H("GetThreadLocale", [](GuestCpu& c) { c.r[0] = 0x409; }); H("GetUserDefaultLangID", [](GuestCpu& c) { c.r[0] = 0x409; });
    H("GetLocaleInfoW", [this](GuestCpu& c) { // (lcid, type, buf, cch)
        const auto type = static_cast<std::uint32_t>(c.arg(1)) & 0x0FFFFFFFu; std::u16string v;
        switch (type) { case 0x0E: v = u"."; break; case 0x0F: v = u","; break; case 0x10: v = u"3;0"; break; case 0x5C: case 0x1: v = u"en-US"; break;
                        case 0x2: case 0x1002: v = u"English"; break; case 0x3: v = u"English (United States)"; break; case 0x14: v = u"$"; break;
                        case 0x1004: v = u"1252"; break; case 0x0B: v = u"1033"; break; default: v = u"0"; break; }
        if (!c.arg(2) || !c.arg(3)) { c.r[0] = v.size() + 1; return; }
        if (v.size() + 1 > c.arg(3)) { last_error_ = 122; c.r[0] = 0; return; }
        write_wstr(c.arg(2), v); c.r[0] = v.size() + 1; });
    if (const auto it = handlers_.find("LCMapStringEx"); it != handlers_.end()) H("LCMapStringW", it->second);
    if (const auto it = handlers_.find("CompareStringEx"); it != handlers_.end()) H("CompareStringW", it->second);

    // ---------------- COM -----------------
    H("CoInitializeEx", zero); H("CoUninitialize", [](GuestCpu&) {});
    H("CoCreateInstance", [this](GuestCpu& c) { // (clsid, outer, ctx, iid, void** out)
        const std::string clsid = guid_text(c.arg(0));
        if (com_log_.insert("cocreate:" + clsid).second) milestone("CoCreateInstance(CLSID " + clsid + ") -> REGDB_E_CLASSNOTREG (COM class not emulated)");
        if (c.arg(4)) { mem_.write<std::uint64_t>(c.arg(4), 0); }  c.r[0] = 0x80040154ULL; });

    // ---------------- handles: files, events, semaphores, mutexes -----------------
    H("GetStdHandle", [](GuestCpu& c) { const auto n = static_cast<std::int32_t>(c.arg(0)); c.r[0] = n == -10 ? 3 : n == -11 ? 7 : n == -12 ? 11 : kInvalidHandleValue(); });
    H("SetStdHandle", ok);
    H("GetFileType", [this](GuestCpu& c) { const auto h = c.arg(0); c.r[0] = (h == 3 || h == 7 || h == 11) ? 2 : (win_handles_.count(h) && win_handles_[h].kind == WinHandle::File ? 1 : 0); });
    H("GetConsoleMode", zero); H("SetConsoleMode", ok); H("SetConsoleCtrlHandler", ok); H("GetConsoleOutputCP", [](GuestCpu& c) { c.r[0] = 437; }); H("GetConsoleCP", [](GuestCpu& c) { c.r[0] = 437; });
    H("WriteConsoleW", [this](GuestCpu& c) { // (h, buf, nchars, written*, reserved)
        const auto w = mem_.read_wstring(c.arg(1), static_cast<std::size_t>((std::min<std::uint64_t>)(c.arg(2), 4096)));
        if (stdout_log_.size() < 200) stdout_log_.push_back(utf16_to_utf8(w));
        if (c.arg(3)) { mem_.write<std::uint32_t>(c.arg(3), static_cast<std::uint32_t>(c.arg(2))); }  c.r[0] = 1; });
    H("CreateFileW", [this](GuestCpu& c) { c.r[0] = win_create_file(utf16_to_utf8(mem_.read_wstring(c.arg(0))), c.arg(1), static_cast<std::uint32_t>(c.arg(4))); });
    H("CreateFile2", [this](GuestCpu& c) { c.r[0] = win_create_file(utf16_to_utf8(mem_.read_wstring(c.arg(0))), c.arg(1), static_cast<std::uint32_t>(c.arg(3))); }); // (name, access, share, disposition, params)
    H("ReadFile", [this](GuestCpu& c) { // (h, buf, n, *read, overlapped)
        const auto it = win_handles_.find(c.arg(0));
        if (it == win_handles_.end() || !it->second.fp) { last_error_ = 6; c.r[0] = 0; return; }
        std::vector<std::uint8_t> tmp(static_cast<std::size_t>((std::min<std::uint64_t>)(c.arg(2), 1ULL << 22)));
        std::uint64_t total = 0, want = c.arg(2);
        while (want) { const std::size_t n = std::fread(tmp.data(), 1, static_cast<std::size_t>((std::min<std::uint64_t>)(want, tmp.size())), it->second.fp); if (!n) break; mem_.write_bytes(c.arg(1) + total, tmp.data(), n); total += n; want -= n; }
        if (c.arg(3)) { mem_.write<std::uint32_t>(c.arg(3), static_cast<std::uint32_t>(total)); }  c.r[0] = 1; });
    H("WriteFile", [this](GuestCpu& c) { // (h, buf, n, *written, overlapped)
        const auto h = c.arg(0); const std::uint64_t n = c.arg(2);
        if (h == 7 || h == 11) { std::string s = mem_.read_cstring(c.arg(1), static_cast<std::size_t>((std::min<std::uint64_t>)(n, 2048))); if (stdout_log_.size() < 200) stdout_log_.push_back(s.substr(0, static_cast<std::size_t>(n))); if (c.arg(3)) mem_.write<std::uint32_t>(c.arg(3), static_cast<std::uint32_t>(n)); c.r[0] = 1; return; }
        const auto it = win_handles_.find(h);
        if (it == win_handles_.end() || !it->second.fp || !it->second.writable) { last_error_ = 5; c.r[0] = 0; return; }
        std::vector<std::uint8_t> tmp(static_cast<std::size_t>((std::min<std::uint64_t>)(n, 1ULL << 22))); std::uint64_t done = 0;
        while (done < n) { const auto k = (std::min<std::uint64_t>)(n - done, tmp.size()); mem_.read_bytes(c.arg(1) + done, tmp.data(), static_cast<std::size_t>(k)); std::fwrite(tmp.data(), 1, static_cast<std::size_t>(k), it->second.fp); done += k; }
        if (c.arg(3)) { mem_.write<std::uint32_t>(c.arg(3), static_cast<std::uint32_t>(n)); }  c.r[0] = 1; });
    H("FlushFileBuffers", [this](GuestCpu& c) { const auto it = win_handles_.find(c.arg(0)); if (it != win_handles_.end() && it->second.fp) std::fflush(it->second.fp); c.r[0] = 1; });
    H("GetFileSizeEx", [this](GuestCpu& c) {
        const auto it = win_handles_.find(c.arg(0)); if (it == win_handles_.end() || !it->second.fp) { last_error_ = 6; c.r[0] = 0; return; }
        const long cur = std::ftell(it->second.fp); std::fseek(it->second.fp, 0, SEEK_END); const long end = std::ftell(it->second.fp); std::fseek(it->second.fp, cur, SEEK_SET);
        mem_.write<std::int64_t>(c.arg(1), end); c.r[0] = 1; });
    H("SetFilePointerEx", [this](GuestCpu& c) { // (h, distance, *newpos, method)
        const auto it = win_handles_.find(c.arg(0)); if (it == win_handles_.end() || !it->second.fp) { last_error_ = 6; c.r[0] = 0; return; }
        const int whence = c.arg(3) == 0 ? SEEK_SET : c.arg(3) == 1 ? SEEK_CUR : SEEK_END;
#if defined(_WIN32)
        _fseeki64(it->second.fp, static_cast<long long>(c.arg(1)), whence); const long long pos = _ftelli64(it->second.fp);
#else
        fseeko(it->second.fp, static_cast<off_t>(c.arg(1)), whence); const long long pos = ftello(it->second.fp);
#endif
        if (c.arg(2)) { mem_.write<std::int64_t>(c.arg(2), pos); }  c.r[0] = 1; });
    H("GetFileAttributesExW", [this](GuestCpu& c) { // (name, level, WIN32_FILE_ATTRIBUTE_DATA*)
        std::error_code ec; const auto p = resolve_path(utf16_to_utf8(mem_.read_wstring(c.arg(0))), false);
        if (!std::filesystem::exists(p, ec)) { last_error_ = 2; c.r[0] = 0; return; }
        const bool dir = std::filesystem::is_directory(p, ec); const std::uint64_t sz = dir ? 0 : static_cast<std::uint64_t>(std::filesystem::file_size(p, ec));
        const std::uint64_t o = c.arg(2); zero_fill(o, 36);
        mem_.write<std::uint32_t>(o, dir ? 0x10 : 0x80); for (unsigned i = 0; i < 3; ++i) mem_.write<std::uint64_t>(o + 4 + i * 8ULL, filetime_);
        mem_.write<std::uint32_t>(o + 28, static_cast<std::uint32_t>(sz >> 32)); mem_.write<std::uint32_t>(o + 32, static_cast<std::uint32_t>(sz)); c.r[0] = 1; });
    H("CreateDirectoryW", [this](GuestCpu& c) { std::error_code ec; std::filesystem::create_directories(resolve_path(utf16_to_utf8(mem_.read_wstring(c.arg(0))), true), ec); c.r[0] = ec ? 0 : 1; });
    H("MoveFileExW", [this](GuestCpu& c) { std::error_code ec; std::filesystem::rename(resolve_path(utf16_to_utf8(mem_.read_wstring(c.arg(0))), true), resolve_path(utf16_to_utf8(mem_.read_wstring(c.arg(1))), true), ec); c.r[0] = ec ? 0 : 1; });
    H("FindFirstFileExW", [this](GuestCpu& c) { // (name, level, WIN32_FIND_DATAW*, ...)
        const std::string pat = utf16_to_utf8(mem_.read_wstring(c.arg(0)));
        std::string dir = pat, mask = "*"; const auto sl = pat.find_last_of("\\/"); if (sl != std::string::npos) { dir = pat.substr(0, sl); mask = pat.substr(sl + 1); }
        WinHandle h; h.kind = WinHandle::Find; std::error_code ec; std::vector<std::string> entries;
        const auto root = resolve_path(dir, false);
        auto matches = [&](const std::string& n) { if (mask == "*" || mask == "*.*") return true; const auto star = mask.find('*'); if (star == std::string::npos) return n == mask; return n.size() >= mask.size() - 1 && n.rfind(mask.substr(star + 1)) == n.size() - (mask.size() - star - 1) && n.compare(0, star, mask, 0, star) == 0; };
        if (std::filesystem::is_directory(root, ec)) for (const auto& e : std::filesystem::directory_iterator(root, ec)) { const auto n = PeImage::path_utf8(e.path().filename()); if (matches(n)) entries.push_back(n); }
        if (entries.empty()) { last_error_ = 2; c.r[0] = kInvalidHandleValue(); return; }
        h.entries = entries; h.dir = root; h.index = 0; const std::uint64_t id = next_win_handle_(); win_handles_[id] = h;
        fill_find_data(c.arg(2), win_handles_[id]); win_handles_[id].index = 1; c.r[0] = id; });
    H("FindNextFileW", [this](GuestCpu& c) { const auto it = win_handles_.find(c.arg(0)); if (it == win_handles_.end() || it->second.index >= it->second.entries.size()) { last_error_ = 18; c.r[0] = 0; return; } fill_find_data(c.arg(1), it->second); ++it->second.index; c.r[0] = 1; });
    H("FindClose", [this](GuestCpu& c) { win_handles_.erase(c.arg(0)); c.r[0] = 1; });
    H("CloseHandle", [this](GuestCpu& c) { const auto it = win_handles_.find(c.arg(0)); if (it != win_handles_.end()) { if (it->second.fp) std::fclose(it->second.fp); win_handles_.erase(it); } c.r[0] = 1; });

    auto create_event = [this](bool manual, bool initial) { WinHandle h; h.kind = WinHandle::Event; h.manual = manual; h.signaled = initial; const auto id = next_win_handle_(); win_handles_[id] = h; return id; };
    H("CreateEventW", [create_event](GuestCpu& c) { c.r[0] = create_event(c.arg(1) != 0, c.arg(2) != 0); }); // (sa, manualReset, initial, name)
    H("CreateEventExW", [create_event](GuestCpu& c) { c.r[0] = create_event((c.arg(2) & 1) != 0, (c.arg(2) & 2) != 0); }); // (sa, name, flags, access)
    H("SetEvent", [this](GuestCpu& c) { const auto it = win_handles_.find(c.arg(0)); if (it != win_handles_.end()) it->second.signaled = true; c.r[0] = 1; });
    H("ResetEvent", [this](GuestCpu& c) { const auto it = win_handles_.find(c.arg(0)); if (it != win_handles_.end()) it->second.signaled = false; c.r[0] = 1; });
    H("CreateSemaphoreExW", [this](GuestCpu& c) { WinHandle h; h.kind = WinHandle::Semaphore; h.count = static_cast<std::int64_t>(c.arg(1)); const auto id = next_win_handle_(); win_handles_[id] = h; c.r[0] = id; }); // (sa, initial, max, name, flags, access)
    H("ReleaseSemaphore", [this](GuestCpu& c) { const auto it = win_handles_.find(c.arg(0)); if (it != win_handles_.end()) { if (c.arg(2)) mem_.write<std::uint32_t>(c.arg(2), static_cast<std::uint32_t>(it->second.count)); it->second.count += static_cast<std::int64_t>(c.arg(1)); } c.r[0] = 1; });
    H("CreateMutexExW", [this](GuestCpu& c) { WinHandle h; h.kind = WinHandle::Mutex; const auto id = next_win_handle_(); win_handles_[id] = h; c.r[0] = id; });
    H("ReleaseMutex", ok);
    auto wait_one = [this](std::uint64_t h, std::uint64_t timeout) -> std::uint64_t { // 0 = signalled, 0x102 = timeout
        const auto it = win_handles_.find(h);
        if (it != win_handles_.end()) {
            WinHandle& w = it->second;
            if (w.kind == WinHandle::Event) { if (w.signaled) { if (!w.manual) w.signaled = false; return 0; } }
            else if (w.kind == WinHandle::Semaphore) { if (w.count > 0) { --w.count; return 0; } }
            else return 0;
            if (timeout == 0) return 0x102;
            qpc_ += (timeout == 0xFFFFFFFFULL ? 10 : timeout) * 10000ULL; // virtual time passes while "waiting"
            if (com_log_.insert("wait-optimistic").second) milestone("a wait on an unsignalled event/semaphore was answered optimistically (no other guest thread is running to signal it)");
            return 0;
        }
        return 0; };
    H("WaitForSingleObject", [wait_one](GuestCpu& c) { c.r[0] = wait_one(c.arg(0), c.arg(1) & 0xFFFFFFFFULL); });
    H("WaitForSingleObjectEx", [wait_one](GuestCpu& c) { c.r[0] = wait_one(c.arg(0), c.arg(1) & 0xFFFFFFFFULL); });
    H("WaitForMultipleObjectsEx", [this, wait_one](GuestCpu& c) { // (count, handles*, waitAll, ms, alertable)
        const auto n = c.arg(0); std::uint64_t result = 0x102;
        for (std::uint64_t i = 0; i < n && i < 64; ++i) { if (wait_one(mem_.read<std::uint64_t>(c.arg(1) + i * 8), 0) == 0) { result = i; break; } }
        if (result == 0x102 && (c.arg(3) & 0xFFFFFFFFULL) != 0) result = 0;
        c.r[0] = result; });
    H("WaitOnAddress", ok); H("WakeByAddressSingle", [](GuestCpu&) {}); H("WakeByAddressAll", [](GuestCpu&) {});
    H("Sleep", [this](GuestCpu& c) { qpc_ += (c.arg(0) & 0xFFFFFFFFULL) * 10000ULL; });
    H("SleepEx", [this](GuestCpu& c) { qpc_ += (c.arg(0) & 0xFFFFFFFFULL) * 10000ULL; c.r[0] = 0; });
    H("SwitchToThread", zero);

    // ---------------- threads / pools / timers (recorded, never run) -----------------
    H("ExitThread", [](GuestCpu& c) { c.request_stop(GuestCpu::Stop::HostRequest, "ExitThread on the main guest thread"); });
    H("SetThreadPriority", ok); H("ResumeThread", zero); H("SuspendThread", zero); H("SetThreadDescription", zero);
    H("CreateThreadpool", [this](GuestCpu& c) { c.r[0] = heap_alloc(32, true); });
    H("CreateThreadpoolTimer", [this](GuestCpu& c) { c.r[0] = heap_alloc(32, true); });
    H("CreateThreadpoolWait", [this](GuestCpu& c) { c.r[0] = heap_alloc(32, true); });
    H("SetThreadpoolTimer", [](GuestCpu&) {}); H("SetThreadpoolWait", [](GuestCpu&) {}); H("CloseThreadpoolTimer", [](GuestCpu&) {}); H("CloseThreadpoolWait", [](GuestCpu&) {});
    H("CreateTimerQueue", [this](GuestCpu& c) { c.r[0] = heap_alloc(32, true); });
    H("CreateTimerQueueTimer", [this](GuestCpu& c) { milestone("CreateTimerQueueTimer requested (timer never fires: no timer thread)"); if (c.arg(0)) mem_.write<std::uint64_t>(c.arg(0), heap_alloc(32, true)); c.r[0] = 1; });
    H("DeleteTimerQueueTimer", ok);
    H("QueueUserWorkItem", [this](GuestCpu& c) { milestone("QueueUserWorkItem(" + locate(c.arg(0)) + ") recorded but NOT run (no worker threads)"); c.r[0] = 1; });

    // ---------------- interlocked singly-linked lists (SLIST_HEADER: Next @0, depth @8) -----------------
    H("InterlockedPushEntrySList", [this](GuestCpu& c) { const auto head = c.arg(0), e = c.arg(1); const auto first = mem_.read<std::uint64_t>(head); mem_.write<std::uint64_t>(e, first); mem_.write<std::uint64_t>(head, e); mem_.write<std::uint64_t>(head + 8, mem_.read<std::uint64_t>(head + 8) + 1); c.r[0] = first; });
    H("InterlockedPopEntrySList", [this](GuestCpu& c) { const auto head = c.arg(0); const auto first = mem_.read<std::uint64_t>(head); if (first) { mem_.write<std::uint64_t>(head, mem_.read<std::uint64_t>(first)); const auto d = mem_.read<std::uint64_t>(head + 8); mem_.write<std::uint64_t>(head + 8, d ? d - 1 : 0); } c.r[0] = first; });
    H("InterlockedFlushSList", [this](GuestCpu& c) { const auto head = c.arg(0); const auto first = mem_.read<std::uint64_t>(head); mem_.write<std::uint64_t>(head, 0); mem_.write<std::uint64_t>(head + 8, 0); c.r[0] = first; });
    H("QueryDepthSList", [this](GuestCpu& c) { c.r[0] = mem_.read<std::uint64_t>(c.arg(0) + 8) & 0xFFFF; });
    H("InitOnceBeginInitialize", [this](GuestCpu& c) { // (once*, flags, BOOL* pending, void** ctx)
        const auto once = c.arg(0); const bool done = mem_.read<std::uint64_t>(once) != 0;
        if (c.arg(2)) { mem_.write<std::uint32_t>(c.arg(2), done ? 0 : 1); }  if (done && c.arg(3)) mem_.write<std::uint64_t>(c.arg(3), mem_.read<std::uint64_t>(once) & ~3ULL); c.r[0] = 1; });
    H("InitOnceComplete", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(0), (c.arg(2) & ~3ULL) | 2); c.r[0] = 1; });
}

// ----- helpers used by the baseline handlers -----
inline std::uint64_t XboxTitleRuntime::virtual_alloc(std::uint64_t addr, std::uint64_t size, std::uint32_t type) {
    if (size == 0) { last_error_ = 87; return 0; }
    const std::uint64_t rsize = (size + 0xFFF) & ~0xFFFULL;
    const bool reserve = (type & 0x2000) != 0, commit = (type & 0x1000) != 0;
    if (addr == 0) {
        if (!reserve && !commit) { last_error_ = 87; return 0; }
        const std::uint64_t gran = rsize >= 0x10000 ? 0x10000 : 0x1000; (void)gran;
        std::uint64_t base = (virt_next_ + 0xFFFFULL) & ~0xFFFFULL;
        virt_next_ = base + ((rsize + 0xFFFFULL) & ~0xFFFFULL);
        if (virt_next_ > xrt::kVirtMax) { last_error_ = 8; return 0; }
        virt_regions_[base] = {rsize};
        if (commit && !mem_.map(base, rsize, "virtualalloc")) { last_error_ = 8; virt_regions_.erase(base); return 0; }
        return base;
    }
    // explicit address: (re)commit within, or create, a reservation
    const std::uint64_t base = addr & ~0xFFFULL, end = (addr + size + 0xFFF) & ~0xFFFULL;
    if (reserve && !commit && !virt_regions_.count(base)) virt_regions_[base] = {end - base};
    if (commit) {
        if (!virt_regions_.count(base)) { bool inside = false; for (const auto& kv : virt_regions_) if (base >= kv.first && base < kv.first + kv.second.size) inside = true; if (!inside) virt_regions_[base] = {end - base}; }
        if (!mem_.map(base, end - base, "virtualalloc")) { last_error_ = 8; return 0; }
    }
    return addr;
}

inline void XboxTitleRuntime::fill_system_info(std::uint64_t o) {
    zero_fill(o, 48);
    mem_.write<std::uint16_t>(o, 9);                 // PROCESSOR_ARCHITECTURE_AMD64
    mem_.write<std::uint32_t>(o + 4, 4096);
    mem_.write<std::uint64_t>(o + 8, 0x10000); mem_.write<std::uint64_t>(o + 16, 0x00007FFFFFFEFFFFULL);
    mem_.write<std::uint64_t>(o + 24, 0x3F); mem_.write<std::uint32_t>(o + 32, 6);
    mem_.write<std::uint32_t>(o + 36, 8664); mem_.write<std::uint32_t>(o + 40, 0x10000);
    mem_.write<std::uint16_t>(o + 44, 6); mem_.write<std::uint16_t>(o + 46, 0x6001);
}

inline std::uint64_t XboxTitleRuntime::tick_ms() { qpc_ += 2000; return 100000 + qpc_ / 10000; }

inline std::uint64_t XboxTitleRuntime::win_create_file(const std::string& path, std::uint64_t access, std::uint32_t disposition) {
    const bool write = (access & (0x40000000ULL | 0x2 | 0x4 | 0x10000000ULL)) != 0;
    std::error_code ec;
    WinHandle h; h.kind = WinHandle::File; h.writable = write; h.path = path;
    const auto src = resolve_path(path, false);
    const bool exists = std::filesystem::exists(src, ec);
    std::string mode = "rb";
    if (!write) {
        if (!exists || std::filesystem::is_directory(src, ec)) { last_error_ = 2; if (files_opened_.size() < 64) files_opened_.push_back("FAILED (CreateFileW) " + path); return ~0ULL; }
        h.fp = open_host_file(src, mode);
    } else {
        const auto dst = resolve_path(path, true);
        std::filesystem::create_directories(dst.parent_path(), ec);
        const bool dst_exists = std::filesystem::exists(dst, ec);
        if (disposition == 1 && (dst_exists || exists)) { last_error_ = 80; return ~0ULL; }                    // CREATE_NEW
        if ((disposition == 3 || disposition == 5) && !dst_exists && !exists) { last_error_ = 2; return ~0ULL; } // OPEN_EXISTING / TRUNCATE_EXISTING
        if (!dst_exists && exists && disposition != 2 && disposition != 5) std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec); // copy-on-write from the package
        mode = (disposition == 2 || disposition == 5 || disposition == 1 || !(dst_exists || exists)) ? "wb+" : "rb+";
        h.fp = open_host_file(dst, mode);
    }
    if (files_opened_.size() < 64) files_opened_.push_back(std::string(h.fp ? "opened " : "FAILED ") + "(CreateFileW" + (write ? ", write" : "") + ") " + path);
    if (!h.fp) { last_error_ = 5; return ~0ULL; }
    const std::uint64_t id = next_win_handle_();
    win_handles_[id] = h;
    last_error_ = 0;
    return id;
}

inline void XboxTitleRuntime::fill_find_data(std::uint64_t out, const WinHandle& h) {
    zero_fill(out, 592);
    const std::string name = h.entries[h.index];
    std::error_code ec; const auto p = h.dir / name;
    const bool dir = std::filesystem::is_directory(p, ec);
    const std::uint64_t sz = dir ? 0 : static_cast<std::uint64_t>(std::filesystem::file_size(p, ec));
    mem_.write<std::uint32_t>(out, dir ? 0x10 : 0x80);
    for (unsigned i = 0; i < 3; ++i) mem_.write<std::uint64_t>(out + 4 + i * 8ULL, filetime_);
    mem_.write<std::uint32_t>(out + 28, static_cast<std::uint32_t>(sz >> 32)); mem_.write<std::uint32_t>(out + 32, static_cast<std::uint32_t>(sz));
    const auto w = utf8_to_utf16(name);
    for (std::size_t i = 0; i < w.size() && i < 259; ++i) mem_.write<std::uint16_t>(out + 44 + i * 2, w[i]);
}

} // namespace someboxone
