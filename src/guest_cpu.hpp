#pragma once

// SomeBoxOne v0.22.0 - user-mode x86-64 interpreter for title execution.
//
// Unlike X64CpuSubset (cpu_x64.hpp), which is a tiny synthetic-test scaffold for
// firmware/paging research, this interpreter targets real compiler output (MSVC
// x64): the full integer ISA with architectural flags, string operations,
// multiply/divide, bit operations, atomics, CPUID/XGETBV/RDTSC and the SSE/SSE2
// subset that MSVC emits. It is user-mode only: no paging, privilege levels or
// descriptor tables. FS/GS bases are explicit so the HLE layer can host a TEB.
//
// Anything not implemented stops execution with an exact opcode report rather than
// guessing, so every stop reason is reproducible.

#include "guest_mem.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace someboxone {

struct GuestUnsupported {
    std::string what;
};

struct FastExec;

// 1 = the low byte has an even number of set bits (x86 PF)
inline constexpr std::array<std::uint8_t, 256> kParityEvenTable = [] {
    std::array<std::uint8_t, 256> t{};
    for (unsigned i = 0; i < 256; ++i) { unsigned v = i, p = 0; while (v) { p ^= v & 1; v >>= 1; } t[i] = p ? 0 : 1; }
    return t;
}();

class GuestCpu {
    friend struct FastExec;
public:
    using u8 = std::uint8_t; using u16 = std::uint16_t; using u32 = std::uint32_t; using u64 = std::uint64_t;
    using i8 = std::int8_t; using i16 = std::int16_t; using i32 = std::int32_t; using i64 = std::int64_t;

    // Portable 128-bit helpers (MSVC has no __int128).
    struct Wide { u64 lo{}, hi{}; };
    static Wide umul64(u64 a, u64 b) {
        const u64 a0 = a & 0xFFFFFFFFULL, a1 = a >> 32, b0 = b & 0xFFFFFFFFULL, b1 = b >> 32;
        const u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
        const u64 mid = (p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);
        return {(mid << 32) | (p00 & 0xFFFFFFFFULL), p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32)};
    }
    static Wide smul64(i64 a, i64 b) {
        Wide w = umul64(static_cast<u64>(a), static_cast<u64>(b));
        if (a < 0) w.hi -= static_cast<u64>(b);
        if (b < 0) w.hi -= static_cast<u64>(a);
        return w;
    }
    // Full product of two operands interpreted at `size` bytes.
    static Wide mul_full(u64 a, u64 b, unsigned size, bool is_signed) {
        if (size == 8) return is_signed ? smul64(static_cast<i64>(a), static_cast<i64>(b)) : umul64(a, b);
        if (is_signed) { const i64 p = static_cast<i64>(sext(a, size)) * static_cast<i64>(sext(b, size)); return {static_cast<u64>(p), p < 0 ? ~0ULL : 0ULL}; }
        return {(a & mask(size)) * (b & mask(size)), 0};
    }
    static bool imul_overflowed(const Wide& w, u64 res, unsigned size) {
        if (size == 8) return w.hi != (static_cast<i64>(w.lo) < 0 ? ~0ULL : 0ULL);
        return static_cast<i64>(w.lo) != static_cast<i64>(sext(res & mask(size), size));
    }
    // 128/64 unsigned division; false on quotient overflow (#DE).
    static bool udiv128(u64 hi, u64 lo, u64 d, u64& q, u64& r) {
        if (hi >= d) return false;
        if (hi == 0) { q = lo / d; r = lo % d; return true; }
        u64 rem = hi; q = 0;
        for (int i = 63; i >= 0; --i) {
            const bool top = (rem >> 63) != 0;
            rem = (rem << 1) | ((lo >> i) & 1);
            q <<= 1;
            if (top || rem >= d) { rem -= d; q |= 1; }
        }
        r = rem; return true;
    }
    static bool sdiv128(u64 hi, u64 lo, i64 d, i64& q, i64& r) {
        const bool nneg = (hi >> 63) != 0, dneg = d < 0;
        u64 mh = hi, ml = lo;
        if (nneg) { ml = ~lo + 1; mh = ~hi + (ml == 0 ? 1 : 0); }
        const u64 md = dneg ? (~static_cast<u64>(d) + 1) : static_cast<u64>(d);
        u64 uq, ur;
        if (!udiv128(mh, ml, md, uq, ur)) return false;
        const bool qneg = nneg != dneg;
        if (!qneg && uq > 0x7FFFFFFFFFFFFFFFULL) return false;
        if (qneg && uq > 0x8000000000000000ULL) return false;
        q = static_cast<i64>(qneg ? (~uq + 1) : uq);
        r = static_cast<i64>(nneg ? (~ur + 1) : ur);
        return true;
    }
    static u64 bswap64(u64 v) { u64 o = 0; for (int i = 0; i < 8; ++i) { o = (o << 8) | (v & 0xFF); v >>= 8; } return o; }
    static u32 bswap32(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }

    struct Xmm {
        union { u8 b[16]; u16 w[8]; u32 d[4]; u64 q[2]; float f[4]; double df[2]; };
        Xmm() { q[0] = q[1] = 0; }
    };

    enum Flag : u64 { CF = 1, PF = 4, AF = 0x10, ZF = 0x40, SF = 0x80, TF = 0x100, IF = 0x200, DF = 0x400, OF = 0x800 };
    enum class Stop { None, MemoryFault, Unsupported, Halt, StepLimit, HostRequest, DivideError, Breakpoint, InvalidOpcode };

    // One decoded-instruction cache entry (see guest_cpu_fast.hpp). `dcache_entries` must be a power of two.
    struct Insn {
        using Handler = void (*)(GuestCpu&, const Insn&);
        Handler fn{nullptr};
        std::uint64_t tag{~0ULL};
        std::uint64_t next{}, target{};
        std::int64_t imm{}, disp{};
        std::uint8_t dst{}, src{}, base{0xFF}, index{0xFF}, scale{}, op{}, cc{}, seg{}, aux{};
    };

    explicit GuestCpu(GuestMemory& memory, std::size_t dcache_entries = 1u << 16) : mem(memory), dcache_entries_(dcache_entries) {}

    bool use_fast{true};   // false = always use the original interpreter (kept for differential testing)

    GuestMemory& mem;
    std::array<u64, 16> r{};
    u64 rip{};
    u64 rflags{0x202};
    int rm_override_{-1};            // one-shot: ModRM register operand redirected to a scratch XMM (see vex_legacy_pass)
    std::array<Xmm, 16> xmm{};
    std::array<Xmm, 16> ymm_hi{};   // upper 128 bits of YMM0-15 (AVX)
    u32 mxcsr{0x1F80};
    u64 fs_base{}, gs_base{};
    u64 instructions{};

    // Trap window: when RIP enters [trap_lo, trap_hi) the host trap handler runs
    // instead of guest code (used for HLE'd imports). The handler must either
    // redirect RIP (usually by emulating RET) or set a stop reason.
    u64 trap_lo{}, trap_hi{};
    std::function<void(GuestCpu&)> trap_handler;
    // Optional per-instruction trace hook (debug only).
    std::function<void(GuestCpu&)> trace_hook;

    Stop stop{Stop::None};
    std::string stop_detail;
    u64 fault_rip{};
    GuestFault last_fault{};

    void request_stop(Stop s, std::string detail) {
        stop = s; stop_detail = std::move(detail);
    }

    // ---- stack/call-convention helpers for HLE ----
    void push64(u64 v) { r[4] -= 8; mem.write<u64>(r[4], v); }
    u64 pop64() { const u64 v = mem.read<u64>(r[4]); r[4] += 8; return v; }
    // Windows x64: args RCX, RDX, R8, R9 then stack at [rsp+0x28...] on entry (return addr at [rsp]).
    u64 arg(unsigned n) const {
        switch (n) {
        case 0: return r[1]; case 1: return r[2]; case 2: return r[8]; case 3: return r[9];
        default: return mem.read<u64>(r[4] + 8 + 32 + (n - 4) * 8);
        }
    }
    void do_return(u64 rax_value) { r[0] = rax_value; rip = pop64(); }

    // Runs until `max_instructions` retire, a stop reason is set, or RIP reaches
    // `stop_at` (used for re-entrant HLE -> guest calls such as _initterm).
    u64 run(u64 max_instructions, u64 stop_at = ~0ULL) {
        stop = Stop::None;
        u64 n = 0;
        if (use_fast && !trace_hook) {
            n = run_fast(max_instructions, stop_at);
            if (stop == Stop::None && rip == stop_at) return n;
        } else {
            while (n < max_instructions && stop == Stop::None) {
                if (rip == stop_at) return n;
                if (rip >= trap_lo && rip < trap_hi && trap_handler) {
                    trap_handler(*this);
                    ++n; ++instructions;
                    continue;
                }
                step();
                ++n;
            }
        }
        if (stop == Stop::None && n >= max_instructions) request_stop(Stop::StepLimit, "instruction limit reached");
        return n;
    }

    void step() {
        insn_rip_ = rip;
        pc_ = rip;
        fetch_len_ = 0;
        try {
            fetch_ptr_ = mem.code_ptr(rip);
            fetch_base_ = rip;
            fetch_len_ = GuestMemory::page_size - (rip & GuestMemory::page_mask);
            if (trace_hook) trace_hook(*this);
#ifdef SBO_HIST
            hist_record();
#endif
            execute();
            rip = pc_;
            ++instructions;
        } catch (const GuestFault& f) {
            last_fault = f;
            fault_rip = insn_rip_;
            rip = insn_rip_;
            request_stop(Stop::MemoryFault,
                std::string(f.fetch || f.address == insn_rip_ ? "instruction fetch" : (f.write ? "write" : "read")) +
                " of unmapped guest address 0x" + hex(f.address));
        } catch (const GuestUnsupported& u) {
            fault_rip = insn_rip_;
            rip = insn_rip_;
            request_stop(Stop::Unsupported, u.what);
        } catch (const DivideFault&) {
            fault_rip = insn_rip_;
            rip = insn_rip_;
            request_stop(Stop::DivideError, "#DE divide error");
        }
    }

#ifdef SBO_HIST
    // Debug-only: histogram of (opcode map, opcode byte[, modrm.reg for group opcodes]) over executed instructions.
    inline static std::map<std::string, u64> g_hist;
    void hist_record() {
        u64 a = rip; u8 b = mem.read<u8>(a); std::string key;
        bool p66 = false, pf3 = false, pf2 = false, rexw = false;
        for (int i = 0; i < 6; ++i) { if (b == 0x66) p66 = true; else if (b == 0xF3) pf3 = true; else if (b == 0xF2) pf2 = true; else if ((b & 0xF0) == 0x40) { rexw = (b & 8) != 0; } else if (b == 0x64 || b == 0x65 || b == 0x2E || b == 0x3E || b == 0xF0 || b == 0x67) {} else break; b = mem.read<u8>(++a); }
        char buf[64];
        if (b == 0x0F) { u8 c = mem.read<u8>(a + 1); std::snprintf(buf, sizeof buf, "0F %02X%s%s%s", c, p66 ? " 66" : "", pf3 ? " F3" : "", pf2 ? " F2" : ""); }
        else if (b == 0xC5 || b == 0xC4) std::snprintf(buf, sizeof buf, "VEX %02X", b);
        else { u8 m = mem.read<u8>(a + 1); const bool grp = b == 0x80 || b == 0x81 || b == 0x83 || b == 0xC0 || b == 0xC1 || b == 0xD0 || b == 0xD1 || b == 0xD2 || b == 0xD3 || b == 0xF6 || b == 0xF7 || b == 0xFE || b == 0xFF || b == 0xC6 || b == 0xC7;
               if (grp) std::snprintf(buf, sizeof buf, "%02X /%d%s", b, (m >> 3) & 7, (m >> 6) == 3 ? " rr" : " mem"); else std::snprintf(buf, sizeof buf, "%02X%s", b, p66 ? " 66" : ""); }
        (void)rexw; ++g_hist[buf];
    }
#endif

    static std::string hex(u64 v) {
        char buf[20]; std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(v)); return buf;
    }

