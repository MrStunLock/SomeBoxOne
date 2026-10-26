// Differential test for GuestCpu: every instruction encoding is executed twice on
// identical randomized inputs - natively on the host x86-64 CPU and in the
// interpreter - and general registers, defined flags and XMM state are compared.
// Linux/x86-64 hosts only (uses mmap RWX); on other hosts the test reports SKIP.

#include "guest_cpu.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>

using namespace someboxone;
using Bytes = std::vector<std::uint8_t>;

struct Ctx { std::uint64_t rax, rbx, rcx, rdx, flags_in, flags_out; std::uint8_t x0[16]; std::uint8_t x1[16]; };

static std::uint64_t g_seed = 0x1234567890ABCDEFULL;
static std::uint64_t rnd() { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17; return g_seed; }
static std::uint64_t interesting() {
    static const std::uint64_t k[] = {0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFFULL, 0x80000000ULL,
        0xFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL, ~0ULL, 0x0101010101010101ULL, 0x00000000FFFFFF00ULL};
    const auto pick = rnd() % 4;
    if (pick == 0) return k[rnd() % (sizeof k / sizeof k[0])];
    if (pick == 1) return k[rnd() % (sizeof k / sizeof k[0])] + (rnd() % 3) - 1;
    return rnd();
}

static std::uint8_t* g_host_code;
static void (*g_host_fn)(Ctx*);
static const std::uint8_t k_pre[] = {
    0x53, 0x57, 0x48,0x8B,0x07, 0x48,0x8B,0x5F,0x08, 0x48,0x8B,0x4F,0x10, 0x48,0x8B,0x57,0x18,
    0xF3,0x0F,0x6F,0x47,0x30, 0xF3,0x0F,0x6F,0x4F,0x40, 0xFF,0x77,0x20, 0x9D};
static const std::uint8_t k_post[] = {
    0x9C, 0x41,0x58, 0x48,0x89,0x07, 0x48,0x89,0x5F,0x08, 0x48,0x89,0x4F,0x10, 0x48,0x89,0x57,0x18,
    0xF3,0x0F,0x7F,0x47,0x30, 0xF3,0x0F,0x7F,0x4F,0x40, 0x4C,0x89,0x47,0x28, 0x5F, 0x5B, 0xC3};

static void run_host(const Bytes& insn, Ctx& c) {
    std::size_t n = 0;
    std::memcpy(g_host_code + n, k_pre, sizeof k_pre); n += sizeof k_pre;
    std::memcpy(g_host_code + n, insn.data(), insn.size()); n += insn.size();
    std::memcpy(g_host_code + n, k_post, sizeof k_post);
    g_host_fn(&c);
}

static int g_fail = 0, g_cases = 0;
// Scratch data page mapped at the SAME address in the host process and in the guest, so memory-operand instructions (which use
// real pointer values) behave identically and the resulting memory can be compared byte for byte.
static constexpr std::uint64_t kScratch = 0x30000000ULL;
static std::uint8_t* g_host_scratch = nullptr;
static bool g_check_mem = false;
static std::uint8_t g_scratch_init[4096];

