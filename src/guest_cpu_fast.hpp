#pragma once
// SomeBoxOne v0.31.0 - decoded-instruction cache ("fast path") for GuestCpu.
//
// Each guest instruction is decoded ONCE into a cache entry that holds a handler pointer and pre-parsed operands
// (registers, effective-address parts, immediates, branch target). Re-executing it skips prefix scanning, ModRM/SIB
// decoding and byte fetching, which dominated the old interpreter (~120 cycles per instruction).
//
// The ~45 most frequent integer forms (mov, movzx/movsx, ALU, test, lea, inc/dec, neg/not, shifts, imul, cmovcc, setcc,
// jcc/jmp/call/ret, push/pop, ...) get specialized handlers. Everything else is cached as a "legacy" entry that runs the
// original, differential-tested interpreter (GuestCpu::execute), so behaviour is identical by construction for those.
// Specialized handlers reuse the interpreter's own flag/ALU/shift helpers, so flag semantics cannot drift.
//
// Self-modifying code: pages that were decoded are flagged in GuestMemory; a write to one bumps its epoch and the CPU
// drops the whole cache on the next iteration.

// (Included from the end of guest_cpu.hpp; it relies on GuestCpu being fully defined.)

namespace someboxone {

// Size -> C++ integer type, declared (with its specializations) BEFORE any use so every compiler sees them first.
template <unsigned SZ> struct FastTy;
template <> struct FastTy<1> { using type = std::uint8_t; };
template <> struct FastTy<2> { using type = std::uint16_t; };
template <> struct FastTy<4> { using type = std::uint32_t; };
template <> struct FastTy<8> { using type = std::uint64_t; };

struct FastExec {
    using u8 = GuestCpu::u8; using u16 = GuestCpu::u16; using u32 = GuestCpu::u32; using u64 = GuestCpu::u64;
    using i8 = GuestCpu::i8; using i16 = GuestCpu::i16; using i32 = GuestCpu::i32; using i64 = GuestCpu::i64;
    using Insn = GuestCpu::Insn;

    template <unsigned SZ> using T = typename FastTy<SZ>::type;

    // ---- register / memory access helpers ----
    template <unsigned SZ> static inline u64 R(const GuestCpu& c, unsigned i) {
        if constexpr (SZ == 8) return c.r[i]; else if constexpr (SZ == 4) return static_cast<u32>(c.r[i]);
        else if constexpr (SZ == 2) return static_cast<u16>(c.r[i]); else return static_cast<u8>(c.r[i]);
    }
    template <unsigned SZ> static inline void W(GuestCpu& c, unsigned i, u64 v) {
        if constexpr (SZ == 8) c.r[i] = v; else if constexpr (SZ == 4) c.r[i] = static_cast<u32>(v);
        else if constexpr (SZ == 2) c.r[i] = (c.r[i] & ~0xFFFFULL) | static_cast<u16>(v);
        else c.r[i] = (c.r[i] & ~0xFFULL) | static_cast<u8>(v);
    }
    static inline u64 EA(const GuestCpu& c, const Insn& e) {
        u64 a = static_cast<u64>(e.disp);
        if (e.base != 0xFF) a += c.r[e.base];
        if (e.index != 0xFF) a += c.r[e.index] << e.scale;
        if (e.seg) a += (e.seg == 1 ? c.fs_base : c.gs_base);
        return a;
    }

    // ---- handlers ----
    static void h_legacy(GuestCpu& c, const Insn&) {
        c.pc_ = c.insn_rip_;
        c.fetch_ptr_ = c.mem.code_ptr(c.insn_rip_);
        c.fetch_base_ = c.insn_rip_;
        c.fetch_len_ = GuestMemory::page_size - (c.insn_rip_ & GuestMemory::page_mask);
        c.execute();
        c.rip = c.pc_;
    }
    static void h_nop(GuestCpu&, const Insn&) {}