private:
    struct DivideFault {};
    std::vector<Insn> dcache_;
    std::size_t dcache_entries_{1u << 16};
    u64 epoch_seen_{0};
    void fast_decode(Insn& e, u64 pc0);
    u64 run_fast(u64 max_instructions, u64 stop_at);
    u64 insn_rip_{};
    u64 pc_{};
    const u8* fetch_ptr_{}; u64 fetch_base_{}, fetch_len_{};
    u64 mem_disp_{};   // +16 while executing the upper half of a 256-bit AVX op
    unsigned vex_vvvv_{};

    // ---- per-instruction decode state ----
    u8 rex_{};
    bool rex_present_{};
    bool opsz16_{}, addr32_{};
    u8 rep_{};          // 0, 0xF3, 0xF2
    int seg_{};         // 0 none, 1 fs, 2 gs
    bool lock_{};

    struct ModRM { u8 mod{}; u8 reg{}; u8 rm{}; bool mem{}; u64 addr{}; };

    // ---- fetch ----
    // Fetch fast path: step() pins a direct pointer to the code page (fetch_len_ bytes valid from fetch_base_);
    // instructions that straddle the page end, or fetch from elsewhere, fall back to the checked path.
    template <typename T> T fetch() {
        const u64 d = pc_ - fetch_base_;
        T v;
        if (d + sizeof(T) <= fetch_len_) std::memcpy(&v, fetch_ptr_ + d, sizeof(T)); else v = mem.read<T>(pc_);
        pc_ += sizeof(T);
        return v;
    }
    u8 f8() { return fetch<u8>(); }
    u16 f16() { return fetch<u16>(); }
    u32 f32() { return fetch<u32>(); }
    u64 f64() { return fetch<u64>(); }
    static u64 sx8(u8 v) { return static_cast<u64>(static_cast<i64>(static_cast<i8>(v))); }
    static u64 sx16(u16 v) { return static_cast<u64>(static_cast<i64>(static_cast<i16>(v))); }
    static u64 sx32(u32 v) { return static_cast<u64>(static_cast<i64>(static_cast<i32>(v))); }

    static u64 mask(unsigned size) { return size >= 8 ? ~0ULL : ((1ULL << (size * 8)) - 1); }
    static u64 sign_bit(unsigned size) { return 1ULL << (size * 8 - 1); }
    static u64 sext(u64 v, unsigned size) {
        switch (size) { case 1: return sx8(static_cast<u8>(v)); case 2: return sx16(static_cast<u16>(v)); case 4: return sx32(static_cast<u32>(v)); default: return v; }
    }

    unsigned opsize() const { return (rex_ & 8) ? 8 : (opsz16_ ? 2 : 4); }

    // ---- registers ----
    u64 getr(unsigned idx, unsigned size) const {
        switch (size) {
        case 1:
            if (!rex_present_ && idx >= 4 && idx < 8) return (r[idx - 4] >> 8) & 0xFF;
            return r[idx] & 0xFF;
        case 2: return r[idx] & 0xFFFF;
        case 4: return r[idx] & 0xFFFFFFFFULL;
        default: return r[idx];
        }
    }
    void setr(unsigned idx, unsigned size, u64 v) {
        switch (size) {
        case 1:
            if (!rex_present_ && idx >= 4 && idx < 8) r[idx - 4] = (r[idx - 4] & ~0xFF00ULL) | ((v & 0xFF) << 8);
            else r[idx] = (r[idx] & ~0xFFULL) | (v & 0xFF);
            break;
        case 2: r[idx] = (r[idx] & ~0xFFFFULL) | (v & 0xFFFF); break;
        case 4: r[idx] = v & 0xFFFFFFFFULL; break;
        default: r[idx] = v; break;
        }
    }

    // ---- ModRM ----
    ModRM modrm(int imm_after = 0) {
        ModRM m;
        const u8 b = f8();
        m.mod = b >> 6;
        m.reg = static_cast<u8>(((b >> 3) & 7) | ((rex_ & 4) ? 8 : 0));
        const u8 low = b & 7;
        if (m.mod == 3) { m.rm = static_cast<u8>(low | ((rex_ & 1) ? 8 : 0)); if (rm_override_ >= 0) { m.rm = static_cast<u8>(rm_override_); rm_override_ = -1; } return m; }
        m.mem = true;
        u64 addr = 0;
        bool riprel = false;
        i64 disp = 0;
        if (low == 4) {
            const u8 sib = f8();
            const unsigned scale = sib >> 6;
            const unsigned idx = ((sib >> 3) & 7) | ((rex_ & 2) ? 8 : 0);
            const unsigned base = (sib & 7) | ((rex_ & 1) ? 8 : 0);
            if ((sib & 7) == 5 && m.mod == 0) { disp = static_cast<i32>(f32()); }
            else addr = r[base];
            if (idx != 4) addr += r[idx] << scale;
        } else if (low == 5 && m.mod == 0) {
            disp = static_cast<i32>(f32());
            riprel = true;
        } else {
            addr = r[low | ((rex_ & 1) ? 8 : 0)];
        }
        if (m.mod == 1) disp = static_cast<i8>(f8());
        else if (m.mod == 2) disp = static_cast<i32>(f32());
        if (riprel) addr = pc_ + static_cast<u64>(imm_after);
        addr += static_cast<u64>(disp);
        if (addr32_) addr &= 0xFFFFFFFFULL;
        if (seg_ == 1) addr += fs_base; else if (seg_ == 2) addr += gs_base;
        m.addr = addr + mem_disp_;
        return m;
    }
    u64 rd_rm(const ModRM& m, unsigned size) const { return m.mem ? mem.read_sized(m.addr, size) : getr(m.rm, size); }
    void wr_rm(const ModRM& m, unsigned size, u64 v) { if (m.mem) mem.write_sized(m.addr, size, v); else setr(m.rm, size, v); }

    // ---- flags ----
    void setf(u64 f, bool on) { if (on) rflags |= f; else rflags &= ~f; }
    bool getf(u64 f) const { return (rflags & f) != 0; }
    // Flag updates are computed branch-free and applied with one read-modify-write: this runs for every ALU instruction.
    static constexpr u64 kArithFlags = CF | PF | AF | ZF | SF | OF;
    static u64 szp_bits(u64 res, unsigned size) {
        res &= mask(size);
        return (static_cast<u64>(res == 0) * ZF) | (static_cast<u64>((res & sign_bit(size)) != 0) * SF) | (static_cast<u64>(kParityEvenTable[res & 0xFF]) * PF);
    }
    void set_arith_flags(u64 bits) { rflags = (rflags & ~kArithFlags) | bits; }
    void set_szp(u64 res, unsigned size) { rflags = (rflags & ~(ZF | SF | PF)) | szp_bits(res, size); }

    u64 alu(unsigned op, u64 a, u64 b, unsigned size) {
        const u64 m = mask(size), sb = sign_bit(size);
        a &= m; b &= m;
        u64 res;
        switch (op) {
        case 0: case 2: { // ADD / ADC
            const u64 c = (op == 2 && getf(CF)) ? 1 : 0;
            const u64 full = a + b + c;
            res = full & m;
            const bool cf = size == 8 ? (full < a || (c && full == a)) : (full > m);
            set_arith_flags((static_cast<u64>(cf) * CF) | (static_cast<u64>(((a ^ res) & (b ^ res) & sb) != 0) * OF) |
                            (static_cast<u64>(((a ^ b ^ res) & 0x10) != 0) * AF) | szp_bits(res, size));
            return res;
        }
        case 3: case 5: case 7: { // SBB / SUB / CMP
            const u64 c = (op == 3 && getf(CF)) ? 1 : 0;
            res = (a - b - c) & m;
            const bool cf = a < b || (a == b && c);
            set_arith_flags((static_cast<u64>(cf) * CF) | (static_cast<u64>(((a ^ b) & (a ^ res) & sb) != 0) * OF) |
                            (static_cast<u64>(((a ^ b ^ res) & 0x10) != 0) * AF) | szp_bits(res, size));
            return res;
        }
        case 1: res = a | b; break;
        case 4: res = a & b; break;
        case 6: res = a ^ b; break;
        default: return 0;
        }
        set_arith_flags(szp_bits(res, size));   // logic ops: CF = OF = AF = 0
        return res;
    }
    void logic_flags(u64 res, unsigned size) { set_arith_flags(szp_bits(res, size)); }

    bool cond(unsigned cc) const {
        bool v = false;
        switch (cc >> 1) {
        case 0: v = getf(OF); break;
        case 1: v = getf(CF); break;
        case 2: v = getf(ZF); break;
        case 3: v = getf(CF) || getf(ZF); break;
        case 4: v = getf(SF); break;
        case 5: v = getf(PF); break;
        case 6: v = getf(SF) != getf(OF); break;
        default: v = getf(ZF) || (getf(SF) != getf(OF)); break;
        }
        return (cc & 1) ? !v : v;
    }

    u64 shift_op(unsigned op, u64 val, unsigned count, unsigned size) {
        const unsigned bits = size * 8;
        count &= (size == 8) ? 0x3F : 0x1F;
        if (count == 0) return val & mask(size);
        const u64 m = mask(size), sb = sign_bit(size);
        val &= m;
        u64 res = val;
        switch (op) {
        case 0: { // ROL
            const unsigned c = count % bits;
            res = c ? (((val << c) | (val >> (bits - c))) & m) : val;
            setf(CF, (res & 1) != 0);
            if (count == 1) setf(OF, ((res & sb) != 0) != getf(CF));
            break;
        }
        case 1: { // ROR
            const unsigned c = count % bits;
            res = c ? (((val >> c) | (val << (bits - c))) & m) : val;
            setf(CF, (res & sb) != 0);
            if (count == 1) setf(OF, ((res ^ (res << 1)) & sb) != 0);
            break;
        }
        case 2: case 3: { // RCL / RCR (rotate through carry, bit by bit)
            const unsigned c = count % (bits + 1);
            u64 v = val; bool cf = getf(CF);
            for (unsigned i = 0; i < c; ++i) {
                if (op == 2) { const bool out = (v & sb) != 0; v = ((v << 1) & m) | (cf ? 1 : 0); cf = out; }
                else { const bool out = (v & 1) != 0; v = (v >> 1) | (cf ? sb : 0); cf = out; }
            }
            res = v & m;
            setf(CF, cf);
            if (count == 1) setf(OF, op == 2 ? (((res & sb) != 0) != getf(CF)) : (((res ^ (res << 1)) & sb) != 0));
            break;
        }
        case 4: case 6: { // SHL / SAL
            if (count > bits) { res = 0; setf(CF, false); }
            else { setf(CF, ((val >> (bits - count)) & 1) != 0); res = count == bits ? 0 : (val << count) & m; }
            set_szp(res, size); setf(AF, false);
            if (count == 1) setf(OF, ((res & sb) != 0) != getf(CF));
            break;
        }
        case 5: { // SHR
            setf(CF, ((val >> (count - 1)) & 1) != 0);
            res = count >= bits ? 0 : (val >> count);
            set_szp(res, size); setf(AF, false);
            if (count == 1) setf(OF, (val & sb) != 0);
            break;
        }
        case 7: { // SAR
            const i64 sv = static_cast<i64>(sext(val, size));
            setf(CF, ((sv >> (count - 1 > 63 ? 63 : count - 1)) & 1) != 0);
            res = static_cast<u64>(sv >> (count > 63 ? 63 : count)) & m;
            set_szp(res, size); setf(AF, false);
            if (count == 1) setf(OF, false);
            break;
        }
        }
        return res;
    }

    // ---- stack ----
    void push(u64 v, unsigned size = 8) { r[4] -= size; mem.write_sized(r[4], size, v); }
    u64 pop(unsigned size = 8) { const u64 v = mem.read_sized(r[4], size); r[4] += size; return v; }

    [[noreturn]] void unsupported(const std::string& what) {
        std::string bytes;
        for (u64 a = insn_rip_; a < insn_rip_ + 12; ++a) {
            if (!mem.is_mapped(a)) break;
            char b[4]; std::snprintf(b, sizeof b, "%02X ", mem.read<u8>(a)); bytes += b;
        }
        throw GuestUnsupported{what + " @0x" + hex(insn_rip_) + " [" + bytes + "]"};
    }

    // ================= SSE helpers =================
    Xmm rd_x(const ModRM& m, unsigned bytes = 16) const {
        Xmm x;
        if (m.mem) {
            if (bytes == 16) { x.q[0] = mem.read<u64>(m.addr); x.q[1] = mem.read<u64>(m.addr + 8); }
            else if (bytes == 8) x.q[0] = mem.read<u64>(m.addr);
            else x.d[0] = mem.read<u32>(m.addr);
        } else x = xmm[m.rm];
        return x;
    }
    void wr_x_mem(const ModRM& m, const Xmm& x, unsigned bytes = 16) {
        if (bytes == 16) { mem.write<u64>(m.addr, x.q[0]); mem.write<u64>(m.addr + 8, x.q[1]); }
        else if (bytes == 8) mem.write<u64>(m.addr, x.q[0]);
        else mem.write<u32>(m.addr, x.d[0]);
    }
    unsigned sse_pfx() const { return rep_ == 0xF3 ? 2 : rep_ == 0xF2 ? 3 : (opsz16_ ? 1 : 0); }

    template <typename T, typename F>
    static Xmm lanes(const Xmm& a, const Xmm& b, F f) {
        Xmm out;
        constexpr int n = 16 / sizeof(T);
        for (int i = 0; i < n; ++i) {
            T x, y;
            std::memcpy(&x, a.b + i * sizeof(T), sizeof(T));
            std::memcpy(&y, b.b + i * sizeof(T), sizeof(T));
            const T z = f(x, y);
            std::memcpy(out.b + i * sizeof(T), &z, sizeof(T));
        }
        return out;
    }

    static float f_op(unsigned op, float a, float b) {
        switch (op) { case 0x58: return a + b; case 0x59: return a * b; case 0x5C: return a - b;
        case 0x5D: return a < b ? a : b; case 0x5E: return a / b; default: return a > b ? a : b; }
    }
    static double d_op(unsigned op, double a, double b) {
        switch (op) { case 0x58: return a + b; case 0x59: return a * b; case 0x5C: return a - b;
        case 0x5D: return a < b ? a : b; case 0x5E: return a / b; default: return a > b ? a : b; }
    }

    static u32 sat_i32_to_i16(i32 v) { return static_cast<u16>(static_cast<i16>(v < -32768 ? -32768 : v > 32767 ? 32767 : v)); }

    // CVT float->int with MXCSR rounding (nearest-even default) or truncation.
    static i64 cvt_f2i(double v, bool trunc, bool is64) {
        const double lim = is64 ? 9.223372036854775807e18 : 2147483648.0;
        if (std::isnan(v) || v >= lim || v < -lim) return is64 ? INT64_MIN : static_cast<i64>(INT32_MIN);
        const double rv = trunc ? std::trunc(v) : std::nearbyint(v);
        return static_cast<i64>(rv);
    }

    void execute_0f();
    void execute_0f_sse(u8 op);
    void execute_0f38(); void execute_0f3a();
    void execute_vex(u8 first);
    void execute_0f38_op(u8 op);
    void execute_vex_ext(unsigned map, u8 op, unsigned L, unsigned pp, unsigned W);
    void vex_legacy_pass(u8 map, u8 op, bool nds, bool dest_xmm_hi_copy);
    void execute_string(u8 op);
    void execute();
};

