// Portable regression tests for the v0.22.0 title runner: GuestMemory, GuestCpu, PeImage,
// and XboxTitleRuntime (import binding, HLE heap/exit, faults, step limit, real-DLL-free path).
// No game files are needed: a tiny PE64 image is assembled in memory.

#include "title_launch.hpp"

#include <cstdint>
#include <cstring>
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

// Builds a minimal PE32+: .text at RVA 0x1000, .idata at RVA 0x2000 importing `names` from ucrtbase.dll.
// `make_code` receives the IAT RVA of each import and returns the machine code for .text.
template <typename F>
static Bytes build_pe(const std::vector<std::string>& names, F make_code) {
    Bytes f(0x800, 0);
    f[0] = 'M'; f[1] = 'Z'; put32(f, 0x3C, 0x80);
    const std::size_t pe = 0x80;
    std::memcpy(&f[pe], "PE\0\0", 4);
    put16(f, pe + 4, 0x8664); put16(f, pe + 6, 2); put16(f, pe + 20, 240); put16(f, pe + 22, 0x22);
    const std::size_t opt = pe + 24;
    put16(f, opt, 0x20B); put32(f, opt + 16, 0x1000);
    put64(f, opt + 24, 0x140000000ULL); put32(f, opt + 32, 0x1000); put32(f, opt + 36, 0x200);
    put32(f, opt + 56, 0x3000); put32(f, opt + 60, 0x400); put16(f, opt + 68, 2);
    put64(f, opt + 72, 0x100000); put64(f, opt + 80, 0x1000); put32(f, opt + 108, 16);
    put32(f, opt + 112 + 8, 0x2000); put32(f, opt + 112 + 12, 0x28); // import directory
    const std::size_t sec = opt + 240;
    std::memcpy(&f[sec], ".text", 5); put32(f, sec + 8, 0x200); put32(f, sec + 12, 0x1000); put32(f, sec + 16, 0x200); put32(f, sec + 20, 0x400); put32(f, sec + 36, 0x60000020);
    std::memcpy(&f[sec + 40], ".idata", 6); put32(f, sec + 48, 0x200); put32(f, sec + 52, 0x2000); put32(f, sec + 56, 0x200); put32(f, sec + 60, 0x600); put32(f, sec + 76, 0xC0000040);
    // .idata layout (file offset 0x600 == RVA 0x2000)
    const std::size_t idata = 0x600;
    put32(f, idata + 0, 0x2040); put32(f, idata + 12, 0x20E0); put32(f, idata + 16, 0x2140); // ILT, name, IAT (kept separate from names)
    std::vector<std::uint32_t> iat_rvas; std::size_t hn = 0x2090;
    for (std::size_t i = 0; i < names.size(); ++i) {
        iat_rvas.push_back(static_cast<std::uint32_t>(0x2140 + i * 8));
        put64(f, idata + 0x40 + i * 8, hn); put64(f, idata + 0x140 + i * 8, hn);
        std::memcpy(&f[idata + (hn - 0x2000) + 2], names[i].c_str(), names[i].size() + 1);
        hn += names[i].size() + 4;
    }
    std::memcpy(&f[idata + 0xE0], "ucrtbase.dll", 13);
    const Bytes code = make_code(iat_rvas);
    std::memcpy(&f[0x400], code.data(), code.size());
    return f;
}

static void emit_call_iat(Bytes& code, std::uint32_t iat_rva) { // call [rip+disp32]
    const std::uint32_t next = 0x1000 + static_cast<std::uint32_t>(code.size()) + 6;
    code.push_back(0xFF); code.push_back(0x15);
    const std::uint32_t d = iat_rva - next;
    for (int i = 0; i < 4; ++i) code.push_back(static_cast<std::uint8_t>(d >> (i * 8)));
}

static TitleLaunchReport run_image(Bytes file, std::uint64_t max = 100000,
                                   std::vector<std::string>* progress_lines = nullptr) {
    static PeImage img;               // runtime keeps a reference; reuse storage per call
    img = PeImage();
    img.file = std::move(file);
    if (!img.parse()) { TitleLaunchReport r; r.load_error = img.error; return r; }
    TitleLaunchOptions o; o.package_root = "."; o.save_root = "."; o.max_instructions = max; o.real_dlls.clear();
    o.progress_sink = [progress_lines](const std::string& line) {
        if (progress_lines) progress_lines->push_back(line);
    };
    XboxTitleRuntime rt(img, o);
    return rt.run();
}