    template <unsigned SZ> static void mov_rr(GuestCpu& c, const Insn& e) { W<SZ>(c, e.dst, R<SZ>(c, e.src)); }
    template <unsigned SZ> static void mov_load(GuestCpu& c, const Insn& e) { W<SZ>(c, e.dst, c.mem.template read<T<SZ>>(EA(c, e))); }
    template <unsigned SZ> static void mov_store(GuestCpu& c, const Insn& e) { c.mem.template write<T<SZ>>(EA(c, e), static_cast<T<SZ>>(R<SZ>(c, e.src))); }
    template <unsigned SZ> static void mov_ri(GuestCpu& c, const Insn& e) { W<SZ>(c, e.dst, static_cast<u64>(e.imm)); }
    template <unsigned SZ> static void mov_store_imm(GuestCpu& c, const Insn& e) { c.mem.template write<T<SZ>>(EA(c, e), static_cast<T<SZ>>(e.imm)); }
    template <unsigned SZ> static void lea(GuestCpu& c, const Insn& e) { W<SZ>(c, e.dst, EA(c, e)); }

    template <unsigned DS, unsigned SS, bool SGN, bool MEM> static void movx(GuestCpu& c, const Insn& e) {
        u64 v = MEM ? static_cast<u64>(c.mem.template read<T<SS>>(EA(c, e))) : R<SS>(c, e.src);
        if constexpr (SGN) v = GuestCpu::sext(v, SS);
        W<DS>(c, e.dst, v);
    }

    // ALU: e.op = 0..7 (add or adc sbb and sub xor cmp); e.aux == 1 means "no write-back" (cmp / test)
    template <unsigned SZ> static void alu_rr(GuestCpu& c, const Insn& e) { const u64 r = c.alu(e.op, R<SZ>(c, e.dst), R<SZ>(c, e.src), SZ); if (!e.aux) W<SZ>(c, e.dst, r); }
    template <unsigned SZ> static void alu_rm(GuestCpu& c, const Insn& e) { const u64 b = c.mem.template read<T<SZ>>(EA(c, e)); const u64 r = c.alu(e.op, R<SZ>(c, e.dst), b, SZ); if (!e.aux) W<SZ>(c, e.dst, r); }
    template <unsigned SZ> static void alu_mr(GuestCpu& c, const Insn& e) {
        const u64 a = EA(c, e); const u64 v = c.mem.template read<T<SZ>>(a);
        const u64 r = c.alu(e.op, v, R<SZ>(c, e.src), SZ); if (!e.aux) c.mem.template write<T<SZ>>(a, static_cast<T<SZ>>(r)); }
    template <unsigned SZ> static void alu_ri(GuestCpu& c, const Insn& e) { const u64 r = c.alu(e.op, R<SZ>(c, e.dst), static_cast<u64>(e.imm), SZ); if (!e.aux) W<SZ>(c, e.dst, r); }
    template <unsigned SZ> static void alu_mi(GuestCpu& c, const Insn& e) {
        const u64 a = EA(c, e); const u64 v = c.mem.template read<T<SZ>>(a);
        const u64 r = c.alu(e.op, v, static_cast<u64>(e.imm), SZ); if (!e.aux) c.mem.template write<T<SZ>>(a, static_cast<T<SZ>>(r)); }

    template <unsigned SZ> static void incdec_r(GuestCpu& c, const Insn& e) { const bool cf = c.getf(GuestCpu::CF); const u64 r = c.alu(e.op, R<SZ>(c, e.dst), 1, SZ); W<SZ>(c, e.dst, r); c.setf(GuestCpu::CF, cf); }
    template <unsigned SZ> static void incdec_m(GuestCpu& c, const Insn& e) {
        const u64 a = EA(c, e); const bool cf = c.getf(GuestCpu::CF);
        const u64 r = c.alu(e.op, c.mem.template read<T<SZ>>(a), 1, SZ); c.mem.template write<T<SZ>>(a, static_cast<T<SZ>>(r)); c.setf(GuestCpu::CF, cf); }
    template <unsigned SZ> static void not_r(GuestCpu& c, const Insn& e) { W<SZ>(c, e.dst, ~R<SZ>(c, e.dst) & GuestCpu::mask(SZ)); }
    template <unsigned SZ> static void not_m(GuestCpu& c, const Insn& e) { const u64 a = EA(c, e); c.mem.template write<T<SZ>>(a, static_cast<T<SZ>>(~c.mem.template read<T<SZ>>(a))); }
    template <unsigned SZ> static void neg_r(GuestCpu& c, const Insn& e) { const u64 v = R<SZ>(c, e.dst); const u64 r = c.alu(5, 0, v, SZ); W<SZ>(c, e.dst, r); c.setf(GuestCpu::CF, (v & GuestCpu::mask(SZ)) != 0); }
    template <unsigned SZ> static void neg_m(GuestCpu& c, const Insn& e) {
        const u64 a = EA(c, e); const u64 v = c.mem.template read<T<SZ>>(a); const u64 r = c.alu(5, 0, v, SZ);
        c.mem.template write<T<SZ>>(a, static_cast<T<SZ>>(r)); c.setf(GuestCpu::CF, (v & GuestCpu::mask(SZ)) != 0); }