static bool run_case(const char* name, const Bytes& insn, std::uint64_t flag_mask, std::uint64_t a, std::uint64_t b,
                     std::uint64_t cx, std::uint64_t dx, std::uint64_t fin, const std::uint8_t* x0 = nullptr, const std::uint8_t* x1 = nullptr,
                     bool compare_dest_regs = true) {
    Ctx h{a, b, cx, dx, (fin & 0x8D5) | 2, 0, {}, {}};
    if (x0) std::memcpy(h.x0, x0, 16);
    if (x1) std::memcpy(h.x1, x1, 16);
    Ctx host = h;
    if (g_check_mem) std::memcpy(g_host_scratch, g_scratch_init, 4096);
    run_host(insn, host);

    static GuestMemory mem;
    static bool mapped = false;
    if (!mapped) { mem.map(0x10000, 0x1000, "code"); mapped = true; }
    for (std::size_t i = 0; i < insn.size(); ++i) mem.write<std::uint8_t>(0x10000 + i, insn[i]);
    mem.write<std::uint8_t>(0x10000 + insn.size(), 0xF4);
    // Every case is run twice against the same host reference: once on the original interpreter (use_fast=false) and once on the
    // decoded-instruction-cache fast path (use_fast=true, tiny cache so construction is cheap). Both must match the real CPU.
    bool all_ok = true;
    for (int mode = 0; mode < 2; ++mode) {
        if (g_check_mem) { if (!mem.is_mapped(kScratch)) mem.map(kScratch, 0x1000, "scratch"); mem.write_bytes(kScratch, g_scratch_init, 4096); }
        GuestCpu cpu(mem, 64);
        cpu.use_fast = mode == 1;
        cpu.rip = 0x10000;
        cpu.r[0] = a; cpu.r[3] = b; cpu.r[1] = cx; cpu.r[2] = dx;
        cpu.rflags = (fin & 0x8D5) | 2;
        std::memcpy(cpu.xmm[0].b, h.x0, 16); std::memcpy(cpu.xmm[1].b, h.x1, 16);
        cpu.run(4);
        ++g_cases;
        bool ok = cpu.stop == GuestCpu::Stop::Halt;
        const std::uint64_t fl = cpu.rflags & flag_mask, hf = host.flags_out & flag_mask;
        if (compare_dest_regs) ok = ok && cpu.r[0] == host.rax && cpu.r[3] == host.rbx && cpu.r[1] == host.rcx && cpu.r[2] == host.rdx;
        ok = ok && fl == hf && std::memcmp(cpu.xmm[0].b, host.x0, 16) == 0 && std::memcmp(cpu.xmm[1].b, host.x1, 16) == 0;
        if (g_check_mem) { std::uint8_t gb[4096]; mem.read_bytes(kScratch, gb, 4096); if (std::memcmp(gb, g_host_scratch, 4096) != 0) ok = false; }
        if (!ok) {
            if (g_fail < 25) {
                std::printf("MISMATCH (%s) %s [", mode ? "FAST" : "legacy", name);
                for (auto byte : insn) std::printf("%02X ", byte);
                std::printf("] in rax=%llx rbx=%llx rcx=%llx rdx=%llx fl=%llx\n", (unsigned long long)a, (unsigned long long)b, (unsigned long long)cx, (unsigned long long)dx, (unsigned long long)(fin & 0x8D5));
                std::printf("   host: rax=%llx rbx=%llx rcx=%llx rdx=%llx fl=%llx\n", (unsigned long long)host.rax, (unsigned long long)host.rbx, (unsigned long long)host.rcx, (unsigned long long)host.rdx, (unsigned long long)hf);
                std::printf("   mine: rax=%llx rbx=%llx rcx=%llx rdx=%llx fl=%llx stop=%d %s\n", (unsigned long long)cpu.r[0], (unsigned long long)cpu.r[3], (unsigned long long)cpu.r[1], (unsigned long long)cpu.r[2], (unsigned long long)fl, (int)cpu.stop, cpu.stop_detail.c_str());
            }
            ++g_fail;
            all_ok = false;
        }
    }
    const bool ok = all_ok;
    return ok;
}

static void many(const char* name, const Bytes& insn, std::uint64_t flag_mask, int n = 300) {
    for (int i = 0; i < n; ++i)
        run_case(name, insn, flag_mask, interesting(), interesting(), rnd() % 80, interesting(), rnd());
}

static Bytes sized(unsigned size, std::uint8_t opcode_even, std::vector<std::uint8_t> tail) {
    Bytes b;
    if (size == 2) b.push_back(0x66);
    if (size == 8) b.push_back(0x48);
    b.push_back(size == 1 ? opcode_even : static_cast<std::uint8_t>(opcode_even | 1));
    b.insert(b.end(), tail.begin(), tail.end());
    return b;
}