static void test_cpu_and_memory() {
    GuestMemory mem;
    CHECK(mem.map(0x10000, 0x2000, "t"));
    CHECK(!mem.is_mapped(0x20000));
    mem.write<std::uint64_t>(0x10FFC, 0x1122334455667788ULL);   // crosses a page boundary
    CHECK(mem.read<std::uint64_t>(0x10FFC) == 0x1122334455667788ULL);
    bool faulted = false;
    try { (void)mem.read<std::uint8_t>(0x30000); } catch (const GuestFault& f) { faulted = f.address == 0x30000 && !f.write; }
    CHECK(faulted);

    // loop: sum 1..10 via rax, then HLT  (xor eax,eax; mov ecx,10; add rax,rcx; loop-by-dec/jnz; hlt)
    const Bytes prog = {0x31, 0xC0, 0xB9, 0x0A, 0x00, 0x00, 0x00, 0x48, 0x01, 0xC8, 0xFF, 0xC9, 0x75, 0xF9, 0xF4};
    for (std::size_t i = 0; i < prog.size(); ++i) mem.write<std::uint8_t>(0x10000 + i, prog[i]);
    GuestCpu cpu(mem); cpu.rip = 0x10000; cpu.run(1000);
    CHECK(cpu.stop == GuestCpu::Stop::Halt && cpu.r[0] == 55);

    // rep stosb + rep movsb across mapped memory
    const Bytes fill = {0x48, 0x8D, 0x3D, 0xF9, 0x0E, 0x00, 0x00, // lea rdi,[rip+0xEF9] -> 0x11000
                        0xB9, 0x10, 0x00, 0x00, 0x00, 0xB0, 0xAB, 0xF3, 0xAA, 0xF4};
    for (std::size_t i = 0; i < fill.size(); ++i) mem.write<std::uint8_t>(0x10100 + i, fill[i]);
    GuestCpu c2(mem); c2.rip = 0x10100; c2.run(100);
    CHECK(c2.stop == GuestCpu::Stop::Halt && mem.read<std::uint8_t>(0x11000) == 0xAB && mem.read<std::uint8_t>(0x1100F) == 0xAB && mem.read<std::uint8_t>(0x11010) == 0);

    // divide error and unsupported opcode are reported, not crashed
    mem.write<std::uint8_t>(0x10200, 0xF7); mem.write<std::uint8_t>(0x10201, 0xF1); // div ecx with ecx=0
    GuestCpu c3(mem); c3.rip = 0x10200; c3.run(10);
    CHECK(c3.stop == GuestCpu::Stop::DivideError);
    mem.write<std::uint8_t>(0x10300, 0x0F); mem.write<std::uint8_t>(0x10301, 0xFF);
    GuestCpu c4(mem); c4.rip = 0x10300; c4.run(10);
    CHECK(c4.stop == GuestCpu::Stop::Unsupported && c4.stop_detail.find("0x10300") != std::string::npos);
}

static void test_runtime_malloc_exit() {
    // sub rsp,0x28; mov ecx,0x40; call malloc; mov dword [rax],0x12345678; mov ecx,[rax]; call exit
    std::vector<std::string> progress_lines;
    auto r = run_image(build_pe({"malloc", "exit"}, [](const std::vector<std::uint32_t>& iat) {
        Bytes c = {0x48, 0x83, 0xEC, 0x28, 0xB9, 0x40, 0x00, 0x00, 0x00};
        emit_call_iat(c, iat[0]);
        for (auto b : Bytes{0xC7, 0x00, 0x78, 0x56, 0x34, 0x12, 0x8B, 0x08}) c.push_back(b);
        emit_call_iat(c, iat[1]);
        c.push_back(0xCC);
        return c;
    }), 100000, &progress_lines);
    CHECK(!progress_lines.empty());
    bool saw_map_milestone = false;
    for (const auto& line : progress_lines) if (line.find("image mapped") != std::string::npos) saw_map_milestone = true;
    CHECK(saw_map_milestone);
    CHECK(r.image_loaded);
    CHECK(r.exited && r.exit_code == 0x12345678);
    CHECK(r.stop_kind == "exit");
    CHECK(r.imports_total == 2 && r.imports_implemented == 2);
    CHECK(r.heap_bytes >= 0x40);
}