    // shifts / rotates: e.op = ModRM.reg (0..7); e.aux: 0 = immediate count in e.imm, 2 = CL
    template <unsigned SZ> static void shift_r(GuestCpu& c, const Insn& e) {
        const unsigned cnt = e.aux == 2 ? static_cast<unsigned>(c.r[1] & 0xFF) : static_cast<unsigned>(e.imm);
        W<SZ>(c, e.dst, c.shift_op(e.op, R<SZ>(c, e.dst), cnt, SZ)); }
    template <unsigned SZ> static void shift_m(GuestCpu& c, const Insn& e) {
        const unsigned cnt = e.aux == 2 ? static_cast<unsigned>(c.r[1] & 0xFF) : static_cast<unsigned>(e.imm);
        const u64 a = EA(c, e); const u64 r = c.shift_op(e.op, c.mem.template read<T<SZ>>(a), cnt, SZ); c.mem.template write<T<SZ>>(a, static_cast<T<SZ>>(r)); }

    // imul r, r/m  and  imul r, r/m, imm
    template <unsigned SZ, bool MEM> static void imul2(GuestCpu& c, const Insn& e) {
        const u64 b = MEM ? static_cast<u64>(c.mem.template read<T<SZ>>(EA(c, e))) : R<SZ>(c, e.src);
        const GuestCpu::Wide prod = GuestCpu::mul_full(R<SZ>(c, e.dst), b, SZ, true);
        const u64 res = prod.lo & GuestCpu::mask(SZ); W<SZ>(c, e.dst, res);
        const bool ov = GuestCpu::imul_overflowed(prod, res, SZ); c.setf(GuestCpu::CF, ov); c.setf(GuestCpu::OF, ov); c.set_szp(res, SZ); }
    template <unsigned SZ, bool MEM> static void imul3(GuestCpu& c, const Insn& e) {
        const u64 b = MEM ? static_cast<u64>(c.mem.template read<T<SZ>>(EA(c, e))) : R<SZ>(c, e.src);
        const GuestCpu::Wide prod = GuestCpu::mul_full(b, static_cast<u64>(e.imm), SZ, true);
        const u64 res = prod.lo & GuestCpu::mask(SZ); W<SZ>(c, e.dst, res);
        const bool ov = GuestCpu::imul_overflowed(prod, res, SZ); c.setf(GuestCpu::CF, ov); c.setf(GuestCpu::OF, ov); c.set_szp(res, SZ); }

    template <unsigned SZ, bool MEM> static void cmov(GuestCpu& c, const Insn& e) {
        const u64 v = MEM ? static_cast<u64>(c.mem.template read<T<SZ>>(EA(c, e))) : R<SZ>(c, e.src);
        if (c.cond(e.cc)) W<SZ>(c, e.dst, v); else if constexpr (SZ == 4) W<4>(c, e.dst, R<4>(c, e.dst)); }
    static void setcc_r(GuestCpu& c, const Insn& e) { W<1>(c, e.dst, c.cond(e.cc) ? 1 : 0); }
    static void setcc_m(GuestCpu& c, const Insn& e) { c.mem.template write<u8>(EA(c, e), c.cond(e.cc) ? 1 : 0); }