// ===================== main one-byte opcode dispatch =====================
inline void GuestCpu::execute() {
    rex_ = 0; rex_present_ = false; opsz16_ = false; addr32_ = false; rep_ = 0; seg_ = 0; lock_ = false;
    u8 op;
    for (;;) {
        op = f8();
        if (op == 0x66) { opsz16_ = true; continue; }
        if (op == 0x67) { addr32_ = true; continue; }
        if (op == 0xF3 || op == 0xF2) { rep_ = op; continue; }
        if (op == 0xF0) { lock_ = true; continue; }
        if (op == 0x64) { seg_ = 1; continue; }
        if (op == 0x65) { seg_ = 2; continue; }
        if (op == 0x2E || op == 0x3E || op == 0x26 || op == 0x36) continue; // null segment overrides
        break;
    }
    if (op >= 0x40 && op <= 0x4F) { rex_ = op; rex_present_ = true; op = f8(); }
    const unsigned osz = opsize();

    if (op < 0x40 && (op & 7) < 6) { // ALU block: ADD OR ADC SBB AND SUB XOR CMP
        const unsigned aop = (op >> 3) & 7, form = op & 7;
        if (form == 4 || form == 5) {
            const unsigned sz = form == 4 ? 1 : osz;
            const u64 imm = form == 4 ? f8() : (sz == 2 ? f16() : sx32(f32()));
            const u64 res = alu(aop, getr(0, sz), imm, sz);
            if (aop != 7) setr(0, sz, res);
            return;
        }
        const unsigned sz = (form & 1) ? osz : 1;
        const ModRM m = modrm();
        if (form < 2) { // r/m, reg
            const u64 res = alu(aop, rd_rm(m, sz), getr(m.reg, sz), sz);
            if (aop != 7) wr_rm(m, sz, res);
        } else {        // reg, r/m
            const u64 res = alu(aop, getr(m.reg, sz), rd_rm(m, sz), sz);
            if (aop != 7) setr(m.reg, sz, res);
        }
        return;
    }

    switch (op) {
    case 0x0F: execute_0f(); return;
    case 0xC4: case 0xC5: execute_vex(op); return;

    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
        const unsigned reg = (op - 0x50) | ((rex_ & 1) ? 8 : 0);
        const u64 v = r[reg]; push(v, opsz16_ ? 2 : 8); return;
    }
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        const unsigned reg = (op - 0x58) | ((rex_ & 1) ? 8 : 0);
        const u64 v = pop(opsz16_ ? 2 : 8);
        if (opsz16_) setr(reg, 2, v); else r[reg] = v;
        return;
    }
    case 0x63: { // MOVSXD
        const ModRM m = modrm();
        const u64 v = rd_rm(m, 4);
        setr(m.reg, osz, osz == 8 ? sx32(static_cast<u32>(v)) : v);
        return;
    }
    case 0x68: push(sx32(f32())); return;
    case 0x6A: push(sx8(f8())); return;
    case 0x69: case 0x6B: {
        const ModRM m = modrm(op == 0x69 ? (osz == 2 ? 2 : 4) : 1);
        const u64 imm = op == 0x6B ? sx8(f8()) : (osz == 2 ? sx16(f16()) : sx32(f32()));
        const Wide prod = mul_full(rd_rm(m, osz), imm, osz, true);
        const u64 res = prod.lo & mask(osz);
        setr(m.reg, osz, res);
        const bool ov = imul_overflowed(prod, res, osz);
        setf(CF, ov); setf(OF, ov); set_szp(res, osz);
        return;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        const u64 d = sx8(f8());
        if (cond(op & 0xF)) pc_ += d;
        return;
    }
    case 0x80: case 0x81: case 0x83: {
        const unsigned sz = op == 0x80 ? 1 : osz;
        const ModRM m = modrm(op == 0x81 ? (sz == 2 ? 2 : 4) : 1);
        const u64 imm = op == 0x81 ? (sz == 2 ? f16() : sx32(f32())) : sx8(f8());
        const unsigned aop = m.reg & 7;
        const u64 res = alu(aop, rd_rm(m, sz), imm, sz);
        if (aop != 7) wr_rm(m, sz, res);
        return;
    }
    case 0x84: case 0x85: {
        const unsigned sz = op == 0x84 ? 1 : osz;
        const ModRM m = modrm();
        alu(4, rd_rm(m, sz), getr(m.reg, sz), sz);
        return;
    }
    case 0x86: case 0x87: {
        const unsigned sz = op == 0x86 ? 1 : osz;
        const ModRM m = modrm();
        const u64 a = rd_rm(m, sz), b = getr(m.reg, sz);
        wr_rm(m, sz, b); setr(m.reg, sz, a);
        return;
    }
    case 0x88: case 0x89: { const unsigned sz = op == 0x88 ? 1 : osz; const ModRM m = modrm(); wr_rm(m, sz, getr(m.reg, sz)); return; }
    case 0x8A: case 0x8B: { const unsigned sz = op == 0x8A ? 1 : osz; const ModRM m = modrm(); setr(m.reg, sz, rd_rm(m, sz)); return; }
    case 0x8D: {
        const int saved_seg = seg_; seg_ = 0;
        const ModRM m = modrm();
        seg_ = saved_seg;
        setr(m.reg, osz, m.addr);
        return;
    }
    case 0x8F: { const ModRM m = modrm(); const u64 v = pop(); wr_rm(m, 8, v); return; }
    case 0x90:
        if (rex_ & 1) { const u64 a = getr(0, osz), b = getr(8, osz); setr(0, osz, b); setr(8, osz, a); }
        return; // NOP / PAUSE
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        const unsigned reg = (op - 0x90) | ((rex_ & 1) ? 8 : 0);
        const u64 a = getr(0, osz), b = getr(reg, osz);
        setr(0, osz, b); setr(reg, osz, a);
        return;
    }
    case 0x98:
        if (osz == 8) r[0] = sx32(static_cast<u32>(r[0]));
        else if (osz == 4) r[0] = static_cast<u32>(sx16(static_cast<u16>(r[0])));
        else setr(0, 2, sx8(static_cast<u8>(r[0])));
        return;
    case 0x99:
        if (osz == 8) r[2] = (r[0] >> 63) ? ~0ULL : 0;
        else if (osz == 4) r[2] = (r[0] & 0x80000000ULL) ? 0xFFFFFFFFULL : 0;
        else setr(2, 2, (r[0] & 0x8000) ? 0xFFFF : 0);
        return;
    case 0x9C: push(rflags & ~0x10000ULL); return;
    case 0x9D: { const u64 v = pop(); rflags = (v & 0xDD5) | (rflags & ~0xDD5ULL) | 2 | IF; return; }
    case 0x9E: { const u64 ah = (r[0] >> 8) & 0xFF; rflags = (rflags & ~0xD5ULL) | (ah & 0xD5) | 2; return; }
    case 0x9F: r[0] = (r[0] & ~0xFF00ULL) | ((rflags & 0xD7) << 8) | 0x200; return;
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        execute_string(op); return;
    case 0xA8: { const u64 imm = f8(); alu(4, getr(0, 1), imm, 1); return; }
    case 0xA9: { const u64 imm = osz == 2 ? f16() : sx32(f32()); alu(4, getr(0, osz), imm, osz); return; }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
        const unsigned reg = (op - 0xB0) | ((rex_ & 1) ? 8 : 0);
        setr(reg, 1, f8()); return;
    }
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        const unsigned reg = (op - 0xB8) | ((rex_ & 1) ? 8 : 0);
        if (osz == 8) r[reg] = f64();
        else if (osz == 4) r[reg] = f32();
        else setr(reg, 2, f16());
        return;
    }
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        const unsigned sz = (op & 1) ? osz : 1;
        const ModRM m = modrm((op == 0xC0 || op == 0xC1) ? 1 : 0);
        unsigned count = (op == 0xC0 || op == 0xC1) ? f8() : (op >= 0xD2 ? static_cast<unsigned>(r[1] & 0xFF) : 1);
        const u64 res = shift_op(m.reg & 7, rd_rm(m, sz), count, sz);
        wr_rm(m, sz, res);
        return;
    }
    case 0xC2: { const u16 n = f16(); rip = pop(); pc_ = rip; r[4] += n; return; }
    case 0xC3: pc_ = pop(); return;
    case 0xC6: case 0xC7: {
        const unsigned sz = op == 0xC6 ? 1 : osz;
        const ModRM m = modrm(sz == 1 ? 1 : (sz == 2 ? 2 : 4));
        const u64 imm = sz == 1 ? f8() : (sz == 2 ? f16() : sx32(f32()));
        wr_rm(m, sz, imm);
        return;
    }
    case 0xC9: r[4] = r[5]; r[5] = pop(); return;
    case 0xCC: request_stop(Stop::Breakpoint, "INT3 executed"); pc_ = insn_rip_; return;
    case 0xE3: { const u64 d = sx8(f8()); if ((addr32_ ? (r[1] & 0xFFFFFFFFULL) : r[1]) == 0) pc_ += d; return; }
    case 0xE8: { const u64 d = sx32(f32()); push(pc_); pc_ += d; return; }
    case 0xE9: { const u64 d = sx32(f32()); pc_ += d; return; }
    case 0xEB: { const u64 d = sx8(f8()); pc_ += d; return; }
    case 0xF4: request_stop(Stop::Halt, "HLT executed"); pc_ = insn_rip_; return;
    case 0xF5: setf(CF, !getf(CF)); return;
    case 0xF8: setf(CF, false); return;
    case 0xF9: setf(CF, true); return;
    case 0xFC: setf(DF, false); return;
    case 0xFD: setf(DF, true); return;
    case 0xF6: case 0xF7: {
        const unsigned sz = op == 0xF6 ? 1 : osz;
        const unsigned sub = (static_cast<unsigned>(mem.read<u8>(pc_)) >> 3) & 7;
        const ModRM m = modrm(sub < 2 ? (sz == 1 ? 1 : (sz == 2 ? 2 : 4)) : 0);
        const u64 v = rd_rm(m, sz);
        switch (sub) {
        case 0: case 1: { const u64 imm = sz == 1 ? f8() : (sz == 2 ? f16() : sx32(f32())); alu(4, v, imm, sz); return; }
        case 2: wr_rm(m, sz, ~v & mask(sz)); return;
        case 3: { const u64 res = alu(5, 0, v, sz); wr_rm(m, sz, res); setf(CF, (v & mask(sz)) != 0); return; }
        case 4: case 5: { // MUL / IMUL (one operand)
            const bool sg = sub == 5;
            const Wide prod = mul_full(getr(0, sz), v, sz, sg);
            if (sz == 1) setr(0, 2, prod.lo);
            else if (sz == 2) { setr(0, 2, prod.lo); setr(2, 2, prod.lo >> 16); }
            else if (sz == 4) { setr(0, 4, prod.lo); setr(2, 4, prod.lo >> 32); }
            else { r[0] = prod.lo; r[2] = prod.hi; }
            bool ov;
            if (!sg) ov = sz == 8 ? prod.hi != 0 : (prod.lo >> (sz * 8)) != 0;
            else ov = imul_overflowed(prod, prod.lo & mask(sz), sz);
            setf(CF, ov); setf(OF, ov); return;
        }
        default: { // DIV / IDIV
            if ((v & mask(sz)) == 0) throw DivideFault{};
            const bool sgn = sub == 7;
            if (sz == 1) {
                if (!sgn) { const u64 n = r[0] & 0xFFFF; const u64 q = n / v, rem = n % v; if (q > 0xFF) throw DivideFault{}; setr(0, 1, q); r[0] = (r[0] & ~0xFF00ULL) | (rem << 8); }
                else { const i64 n = static_cast<i16>(r[0]); const i64 d = static_cast<i8>(v); const i64 q = n / d, rem = n % d; if (q > 127 || q < -128) throw DivideFault{}; setr(0, 1, static_cast<u64>(q)); r[0] = (r[0] & ~0xFF00ULL) | ((static_cast<u64>(rem) & 0xFF) << 8); }
            } else if (sz == 2) {
                if (!sgn) { const u64 n = ((r[2] & 0xFFFF) << 16) | (r[0] & 0xFFFF); const u64 q = n / v, rem = n % v; if (q > 0xFFFF) throw DivideFault{}; setr(0, 2, q); setr(2, 2, rem); }
                else { const i64 n = static_cast<i32>(((r[2] & 0xFFFF) << 16) | (r[0] & 0xFFFF)); const i64 d = static_cast<i16>(v); const i64 q = n / d, rem = n % d; if (q > 32767 || q < -32768) throw DivideFault{}; setr(0, 2, static_cast<u64>(q)); setr(2, 2, static_cast<u64>(rem)); }
            } else if (sz == 4) {
                if (!sgn) { const u64 n = ((r[2] & 0xFFFFFFFFULL) << 32) | (r[0] & 0xFFFFFFFFULL); const u64 q = n / v, rem = n % v; if (q > 0xFFFFFFFFULL) throw DivideFault{}; setr(0, 4, q); setr(2, 4, rem); }
                else { const i64 n = static_cast<i64>(((r[2] & 0xFFFFFFFFULL) << 32) | (r[0] & 0xFFFFFFFFULL)); const i64 d = static_cast<i32>(v); const i64 q = n / d, rem = n % d; if (q > INT32_MAX || q < INT32_MIN) throw DivideFault{}; setr(0, 4, static_cast<u64>(q)); setr(2, 4, static_cast<u64>(rem)); }
            } else {
                if (!sgn) { u64 q, rem; if (!udiv128(r[2], r[0], v, q, rem)) throw DivideFault{}; r[0] = q; r[2] = rem; }
                else { i64 q, rem; if (!sdiv128(r[2], r[0], static_cast<i64>(v), q, rem)) throw DivideFault{}; r[0] = static_cast<u64>(q); r[2] = static_cast<u64>(rem); }
            }
            return;
        }
        }
    }
    case 0xFE: case 0xFF: {
        const unsigned sz = op == 0xFE ? 1 : osz;
        const unsigned sub = (static_cast<unsigned>(mem.read<u8>(pc_)) >> 3) & 7;
        const ModRM m = modrm();
        if (sub == 0 || sub == 1) {
            const bool cf = getf(CF);
            const u64 res = alu(sub == 0 ? 0 : 5, rd_rm(m, sz), 1, sz);
            wr_rm(m, sz, res); setf(CF, cf);
            return;
        }
        if (op == 0xFF) {
            if (sub == 2) { const u64 t = rd_rm(m, 8); push(pc_); pc_ = t; return; }
            if (sub == 4) { pc_ = rd_rm(m, 8); return; }
            if (sub == 6) { push(rd_rm(m, 8)); return; }
        }
        unsupported("unsupported FE/FF group opcode extension");
    }
    default: break;
    }
    unsupported("unsupported one-byte opcode 0x" + hex(op));
}