static void test_runtime_operator_new_import() {
    // Peggle 2 imports this MSVC x64 scalar operator-new spelling. It must return
    // a guest heap block rather than the generic zero-valued missing-import stub.
    auto r = run_image(build_pe({"??2@YAPEAX_K@Z", "exit"}, [](const std::vector<std::uint32_t>& iat) {
        Bytes c = {0x48, 0x83, 0xEC, 0x28, 0xB9, 0xA8, 0x00, 0x00, 0x00}; // RCX = 0xa8
        emit_call_iat(c, iat[0]);
        for (auto b : Bytes{0xC7, 0x00, 0x78, 0x56, 0x34, 0x12, 0x8B, 0x08}) c.push_back(b);
        emit_call_iat(c, iat[1]);
        c.push_back(0xCC);
        return c;
    }));
    CHECK(r.image_loaded);
    CHECK(r.exited && r.exit_code == 0x12345678);
    CHECK(r.imports_total == 2 && r.imports_implemented == 2);
    CHECK(r.heap_bytes >= 0xA8);
}

static void test_runtime_semaphore_imports() {
    // Peggle 2 creates a semaphore with an initial count of zero during CRT startup.
    // Verify the HLE returns a real synthetic handle and accepts ReleaseSemaphore.
    auto r = run_image(build_pe({"CreateSemaphoreExW", "ReleaseSemaphore", "exit"},
        [](const std::vector<std::uint32_t>& iat) {
            Bytes c = {0x48, 0x83, 0xEC, 0x28,             // sub rsp, 0x28
                       0x31, 0xC9,                         // xor ecx, ecx (attributes)
                       0x31, 0xD2,                         // xor edx, edx (initial count = 0)
                       0x41, 0xB8, 0x04, 0x00, 0x00, 0x00, // mov r8d, 4 (maximum count)
                       0x45, 0x33, 0xC9};                  // xor r9d, r9d (name = null)
            emit_call_iat(c, iat[0]);
            c.insert(c.end(), {0x48, 0x89, 0xC3,             // mov rbx, rax (save handle)
                               0x48, 0x89, 0xD9,             // mov rcx, rbx
                               0xBA, 0x01, 0x00, 0x00, 0x00,// mov edx, 1 (release count)
                               0x4C, 0x8D, 0x44, 0x24, 0x20}); // lea r8, [rsp+0x20] (previous count)
            emit_call_iat(c, iat[1]);
            c.insert(c.end(), {0x8B, 0xC8});                 // mov ecx, eax (ReleaseSemaphore BOOL)
            emit_call_iat(c, iat[2]);
            c.push_back(0xCC);
            return c;
        }));
    CHECK(r.image_loaded);
    CHECK(r.exited && r.exit_code == 1);
    CHECK(r.imports_total == 3 && r.imports_implemented == 3);
}

static void test_runtime_capture_context() {
    // RtlCaptureContext must preserve a nonzero continuation RIP; zero-filling the
    // Windows CONTEXT made later CRT diagnostics attempt to unwind from address 0.
    auto r = run_image(build_pe({"malloc", "RtlCaptureContext", "exit"},
        [](const std::vector<std::uint32_t>& iat) {
            Bytes c = {0x48, 0x83, 0xEC, 0x28,
                       0xB9, 0xD0, 0x04, 0x00, 0x00}; // 0x4D0-byte AMD64 CONTEXT
            emit_call_iat(c, iat[0]);
            c.insert(c.end(), {0x48, 0x89, 0xC3,             // mov rbx, rax
                               0x48, 0x89, 0xC1});            // mov rcx, rax
            emit_call_iat(c, iat[1]);
            c.insert(c.end(), {0x48, 0x83, 0xBB, 0xF8, 0x00, 0x00, 0x00, 0x00, // cmp qword [rbx+0xF8], 0
                               0x74, 0x0B,                                      // jz failed
                               0xB9, 0x00, 0x00, 0x00, 0x00});                  // exit(0)
            emit_call_iat(c, iat[2]);
            c.insert(c.end(), {0xB9, 0x01, 0x00, 0x00, 0x00});                 // failed: exit(1)
            emit_call_iat(c, iat[2]);
            c.push_back(0xCC);
            return c;
        }));
    CHECK(r.image_loaded);
    CHECK(r.exited && r.exit_code == 0);
    CHECK(r.imports_total == 3 && r.imports_implemented == 3);
}