    // control flow
    static void jcc(GuestCpu& c, const Insn& e) { if (c.cond(e.cc)) c.rip = e.target; }
    static void jmp(GuestCpu& c, const Insn& e) { c.rip = e.target; }
    static void call(GuestCpu& c, const Insn& e) { c.push(e.next); c.rip = e.target; }
    static void ret(GuestCpu& c, const Insn&) { c.rip = c.pop(); }
    static void jmp_ind_r(GuestCpu& c, const Insn& e) { c.rip = c.r[e.src]; }
    static void jmp_ind_m(GuestCpu& c, const Insn& e) { c.rip = c.mem.template read<u64>(EA(c, e)); }
    static void call_ind_r(GuestCpu& c, const Insn& e) { const u64 t = c.r[e.src]; c.push(e.next); c.rip = t; }
    static void call_ind_m(GuestCpu& c, const Insn& e) { const u64 t = c.mem.template read<u64>(EA(c, e)); c.push(e.next); c.rip = t; }
    static void push_r(GuestCpu& c, const Insn& e) { c.push(c.r[e.src]); }
    static void pop_r(GuestCpu& c, const Insn& e) { const u64 v = c.pop(); c.r[e.dst] = v; }
    static void push_imm(GuestCpu& c, const Insn& e) { c.push(static_cast<u64>(e.imm)); }
    static void cdqe(GuestCpu& c, const Insn&) { c.r[0] = GuestCpu::sx32(static_cast<u32>(c.r[0])); }
    static void cwde(GuestCpu& c, const Insn&) { c.r[0] = static_cast<u32>(GuestCpu::sx16(static_cast<u16>(c.r[0]))); }
    static void cqo(GuestCpu& c, const Insn&) { c.r[2] = (c.r[0] >> 63) ? ~0ULL : 0; }
    static void cdq(GuestCpu& c, const Insn&) { c.r[2] = (c.r[0] & 0x80000000ULL) ? 0xFFFFFFFFULL : 0; }

    // ---- decode ----
    struct Rd {
        const u8* p; unsigned n; unsigned i{0}; bool ok{true};
        u8 peek() const { return i < n ? p[i] : 0; }
        u8 b() { if (i >= n) { ok = false; return 0; } return p[i++]; }
        u32 d() { if (i + 4 > n) { ok = false; i = n; return 0; } u32 v; std::memcpy(&v, p + i, 4); i += 4; return v; }
        u16 w() { if (i + 2 > n) { ok = false; i = n; return 0; } u16 v; std::memcpy(&v, p + i, 2); i += 2; return v; }
        u64 q() { if (i + 8 > n) { ok = false; i = n; return 0; } u64 v; std::memcpy(&v, p + i, 8); i += 8; return v; }
    };

#define SBO_SZ(name, sz) ((sz) == 1 ? &name<1> : (sz) == 2 ? &name<2> : (sz) == 4 ? &name<4> : &name<8>)

    // Returns true if `e` was filled with a specialized handler; false = run the legacy interpreter for this instruction.
    static bool decode(GuestCpu& c, Insn& e, u64 pc0) {
        const u8* code;
        try { code = c.mem.code_ptr(pc0); } catch (const GuestFault&) { return false; }
        c.mem.mark_code_page(pc0 >> 12);
        Rd rd{code, static_cast<unsigned>((std::min<u64>)(15, GuestMemory::page_size - (pc0 & GuestMemory::page_mask)))};
        bool p66 = false; unsigned seg = 0; u8 rex = 0; bool has_rex = false;
        for (;;) {
            const u8 b = rd.peek();
            if (b == 0x66) { p66 = true; ++rd.i; }
            else if (b == 0x64) { seg = 1; ++rd.i; }
            else if (b == 0x65) { seg = 2; ++rd.i; }
            else if (b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36) { ++rd.i; }
            else if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3) return false;
            else break;
            if (rd.i >= 6) return false;
        }
        if ((rd.peek() & 0xF0) == 0x40) { rex = rd.b(); has_rex = true; }
        const unsigned osz = (rex & 8) ? 8 : (p66 ? 2 : 4);
        const u8 op = rd.b();
        if (!rd.ok) return false;