// ===================== string operations =====================
inline void GuestCpu::execute_string(u8 op) {
    const unsigned sz = (op & 1) ? opsize() : 1;
    const i64 delta = getf(DF) ? -static_cast<i64>(sz) : static_cast<i64>(sz);
    const bool repeat = rep_ != 0;
    auto cnt = [&]() -> u64& { return r[1]; };
    auto step_once = [&]() -> bool { // returns true to continue (for REPE/REPNE compare ops)
        switch (op) {
        case 0xA4: case 0xA5: mem.write_sized(r[7], sz, mem.read_sized(r[6], sz)); r[6] += static_cast<u64>(delta); r[7] += static_cast<u64>(delta); return true;
        case 0xAA: case 0xAB: mem.write_sized(r[7], sz, getr(0, sz)); r[7] += static_cast<u64>(delta); return true;
        case 0xAC: case 0xAD: setr(0, sz, mem.read_sized(r[6], sz)); r[6] += static_cast<u64>(delta); return true;
        case 0xA6: case 0xA7: alu(7, mem.read_sized(r[6], sz), mem.read_sized(r[7], sz), sz); r[6] += static_cast<u64>(delta); r[7] += static_cast<u64>(delta); return true;
        default: alu(7, getr(0, sz), mem.read_sized(r[7], sz), sz); r[7] += static_cast<u64>(delta); return true;
        }
    };
    const bool is_cmp = op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF;
    if (!repeat) { step_once(); return; }
    // Fast path: forward REP MOVSB / STOSB over mapped ranges.
    while (cnt() != 0) {
        step_once();
        --cnt();
        if (is_cmp) {
            if (rep_ == 0xF3 && !getf(ZF)) break;
            if (rep_ == 0xF2 && getf(ZF)) break;
        }
    }
}

// ===================== 0F two-byte opcodes =====================
inline void GuestCpu::execute_0f() {
    const u8 op = f8();
    const unsigned osz = opsize();
    switch (op) {
    case 0x05: unsupported("SYSCALL is not available to user-mode titles");
    case 0x0B: request_stop(Stop::InvalidOpcode, "UD2 executed"); pc_ = insn_rip_; return;
    case 0x0D: case 0x18: { modrm(); return; } // PREFETCH*
    case 0x1F: { modrm(); return; }            // multi-byte NOP
    case 0x31: { // RDTSC: deterministic monotonic counter derived from retired instructions
        const u64 t = 1000000ULL + instructions * 3ULL;
        r[0] = t & 0xFFFFFFFFULL; r[2] = t >> 32; return;
    }
    case 0xA2: { // CPUID: baseline x86-64 + SSE/SSE2/CMOV/CX8/FXSR only (what this interpreter implements)
        const u32 leaf = static_cast<u32>(r[0]);
        u32 a = 0, b = 0, c = 0, d = 0;
        if (leaf == 0) { a = 0xD; b = 0x756e6547; d = 0x49656e69; c = 0x6c65746e; } // "GenuineIntel"
        else if (leaf == 1) {
            // Only features this interpreter implements: FPU bit, TSC, CX8, CMOV, FXSR, SSE, SSE2, POPCNT, AVX
            // (Xbox One titles are built /arch:AVX). MMX, SSE3+, F16C, FMA, AVX2, BMI are NOT advertised.
            a = 0x000306A9; b = 0x00000800;
            c = (1u << 23) | (1u << 27) /*OSXSAVE*/ | (1u << 28) /*AVX*/;
            d = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 24) | (1u << 25) | (1u << 26);
        }
        else if (leaf == 7) { /* no AVX2/BMI */ }
        else if (leaf == 0x80000000u) a = 0x80000008u;
        else if (leaf == 0x80000001u) { c = (1u << 0) | (1u << 5) /*LZCNT*/; d = (1u << 29) | (1u << 11); }
        r[0] = a; r[3] = b; r[1] = c; r[2] = d;
        return;
    }
    case 0x01: {
        const u8 b = mem.read<u8>(pc_);
        if (b == 0xD0) { pc_ += 1; r[0] = 7; r[2] = 0; return; } // XGETBV: x87+SSE+AVX state
        unsupported("unsupported 0F 01 form");
    }
    case 0xAE: {
        const u8 b = mem.read<u8>(pc_);
        if ((b >> 6) == 3) { pc_ += 1; return; } // LFENCE/MFENCE/SFENCE
        const ModRM m = modrm();
        const unsigned sub = m.reg & 7;
        if (sub == 2) { mxcsr = mem.read<u32>(m.addr); return; }  // LDMXCSR
        if (sub == 3) { mem.write<u32>(m.addr, mxcsr); return; }  // STMXCSR
        if (sub == 0 || sub == 1) { // FXSAVE/FXRSTOR: only XMM + MXCSR are modelled
            for (int i = 0; i < 16; ++i) {
                if (sub == 0) { mem.write<u64>(m.addr + 160 + i * 16, xmm[i].q[0]); mem.write<u64>(m.addr + 168 + i * 16, xmm[i].q[1]); }
                else { xmm[i].q[0] = mem.read<u64>(m.addr + 160 + i * 16); xmm[i].q[1] = mem.read<u64>(m.addr + 168 + i * 16); }
            }
            if (sub == 0) mem.write<u32>(m.addr + 24, mxcsr); else mxcsr = mem.read<u32>(m.addr + 24);
            return;
        }
        unsupported("unsupported 0F AE form");
    }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
        const ModRM m = modrm();
        const u64 v = rd_rm(m, osz);
        if (cond(op & 0xF)) setr(m.reg, osz, v); else if (osz == 4) setr(m.reg, 4, getr(m.reg, 4));
        return;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
        const u64 d = sx32(f32());
        if (cond(op & 0xF)) pc_ += d;
        return;
    }
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
        const ModRM m = modrm();
        wr_rm(m, 1, cond(op & 0xF) ? 1 : 0);
        return;
    }
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: case 0xBA: {
        unsigned sub = 0; ModRM m; u64 bitoff;
        if (op == 0xBA) {
            m = modrm(1); sub = m.reg & 7; bitoff = f8();
            if (sub < 4) unsupported("invalid 0F BA group extension");
            sub -= 4; // 0 BT, 1 BTS, 2 BTR, 3 BTC
        } else {
            m = modrm();
            sub = op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
            bitoff = getr(m.reg, osz);
        }
        const unsigned bits = osz * 8;
        u64 addr = m.addr; unsigned bit;
        if (m.mem && op != 0xBA) {
            const i64 sbo = static_cast<i64>(sext(bitoff, osz));
            addr += static_cast<u64>((sbo >> (osz == 8 ? 6 : osz == 4 ? 5 : 4)) * static_cast<i64>(osz));
            bit = static_cast<unsigned>(static_cast<u64>(sbo) & (bits - 1));
        } else bit = static_cast<unsigned>(bitoff & (bits - 1));
        u64 val = m.mem ? mem.read_sized(addr, osz) : getr(m.rm, osz);
        setf(CF, ((val >> bit) & 1) != 0);
        if (sub == 1) val |= (1ULL << bit); else if (sub == 2) val &= ~(1ULL << bit); else if (sub == 3) val ^= (1ULL << bit);
        if (sub != 0) { if (m.mem) mem.write_sized(addr, osz, val); else setr(m.rm, osz, val); }
        return;
    }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: { // SHLD/SHRD
        const ModRM m = modrm((op == 0xA4 || op == 0xAC) ? 1 : 0);
        unsigned count = (op == 0xA4 || op == 0xAC) ? f8() : static_cast<unsigned>(r[1] & 0xFF);
        count &= osz == 8 ? 0x3F : 0x1F;
        if (!count) return;
        const unsigned bits = osz * 8;
        const u64 dst = rd_rm(m, osz), src = getr(m.reg, osz);
        u64 res;
        if (op == 0xA4 || op == 0xA5) {
            res = ((dst << count) | (count <= bits ? (src >> (bits - count)) : 0)) & mask(osz);
            setf(CF, ((dst >> (bits - count)) & 1) != 0);
        } else {
            res = ((dst >> count) | (src << (bits - count))) & mask(osz);
            setf(CF, ((dst >> (count - 1)) & 1) != 0);
        }
        setf(OF, ((res ^ dst) & sign_bit(osz)) != 0);
        set_szp(res, osz);
        wr_rm(m, osz, res);
        return;
    }
    case 0xAF: {
        const ModRM m = modrm();
        const Wide prod = mul_full(getr(m.reg, osz), rd_rm(m, osz), osz, true);
        const u64 res = prod.lo & mask(osz);
        setr(m.reg, osz, res);
        const bool ov = imul_overflowed(prod, res, osz);
        setf(CF, ov); setf(OF, ov); set_szp(res, osz);
        return;
    }
    case 0xB0: case 0xB1: { // CMPXCHG
        const unsigned sz = op == 0xB0 ? 1 : osz;
        const ModRM m = modrm();
        const u64 dst = rd_rm(m, sz);
        alu(7, getr(0, sz), dst, sz);
        if (getf(ZF)) wr_rm(m, sz, getr(m.reg, sz)); else setr(0, sz, dst);
        return;
    }
    case 0xC0: case 0xC1: { // XADD
        const unsigned sz = op == 0xC0 ? 1 : osz;
        const ModRM m = modrm();
        const u64 dst = rd_rm(m, sz), src = getr(m.reg, sz);
        const u64 sum = alu(0, dst, src, sz);
        setr(m.reg, sz, dst); wr_rm(m, sz, sum);
        return;
    }
    case 0xC7: { // CMPXCHG8B/16B, RDRAND
        const u8 nxt = mem.read<u8>(pc_);
        if ((nxt >> 6) == 3) unsupported("RDRAND/RDSEED not reported by CPUID");
        const ModRM m = modrm();
        if ((m.reg & 7) != 1) unsupported("unsupported 0F C7 form");
        if (rex_ & 8) {
            const u64 lo = mem.read<u64>(m.addr), hi = mem.read<u64>(m.addr + 8);
            if (lo == r[0] && hi == r[2]) { mem.write<u64>(m.addr, r[3]); mem.write<u64>(m.addr + 8, r[1]); setf(ZF, true); }
            else { r[0] = lo; r[2] = hi; setf(ZF, false); }
        } else {
            const u64 v = mem.read<u64>(m.addr);
            const u64 cmp = ((r[2] & 0xFFFFFFFFULL) << 32) | (r[0] & 0xFFFFFFFFULL);
            if (v == cmp) { mem.write<u64>(m.addr, ((r[1] & 0xFFFFFFFFULL) << 32) | (r[3] & 0xFFFFFFFFULL)); setf(ZF, true); }
            else { r[0] = v & 0xFFFFFFFFULL; r[2] = v >> 32; setf(ZF, false); }
        }
        return;
    }
    case 0xB6: case 0xB7: case 0xBE: case 0xBF: {
        const unsigned srcsz = (op & 1) ? 2 : 1;
        const ModRM m = modrm();
        u64 v = rd_rm(m, srcsz);
        if (op >= 0xBE) v = sext(v, srcsz);
        setr(m.reg, osz, v);
        return;
    }
    case 0xB8: { // POPCNT
        if (rep_ != 0xF3) unsupported("JMPE not supported");
        const ModRM m = modrm();
        const u64 v = rd_rm(m, osz);
        const u64 c = static_cast<u64>(std::popcount(v));
        setr(m.reg, osz, c);
        rflags &= ~(CF | PF | AF | SF | OF); setf(ZF, v == 0);
        return;
    }
    case 0xBC: case 0xBD: { // BSF/BSR (TZCNT/LZCNT with F3)
        const ModRM m = modrm();
        const u64 v = rd_rm(m, osz) & mask(osz);
        const unsigned bits = osz * 8;
        if (rep_ == 0xF3) {
            const u64 res = v == 0 ? bits : (op == 0xBC ? static_cast<u64>(std::countr_zero(v)) : static_cast<u64>(std::countl_zero(v << (64 - bits))));
            setr(m.reg, osz, res);
            setf(CF, v == 0); setf(ZF, res == 0);
        } else {
            setf(ZF, v == 0);
            if (v != 0) setr(m.reg, osz, op == 0xBC ? static_cast<u64>(std::countr_zero(v)) : static_cast<u64>(63 - std::countl_zero(v)));
        }
        return;
    }
    case 0xC3: { const ModRM m = modrm(); mem.write_sized(m.addr, osz, getr(m.reg, osz)); return; } // MOVNTI
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
        const unsigned reg = (op - 0xC8) | ((rex_ & 1) ? 8 : 0);
        if (osz == 8) r[reg] = bswap64(r[reg]);
        else if (osz == 4) r[reg] = bswap32(static_cast<u32>(r[reg]));
        else unsupported("16-bit BSWAP is undefined");
        return;
    }
    case 0x38: execute_0f38(); return;
    case 0x3A: execute_0f3a(); return;
    default: break;
    }
    execute_0f_sse(op);
}