static void test_runtime_tls_imports() {
    // TLS is modeled for the runtime's single guest thread: allocate, set/get a value,
    // free the slot, and return the retrieved value as the process exit code.
    auto r = run_image(build_pe({"TlsAlloc", "TlsSetValue", "TlsGetValue", "TlsFree", "exit"},
        [](const std::vector<std::uint32_t>& iat) {
            Bytes c = {0x48, 0x83, 0xEC, 0x28};
            emit_call_iat(c, iat[0]);                         // RAX = TLS index
            c.insert(c.end(), {0x89, 0xC3,                 // mov ebx,eax
                                0x89, 0xD9,                 // mov ecx,ebx
                                0xBA, 0x78,0x56,0x34,0x12}); // mov edx,0x12345678
            emit_call_iat(c, iat[1]);                         // TlsSetValue(index,value)
            c.insert(c.end(), {0x89, 0xD9});                  // mov ecx,ebx
            emit_call_iat(c, iat[2]);                         // RAX = TLS value
            c.insert(c.end(), {0x89, 0xC6, 0x89, 0xD9});     // mov esi,eax; mov ecx,ebx
            emit_call_iat(c, iat[3]);                         // TlsFree(index)
            c.insert(c.end(), {0x89, 0xF1});                  // mov ecx,esi
            emit_call_iat(c, iat[4]);                         // exit(value)
            c.push_back(0xCC);
            return c;
        }));
    CHECK(r.image_loaded);
    CHECK(r.exited && r.exit_code == 0x12345678);
    CHECK(r.imports_total == 5 && r.imports_implemented == 5);
}

static void test_runtime_unimplemented_import_is_reported() {
    // call an import with no handler, then ret -> entry returns normally with rax from the stub (0)
    auto r = run_image(build_pe({"SomeFunctionNobodyImplemented"}, [](const std::vector<std::uint32_t>& iat) {
        Bytes c = {0x48, 0x83, 0xEC, 0x28};
        emit_call_iat(c, iat[0]);
        for (auto b : Bytes{0x48, 0x83, 0xC4, 0x28, 0xC3}) c.push_back(b);
        return c;
    }));
    CHECK(r.image_loaded && r.stop_kind == "exit" && r.exit_code == 0);
    CHECK(r.imports_implemented == 0);
    CHECK(r.stubbed_imports_called.size() == 1 && r.stubbed_imports_called[0].find("SomeFunctionNobodyImplemented") != std::string::npos);
}

static void test_runtime_fault_and_limit() {
    // mov rax, 0 ; mov rax,[rax]  -> null dereference must be a reported fault with location
    auto r = run_image(build_pe({"exit"}, [](const std::vector<std::uint32_t>&) {
        return Bytes{0x31, 0xC0, 0x48, 0x8B, 0x00, 0xCC};
    }));
    CHECK(r.stop_kind == "fault" && r.stop_detail.find("unmapped guest address 0x0") != std::string::npos);
    CHECK(r.stop_location.find(".text+0x2") != std::string::npos);
    auto lim = run_image(build_pe({"exit"}, [](const std::vector<std::uint32_t>&) { return Bytes{0xEB, 0xFE}; }), 5000);
    CHECK(lim.stop_kind == "limit" && lim.instructions >= 5000);
}

static void test_loader_rejects_garbage() {
    PeImage bad; bad.file = Bytes(0x300, 0x41);
    CHECK(!bad.parse());
    Bytes trunc(0x100, 0); trunc[0] = 'M'; trunc[1] = 'Z'; put32(trunc, 0x3C, 0x80);
    PeImage t; t.file = trunc; CHECK(!t.parse());
    // 32-bit PE must be rejected with a clear reason
    Bytes b32 = build_pe({"exit"}, [](const std::vector<std::uint32_t>&) { return Bytes{0xC3}; });
    put16(b32, 0x84, 0x14C);
    PeImage p; p.file = b32; CHECK(!p.parse() && p.error.find("AMD64") != std::string::npos);
}