        struct M { u8 mod{}, reg{}, rm{}; bool mem{}; bool riprel{}; i32 rip_disp{}; } m;
        auto parse_modrm = [&]() {
            const u8 b = rd.b();
            m.mod = b >> 6; m.reg = static_cast<u8>(((b >> 3) & 7) | ((rex & 4) ? 8 : 0));
            const u8 low = b & 7;
            if (m.mod == 3) { m.rm = static_cast<u8>(low | ((rex & 1) ? 8 : 0)); m.mem = false; return; }
            m.mem = true; e.base = 0xFF; e.index = 0xFF; e.scale = 0; e.disp = 0;
            if (low == 4) {
                const u8 sib = rd.b();
                e.scale = sib >> 6;
                const unsigned idx = ((sib >> 3) & 7) | ((rex & 2) ? 8 : 0), bs = (sib & 7) | ((rex & 1) ? 8 : 0);
                if ((sib & 7) == 5 && m.mod == 0) e.disp = static_cast<i32>(rd.d()); else e.base = static_cast<u8>(bs);
                if (idx != 4) e.index = static_cast<u8>(idx);
            } else if (low == 5 && m.mod == 0) { m.riprel = true; m.rip_disp = static_cast<i32>(rd.d()); }
            else e.base = static_cast<u8>(low | ((rex & 1) ? 8 : 0));
            if (m.mod == 1) e.disp += static_cast<i8>(rd.b()); else if (m.mod == 2) e.disp += static_cast<i32>(rd.d());
            e.seg = static_cast<u8>(seg);
        };
        // a byte-register operand 4..7 without REX means AH/CH/DH/BH, which only the legacy path models
        auto hi8 = [&](unsigned idx) { return !has_rex && idx >= 4 && idx < 8; };
        auto finish = [&](GuestCpu::Insn::Handler fn) {
            if (!rd.ok) return false;
            e.next = pc0 + rd.i;
            if (m.riprel) { e.disp = static_cast<i64>(e.next) + m.rip_disp; e.base = 0xFF; e.index = 0xFF; }
            e.fn = fn; return true;
        };
        e.aux = 0; e.imm = 0; e.seg = 0; e.base = 0xFF; e.index = 0xFF;