// ===================== SSE / SSE2 =====================
inline void GuestCpu::execute_0f_sse(u8 op) {
    const unsigned pfx = sse_pfx(); // 0 none, 1 66, 2 F3, 3 F2
    const unsigned osz = opsize();
    auto need66 = [&](const char* n) { if (pfx != 1) unsupported(std::string("MMX/non-66 form of ") + n + " not implemented"); };

    switch (op) {
    case 0x10: case 0x11: {
        const ModRM m = modrm();
        if (pfx == 0 || pfx == 1) {
            if (op == 0x10) xmm[m.reg] = rd_x(m); else if (m.mem) wr_x_mem(m, xmm[m.reg]); else xmm[m.rm] = xmm[m.reg];
        } else {
            const unsigned bytes = pfx == 2 ? 4 : 8;
            if (op == 0x10) {
                if (m.mem) { Xmm x; if (bytes == 4) x.d[0] = mem.read<u32>(m.addr); else x.q[0] = mem.read<u64>(m.addr); xmm[m.reg] = x; }
                else { if (bytes == 4) xmm[m.reg].d[0] = xmm[m.rm].d[0]; else xmm[m.reg].q[0] = xmm[m.rm].q[0]; }
            } else {
                if (m.mem) wr_x_mem(m, xmm[m.reg], bytes);
                else { if (bytes == 4) xmm[m.rm].d[0] = xmm[m.reg].d[0]; else xmm[m.rm].q[0] = xmm[m.reg].q[0]; }
            }
        }
        return;
    }
    case 0x12: case 0x13: case 0x16: case 0x17: {
        const ModRM m = modrm();
        const bool hi = op >= 0x16;
        // SSE3 forms used by MSVC/AVX titles. Keep the packed operation semantics
        // explicit instead of claiming the entire SSE3 extension is implemented.
        if (pfx == 3 && op == 0x12) { // MOVDDUP xmm, xmm/m64
            const u64 value = m.mem ? mem.read<u64>(m.addr) : xmm[m.rm].q[0];
            xmm[m.reg].q[0] = value; xmm[m.reg].q[1] = value; return;
        }
        if (pfx == 2 && (op == 0x12 || op == 0x16)) { // MOVSLDUP / MOVSHDUP
            const Xmm source = rd_x(m);
            Xmm result{};
            const unsigned base = op == 0x12 ? 0U : 1U;
            result.d[0] = source.d[base]; result.d[1] = source.d[base];
            result.d[2] = source.d[base + 2]; result.d[3] = source.d[base + 2];
            xmm[m.reg] = result; return;
        }
        if (pfx > 1) unsupported("unsupported SSE3 form for MOVDDUP/MOVSLDUP/MOVSHDUP");
        if (op == 0x12 || op == 0x16) {
            if (m.mem) xmm[m.reg].q[hi ? 1 : 0] = mem.read<u64>(m.addr);
            else if (!hi) xmm[m.reg].q[0] = xmm[m.rm].q[1];     // MOVHLPS
            else xmm[m.reg].q[1] = xmm[m.rm].q[0];              // MOVLHPS
        } else {
            if (!m.mem) unsupported("MOVLPS/MOVHPS store needs memory");
            mem.write<u64>(m.addr, xmm[m.reg].q[hi ? 1 : 0]);
        }
        return;
    }
    case 0x14: case 0x15: { // UNPCKLPS/PD, UNPCKHPS/PD
        const ModRM m = modrm();
        const Xmm a = xmm[m.reg], b = rd_x(m);
        Xmm o;
        const bool hi = op == 0x15;
        if (pfx == 0) { const int s = hi ? 2 : 0; o.d[0] = a.d[s]; o.d[1] = b.d[s]; o.d[2] = a.d[s + 1]; o.d[3] = b.d[s + 1]; }
        else { o.q[0] = a.q[hi ? 1 : 0]; o.q[1] = b.q[hi ? 1 : 0]; }
        xmm[m.reg] = o; return;
    }
    case 0x28: case 0x29: {
        const ModRM m = modrm();
        if (op == 0x28) xmm[m.reg] = rd_x(m); else if (m.mem) wr_x_mem(m, xmm[m.reg]); else xmm[m.rm] = xmm[m.reg];
        return;
    }
    case 0x2B: { const ModRM m = modrm(); wr_x_mem(m, xmm[m.reg]); return; } // MOVNTPS/PD
    case 0x2A: { // CVTSI2SS/SD
        const ModRM m = modrm();
        const unsigned isz = (rex_ & 8) ? 8 : 4;
        const i64 v = static_cast<i64>(sext(rd_rm(m, isz), isz));
        if (pfx == 2) xmm[m.reg].f[0] = static_cast<float>(v);
        else if (pfx == 3) xmm[m.reg].df[0] = static_cast<double>(v);
        else unsupported("CVTPI2PS (MMX) not implemented");
        return;
    }
    case 0x2C: case 0x2D: {
        const ModRM m = modrm();
        const unsigned isz = (rex_ & 8) ? 8 : 4;
        double v;
        if (pfx == 2) v = static_cast<double>(rd_x(m, 4).f[0]);
        else if (pfx == 3) v = rd_x(m, 8).df[0];
        else unsupported("CVTTPS2PI (MMX) not implemented");
        setr(m.reg, isz, static_cast<u64>(cvt_f2i(v, op == 0x2C, isz == 8)) & mask(isz));
        return;
    }
    case 0x2E: case 0x2F: { // (U)COMISS/SD
        const ModRM m = modrm();
        double a, b;
        if (pfx == 0) { a = xmm[m.reg].f[0]; b = rd_x(m, 4).f[0]; }
        else if (pfx == 1) { a = xmm[m.reg].df[0]; b = rd_x(m, 8).df[0]; }
        else unsupported("bad COMIS prefix");
        rflags &= ~(OF | SF | AF | CF | ZF | PF);
        if (std::isnan(a) || std::isnan(b)) rflags |= ZF | PF | CF;
        else if (a > b) {}
        else if (a < b) rflags |= CF;
        else rflags |= ZF;
        return;
    }
    case 0x50: { // MOVMSKPS/PD
        const ModRM m = modrm();
        u64 v = 0;
        if (pfx == 0) for (int i = 0; i < 4; ++i) v |= static_cast<u64>(xmm[m.rm].d[i] >> 31) << i;
        else for (int i = 0; i < 2; ++i) v |= (xmm[m.rm].q[i] >> 63) << i;
        setr(m.reg, 4, v); return;
    }
    case 0x51: case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        const ModRM m = modrm();
        const bool scalar = pfx >= 2;
        const bool dbl = pfx == 1 || pfx == 3;
        const Xmm a = xmm[m.reg], b = rd_x(m, scalar ? (dbl ? 8 : 4) : 16);
        Xmm o = a;
        if (op == 0x51) {
            if (dbl) { for (int i = 0; i < (scalar ? 1 : 2); ++i) o.df[i] = std::sqrt(b.df[i]); }
            else for (int i = 0; i < (scalar ? 1 : 4); ++i) o.f[i] = std::sqrt(b.f[i]);
        } else if (dbl) { for (int i = 0; i < (scalar ? 1 : 2); ++i) o.df[i] = d_op(op, a.df[i], b.df[i]); }
        else for (int i = 0; i < (scalar ? 1 : 4); ++i) o.f[i] = f_op(op, a.f[i], b.f[i]);
        xmm[m.reg] = o; return;
    }
    case 0x52: case 0x53: { // RSQRTPS/RCPPS (+SS)
        const ModRM m = modrm();
        const bool scalar = pfx == 2;
        if (pfx != 0 && pfx != 2) unsupported("bad RSQRT/RCP prefix");
        const Xmm b = rd_x(m, scalar ? 4 : 16); Xmm o = xmm[m.reg];
        for (int i = 0; i < (scalar ? 1 : 4); ++i) o.f[i] = op == 0x52 ? 1.0f / std::sqrt(b.f[i]) : 1.0f / b.f[i];
        xmm[m.reg] = o; return;
    }
    case 0x54: case 0x55: case 0x56: case 0x57: {
        const ModRM m = modrm();
        const Xmm a = xmm[m.reg], b = rd_x(m);
        Xmm o;
        for (int i = 0; i < 2; ++i) o.q[i] = op == 0x54 ? (a.q[i] & b.q[i]) : op == 0x55 ? (~a.q[i] & b.q[i]) : op == 0x56 ? (a.q[i] | b.q[i]) : (a.q[i] ^ b.q[i]);
        xmm[m.reg] = o; return;
    }
    case 0x5A: {
        const ModRM m = modrm();
        Xmm o = xmm[m.reg];
        if (pfx == 0) { const Xmm b = rd_x(m, 8); o.df[0] = b.f[0]; o.df[1] = b.f[1]; }
        else if (pfx == 1) { const Xmm b = rd_x(m); o.f[0] = static_cast<float>(b.df[0]); o.f[1] = static_cast<float>(b.df[1]); o.q[1] = 0; }
        else if (pfx == 2) { const Xmm b = rd_x(m, 4); o.df[0] = static_cast<double>(b.f[0]); }
        else { const Xmm b = rd_x(m, 8); o.f[0] = static_cast<float>(b.df[0]); }
        xmm[m.reg] = o; return;
    }
    case 0x5B: {
        const ModRM m = modrm();
        const Xmm b = rd_x(m); Xmm o;
        if (pfx == 0) for (int i = 0; i < 4; ++i) o.f[i] = static_cast<float>(static_cast<i32>(b.d[i]));
        else for (int i = 0; i < 4; ++i) o.d[i] = static_cast<u32>(static_cast<i32>(cvt_f2i(static_cast<double>(b.f[i]), pfx == 2, false)));
        xmm[m.reg] = o; return;
    }
    case 0xE6: {
        const ModRM m = modrm();
        Xmm o;
        if (pfx == 2) { const Xmm b = rd_x(m, 8); o.df[0] = static_cast<i32>(b.d[0]); o.df[1] = static_cast<i32>(b.d[1]); }
        else if (pfx == 1 || pfx == 3) { const Xmm b = rd_x(m); o.d[0] = static_cast<u32>(static_cast<i32>(cvt_f2i(b.df[0], pfx == 1, false))); o.d[1] = static_cast<u32>(static_cast<i32>(cvt_f2i(b.df[1], pfx == 1, false))); }
        else unsupported("bad 0F E6 prefix");
        xmm[m.reg] = o; return;
    }
    case 0xC2: {
        const ModRM m = modrm(1);
        const u8 imm = f8();
        const bool scalar = pfx >= 2, dbl = pfx == 1 || pfx == 3;
        const Xmm a = xmm[m.reg], b = rd_x(m, scalar ? (dbl ? 8 : 4) : 16);
        Xmm o = a;
        auto cmp = [&](double x, double y) {
            const bool un = std::isnan(x) || std::isnan(y);
            switch (imm & 7) { case 0: return x == y; case 1: return x < y; case 2: return x <= y; case 3: return un; case 4: return x != y || un; case 5: return !(x < y); case 6: return !(x <= y); default: return !un; }
        };
        if (dbl) for (int i = 0; i < (scalar ? 1 : 2); ++i) o.q[i] = cmp(a.df[i], b.df[i]) ? ~0ULL : 0;
        else for (int i = 0; i < (scalar ? 1 : 4); ++i) o.d[i] = cmp(static_cast<double>(a.f[i]), static_cast<double>(b.f[i])) ? ~0U : 0;
        xmm[m.reg] = o; return;
    }
    case 0xC6: {
        const ModRM m = modrm(1);
        const u8 imm = f8();
        const Xmm a = xmm[m.reg], b = rd_x(m); Xmm o;
        if (pfx == 0) { o.d[0] = a.d[imm & 3]; o.d[1] = a.d[(imm >> 2) & 3]; o.d[2] = b.d[(imm >> 4) & 3]; o.d[3] = b.d[(imm >> 6) & 3]; }
        else { o.q[0] = a.q[imm & 1]; o.q[1] = b.q[(imm >> 1) & 1]; }
        xmm[m.reg] = o; return;
    }
    case 0x6E: { // MOVD/MOVQ xmm, r/m
        need66("MOVD");
        const ModRM m = modrm();
        const unsigned sz = (rex_ & 8) ? 8 : 4;
        Xmm x; x.q[0] = rd_rm(m, sz); xmm[m.reg] = x; return;
    }
    case 0x7E: {
        const ModRM m = modrm();
        if (pfx == 2) { Xmm x; x.q[0] = rd_x(m, 8).q[0]; xmm[m.reg] = x; return; } // MOVQ xmm, xmm/m64
        need66("MOVD");
        const unsigned sz = (rex_ & 8) ? 8 : 4;
        wr_rm(m, sz, sz == 8 ? xmm[m.reg].q[0] : xmm[m.reg].d[0]); return;
    }
    case 0xD6: { need66("MOVQ"); const ModRM m = modrm(); if (m.mem) mem.write<u64>(m.addr, xmm[m.reg].q[0]); else { Xmm x; x.q[0] = xmm[m.reg].q[0]; xmm[m.rm] = x; } return; }
    case 0x6F: case 0x7F: {
        if (pfx != 1 && pfx != 2) unsupported("MMX MOVQ not implemented");
        const ModRM m = modrm();
        if (op == 0x6F) xmm[m.reg] = rd_x(m); else if (m.mem) wr_x_mem(m, xmm[m.reg]); else xmm[m.rm] = xmm[m.reg];
        return;
    }
    case 0xE7: { need66("MOVNTDQ"); const ModRM m = modrm(); wr_x_mem(m, xmm[m.reg]); return; }
    case 0xD7: { need66("PMOVMSKB"); const ModRM m = modrm(); u64 v = 0; for (int i = 0; i < 16; ++i) v |= static_cast<u64>(xmm[m.rm].b[i] >> 7) << i; setr(m.reg, 4, v); return; }
    case 0x70: {
        if (pfx == 0) unsupported("MMX PSHUFW not implemented");
        const ModRM m = modrm(1);
        const u8 imm = f8();
        const Xmm b = rd_x(m); Xmm o = b;
        if (pfx == 1) for (int i = 0; i < 4; ++i) o.d[i] = b.d[(imm >> (i * 2)) & 3];
        else if (pfx == 2) for (int i = 0; i < 4; ++i) o.w[4 + i] = b.w[4 + ((imm >> (i * 2)) & 3)];
        else for (int i = 0; i < 4; ++i) o.w[i] = b.w[(imm >> (i * 2)) & 3];
        xmm[m.reg] = o; return;
    }
    case 0x71: case 0x72: case 0x73: { // shift by immediate groups
        need66("PSxx imm");
        const ModRM m = modrm(1);
        const u8 imm = f8();
        Xmm x = xmm[m.rm];
        const unsigned sub = m.reg & 7;
        if (op == 0x71) {
            for (int i = 0; i < 8; ++i) x.w[i] = sub == 2 ? static_cast<u16>(imm > 15 ? 0 : (x.w[i] >> imm)) : sub == 4 ? static_cast<u16>(static_cast<i16>(x.w[i]) >> (imm > 15 ? 15 : imm)) : sub == 6 ? static_cast<u16>(imm > 15 ? 0 : (x.w[i] << imm)) : static_cast<u16>((unsupported("bad 0F 71 ext"), 0));
        } else if (op == 0x72) {
            for (int i = 0; i < 4; ++i) x.d[i] = sub == 2 ? (imm > 31 ? 0 : x.d[i] >> imm) : sub == 4 ? static_cast<u32>(static_cast<i32>(x.d[i]) >> (imm > 31 ? 31 : imm)) : sub == 6 ? (imm > 31 ? 0 : x.d[i] << imm) : (unsupported("bad 0F 72 ext"), 0);
        } else {
            if (sub == 2) for (int i = 0; i < 2; ++i) x.q[i] = imm > 63 ? 0 : x.q[i] >> imm;
            else if (sub == 6) for (int i = 0; i < 2; ++i) x.q[i] = imm > 63 ? 0 : x.q[i] << imm;
            else if (sub == 3 || sub == 7) { // PSRLDQ / PSLLDQ (byte shifts)
                Xmm o; const unsigned n = imm > 16 ? 16 : imm;
                for (unsigned i = 0; i < 16; ++i) { if (sub == 3) o.b[i] = (i + n < 16) ? x.b[i + n] : 0; else o.b[i] = (i >= n) ? x.b[i - n] : 0; }
                x = o;
            } else unsupported("bad 0F 73 ext");
        }
        xmm[m.rm] = x; return;
    }
    case 0xC4: { need66("PINSRW"); const ModRM m = modrm(1); const u8 imm = f8(); xmm[m.reg].w[imm & 7] = static_cast<u16>(rd_rm(m, 2)); return; }
    case 0xC5: { need66("PEXTRW"); const ModRM m = modrm(1); const u8 imm = f8(); setr(m.reg, 4, xmm[m.rm].w[imm & 7]); return; }
    case 0xF6: { need66("PSADBW"); const ModRM m = modrm(); const Xmm a = xmm[m.reg], b = rd_x(m); Xmm o;
        for (int h = 0; h < 2; ++h) { u64 s = 0; for (int i = 0; i < 8; ++i) s += static_cast<u64>(std::abs(static_cast<int>(a.b[h * 8 + i]) - static_cast<int>(b.b[h * 8 + i]))); o.q[h] = s; }
        xmm[m.reg] = o; return; }
    case 0xF7: { need66("MASKMOVDQU"); const ModRM m = modrm(); const Xmm a = xmm[m.reg], mk = xmm[m.rm]; for (int i = 0; i < 16; ++i) if (mk.b[i] & 0x80) mem.write<u8>(r[7] + static_cast<u64>(i), a.b[i]); return; }
    default: break;
    }

    // Remaining packed-integer ops operate on two XMM operands (66-prefixed).
    const bool int_simd = (op >= 0x60 && op <= 0x6D) || (op >= 0x74 && op <= 0x76) || (op >= 0xD1 && op <= 0xFE);
    if (!int_simd) unsupported("unsupported 0F opcode 0x" + hex(op));
    if (pfx != 1) unsupported("MMX (64-bit) form of 0F " + hex(op) + " not implemented");
    const ModRM m = modrm();
    const Xmm a = xmm[m.reg], b = rd_x(m);
    Xmm o;
    auto sat_s8 = [](int v) { return static_cast<u8>(static_cast<i8>(v < -128 ? -128 : v > 127 ? 127 : v)); };
    auto sat_u8 = [](int v) { return static_cast<u8>(v < 0 ? 0 : v > 255 ? 255 : v); };
    auto sat_s16 = [](int v) { return static_cast<u16>(static_cast<i16>(v < -32768 ? -32768 : v > 32767 ? 32767 : v)); };
    auto sat_u16 = [](int v) { return static_cast<u16>(v < 0 ? 0 : v > 65535 ? 65535 : v); };
    switch (op) {
    case 0x60: for (int i = 0; i < 8; ++i) { o.b[2 * i] = a.b[i]; o.b[2 * i + 1] = b.b[i]; } break;
    case 0x61: for (int i = 0; i < 4; ++i) { o.w[2 * i] = a.w[i]; o.w[2 * i + 1] = b.w[i]; } break;
    case 0x62: for (int i = 0; i < 2; ++i) { o.d[2 * i] = a.d[i]; o.d[2 * i + 1] = b.d[i]; } break;
    case 0x63: for (int i = 0; i < 8; ++i) { o.b[i] = static_cast<u8>(static_cast<i8>(static_cast<i16>(a.w[i]) < -128 ? -128 : static_cast<i16>(a.w[i]) > 127 ? 127 : static_cast<i16>(a.w[i]))); o.b[8 + i] = static_cast<u8>(static_cast<i8>(static_cast<i16>(b.w[i]) < -128 ? -128 : static_cast<i16>(b.w[i]) > 127 ? 127 : static_cast<i16>(b.w[i]))); } break;
    case 0x64: for (int i = 0; i < 16; ++i) o.b[i] = static_cast<i8>(a.b[i]) > static_cast<i8>(b.b[i]) ? 0xFF : 0; break;
    case 0x65: for (int i = 0; i < 8; ++i) o.w[i] = static_cast<i16>(a.w[i]) > static_cast<i16>(b.w[i]) ? 0xFFFF : 0; break;
    case 0x66: for (int i = 0; i < 4; ++i) o.d[i] = static_cast<i32>(a.d[i]) > static_cast<i32>(b.d[i]) ? 0xFFFFFFFFU : 0; break;
    case 0x67: for (int i = 0; i < 8; ++i) { o.b[i] = sat_u8(static_cast<i16>(a.w[i])); o.b[8 + i] = sat_u8(static_cast<i16>(b.w[i])); } break;
    case 0x68: for (int i = 0; i < 8; ++i) { o.b[2 * i] = a.b[8 + i]; o.b[2 * i + 1] = b.b[8 + i]; } break;
    case 0x69: for (int i = 0; i < 4; ++i) { o.w[2 * i] = a.w[4 + i]; o.w[2 * i + 1] = b.w[4 + i]; } break;
    case 0x6A: for (int i = 0; i < 2; ++i) { o.d[2 * i] = a.d[2 + i]; o.d[2 * i + 1] = b.d[2 + i]; } break;
    case 0x6B: for (int i = 0; i < 4; ++i) { o.w[i] = static_cast<u16>(sat_i32_to_i16(static_cast<i32>(a.d[i]))); o.w[4 + i] = static_cast<u16>(sat_i32_to_i16(static_cast<i32>(b.d[i]))); } break;
    case 0x6C: o.q[0] = a.q[0]; o.q[1] = b.q[0]; break;
    case 0x6D: o.q[0] = a.q[1]; o.q[1] = b.q[1]; break;
    case 0x74: for (int i = 0; i < 16; ++i) o.b[i] = a.b[i] == b.b[i] ? 0xFF : 0; break;
    case 0x75: for (int i = 0; i < 8; ++i) o.w[i] = a.w[i] == b.w[i] ? 0xFFFF : 0; break;
    case 0x76: for (int i = 0; i < 4; ++i) o.d[i] = a.d[i] == b.d[i] ? 0xFFFFFFFFU : 0; break;
    case 0xD1: case 0xD2: case 0xD3: case 0xE1: case 0xE2: case 0xF1: case 0xF2: case 0xF3: { // shifts by xmm count
        const u64 cnt = b.q[0];
        o = a;
        if (op == 0xD1 || op == 0xE1 || op == 0xF1) for (int i = 0; i < 8; ++i) o.w[i] = op == 0xD1 ? (cnt > 15 ? 0 : static_cast<u16>(a.w[i] >> cnt)) : op == 0xE1 ? static_cast<u16>(static_cast<i16>(a.w[i]) >> (cnt > 15 ? 15 : cnt)) : (cnt > 15 ? 0 : static_cast<u16>(a.w[i] << cnt));
        else if (op == 0xD2 || op == 0xE2 || op == 0xF2) for (int i = 0; i < 4; ++i) o.d[i] = op == 0xD2 ? (cnt > 31 ? 0 : a.d[i] >> cnt) : op == 0xE2 ? static_cast<u32>(static_cast<i32>(a.d[i]) >> (cnt > 31 ? 31 : cnt)) : (cnt > 31 ? 0 : a.d[i] << cnt);
        else for (int i = 0; i < 2; ++i) o.q[i] = op == 0xD3 ? (cnt > 63 ? 0 : a.q[i] >> cnt) : (cnt > 63 ? 0 : a.q[i] << cnt);
        break;
    }
    case 0xD4: o.q[0] = a.q[0] + b.q[0]; o.q[1] = a.q[1] + b.q[1]; break;
    case 0xD5: for (int i = 0; i < 8; ++i) o.w[i] = static_cast<u16>(static_cast<i16>(a.w[i]) * static_cast<i16>(b.w[i])); break;
    case 0xD8: for (int i = 0; i < 16; ++i) o.b[i] = sat_u8(static_cast<int>(a.b[i]) - b.b[i]); break;
    case 0xD9: for (int i = 0; i < 8; ++i) o.w[i] = sat_u16(static_cast<int>(a.w[i]) - b.w[i]); break;
    case 0xDA: for (int i = 0; i < 16; ++i) o.b[i] = (std::min)(a.b[i], b.b[i]); break;
    case 0xDB: o.q[0] = a.q[0] & b.q[0]; o.q[1] = a.q[1] & b.q[1]; break;
    case 0xDC: for (int i = 0; i < 16; ++i) o.b[i] = sat_u8(static_cast<int>(a.b[i]) + b.b[i]); break;
    case 0xDD: for (int i = 0; i < 8; ++i) o.w[i] = sat_u16(static_cast<int>(a.w[i]) + b.w[i]); break;
    case 0xDE: for (int i = 0; i < 16; ++i) o.b[i] = (std::max)(a.b[i], b.b[i]); break;
    case 0xDF: o.q[0] = ~a.q[0] & b.q[0]; o.q[1] = ~a.q[1] & b.q[1]; break;
    case 0xE0: for (int i = 0; i < 16; ++i) o.b[i] = static_cast<u8>((static_cast<int>(a.b[i]) + b.b[i] + 1) >> 1); break;
    case 0xE3: for (int i = 0; i < 8; ++i) o.w[i] = static_cast<u16>((static_cast<int>(a.w[i]) + b.w[i] + 1) >> 1); break;
    case 0xE4: for (int i = 0; i < 8; ++i) o.w[i] = static_cast<u16>((static_cast<u32>(a.w[i]) * b.w[i]) >> 16); break;
    case 0xE5: for (int i = 0; i < 8; ++i) o.w[i] = static_cast<u16>((static_cast<i32>(static_cast<i16>(a.w[i])) * static_cast<i32>(static_cast<i16>(b.w[i]))) >> 16); break;
    case 0xE8: for (int i = 0; i < 16; ++i) o.b[i] = sat_s8(static_cast<i8>(a.b[i]) - static_cast<i8>(b.b[i])); break;
    case 0xE9: for (int i = 0; i < 8; ++i) o.w[i] = sat_s16(static_cast<i16>(a.w[i]) - static_cast<i16>(b.w[i])); break;
    case 0xEA: for (int i = 0; i < 8; ++i) o.w[i] = static_cast<u16>((std::min)(static_cast<i16>(a.w[i]), static_cast<i16>(b.w[i]))); break;
    case 0xEB: o.q[0] = a.q[0] | b.q[0]; o.q[1] = a.q[1] | b.q[1]; break;
    case 0xEC: for (int i = 0; i < 16; ++i) o.b[i] = sat_s8(static_cast<i8>(a.b[i]) + static_cast<i8>(b.b[i])); break;
    case 0xED: for (int i = 0; i < 8; ++i) o.w[i] = sat_s16(static_cast<i16>(a.w[i]) + static_cast<i16>(b.w[i])); break;
    case 0xEE: for (int i = 0; i < 8; ++i) o.w[i] = static_cast<u16>((std::max)(static_cast<i16>(a.w[i]), static_cast<i16>(b.w[i]))); break;
    case 0xEF: o.q[0] = a.q[0] ^ b.q[0]; o.q[1] = a.q[1] ^ b.q[1]; break;
    case 0xF4: o.q[0] = static_cast<u64>(a.d[0]) * b.d[0]; o.q[1] = static_cast<u64>(a.d[2]) * b.d[2]; break;
    case 0xF5: for (int i = 0; i < 4; ++i) o.d[i] = static_cast<u32>(static_cast<i32>(static_cast<i16>(a.w[2 * i])) * static_cast<i16>(b.w[2 * i]) + static_cast<i32>(static_cast<i16>(a.w[2 * i + 1])) * static_cast<i16>(b.w[2 * i + 1])); break;
    case 0xF8: for (int i = 0; i < 16; ++i) o.b[i] = static_cast<u8>(a.b[i] - b.b[i]); break;
    case 0xF9: for (int i = 0; i < 8; ++i) o.w[i] = static_cast<u16>(a.w[i] - b.w[i]); break;
    case 0xFA: for (int i = 0; i < 4; ++i) o.d[i] = a.d[i] - b.d[i]; break;
    case 0xFB: o.q[0] = a.q[0] - b.q[0]; o.q[1] = a.q[1] - b.q[1]; break;
    case 0xFC: for (int i = 0; i < 16; ++i) o.b[i] = static_cast<u8>(a.b[i] + b.b[i]); break;
    case 0xFD: for (int i = 0; i < 8; ++i) o.w[i] = static_cast<u16>(a.w[i] + b.w[i]); break;
    case 0xFE: for (int i = 0; i < 4; ++i) o.d[i] = a.d[i] + b.d[i]; break;
    default: unsupported("unsupported packed-integer opcode 0F " + hex(op));
    }
    xmm[m.reg] = o;
    (void)osz;
}


