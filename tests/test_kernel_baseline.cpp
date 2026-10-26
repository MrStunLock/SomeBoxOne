// Tests for the v0.31.0 runtime pieces: WinRT async objects (including the regression for the "flat vtable overwrites the
// completion delegate" bug seen in Sonic Mania and Minecraft LCE), module handles, and the kernel32-level baseline.
// A tiny synthetic PE gives the runtime a valid image; handlers are then called directly through the public debug hooks.

#include "title_launch.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace someboxone;
using Bytes = std::vector<std::uint8_t>;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; ++g_failures; } } while (0)

static void put16(Bytes& b, std::size_t o, std::uint16_t v) { std::memcpy(&b[o], &v, 2); }
static void put32(Bytes& b, std::size_t o, std::uint32_t v) { std::memcpy(&b[o], &v, 4); }
static void put64(Bytes& b, std::size_t o, std::uint64_t v) { std::memcpy(&b[o], &v, 8); }

// Minimal PE32+ with an empty import table (only used to give the runtime a mapped image).
static Bytes tiny_pe() {
    Bytes f(0x800, 0);
    f[0] = 'M'; f[1] = 'Z'; put32(f, 0x3C, 0x80);
    const std::size_t pe = 0x80;
    std::memcpy(&f[pe], "PE\0\0", 4);
    put16(f, pe + 4, 0x8664); put16(f, pe + 6, 1); put16(f, pe + 20, 240); put16(f, pe + 22, 0x22);
    const std::size_t opt = pe + 24;
    put16(f, opt, 0x20B); put32(f, opt + 16, 0x1000);
    put64(f, opt + 24, 0x140000000ULL); put32(f, opt + 32, 0x1000); put32(f, opt + 36, 0x200);
    put32(f, opt + 56, 0x2000); put32(f, opt + 60, 0x400); put16(f, opt + 68, 2);
    put64(f, opt + 72, 0x100000); put64(f, opt + 80, 0x1000); put32(f, opt + 108, 16);
    const std::size_t sec = opt + 240;
    std::memcpy(&f[sec], ".text", 5); put32(f, sec + 8, 0x200); put32(f, sec + 12, 0x1000); put32(f, sec + 16, 0x200); put32(f, sec + 20, 0x400); put32(f, sec + 36, 0x60000020);
    f[0x400] = 0xC3;
    return f;
}

static void write_wide(GuestMemory& m, std::uint64_t at, const std::u16string& s) {
    for (std::size_t i = 0; i < s.size(); ++i) m.write<std::uint16_t>(at + i * 2, s[i]);
    m.write<std::uint16_t>(at + s.size() * 2, 0);
}
static void write_guid(GuestMemory& m, std::uint64_t at, std::uint32_t d1, std::uint16_t d2, std::uint16_t d3, std::initializer_list<std::uint8_t> d4) {
    m.write<std::uint32_t>(at, d1); m.write<std::uint16_t>(at + 4, d2); m.write<std::uint16_t>(at + 6, d3);
    std::uint64_t o = 8; for (auto b : d4) m.write<std::uint8_t>(at + o++, b);
}