        // ---------------- one-byte opcodes ----------------
        if (op < 0x40 && (op & 7) < 6) { // ALU block
            const unsigned aop = (op >> 3) & 7, form = op & 7;
            e.op = static_cast<u8>(aop); e.aux = aop == 7 ? 1 : 0;
            if (form >= 4) { // accumulator, immediate
                const unsigned sz = form == 4 ? 1 : osz;
                if (sz == 1 && !has_rex) {} // AL is index 0: fine
                const i64 imm = form == 4 ? static_cast<i64>(rd.b()) : (sz == 2 ? static_cast<i64>(rd.w()) : static_cast<i64>(static_cast<i32>(rd.d())));
                e.dst = 0; e.imm = imm; return finish(SBO_SZ(alu_ri, sz));
            }
            const unsigned sz = (form & 1) ? osz : 1;
            parse_modrm();
            if (!rd.ok) return false;
            if (sz == 1 && (hi8(m.reg) || (!m.mem && hi8(m.rm)))) return false;
            if (form < 2) { // r/m, reg
                e.src = m.reg;
                if (m.mem) return finish(SBO_SZ(alu_mr, sz));
                e.dst = m.rm; return finish(SBO_SZ(alu_rr, sz));
            }
            e.dst = m.reg; // reg, r/m
            if (m.mem) return finish(SBO_SZ(alu_rm, sz));
            e.src = m.rm; return finish(SBO_SZ(alu_rr, sz));
        }
        switch (op) {
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
            if (p66) return false;
            e.src = static_cast<u8>((op - 0x50) | ((rex & 1) ? 8 : 0)); return finish(&push_r);
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            if (p66) return false;
            e.dst = static_cast<u8>((op - 0x58) | ((rex & 1) ? 8 : 0)); return finish(&pop_r);
        case 0x63: { // movsxd r64, r/m32
            if (osz != 8) return false;
            parse_modrm(); if (!rd.ok) return false; e.dst = m.reg;
            if (m.mem) return finish(&movx<8, 4, true, true>);
            e.src = m.rm; return finish(&movx<8, 4, true, false>);
        }
        case 0x68: if (p66) return false; e.imm = static_cast<i32>(rd.d()); return finish(&push_imm);
        case 0x6A: if (p66) return false; e.imm = static_cast<i8>(rd.b()); return finish(&push_imm);
        case 0x69: case 0x6B: {
            if (osz == 2) return false;
            parse_modrm(); if (!rd.ok) return false;
            e.imm = op == 0x6B ? static_cast<i64>(static_cast<i8>(rd.b())) : static_cast<i64>(static_cast<i32>(rd.d()));
            e.dst = m.reg;
            if (m.mem) return finish(osz == 8 ? &imul3<8, true> : &imul3<4, true>);
            e.src = m.rm; return finish(osz == 8 ? &imul3<8, false> : &imul3<4, false>);
        }
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            if (p66) return false;
            const i8 d = static_cast<i8>(rd.b()); if (!rd.ok) return false;
            e.cc = op & 0xF; e.target = pc0 + rd.i + static_cast<i64>(d); return finish(&jcc);
        }
        case 0x80: case 0x81: case 0x83: {
            const unsigned sz = op == 0x80 ? 1 : osz;
            parse_modrm(); if (!rd.ok) return false;
            const unsigned aop = m.reg & 7;
            if (sz == 1 && !m.mem && hi8(m.rm)) return false;
            e.imm = op == 0x81 ? (sz == 2 ? static_cast<i64>(rd.w()) : static_cast<i64>(static_cast<i32>(rd.d()))) : static_cast<i64>(static_cast<i8>(rd.b()));
            e.op = static_cast<u8>(aop); e.aux = aop == 7 ? 1 : 0;
            if (m.mem) return finish(SBO_SZ(alu_mi, sz));
            e.dst = m.rm; return finish(SBO_SZ(alu_ri, sz));
        }
        case 0x84: case 0x85: {
            const unsigned sz = op == 0x84 ? 1 : osz;
            parse_modrm(); if (!rd.ok) return false;
            if (sz == 1 && (hi8(m.reg) || (!m.mem && hi8(m.rm)))) return false;
            e.op = 4; e.aux = 1; e.src = m.reg;
            if (m.mem) return finish(SBO_SZ(alu_mr, sz));
            e.dst = m.rm; return finish(SBO_SZ(alu_rr, sz));
        }
        case 0x88: case 0x89: case 0x8A: case 0x8B: {
            const unsigned sz = (op & 1) ? osz : 1;
            parse_modrm(); if (!rd.ok) return false;
            if (sz == 1 && (hi8(m.reg) || (!m.mem && hi8(m.rm)))) return false;
            if (op < 0x8A) { // mov r/m, reg
                e.src = m.reg;
                if (m.mem) return finish(SBO_SZ(mov_store, sz));
                e.dst = m.rm; return finish(SBO_SZ(mov_rr, sz));
            }
            e.dst = m.reg;
            if (m.mem) return finish(SBO_SZ(mov_load, sz));
            e.src = m.rm; return finish(SBO_SZ(mov_rr, sz));
        }
        case 0x8D: { // lea
            if (osz == 2) return false;
            parse_modrm(); if (!rd.ok || !m.mem) return false;
            e.seg = 0; e.dst = m.reg; return finish(osz == 8 ? &lea<8> : &lea<4>);
        }
        case 0x90: if (rex & 1) return false; return finish(&h_nop);
        case 0x98: if (p66) return false; return finish((rex & 8) ? &cdqe : &cwde);
        case 0x99: if (p66) return false; return finish((rex & 8) ? &cqo : &cdq);
        case 0xA8: case 0xA9: {
            const unsigned sz = op == 0xA8 ? 1 : osz;
            e.imm = sz == 1 ? static_cast<i64>(rd.b()) : (sz == 2 ? static_cast<i64>(rd.w()) : static_cast<i64>(static_cast<i32>(rd.d())));
            e.dst = 0; e.op = 4; e.aux = 1; return finish(SBO_SZ(alu_ri, sz));
        }
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
            const unsigned idx = (op - 0xB0) | ((rex & 1) ? 8 : 0);
            if (hi8(idx)) return false;
            e.dst = static_cast<u8>(idx); e.imm = rd.b(); return finish(&mov_ri<1>);
        }
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            e.dst = static_cast<u8>((op - 0xB8) | ((rex & 1) ? 8 : 0));
            if (osz == 8) { e.imm = static_cast<i64>(rd.q()); return finish(&mov_ri<8>); }
            if (osz == 4) { e.imm = static_cast<i64>(rd.d()); return finish(&mov_ri<4>); }
            e.imm = rd.w(); return finish(&mov_ri<2>);
        }
        case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
            const unsigned sz = (op & 1) ? osz : 1;
            parse_modrm(); if (!rd.ok) return false;
            if (sz == 1 && !m.mem && hi8(m.rm)) return false;
            e.op = m.reg & 7;
            if (op == 0xC0 || op == 0xC1) { e.imm = rd.b(); e.aux = 0; } else if (op == 0xD0 || op == 0xD1) { e.imm = 1; e.aux = 0; } else e.aux = 2;
            if (m.mem) return finish(SBO_SZ(shift_m, sz));
            e.dst = m.rm; return finish(SBO_SZ(shift_r, sz));
        }
        case 0xC3: if (p66) return false; return finish(&ret);
        case 0xC6: case 0xC7: {
            const unsigned sz = op == 0xC6 ? 1 : osz;
            parse_modrm(); if (!rd.ok) return false;
            if ((m.reg & 7) != 0) return false;
            if (sz == 1 && !m.mem && hi8(m.rm)) return false;
            e.imm = sz == 1 ? static_cast<i64>(rd.b()) : (sz == 2 ? static_cast<i64>(rd.w()) : static_cast<i64>(static_cast<i32>(rd.d())));
            if (m.mem) return finish(SBO_SZ(mov_store_imm, sz));
            e.dst = m.rm; return finish(SBO_SZ(mov_ri, sz));
        }
        case 0xE8: { if (p66) return false; const i32 d = static_cast<i32>(rd.d()); if (!rd.ok) return false; e.target = pc0 + rd.i + static_cast<i64>(d); return finish(&call); }
        case 0xE9: { if (p66) return false; const i32 d = static_cast<i32>(rd.d()); if (!rd.ok) return false; e.target = pc0 + rd.i + static_cast<i64>(d); return finish(&jmp); }
        case 0xEB: { if (p66) return false; const i8 d = static_cast<i8>(rd.b()); if (!rd.ok) return false; e.target = pc0 + rd.i + static_cast<i64>(d); return finish(&jmp); }
        case 0xF6: case 0xF7: {
            const unsigned sz = op == 0xF6 ? 1 : osz;
            parse_modrm(); if (!rd.ok) return false;
            const unsigned sub = m.reg & 7;
            if (sz == 1 && !m.mem && hi8(m.rm)) return false;
            if (sub == 0 || sub == 1) { // test r/m, imm
                e.imm = sz == 1 ? static_cast<i64>(rd.b()) : (sz == 2 ? static_cast<i64>(rd.w()) : static_cast<i64>(static_cast<i32>(rd.d())));
                e.op = 4; e.aux = 1;
                if (m.mem) return finish(SBO_SZ(alu_mi, sz));
                e.dst = m.rm; return finish(SBO_SZ(alu_ri, sz));
            }
            if (sub == 2) { if (m.mem) return finish(SBO_SZ(not_m, sz)); e.dst = m.rm; return finish(SBO_SZ(not_r, sz)); }
            if (sub == 3) { if (m.mem) return finish(SBO_SZ(neg_m, sz)); e.dst = m.rm; return finish(SBO_SZ(neg_r, sz)); }
            return false; // mul/imul/div/idiv: legacy
        }
        case 0xFE: case 0xFF: {
            const unsigned sz = op == 0xFE ? 1 : osz;
            parse_modrm(); if (!rd.ok) return false;
            const unsigned sub = m.reg & 7;
            if (sub <= 1) {
                if (sz == 1 && !m.mem && hi8(m.rm)) return false;
                e.op = sub == 0 ? 0 : 5;
                if (m.mem) return finish(SBO_SZ(incdec_m, sz));
                e.dst = m.rm; return finish(SBO_SZ(incdec_r, sz));
            }
            if (op == 0xFF && !p66 && (sub == 2 || sub == 4)) {
                if (m.mem) return finish(sub == 2 ? &call_ind_m : &jmp_ind_m);
                e.src = m.rm; return finish(sub == 2 ? &call_ind_r : &jmp_ind_r);
            }
            return false;
        }
        case 0x0F: break;
        default: return false;
        }

        // ---------------- two-byte (0F xx) opcodes ----------------
        const u8 op2 = rd.b(); if (!rd.ok) return false;
        switch (op2) {
        case 0x1F: { parse_modrm(); return finish(&h_nop); }
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            parse_modrm(); if (!rd.ok) return false;
            e.cc = op2 & 0xF; e.dst = m.reg;
            if (osz == 2) return false;
            if (m.mem) return finish(osz == 8 ? &cmov<8, true> : &cmov<4, true>);
            e.src = m.rm; return finish(osz == 8 ? &cmov<8, false> : &cmov<4, false>);
        }
        case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
            if (p66) return false;
            const i32 d = static_cast<i32>(rd.d()); if (!rd.ok) return false;
            e.cc = op2 & 0xF; e.target = pc0 + rd.i + static_cast<i64>(d); return finish(&jcc);
        }
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
            parse_modrm(); if (!rd.ok) return false;
            e.cc = op2 & 0xF;
            if (m.mem) return finish(&setcc_m);
            if (hi8(m.rm)) return false;
            e.dst = m.rm; return finish(&setcc_r);
        }
        case 0xAF: {
            if (osz == 2) return false;
            parse_modrm(); if (!rd.ok) return false; e.dst = m.reg;
            if (m.mem) return finish(osz == 8 ? &imul2<8, true> : &imul2<4, true>);
            e.src = m.rm; return finish(osz == 8 ? &imul2<8, false> : &imul2<4, false>);
        }
        case 0xB6: case 0xB7: case 0xBE: case 0xBF: {
            if (osz == 2) return false;
            const bool byte_src = op2 == 0xB6 || op2 == 0xBE, sgn = op2 >= 0xBE;
            parse_modrm(); if (!rd.ok) return false; e.dst = m.reg;
            if (byte_src && !m.mem && hi8(m.rm)) return false;
            if (m.mem) {
                if (byte_src) return finish(osz == 8 ? (sgn ? &movx<8, 1, true, true> : &movx<8, 1, false, true>) : (sgn ? &movx<4, 1, true, true> : &movx<4, 1, false, true>));
                return finish(osz == 8 ? (sgn ? &movx<8, 2, true, true> : &movx<8, 2, false, true>) : (sgn ? &movx<4, 2, true, true> : &movx<4, 2, false, true>));
            }
            e.src = m.rm;
            if (byte_src) return finish(osz == 8 ? (sgn ? &movx<8, 1, true, false> : &movx<8, 1, false, false>) : (sgn ? &movx<4, 1, true, false> : &movx<4, 1, false, false>));
            return finish(osz == 8 ? (sgn ? &movx<8, 2, true, false> : &movx<8, 2, false, false>) : (sgn ? &movx<4, 2, true, false> : &movx<4, 2, false, false>));
        }
        default: return false;
        }
    }