// ===================== AVX (VEX-encoded) =====================
// Strategy: VEX.128 ops reuse the verified legacy SSE implementation after copying the
// first source (VEX.vvvv) into the destination, then zero the destination's upper half.
// VEX.256 ops run the same legacy code twice: once on the low halves and once on the
// high halves (arrays swapped, memory operand +16), which is exact for in-lane ops.
// Lane-crossing ops (broadcast, vperm2f128, vinsertf128, vextractf128...) are explicit.
inline void GuestCpu::vex_legacy_pass(u8 map, u8 op, bool nds, bool) {
    if (map != 1 && map != 2) return;
    // dest = op(vvvv, rm). The legacy code computes dest = op(dest, rm), so vvvv is copied into dest first. When the register operand
    // rm IS the destination (e.g. `vdivss xmm0, xmm1, xmm0`), that copy would destroy the rm operand: park rm in a scratch register.
    int scratch = -1; Xmm saved{};
    if (nds) {
        const u8 mrm = mem.read<u8>(pc_); const unsigned reg = ((mrm >> 3) & 7) | ((rex_ & 4) ? 8 : 0), rm = (mrm & 7) | ((rex_ & 1) ? 8 : 0);
        const bool rm_is_gpr = map == 1 && (op == 0x6E || op == 0x2A || op == 0xC4);   // VMOVD/Q xmm,r/m / VCVTSI2SS/SD / VPINSRW take a general-purpose register in r/m
        if (!rm_is_gpr && (mrm >> 6) == 3 && rm == reg && reg != vex_vvvv_) {
            scratch = 0; while (static_cast<unsigned>(scratch) == reg || static_cast<unsigned>(scratch) == vex_vvvv_) ++scratch;
            saved = xmm[scratch]; xmm[scratch] = xmm[rm]; rm_override_ = scratch;
        }
        xmm[reg] = xmm[vex_vvvv_];
    }
    try { if (map == 1) execute_0f_sse(op); else execute_0f38_op(op); } catch (...) { rm_override_ = -1; if (scratch >= 0) xmm[scratch] = saved; throw; }
    rm_override_ = -1;
    if (scratch >= 0) xmm[scratch] = saved;
}