int main() {
    const auto tmp = std::filesystem::temp_directory_path() / "someboxone_kernel_test";
    std::error_code ec; std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp / "Mount", ec);
    { std::ofstream f(tmp / "Mount" / "hello.bin", std::ios::binary); f << "SOMEBOXONE"; }

    PeImage img; img.file = tiny_pe();
    CHECK(img.parse());
    TitleLaunchOptions o; o.package_root = tmp / "Mount"; o.save_root = tmp / "Save"; o.real_dlls.clear();
    XboxTitleRuntime rt(img, o);
    std::string err;
    CHECK(rt.debug_setup(&err));
    GuestMemory& m = rt.memory();
    CHECK(m.map(0x50000000, 0x10000, "scratch"));
    const std::uint64_t S = 0x50000000;

    // ---- last error, TLS/FLS ----
    rt.debug_call("SetLastError", {1234});
    CHECK(rt.debug_call("GetLastError", {}) == 1234);
    const auto fls = rt.debug_call("FlsAlloc", {0});
    CHECK(rt.debug_call("FlsSetValue", {fls, 0xABCD}) == 1);
    CHECK(rt.debug_call("FlsGetValue", {fls}) == 0xABCD);
    const auto tls = rt.debug_call("TlsAlloc", {});
    rt.debug_call("TlsSetValue", {tls, 77});
    CHECK(rt.debug_call("TlsGetValue", {tls}) == 77);

    // ---- virtual memory: reserve lazily, commit maps zero pages, query reports state, free unmaps ----
    const std::uint64_t base = rt.debug_call("VirtualAlloc", {0, 0x100000, 0x2000, 4});          // MEM_RESERVE
    CHECK(base != 0 && (base & 0xFFFF) == 0 && !m.is_mapped(base));
    CHECK(rt.debug_call("VirtualAlloc", {base, 0x2000, 0x1000, 4}) == base);                    // MEM_COMMIT 2 pages
    CHECK(m.is_mapped(base, 0x2000) && m.read<std::uint32_t>(base) == 0);
    m.write<std::uint32_t>(base + 0x1000, 0xCAFEBABE);
    CHECK(rt.debug_call("VirtualQuery", {base, S, 48}) == 48);
    CHECK(m.read<std::uint32_t>(S + 32) == 0x1000 && m.read<std::uint64_t>(S + 24) >= 0x2000);  // State=MEM_COMMIT, RegionSize
    CHECK(rt.debug_call("VirtualQuery", {base + 0x10000, S, 48}) == 48);
    CHECK(m.read<std::uint32_t>(S + 32) != 0x1000);                                             // not committed
    CHECK(rt.debug_call("VirtualFree", {base, 0, 0x8000}) == 1 && !m.is_mapped(base));          // MEM_RELEASE

    // ---- heaps ----
    const auto h1 = rt.debug_call("HeapAlloc", {0, 0, 100});
    CHECK(h1 != 0 && rt.debug_call("HeapSize", {0, 0, h1}) >= 100);
    m.write<std::uint32_t>(h1, 0x11223344);
    const auto h2 = rt.debug_call("HeapReAlloc", {0, 0, h1, 4096});
    CHECK(h2 != 0 && m.read<std::uint32_t>(h2) == 0x11223344);

    // ---- system info ----
    rt.debug_call("GetSystemInfo", {S});
    CHECK(m.read<std::uint16_t>(S) == 9 && m.read<std::uint32_t>(S + 4) == 4096 && m.read<std::uint32_t>(S + 32) == 6);

    // ---- events, semaphores, waits ----
    const auto ev = rt.debug_call("CreateEventW", {0, 1, 0, 0});                                // manual reset, not signalled
    CHECK(rt.debug_call("WaitForSingleObject", {ev, 0}) == 0x102);                              // WAIT_TIMEOUT
    rt.debug_call("SetEvent", {ev});
    CHECK(rt.debug_call("WaitForSingleObject", {ev, 0}) == 0);
    const auto sem = rt.debug_call("CreateSemaphoreExW", {0, 1, 4, 0, 0, 0});
    CHECK(rt.debug_call("WaitForSingleObject", {sem, 0}) == 0 && rt.debug_call("WaitForSingleObject", {sem, 0}) == 0x102);

    // ---- SList ----
    const auto head = S + 0x100, e1 = S + 0x200, e2 = S + 0x300;
    rt.debug_call("InitializeSListHead", {head});
    rt.debug_call("InterlockedPushEntrySList", {head, e1}); rt.debug_call("InterlockedPushEntrySList", {head, e2});
    CHECK(rt.debug_call("QueryDepthSList", {head}) == 2);
    CHECK(rt.debug_call("InterlockedPopEntrySList", {head}) == e2 && rt.debug_call("InterlockedPopEntrySList", {head}) == e1 && rt.debug_call("InterlockedPopEntrySList", {head}) == 0);

    // ---- files: package reads, sandboxed writes ----
    write_wide(m, S + 0x1000, u"hello.bin");
    const auto fh = rt.debug_call("CreateFileW", {S + 0x1000, 0x80000000ULL, 1, 0, 3, 0x80, 0});
    CHECK(fh != ~0ULL);
    CHECK(rt.debug_call("GetFileSizeEx", {fh, S + 0x1100}) == 1 && m.read<std::int64_t>(S + 0x1100) == 10);
    CHECK(rt.debug_call("ReadFile", {fh, S + 0x1200, 10, S + 0x1300, 0}) == 1 && m.read<std::uint32_t>(S + 0x1300) == 10);
    CHECK(m.read_cstring(S + 0x1200, 10) == "SOMEBOXONE");
    CHECK(rt.debug_call("CloseHandle", {fh}) == 1);
    write_wide(m, S + 0x1400, u"missing.bin");
    CHECK(rt.debug_call("CreateFileW", {S + 0x1400, 0x80000000ULL, 1, 0, 3, 0x80, 0}) == ~0ULL && rt.debug_call("GetLastError", {}) == 2);
    write_wide(m, S + 0x1500, u"out\\\\save.dat");
    const auto wh = rt.debug_call("CreateFileW", {S + 0x1500, 0x40000000ULL, 0, 0, 2, 0x80, 0});     // CREATE_ALWAYS for write
    CHECK(wh != ~0ULL);
    m.write<std::uint32_t>(S + 0x1600, 0x01020304);
    CHECK(rt.debug_call("WriteFile", {wh, S + 0x1600, 4, S + 0x1610, 0}) == 1);
    rt.debug_call("CloseHandle", {wh});
    CHECK(std::filesystem::exists(tmp / "Save" / "out" / "save.dat"));                          // wrote to the SAVE sandbox...
    CHECK(!std::filesystem::exists(tmp / "Mount" / "out"));                                     // ...never into the package folder

    // ---- module handles ----
    write_wide(m, S + 0x2000, u"KERNEL32.dll");
    const auto k32 = rt.debug_call("GetModuleHandleW", {S + 0x2000});
    CHECK(k32 != 0);
    write_wide(m, S + 0x2100, u"api-ms-win-core-synch-l1-2-0.dll");
    CHECK(rt.debug_call("GetModuleHandleW", {S + 0x2100}) != 0);
    write_wide(m, S + 0x2200, u"definitely_not_a_module.dll");
    CHECK(rt.debug_call("GetModuleHandleW", {S + 0x2200}) == 0 && rt.debug_call("GetLastError", {}) == 126);
    const char* nm = "VirtualAlloc"; m.write_bytes(S + 0x2300, nm, 13);
    CHECK(rt.debug_call("GetProcAddress", {k32, S + 0x2300}) >= xrt::kTrapBase);
    const char* bad = "NoSuchExport"; m.write_bytes(S + 0x2400, bad, 13);
    CHECK(rt.debug_call("GetProcAddress", {k32, S + 0x2400}) == 0);

    // ---- WinRT async: typed interface and IAsyncInfo are DIFFERENT vtables (regression for the Sonic Mania / Minecraft crash) ----
    const std::uint64_t kResult = 0x00000000DEADBEEFULL;
    const auto typed = rt.debug_make_async("test", true, 0);                                    // result 0: avoids refcount writes
    (void)kResult;
    write_guid(m, S + 0x3000, 0x36, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46});                       // IAsyncInfo
    CHECK(rt.debug_vcall(typed, 0, {S + 0x3000, S + 0x3100}) == 0);                             // QueryInterface(IAsyncInfo)
    const auto info = m.read<std::uint64_t>(S + 0x3100);
    CHECK(info != 0 && info != typed);
    CHECK(m.read<std::uint64_t>(info) != m.read<std::uint64_t>(typed));                          // distinct vtables
    CHECK(rt.debug_vcall(info, 6, {S + 0x3200}) == 0 && m.read<std::uint32_t>(S + 0x3200) != 0); // get_Id
    CHECK(rt.debug_vcall(info, 7, {S + 0x3210}) == 0 && m.read<std::uint32_t>(S + 0x3210) == 1); // get_Status = Completed
    // put_Completed(delegate): the title's delegate must be INVOKED, and its own vtable pointer must stay intact.
    std::uint64_t seen_async = 0, seen_status = 99; int invokes = 0;
    const auto del = rt.debug_make_delegate([&](GuestCpu& c) { ++invokes; seen_async = c.arg(1); seen_status = c.arg(2); c.r[0] = 0; });
    const auto vt_before = m.read<std::uint64_t>(del);
    CHECK(rt.debug_vcall(typed, 6, {del}) == 0);                                                // put_Completed is slot 6 on the typed object
    CHECK(m.read<std::uint64_t>(del) == vt_before);                                             // NOT overwritten (the original bug wrote an id here)
    CHECK(invokes == 1 && seen_async == typed && seen_status == 1);
    CHECK(rt.debug_vcall(typed, 7, {S + 0x3300}) == 0 && m.read<std::uint64_t>(S + 0x3300) == del); // get_Completed returns the handler

    std::filesystem::remove_all(tmp, ec);
    if (g_failures) { std::cerr << g_failures << " kernel-baseline test failure(s)\n"; return 1; }
    std::cout << "kernel baseline / async / module-handle tests passed\n";
    return 0;
}