static void test_png_writer() {
    // Known-answer CRC-32 vector and the IEND chunk CRC every PNG ends with.
    const char* v = "123456789";
    CHECK((png_crc32(reinterpret_cast<const std::uint8_t*>(v), 9) ^ 0xFFFFFFFFu) == 0xCBF43926u);
    const std::uint8_t iend[4] = {'I', 'E', 'N', 'D'};
    CHECK((png_crc32(iend, 4) ^ 0xFFFFFFFFu) == 0xAE426082u);
    const auto path = std::filesystem::temp_directory_path() / "someboxone_png_test.png";
    const std::vector<std::uint8_t> rgb = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255}; // 2x2
    CHECK(write_png_rgb(path, rgb, 2, 2, 3));                                              // scaled 3x -> 6x6
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(f.size() > 40 && f[1] == 'P' && f[2] == 'N' && f[3] == 'G');
    CHECK(f[19] == 6 && f[23] == 6);                              // IHDR width/height (low bytes) after scaling
    CHECK(f[f.size() - 4] == 0xAE && f[f.size() - 3] == 0x42 && f[f.size() - 2] == 0x60 && f[f.size() - 1] == 0x82);
    CHECK(!write_png_rgb(path, {}, 2, 2, 1));                     // too little data is rejected
    std::error_code ec; std::filesystem::remove(path, ec);
}

static void test_avx_vex_basics() {
    GuestMemory mem; mem.map(0x10000, 0x1000, "t");
    // vpxor xmm0,xmm0,xmm0 ; vpcmpeqd xmm1,xmm1,xmm1 ; vpaddd xmm2,xmm1,xmm1 ; vzeroupper ; hlt
    const Bytes p = {0xC5, 0xF9, 0xEF, 0xC0, 0xC5, 0xF1, 0x76, 0xC9, 0xC5, 0xF1, 0xFE, 0xD1, 0xC5, 0xF8, 0x77, 0xF4};
    for (std::size_t i = 0; i < p.size(); ++i) mem.write<std::uint8_t>(0x10000 + i, p[i]);
    GuestCpu cpu(mem); cpu.rip = 0x10000; cpu.xmm[0].q[0] = 0x1234; cpu.ymm_hi[2].q[0] = 0x99;
    cpu.run(10);
    CHECK(cpu.stop == GuestCpu::Stop::Halt);
    CHECK(cpu.xmm[0].q[0] == 0 && cpu.xmm[1].q[0] == ~0ULL && cpu.xmm[2].d[0] == 0xFFFFFFFEu);
    CHECK(cpu.ymm_hi[2].q[0] == 0);                               // VEX.128 write and vzeroupper clear the upper half
}

static void test_manifest_parsing() {
    const std::string xml = "<Package><Identity Name=\"X\" Version=\"1.2\"/><Applications><Application Id=\"App\" Executable=\"Game.exe\"/></Applications></Package>";
    CHECK(xml_attr(xml, "Application", "Executable") == "Game.exe");
    CHECK(xml_attr(xml, "Identity", "Version") == "1.2");
    CHECK(xml_attr(xml, "Identity", "Missing").empty());
    const auto info = inspect_title_package("/nonexistent/definitely/not/here");
    CHECK(!info.ok && !info.error.empty());
}

int main() {
    test_cpu_and_memory();
    test_runtime_malloc_exit();
    test_runtime_operator_new_import();
    test_runtime_semaphore_imports();
    test_runtime_capture_context();
    test_runtime_tls_imports();
    test_runtime_unimplemented_import_is_reported();
    test_runtime_fault_and_limit();
    test_loader_rejects_garbage();
    test_png_writer();
    test_avx_vex_basics();
    test_manifest_parsing();
    if (g_failures) { std::cerr << g_failures << " title-runtime test failure(s)\n"; return 1; }
    std::cout << "title runtime tests passed\n";
    return 0;
}