inline void GuestCpu::execute_vex(u8 first) {
    unsigned R = 0, X = 0, B = 0, W = 0, map = 1, L, pp;
    if (first == 0xC5) {
        const u8 b1 = f8();
        R = (~b1 >> 7) & 1; vex_vvvv_ = (~b1 >> 3) & 15; L = (b1 >> 2) & 1; pp = b1 & 3;
    } else {
        const u8 b1 = f8(), b2 = f8();
        R = (~b1 >> 7) & 1; X = (~b1 >> 6) & 1; B = (~b1 >> 5) & 1; map = b1 & 31;
        W = (b2 >> 7) & 1; vex_vvvv_ = (~b2 >> 3) & 15; L = (b2 >> 2) & 1; pp = b2 & 3;
    }
    rex_ = static_cast<u8>(0x40 | (W << 3) | (R << 2) | (X << 1) | B); rex_present_ = true;
    opsz16_ = pp == 1; rep_ = pp == 2 ? 0xF3 : pp == 3 ? 0xF2 : 0;
    const u8 op = f8();
    const u64 body = pc_;                 // points at ModRM (or imm for 0F 77)
    const unsigned vvvv = vex_vvvv_;

    if (map == 1 && op == 0x77) { // VZEROUPPER / VZEROALL
        for (auto& h : ymm_hi) h = Xmm();
        if (L) for (auto& x : xmm) x = Xmm();
        return;
    }
    if (map == 1 && op == 0xAE) { pc_ = body - 1; execute_0f(); return; } // VLDMXCSR / VSTMXCSR
    if (map == 1 && ((op == 0x12 && (pp == 2 || pp == 3)) || (op == 0x16 && pp == 2))) {
        // VMOVSLDUP (F3 0F 12), VMOVDDUP (F2 0F 12), and VMOVSHDUP
        // (F3 0F 16). With VEX.256 the operation repeats independently in
        // each 128-bit lane, while VEX.128 clears the architectural upper half.
        const ModRM m = modrm();
        auto dup_lane = [op, pp](const Xmm& source) {
            Xmm result{};
            if (op == 0x12 && pp == 3) {
                result.q[0] = source.q[0]; result.q[1] = source.q[0];
            } else {
                const unsigned base = op == 0x12 ? 0U : 1U;
                result.d[0] = source.d[base]; result.d[1] = source.d[base];
                result.d[2] = source.d[base + 2]; result.d[3] = source.d[base + 2];
            }
            return result;
        };
        Xmm source_low{};
        if (op == 0x12 && pp == 3) {
            if (m.mem) source_low.q[0] = mem.read<u64>(m.addr); // VMOVDDUP xmm/m64
            else source_low = xmm[m.rm];
        } else {
            source_low = rd_x(m);
        }
        Xmm source_high{};
        if (L) {
            if (m.mem) {
                if (op == 0x12 && pp == 3) source_high.q[0] = mem.read<u64>(m.addr + 16); // YMM/m256 lane 1
                else { source_high.q[0] = mem.read<u64>(m.addr + 16); source_high.q[1] = mem.read<u64>(m.addr + 24); }
            } else {
                source_high = ymm_hi[m.rm];
            }
        }
        xmm[m.reg] = dup_lane(source_low);
        ymm_hi[m.reg] = L ? dup_lane(source_high) : Xmm();
        return;
    }
    if (map == 1 && (op == 0x71 || op == 0x72 || op == 0x73)) { // shifts by imm: dest=vvvv, src=rm
        const u8 mrm = mem.read<u8>(pc_);
        const unsigned rm = (mrm & 7) | ((rex_ & 1) ? 8 : 0);
        for (unsigned half = 0; half <= L; ++half) {
            if (half) { std::swap(xmm, ymm_hi); }
            pc_ = body;
            const Xmm saved = xmm[rm];
            opsz16_ = true; rep_ = 0;
            execute_0f_sse(op);
            const Xmm res = xmm[rm]; xmm[rm] = saved; xmm[vvvv] = res;
            if (half) { std::swap(xmm, ymm_hi); }
        }
        if (!L) ymm_hi[vvvv] = Xmm();
        return;
    }
    if (map == 1 && (op == 0x11 || op == 0x10) && pp >= 2 && (mem.read<u8>(pc_) >> 6) == 3) { // VMOVSS/SD reg,reg (3-operand)
        const u8 mrm = mem.read<u8>(pc_); pc_ += 1;
        const unsigned reg = ((mrm >> 3) & 7) | ((rex_ & 4) ? 8 : 0), rm = (mrm & 7) | ((rex_ & 1) ? 8 : 0);
        const unsigned dst = op == 0x10 ? reg : rm, s2 = op == 0x10 ? rm : reg;
        Xmm t = xmm[vvvv];
        if (pp == 2) t.d[0] = xmm[s2].d[0]; else t.q[0] = xmm[s2].q[0];
        xmm[dst] = t; ymm_hi[dst] = Xmm();
        return;
    }

    // Classification of legacy-pass ops
    auto gpr_or_flags_dest = [&](u8 o) { return o == 0x50 || o == 0x2C || o == 0x2D || o == 0x2E || o == 0x2F || o == 0xD7 || o == 0xC5 || o == 0xF7 || (o == 0x7E && pp != 2); };
    auto store_op = [&](u8 o) { return o == 0x11 || o == 0x13 || o == 0x17 || o == 0x29 || o == 0x2B || o == 0x7F || o == 0xD6 || o == 0xE7 || (o == 0x7E && pp != 2); };

    if (map == 1) {
        const bool store = store_op(op), gpr = gpr_or_flags_dest(op);
        const u8 mrm = mem.read<u8>(pc_);
        const unsigned reg = ((mrm >> 3) & 7) | ((rex_ & 4) ? 8 : 0), rm = (mrm & 7) | ((rex_ & 1) ? 8 : 0);
        const bool dst_is_rm_reg = store && (mrm >> 6) == 3;
        const bool nds = !store && !gpr;
        for (unsigned half = 0; half <= L; ++half) {
            if (half) { std::swap(xmm, ymm_hi); mem_disp_ = 16; }
            pc_ = body;
            try { vex_legacy_pass(1, op, nds, false); } catch (...) { if (half) { std::swap(xmm, ymm_hi); mem_disp_ = 0; } throw; }
            if (half) { std::swap(xmm, ymm_hi); mem_disp_ = 0; }
        }
        if (!L) {
            if (dst_is_rm_reg) ymm_hi[rm] = Xmm();
            else if (!store && !gpr) ymm_hi[reg] = Xmm();
        }
        return;
    }
    if (map == 2 || map == 3) { execute_vex_ext(map, op, L, pp, W); return; }
    unsupported("unsupported VEX map " + std::to_string(map) + " opcode 0x" + hex(op));
}