#undef SBO_SZ
};

// ------------- GuestCpu integration -------------
inline void GuestCpu::fast_decode(Insn& e, u64 pc0) {
    e.tag = pc0; e.next = pc0;
    if (!FastExec::decode(*this, e, pc0)) { e.fn = &FastExec::h_legacy; e.next = pc0; }
}

inline std::uint64_t GuestCpu::run_fast(u64 max_instructions, u64 stop_at) {
    if (dcache_.empty()) { dcache_.assign(dcache_entries_, Insn{}); epoch_seen_ = mem.code_epoch(); }
    const u64 mask_ = dcache_entries_ - 1;
    u64 n = 0;
    try {
        while (n < max_instructions && stop == Stop::None) {
            if (rip == stop_at) return n;
            if (rip >= trap_lo && rip < trap_hi && trap_handler) {
                trap_handler(*this);
                ++n; ++instructions;
                continue;
            }
            if (mem.code_epoch() != epoch_seen_) { for (auto& d : dcache_) d.tag = ~0ULL; epoch_seen_ = mem.code_epoch(); }
            Insn& e = dcache_[rip & mask_];
            if (e.tag != rip) fast_decode(e, rip);
            insn_rip_ = rip;
            rip = e.next;
            e.fn(*this, e);
            ++instructions; ++n;
        }
    } catch (const GuestFault& f) {
        last_fault = f; fault_rip = insn_rip_; rip = insn_rip_;
        request_stop(Stop::MemoryFault,
            std::string(f.fetch || f.address == insn_rip_ ? "instruction fetch" : (f.write ? "write" : "read")) + " of unmapped guest address 0x" + hex(f.address));
    } catch (const GuestUnsupported& u) {
        fault_rip = insn_rip_; rip = insn_rip_; request_stop(Stop::Unsupported, u.what);
    } catch (const DivideFault&) {
        fault_rip = insn_rip_; rip = insn_rip_; request_stop(Stop::DivideError, "#DE divide error");
    }
    return n;
}

} // namespace someboxone