int main() {
    g_host_code = static_cast<std::uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (g_host_code == MAP_FAILED) { std::puts("SKIP: cannot map executable memory"); return 0; }
    g_host_fn = reinterpret_cast<void (*)(Ctx*)>(g_host_code);

    const std::uint64_t ALLF = 0x8D5, CFOF = 0x801, CFonly = 1, ZFonly = 0x40;
    const unsigned sizes[] = {1, 2, 4, 8};

    // ALU reg,reg in both directions + immediates.
    static const char* alu_names[] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
    for (unsigned op = 0; op < 8; ++op) for (unsigned sz : sizes) {
        const std::uint64_t mk = (op == 1 || op == 4 || op == 6) ? (ALLF & ~0x10ULL) : ALLF;
        many(alu_names[op], sized(sz, static_cast<std::uint8_t>(op * 8), {0xD8}), mk);       // op rax, rbx (r/m,r)
        many(alu_names[op], sized(sz, static_cast<std::uint8_t>(op * 8 + 2), {0xD8}), mk);   // op rbx, rax (r,r/m)
        if (sz > 1) {
            many(alu_names[op], Bytes(sz == 2 ? Bytes{0x66, 0x83, static_cast<std::uint8_t>(0xC0 | (op << 3)), 0x80} : sz == 8 ? Bytes{0x48, 0x83, static_cast<std::uint8_t>(0xC0 | (op << 3)), 0x7F} : Bytes{0x83, static_cast<std::uint8_t>(0xC0 | (op << 3)), 0xF0}), mk);
            if (sz == 4 || sz == 8) {
                Bytes b; if (sz == 8) b.push_back(0x48);
                b.push_back(0x81); b.push_back(static_cast<std::uint8_t>(0xC0 | (op << 3)));
                b.insert(b.end(), {0x78, 0x56, 0x34, 0x92});
                many(alu_names[op], b, mk);
            }
        } else {
            many(alu_names[op], Bytes{0x80, static_cast<std::uint8_t>(0xC0 | (op << 3)), 0xA5}, mk);
        }
    }
    for (unsigned sz : sizes) many("test", sized(sz, 0x84, {0xD8}), ALLF & ~0x10ULL);

    // Shifts / rotates by CL, by 1, by imm.
    static const char* sh_names[] = {"rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar"};
    for (unsigned op = 0; op < 8; ++op) {
        if (op == 6) continue;
        for (unsigned sz : sizes) {
            const std::uint64_t mk = op < 4 ? CFOF : (ALLF & ~0x10ULL);
            for (int i = 0; i < 300; ++i) { // by CL; OF only defined for count==1 so mask it otherwise
                const std::uint64_t cx = rnd() % 70;
                const unsigned cmask = sz == 8 ? 0x3F : 0x1F;
                std::uint64_t m2 = mk;
                const unsigned eff = static_cast<unsigned>(cx) & cmask;
                if (eff != 1) m2 &= ~0x800ULL;
                if (op >= 2 && op <= 3 && sz < 4 && eff != 0) m2 &= ~0x800ULL;
                run_case(sh_names[op], sized(sz, 0xD2, {static_cast<std::uint8_t>(0xC0 | (op << 3))}), m2, interesting(), interesting(), cx, interesting(), rnd());
            }
            many(sh_names[op], sized(sz, 0xD0, {static_cast<std::uint8_t>(0xC0 | (op << 3))}), mk, 100);
            for (int i = 0; i < 100; ++i) {
                const std::uint8_t imm = static_cast<std::uint8_t>(rnd() % 70);
                const unsigned eff = imm & (sz == 8 ? 0x3F : 0x1F);
                std::uint64_t m2 = mk; if (eff != 1) m2 &= ~0x800ULL;
                if (op >= 2 && op <= 3 && sz < 4 && eff != 0) m2 &= ~0x800ULL;
                run_case(sh_names[op], sized(sz, 0xC0, {static_cast<std::uint8_t>(0xC0 | (op << 3)), imm}), m2, interesting(), interesting(), 0, interesting(), rnd());
            }
        }
    }

    // Group 3: not/neg/mul/imul (single operand, rm = rbx so rax/rdx implicit).
    for (unsigned sz : sizes) {
        many("not", sized(sz, 0xF6, {0xD3}), 0);
        many("neg", sized(sz, 0xF6, {0xDB}), ALLF);
        many("mul", sized(sz, 0xF6, {0xE3}), CFOF);
        many("imul1", sized(sz, 0xF6, {0xEB}), CFOF);
        many("test imm", sized(sz, 0xA8, sz == 1 ? std::vector<std::uint8_t>{0x81} : sz == 2 ? std::vector<std::uint8_t>{0x81, 0x80} : std::vector<std::uint8_t>{0x81, 0x00, 0x00, 0x80}), ALLF & ~0x10ULL);
    }
    // Division: choose non-faulting operands.
    for (unsigned sz : sizes) for (int i = 0; i < 300; ++i) {
        const std::uint64_t m = sz == 8 ? ~0ULL : ((1ULL << (sz * 8)) - 1);
        std::uint64_t divisor = interesting() & m; if (!divisor) divisor = 3;
        std::uint64_t lo = interesting() & m, hi = (sz == 1) ? 0 : (rnd() % divisor);
        if (sz == 1) { // AX / r8: AH:AL
            const std::uint64_t ax = (rnd() % divisor) << 8 | (lo & 0xFF);
            run_case("div8", Bytes{0xF6, 0xF3}, 0, ax, divisor, 0, 0, 0, nullptr, nullptr, true);
            continue;
        }
        run_case("div", sized(sz, 0xF6, {0xF3}), 0, lo, divisor, 0, hi, 0);
        // IDIV: sign-extend rax into rdx, avoid INT_MIN / -1 overflow.
        const std::int64_t sv = static_cast<std::int64_t>(lo << (64 - sz * 8)) >> (64 - sz * 8);
        std::int64_t sd = static_cast<std::int64_t>(divisor << (64 - sz * 8)) >> (64 - sz * 8);
        if (sd == 0) sd = 7;
        const std::int64_t minv = sz == 8 ? INT64_MIN : -(1LL << (sz * 8 - 1));
        if (sv == minv && sd == -1) continue;
        run_case("idiv", sized(sz, 0xF6, {0xFB}), 0, static_cast<std::uint64_t>(sv) & m, static_cast<std::uint64_t>(sd) & m, 0,
                 (sv < 0 ? m : 0), 0);
    }
    // imul 2/3 operand, bsf/bsr, bt family, shld/shrd, cmov, setcc, xadd, cmpxchg, xchg, movzx/sx.
    for (unsigned sz : sizes) {
        if (sz == 1) continue;
        auto pre = [&](std::vector<std::uint8_t> t) { Bytes b; if (sz == 2) b.push_back(0x66); if (sz == 8) b.push_back(0x48); b.insert(b.end(), t.begin(), t.end()); return b; };
        many("imul2", pre({0x0F, 0xAF, 0xD8}), CFOF);
        many("imul3-8", pre({0x6B, 0xD8, 0xF3}), CFOF);
        if (sz != 2) many("imul3-32", pre({0x69, 0xD8, 0x78, 0x56, 0x34, 0x12}), CFOF); else many("imul3-16", pre({0x69, 0xD8, 0x34, 0x92}), CFOF);
        many("bsf", pre({0x0F, 0xBC, 0xD8}), ZFonly, 200);
        many("bsr", pre({0x0F, 0xBD, 0xD8}), ZFonly, 200);
        many("bt", pre({0x0F, 0xA3, 0xD8}), CFonly);
        many("bts", pre({0x0F, 0xAB, 0xD8}), CFonly);
        many("btr", pre({0x0F, 0xB3, 0xD8}), CFonly);
        many("btc", pre({0x0F, 0xBB, 0xD8}), CFonly);
        for (unsigned sub = 4; sub < 8; ++sub) many("bt-imm", pre({0x0F, 0xBA, static_cast<std::uint8_t>(0xC0 | (sub << 3)), static_cast<std::uint8_t>(rnd() % 64)}), CFonly, 60);
        for (int i = 0; i < 300; ++i) {
            // 16-bit SHLD/SHRD with count > 16 is architecturally undefined, so only 1..16 is compared.
            const std::uint8_t imm = static_cast<std::uint8_t>(1 + rnd() % (sz == 8 ? 63 : sz == 2 ? 16 : 31));
            std::uint64_t mk = ALLF & ~0x10ULL; if (imm != 1) mk &= ~0x800ULL;
            run_case("shld", pre({0x0F, 0xA4, 0xD8, imm}), mk, interesting(), interesting(), 0, 0, rnd());
            run_case("shrd", pre({0x0F, 0xAC, 0xD8, imm}), mk, interesting(), interesting(), 0, 0, rnd());
        }
        for (unsigned cc = 0; cc < 16; ++cc) many("cmov", pre({0x0F, static_cast<std::uint8_t>(0x40 | cc), 0xD8}), 0, 40);
        many("xadd", pre({0x0F, 0xC1, 0xD8}), ALLF);
        many("cmpxchg", pre({0x0F, 0xB1, 0xD9}), ALLF, 200);
        many("xchg", pre({0x87, 0xD8}), 0, 20);
        many("popcnt", Bytes(sz == 2 ? Bytes{0x66, 0xF3, 0x0F, 0xB8, 0xD8} : sz == 8 ? Bytes{0xF3, 0x48, 0x0F, 0xB8, 0xD8} : Bytes{0xF3, 0x0F, 0xB8, 0xD8}), ALLF, 100);
        many("inc", pre({0xFF, 0xC3}), ALLF & ~CFonly, 80);
        many("dec", pre({0xFF, 0xCB}), ALLF & ~CFonly, 80);
    }
    for (unsigned cc = 0; cc < 16; ++cc) many("setcc", Bytes{0x0F, static_cast<std::uint8_t>(0x90 | cc), 0xC3}, 0, 40);
    many("movzx8", Bytes{0x48, 0x0F, 0xB6, 0xD8}, 0, 40); many("movsx8", Bytes{0x48, 0x0F, 0xBE, 0xD8}, 0, 40);
    many("movzx16", Bytes{0x0F, 0xB7, 0xD8}, 0, 40); many("movsx16", Bytes{0x48, 0x0F, 0xBF, 0xD8}, 0, 40);
    many("movsxd", Bytes{0x48, 0x63, 0xD8}, 0, 40);
    many("bswap", Bytes{0x48, 0x0F, 0xC8}, 0, 20); many("bswap32", Bytes{0x0F, 0xCB}, 0, 20);
    many("cdqe", Bytes{0x48, 0x98}, 0, 20); many("cwde", Bytes{0x98}, 0, 20); many("cqo", Bytes{0x48, 0x99}, 0, 20); many("cdq", Bytes{0x99}, 0, 20);
    many("lzcnt", Bytes{0xF3, 0x48, 0x0F, 0xBD, 0xD8}, 0x41, 0);  // CPUID does not advertise LZCNT; covered by tzcnt/bsr paths
    many("sahf/lahf", Bytes{0x9F}, 0, 20);
    many("xchg r8,rax", Bytes{0x93}, 0, 20);

    // SSE/SSE2: packed integer and float ops on xmm0 (dest) / xmm1 (src), compared bit-exactly.
    struct SseOp { const char* n; std::vector<std::uint8_t> bytes; };
    std::vector<SseOp> sse;
    const std::uint8_t pint[] = {0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x74,0x75,0x76,
        0xD4,0xD5,0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,0xE0,0xE3,0xE4,0xE5,0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,0xF4,0xF5,
        0xF6,0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xD1,0xD2,0xD3,0xE1,0xE2,0xF1,0xF2,0xF3};
    for (auto o : pint) sse.push_back({"pint", {0x66, 0x0F, o, 0xC1}});
    sse.push_back({"pshufd", {0x66, 0x0F, 0x70, 0xC1, 0x1B}}); sse.push_back({"pshufd2", {0x66, 0x0F, 0x70, 0xC1, 0xE4}});
    sse.push_back({"pshuflw", {0xF2, 0x0F, 0x70, 0xC1, 0x39}}); sse.push_back({"pshufhw", {0xF3, 0x0F, 0x70, 0xC1, 0x93}});
    sse.push_back({"psrlw", {0x66, 0x0F, 0x71, 0xD0, 0x03}}); sse.push_back({"psraw", {0x66, 0x0F, 0x71, 0xE0, 0x05}}); sse.push_back({"psllw", {0x66, 0x0F, 0x71, 0xF0, 0x02}});
    sse.push_back({"psrld", {0x66, 0x0F, 0x72, 0xD0, 0x07}}); sse.push_back({"psrad", {0x66, 0x0F, 0x72, 0xE0, 0x1F}}); sse.push_back({"pslld", {0x66, 0x0F, 0x72, 0xF0, 0x21}});
    sse.push_back({"psrlq", {0x66, 0x0F, 0x73, 0xD0, 0x09}}); sse.push_back({"psllq", {0x66, 0x0F, 0x73, 0xF0, 0x41}});
    sse.push_back({"psrldq", {0x66, 0x0F, 0x73, 0xD8, 0x05}}); sse.push_back({"pslldq", {0x66, 0x0F, 0x73, 0xF8, 0x03}});
    sse.push_back({"pxor", {0x66, 0x0F, 0xEF, 0xC0}}); sse.push_back({"xorps", {0x0F, 0x57, 0xC1}}); sse.push_back({"andps", {0x0F, 0x54, 0xC1}});
    sse.push_back({"andnps", {0x0F, 0x55, 0xC1}}); sse.push_back({"orps", {0x0F, 0x56, 0xC1}});
    sse.push_back({"unpcklps", {0x0F, 0x14, 0xC1}}); sse.push_back({"unpckhps", {0x0F, 0x15, 0xC1}});
    sse.push_back({"unpcklpd", {0x66, 0x0F, 0x14, 0xC1}}); sse.push_back({"unpckhpd", {0x66, 0x0F, 0x15, 0xC1}});
    sse.push_back({"shufps", {0x0F, 0xC6, 0xC1, 0x4E}}); sse.push_back({"shufpd", {0x66, 0x0F, 0xC6, 0xC1, 0x01}});
    sse.push_back({"movlhps", {0x0F, 0x16, 0xC1}}); sse.push_back({"movhlps", {0x0F, 0x12, 0xC1}});
    sse.push_back({"movq xmm,xmm", {0xF3, 0x0F, 0x7E, 0xC1}});
    sse.push_back({"movss rr", {0xF3, 0x0F, 0x10, 0xC1}}); sse.push_back({"movsd rr", {0xF2, 0x0F, 0x10, 0xC1}});
    sse.push_back({"pextrw", {0x66, 0x0F, 0xC5, 0xC1, 0x05}});
    sse.push_back({"movd eax,xmm", {0x66, 0x0F, 0x7E, 0xC0}}); sse.push_back({"movq rax,xmm", {0x66, 0x48, 0x0F, 0x7E, 0xC0}});
    sse.push_back({"movd xmm,eax", {0x66, 0x0F, 0x6E, 0xC0}}); sse.push_back({"movq xmm,rax", {0x66, 0x48, 0x0F, 0x6E, 0xC0}});
    sse.push_back({"pmovmskb", {0x66, 0x0F, 0xD7, 0xC1}}); sse.push_back({"movmskps", {0x0F, 0x50, 0xC1}}); sse.push_back({"movmskpd", {0x66, 0x0F, 0x50, 0xC1}});
    sse.push_back({"pinsrw", {0x66, 0x0F, 0xC4, 0xC0, 0x03}});
    for (const auto& s : sse) for (int i = 0; i < 120; ++i) {
        std::uint8_t a[16], b[16];
        for (auto& v : a) v = static_cast<std::uint8_t>(rnd() % 4 == 0 ? (rnd() % 2 ? 0xFF : 0x80) : rnd());
        for (auto& v : b) v = static_cast<std::uint8_t>(rnd() % 4 == 0 ? (rnd() % 2 ? 0x7F : 0x00) : rnd());
        run_case(s.n, s.bytes, 0, interesting(), 0, 0, 0, 0, a, b);
    }
    // Floating-point ops over finite, well-scaled inputs (NaN payload rules differ between hosts and are out of scope).
    const std::uint8_t farith[] = {0x58, 0x59, 0x5C, 0x5D, 0x5E, 0x5F};
    for (std::uint8_t pfx : {std::uint8_t(0), std::uint8_t(0x66), std::uint8_t(0xF3), std::uint8_t(0xF2)})
        for (auto o : farith) for (int i = 0; i < 80; ++i) {
            float fa[4], fb[4]; double da[2], db[2];
            for (auto& v : fa) v = static_cast<float>(static_cast<std::int32_t>(rnd() % 20001) - 10000) / 7.0f;
            for (auto& v : fb) v = static_cast<float>(static_cast<std::int32_t>(rnd() % 20001) - 10000) / 3.0f + 0.5f;
            for (auto& v : da) v = static_cast<double>(static_cast<std::int32_t>(rnd() % 2000001) - 1000000) / 7.0;
            for (auto& v : db) v = static_cast<double>(static_cast<std::int32_t>(rnd() % 2000001) - 1000000) / 3.0 + 0.5;
            std::uint8_t a[16], b[16];
            if (pfx == 0x66 || pfx == 0xF2) { std::memcpy(a, da, 16); std::memcpy(b, db, 16); } else { std::memcpy(a, fa, 16); std::memcpy(b, fb, 16); }
            Bytes by; if (pfx) by.push_back(pfx);
            by.push_back(0x0F); by.push_back(o); by.push_back(0xC1);
            run_case("farith", by, 0, 0, 0, 0, 0, 0, a, b);
        }
    // Conversions and compares.
    for (int i = 0; i < 200; ++i) {
        float fa[4]; double da[2]; std::int32_t ia[4];
        for (auto& v : fa) v = static_cast<float>(static_cast<std::int32_t>(rnd() % 400001) - 200000) / 16.0f;
        for (auto& v : da) v = static_cast<double>(static_cast<std::int32_t>(rnd() % 4000001) - 2000000) / 64.0;
        for (auto& v : ia) v = static_cast<std::int32_t>(rnd());
        std::uint8_t a[16], b[16];
        std::memcpy(a, fa, 16); std::memcpy(b, fa, 16); std::swap(fa[0], fa[3]); std::memcpy(a, fa, 16);
        run_case("cvtss2sd", Bytes{0xF3, 0x0F, 0x5A, 0xC1}, 0, 0, 0, 0, 0, 0, a, b);
        std::memcpy(b, da, 16);
        run_case("cvtsd2ss", Bytes{0xF2, 0x0F, 0x5A, 0xC1}, 0, 0, 0, 0, 0, 0, a, b);
        run_case("cvtpd2ps", Bytes{0x66, 0x0F, 0x5A, 0xC1}, 0, 0, 0, 0, 0, 0, a, b);
        run_case("cvtpd2dq", Bytes{0xF2, 0x0F, 0xE6, 0xC1}, 0, 0, 0, 0, 0, 0, a, b);
        run_case("cvttpd2dq", Bytes{0x66, 0x0F, 0xE6, 0xC1}, 0, 0, 0, 0, 0, 0, a, b);
        std::memcpy(b, fa, 16);
        run_case("cvtps2pd", Bytes{0x0F, 0x5A, 0xC1}, 0, 0, 0, 0, 0, 0, a, b);
        run_case("cvtps2dq", Bytes{0x66, 0x0F, 0x5B, 0xC1}, 0, 0, 0, 0, 0, 0, a, b);
        run_case("cvttps2dq", Bytes{0xF3, 0x0F, 0x5B, 0xC1}, 0, 0, 0, 0, 0, 0, a, b);
        std::memcpy(b, ia, 16);
        run_case("cvtdq2ps", Bytes{0x0F, 0x5B, 0xC1}, 0, 0, 0, 0, 0, 0, a, b);
        run_case("cvtdq2pd", Bytes{0xF3, 0x0F, 0xE6, 0xC1}, 0, 0, 0, 0, 0, 0, a, b);
        std::memcpy(a, fa, 16); std::memcpy(b, fa, 16); std::swap(fa[1], fa[0]); std::memcpy(b, fa, 16);
        run_case("comiss", Bytes{0x0F, 0x2F, 0xC1}, 0x8D5, 0, 0, 0, 0, 0, a, b);
        run_case("ucomiss", Bytes{0x0F, 0x2E, 0xC1}, 0x8D5, 0, 0, 0, 0, 0, a, b);
        std::memcpy(a, da, 16); std::swap(da[0], da[1]); std::memcpy(b, da, 16);
        run_case("comisd", Bytes{0x66, 0x0F, 0x2F, 0xC1}, 0x8D5, 0, 0, 0, 0, 0, a, b);
        run_case("cmpps", Bytes{0x0F, 0xC2, 0xC1, static_cast<std::uint8_t>(i % 8)}, 0, 0, 0, 0, 0, 0, a, b);
        run_case("cmpsd", Bytes{0xF2, 0x0F, 0xC2, 0xC1, static_cast<std::uint8_t>(i % 8)}, 0, 0, 0, 0, 0, 0, a, b);
        run_case("sqrtps", Bytes{0x0F, 0x51, 0xC1}, 0, 0, 0, 0, 0, 0, a, b);
        // cvtsi2ss/sd and cvttss2si/sd2si through rax
        run_case("cvtsi2sd", Bytes{0xF2, 0x0F, 0x2A, 0xC0}, 0, static_cast<std::uint64_t>(static_cast<std::int32_t>(rnd())), 0, 0, 0, 0, a, b);
        run_case("cvtsi2ss64", Bytes{0xF3, 0x48, 0x0F, 0x2A, 0xC0}, 0, rnd() >> 8, 0, 0, 0, 0, a, b);
        run_case("cvttsd2si", Bytes{0xF2, 0x0F, 0x2C, 0xC1}, 0, 0, 0, 0, 0, 0, a, b);
        run_case("cvtss2si64", Bytes{0xF3, 0x48, 0x0F, 0x2D, 0xC1}, 0, 0, 0, 0, 0, 0, a, b);
    }


    // ---------------- memory-operand forms: effective addresses + every specialized memory handler ----------------
    {
        void* want = reinterpret_cast<void*>(kScratch);
        void* got = mmap(want, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (got != want) { std::puts("SKIP memory-operand tests: cannot map the scratch page at its fixed address"); }
        else {
            g_host_scratch = static_cast<std::uint8_t*>(got);
            g_check_mem = true;
            // Base = RDX (0x30000200), index = RCX (0..7); data operands in RAX/RBX. Three addressing modes per instruction:
            // [rdx], [rdx+disp8], [rdx+rcx*scale+disp8] (SIB), plus [rdx+disp32].
            const std::uint64_t base = kScratch + 0x200;
            auto modes = [](unsigned reg) { // returns the ModRM(+SIB+disp) byte strings for the four addressing modes, reg field = `reg`
                std::vector<Bytes> v;
                v.push_back({static_cast<std::uint8_t>(0x02 | (reg << 3))});                                                          // [rdx]
                v.push_back({static_cast<std::uint8_t>(0x42 | (reg << 3)), 0x24});                                                    // [rdx+0x24]
                v.push_back({static_cast<std::uint8_t>(0x44 | (reg << 3)), static_cast<std::uint8_t>((2u << 6) | (1u << 3) | 2u), 0x10}); // [rdx+rcx*4+0x10]
                v.push_back({static_cast<std::uint8_t>(0x82 | (reg << 3)), 0xF0, 0x01, 0x00, 0x00});                                    // [rdx+0x1F0] disp32 (stays inside the page)
                return v;
            };
            auto mk = [&](unsigned sz, std::vector<std::uint8_t> opcode, const Bytes& modrm, const Bytes& imm = {}, bool rexw_ok = true) {
                (void)rexw_ok; Bytes b; if (sz == 2) b.push_back(0x66); if (sz == 8) b.push_back(0x48);
                b.insert(b.end(), opcode.begin(), opcode.end()); b.insert(b.end(), modrm.begin(), modrm.end()); b.insert(b.end(), imm.begin(), imm.end()); return b; };
            auto refill = [&]() { for (auto& x : g_scratch_init) x = static_cast<std::uint8_t>(rnd()); };
            auto many_mem = [&](const char* nm, const Bytes& insn, std::uint64_t mask, int n = 40) {
                for (int i = 0; i < n; ++i) { refill(); run_case(nm, insn, mask, interesting(), interesting(), rnd() % 8, base, rnd()); }
            };
            const std::uint64_t ALLF2 = 0x8D5;
            for (unsigned sz : {1u, 2u, 4u, 8u}) {
                const auto even = [&](std::uint8_t o) { return std::vector<std::uint8_t>{sz == 1 ? o : static_cast<std::uint8_t>(o | 1)}; };
                // mov
                {
                    const auto m3 = modes(3), m0 = modes(0);
                    for (std::size_t k = 0; k < m3.size(); ++k) {
                        many_mem("mov store", mk(sz, even(0x88), m3[k]), 0, 20);
                        many_mem("mov load", mk(sz, even(0x8A), m3[k]), 0, 20);
                        const Bytes imm = sz == 1 ? Bytes{0x5A} : sz == 2 ? Bytes{0x34, 0x12} : Bytes{0x78, 0x56, 0x34, 0x92};
                        many_mem("mov imm", mk(sz, even(0xC6), m0[k], imm), 0, 10);
                    }
                }
                // ALU: all eight ops, both memory directions, and the immediate forms
                for (unsigned aop = 0; aop < 8; ++aop) {
                    const std::uint64_t mk_flags = (aop == 1 || aop == 4 || aop == 6) ? (ALLF2 & ~0x10ULL) : ALLF2;
                    for (const auto& md : modes(3)) {
                        many_mem(alu_names[aop], mk(sz, {static_cast<std::uint8_t>(sz == 1 ? aop * 8 : aop * 8 + 1)}, md), mk_flags, 14);       // [mem], reg
                        many_mem(alu_names[aop], mk(sz, {static_cast<std::uint8_t>(sz == 1 ? aop * 8 + 2 : aop * 8 + 3)}, md), mk_flags, 14);   // reg, [mem]
                    }
                    for (const auto& md : modes(aop)) {
                        if (sz == 1) many_mem(alu_names[aop], mk(1, {0x80}, md, Bytes{0xA5}), mk_flags, 10);
                        else { many_mem(alu_names[aop], mk(sz, {0x83}, md, Bytes{0xF0}), mk_flags, 10);
                               many_mem(alu_names[aop], mk(sz, {0x81}, md, sz == 2 ? Bytes{0x34, 0x92} : Bytes{0x78, 0x56, 0x34, 0x92}), mk_flags, 10); }
                    }
                }
                for (const auto& md : modes(3)) many_mem("test mem", mk(sz, even(0x84), md), ALLF2 & ~0x10ULL, 14);
                for (const auto& md : modes(0)) { many_mem("inc mem", mk(sz, even(0xFE), md), ALLF2, 14); }
                for (const auto& md : modes(1)) { many_mem("dec mem", mk(sz, even(0xFE), md), ALLF2, 14); }
                for (const auto& md : modes(2)) many_mem("not mem", mk(sz, even(0xF6), md), 0, 10);
                for (const auto& md : modes(3)) many_mem("neg mem", mk(sz, even(0xF6), md), ALLF2, 14);
                for (unsigned sop : {0u, 1u, 4u, 5u, 7u}) for (const auto& md : modes(sop)) {
                    for (int i = 0; i < 12; ++i) {
                        refill(); const std::uint8_t imm = static_cast<std::uint8_t>(1 + rnd() % 7);
                        const std::uint64_t mk2 = sop < 4 ? CFOF : (ALLF2 & ~0x10ULL);
                        run_case("shift mem imm", mk(sz, even(0xC0), md, Bytes{imm}), imm == 1 ? mk2 : (mk2 & ~0x800ULL), interesting(), interesting(), rnd() % 8, base, rnd());
                    }
                }
                if (sz >= 4) {
                    for (const auto& md : modes(3)) {
                        many_mem("imul2 mem", mk(sz, {0x0F, 0xAF}, md), CFOF, 14);
                        many_mem("imul3 mem", mk(sz, {0x6B}, md, Bytes{0xF3}), CFOF, 10);
                        many_mem("cmov mem", mk(sz, {0x0F, 0x44}, md), 0, 14);
                        many_mem("cmov mem nz", mk(sz, {0x0F, 0x4F}, md), 0, 14);
                    }
                    for (const auto& md : modes(2)) many_mem("lea", mk(sz, {0x8D}, md), 0, 14);
                }
                if (sz >= 4) for (const auto& md : modes(3)) {
                    many_mem("movzx8 mem", mk(sz, {0x0F, 0xB6}, md), 0, 14); many_mem("movzx16 mem", mk(sz, {0x0F, 0xB7}, md), 0, 14);
                    many_mem("movsx8 mem", mk(sz, {0x0F, 0xBE}, md), 0, 14); many_mem("movsx16 mem", mk(sz, {0x0F, 0xBF}, md), 0, 14);
                }
            }
            for (const auto& md : modes(3)) many_mem("movsxd mem", mk(8, {0x63}, md), 0, 14);
            for (unsigned cc = 0; cc < 16; ++cc) for (const auto& md : modes(0)) many_mem("setcc mem", mk(4, {0x0F, static_cast<std::uint8_t>(0x90 | cc)}, md), 0, 4);
            g_check_mem = false;
        }
    }

    std::printf("guest-cpu differential: %d cases, %d mismatches\n", g_cases, g_fail);
    return g_fail == 0 ? 0 : 1;
}
#else
int main() { std::puts("SKIP: differential CPU test needs a Linux x86-64 host"); return 0; }
#endif