inline void GuestCpu::execute_vex_ext(unsigned map, u8 op, unsigned L, unsigned pp, unsigned W) {
    const unsigned vvvv = vex_vvvv_;
    const u64 body = pc_;
    (void)W;
    auto two_pass_generic = [&](bool nds) {
        for (unsigned half = 0; half <= L; ++half) {
            if (half) { std::swap(xmm, ymm_hi); mem_disp_ = 16; }
            pc_ = body;
            try { vex_legacy_pass(static_cast<u8>(map), op, nds, false); } catch (...) { if (half) { std::swap(xmm, ymm_hi); mem_disp_ = 0; } throw; }
            if (half) { std::swap(xmm, ymm_hi); mem_disp_ = 0; }
        }
    };
    auto round_val = [&](double v, unsigned imm) {
        const unsigned mode = (imm & 4) ? 0 : (imm & 3);
        switch (mode) { case 1: return std::floor(v); case 2: return std::ceil(v); case 3: return std::trunc(v); default: return std::nearbyint(v); }
    };
    if (map == 2) {
        if (pp != 1) unsupported("VEX 0F38 op 0x" + hex(op) + " with this prefix is not implemented");
        const ModRM m = modrm();
        switch (op) {
        case 0x00: { pc_ = body; two_pass_generic(true); if (!L) ymm_hi[m.reg] = Xmm(); return; } // VPSHUFB
        case 0x18: { // VBROADCASTSS
            u32 v = m.mem ? mem.read<u32>(m.addr) : xmm[m.rm].d[0];
            Xmm x; for (auto& d : x.d) d = v; xmm[m.reg] = x; ymm_hi[m.reg] = L ? x : Xmm(); return;
        }
        case 0x19: { // VBROADCASTSD
            if (!L) unsupported("VBROADCASTSD requires VEX.256");
            u64 v = m.mem ? mem.read<u64>(m.addr) : xmm[m.rm].q[0];
            Xmm x; x.q[0] = x.q[1] = v; xmm[m.reg] = x; ymm_hi[m.reg] = x; return;
        }
        case 0x1A: { // VBROADCASTF128
            if (!L || !m.mem) unsupported("VBROADCASTF128 requires VEX.256 and a memory source");
            Xmm x; x.q[0] = mem.read<u64>(m.addr); x.q[1] = mem.read<u64>(m.addr + 8); xmm[m.reg] = x; ymm_hi[m.reg] = x; return;
        }
        case 0x17: { // VPTEST
            Xmm a = xmm[m.reg], b = rd_x(m);
            u64 andz = (a.q[0] & b.q[0]) | (a.q[1] & b.q[1]), cf = (~a.q[0] & b.q[0]) | (~a.q[1] & b.q[1]);
            if (L) {
                Xmm a2 = ymm_hi[m.reg], b2;
                if (m.mem) { b2.q[0] = mem.read<u64>(m.addr + 16); b2.q[1] = mem.read<u64>(m.addr + 24); } else b2 = ymm_hi[m.rm];
                andz |= (a2.q[0] & b2.q[0]) | (a2.q[1] & b2.q[1]); cf |= (~a2.q[0] & b2.q[0]) | (~a2.q[1] & b2.q[1]);
            }
            rflags &= ~(OF | SF | AF | CF | ZF | PF); if (!andz) rflags |= ZF; if (!cf) rflags |= CF; return;
        }
        default: unsupported("unsupported VEX 0F38 opcode 0x" + hex(op));
        }
    }
    // map 3 (0F3A) - imm8 follows ModRM
    if (pp != 1) unsupported("VEX 0F3A op 0x" + hex(op) + " with this prefix is not implemented");
    const ModRM m = modrm(1);
    const u8 imm = f8();
    switch (op) {
    case 0x16: { // VPEXTRD / VPEXTRQ (the immediate selects a lane in XMM)
        if (L) unsupported("VPEXTRD/VPEXTRQ requires VEX.128");
        if (W) wr_rm(m, 8, xmm[m.reg].q[imm & 1]);
        else wr_rm(m, 4, xmm[m.reg].d[imm & 3]);
        return;
    }
    case 0x06: { // VPERM2F128
        if (!L) unsupported("VPERM2F128 requires VEX.256");
        Xmm src[4]; src[0] = xmm[vvvv]; src[1] = ymm_hi[vvvv];
        if (m.mem) { src[2].q[0] = mem.read<u64>(m.addr); src[2].q[1] = mem.read<u64>(m.addr + 8); src[3].q[0] = mem.read<u64>(m.addr + 16); src[3].q[1] = mem.read<u64>(m.addr + 24); }
        else { src[2] = xmm[m.rm]; src[3] = ymm_hi[m.rm]; }
        auto sel = [&](unsigned ctl) { return (ctl & 8) ? Xmm() : src[ctl & 3]; };
        const Xmm lo = sel(imm & 0xF), hi = sel((imm >> 4) & 0xF);
        xmm[m.reg] = lo; ymm_hi[m.reg] = hi; return;
    }
    case 0x18: { // VINSERTF128
        if (!L) unsupported("VINSERTF128 requires VEX.256");
        Xmm ins; if (m.mem) { ins.q[0] = mem.read<u64>(m.addr); ins.q[1] = mem.read<u64>(m.addr + 8); } else ins = xmm[m.rm];
        const Xmm lo = xmm[vvvv], hi = ymm_hi[vvvv];
        if (imm & 1) { xmm[m.reg] = lo; ymm_hi[m.reg] = ins; } else { xmm[m.reg] = ins; ymm_hi[m.reg] = hi; }
        return;
    }
    case 0x19: { // VEXTRACTF128
        if (!L) unsupported("VEXTRACTF128 requires VEX.256");
        const Xmm v = (imm & 1) ? ymm_hi[m.reg] : xmm[m.reg];
        if (m.mem) { mem.write<u64>(m.addr, v.q[0]); mem.write<u64>(m.addr + 8, v.q[1]); } else { xmm[m.rm] = v; ymm_hi[m.rm] = Xmm(); }
        return;
    }
    case 0x08: case 0x09: { // VROUNDPS / VROUNDPD (per half)
        for (unsigned half = 0; half <= L; ++half) {
            Xmm src; const u64 off = half ? 16 : 0;
            if (m.mem) { src.q[0] = mem.read<u64>(m.addr + off); src.q[1] = mem.read<u64>(m.addr + off + 8); } else src = half ? ymm_hi[m.rm] : xmm[m.rm];
            Xmm& dst = half ? ymm_hi[m.reg] : xmm[m.reg];
            if (op == 0x08) for (int i = 0; i < 4; ++i) dst.f[i] = static_cast<float>(round_val(static_cast<double>(src.f[i]), imm));
            else for (int i = 0; i < 2; ++i) dst.df[i] = round_val(src.df[i], imm);
        }
        if (!L) ymm_hi[m.reg] = Xmm();
        return;
    }
    case 0x0A: case 0x0B: { // VROUNDSS / VROUNDSD
        Xmm t = xmm[vvvv];
        if (op == 0x0A) { const float v = m.mem ? mem.read<float>(m.addr) : xmm[m.rm].f[0]; t.f[0] = static_cast<float>(round_val(static_cast<double>(v), imm)); }
        else { const double v = m.mem ? mem.read<double>(m.addr) : xmm[m.rm].df[0]; t.df[0] = round_val(v, imm); }
        xmm[m.reg] = t; ymm_hi[m.reg] = Xmm(); return;
    }
    case 0x04: case 0x05: { // VPERMILPS / VPERMILPD immediate
        for (unsigned half = 0; half <= L; ++half) {
            Xmm src; const u64 off = half ? 16 : 0;
            if (m.mem) { src.q[0] = mem.read<u64>(m.addr + off); src.q[1] = mem.read<u64>(m.addr + off + 8); } else src = half ? ymm_hi[m.rm] : xmm[m.rm];
            Xmm o;
            if (op == 0x04) for (int i = 0; i < 4; ++i) o.d[i] = src.d[(imm >> (i * 2)) & 3];
            else { o.q[0] = src.q[(imm >> (half * 2)) & 1]; o.q[1] = src.q[(imm >> (half * 2 + 1)) & 1]; }
            (half ? ymm_hi[m.reg] : xmm[m.reg]) = o;
        }
        if (!L) ymm_hi[m.reg] = Xmm();
        return;
    }
    case 0x21: { // VINSERTPS xmm1, xmm2, xmm3/m32, imm8  (bits 7:6 source lane [register form], 5:4 destination lane, 3:0 zero mask)
        if (L) unsupported("VINSERTPS requires VEX.128");
        const u32 v = m.mem ? mem.read<u32>(m.addr) : xmm[m.rm].d[(imm >> 6) & 3];
        Xmm t = xmm[vvvv]; t.d[(imm >> 4) & 3] = v;
        for (int i = 0; i < 4; ++i) if (imm & (1 << i)) t.d[i] = 0;
        xmm[m.reg] = t; ymm_hi[m.reg] = Xmm(); return;
    }
    case 0x22: { // VPINSRD / VPINSRQ
        if (L) unsupported("VPINSRD/VPINSRQ requires VEX.128");
        Xmm t = xmm[vvvv];
        if (W) t.q[imm & 1] = rd_rm(m, 8); else t.d[imm & 3] = static_cast<u32>(rd_rm(m, 4));
        xmm[m.reg] = t; ymm_hi[m.reg] = Xmm(); return;
    }
    case 0x20: { // VPINSRB
        if (L) unsupported("VPINSRB requires VEX.128");
        Xmm t = xmm[vvvv]; const unsigned lane = imm & 15; const u8 b = static_cast<u8>(rd_rm(m, m.mem ? 1 : 4));
        t.q[lane >> 3] = (t.q[lane >> 3] & ~(0xFFULL << ((lane & 7) * 8))) | (static_cast<u64>(b) << ((lane & 7) * 8));
        xmm[m.reg] = t; ymm_hi[m.reg] = Xmm(); return;
    }
    case 0x17: { // VEXTRACTPS r/m32, xmm, imm8
        if (L) unsupported("VEXTRACTPS requires VEX.128");
        wr_rm(m, 4, xmm[m.reg].d[imm & 3]); return;
    }
    case 0x14: case 0x15: { // VPEXTRB / VPEXTRW
        if (L) unsupported("VPEXTRB/VPEXTRW requires VEX.128");
        const unsigned lane = op == 0x14 ? (imm & 15) : (imm & 7), width = op == 0x14 ? 1 : 2;
        const u64 v = (xmm[m.reg].q[(lane * width) >> 3] >> (((lane * width) & 7) * 8)) & (width == 1 ? 0xFFULL : 0xFFFFULL);
        wr_rm(m, m.mem ? width : 4, v); return;
    }
    case 0x0C: case 0x0D: case 0x0E: { // VBLENDPS / VBLENDPD / VPBLENDW (imm bit i picks element i from the r/m operand)
        for (unsigned half = 0; half <= L; ++half) {
            Xmm a = half ? ymm_hi[vvvv] : xmm[vvvv], b; const u64 off = half ? 16 : 0;
            if (m.mem) { b.q[0] = mem.read<u64>(m.addr + off); b.q[1] = mem.read<u64>(m.addr + off + 8); } else b = half ? ymm_hi[m.rm] : xmm[m.rm];
            if (op == 0x0C) { for (int i = 0; i < 4; ++i) if (imm & (1 << (i + half * 4))) a.d[i] = b.d[i]; }
            else if (op == 0x0D) { for (int i = 0; i < 2; ++i) if (imm & (1 << (i + half * 2))) a.q[i] = b.q[i]; }
            else { for (int i = 0; i < 8; ++i) if (imm & (1 << i)) { const u64 sh = (i & 3) * 16; const int q = i >> 2; a.q[q] = (a.q[q] & ~(0xFFFFULL << sh)) | (b.q[q] & (0xFFFFULL << sh)); } }
            (half ? ymm_hi[m.reg] : xmm[m.reg]) = a;
        }
        if (!L) ymm_hi[m.reg] = Xmm();
        return;
    }
    case 0x0F: { // VPALIGNR: per 128-bit lane, (src1:src2) >> (imm * 8 bits)
        for (unsigned half = 0; half <= L; ++half) {
            Xmm a = half ? ymm_hi[vvvv] : xmm[vvvv], b; const u64 off = half ? 16 : 0;
            if (m.mem) { b.q[0] = mem.read<u64>(m.addr + off); b.q[1] = mem.read<u64>(m.addr + off + 8); } else b = half ? ymm_hi[m.rm] : xmm[m.rm];
            u8 cat[32]; std::memcpy(cat, &b, 16); std::memcpy(cat + 16, &a, 16);   // b = low 16 bytes, a = high 16 bytes
            Xmm o{}; u8* ob = reinterpret_cast<u8*>(&o);
            for (unsigned i = 0; i < 16; ++i) { const unsigned k = i + imm; ob[i] = k < 32 ? cat[k] : 0; }
            (half ? ymm_hi[m.reg] : xmm[m.reg]) = o;
        }
        if (!L) ymm_hi[m.reg] = Xmm();
        return;
    }
    default: unsupported("unsupported VEX 0F3A opcode 0x" + hex(op));
    }
}

inline void GuestCpu::execute_0f38() { const u8 op = f8(); execute_0f38_op(op); }
inline void GuestCpu::execute_0f38_op(u8 op) {
    if (sse_pfx() != 1) unsupported("0F 38 " + hex(op) + " (non-66) not implemented");
    const ModRM m = modrm();
    const Xmm a = xmm[m.reg], b = rd_x(m);
    Xmm o;
    switch (op) {
    case 0x00: for (int i = 0; i < 16; ++i) o.b[i] = (b.b[i] & 0x80) ? 0 : a.b[b.b[i] & 15]; break; // PSHUFB
    default: unsupported("unsupported 0F 38 opcode 0x" + hex(op));
    }
    xmm[m.reg] = o;
}
inline void GuestCpu::execute_0f3a() {
    const u8 op = f8();
    unsupported("unsupported 0F 3A opcode 0x" + hex(op) + " (SSSE3/SSE4 not reported by CPUID)");
}

} // namespace someboxone

#include "guest_cpu_fast.hpp"

