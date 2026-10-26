#pragma once

// SomeBoxOne v0.22.0 - PE32+ (x64) image parser and loader for Xbox One titles.
//
// Xbox One titles are ordinary AMD64 PE images (here: "Sonic Mania.exe" from the
// package's Mount folder). This file parses headers bounds-checked, maps sections
// into GuestMemory at the preferred base (applying base relocations if a different
// base is requested), and enumerates imports and TLS so the HLE layer can bind them.

#include "guest_mem.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace someboxone {

struct PeSection {
    std::string name;
    std::uint32_t virtual_address{};
    std::uint32_t virtual_size{};
    std::uint32_t raw_offset{};
    std::uint32_t raw_size{};
    std::uint32_t characteristics{};
};

struct PeImport {
    std::string dll;
    std::string name;          // empty when imported by ordinal
    std::uint16_t ordinal{};
    bool by_ordinal{};
    std::uint32_t iat_rva{};   // RVA of the 8-byte IAT slot
};

struct PeTls {
    bool present{};
    std::uint64_t raw_start_va{}, raw_end_va{}, index_va{}, callbacks_va{};
    std::uint32_t zero_fill{};
};

class PeImage {
public:
    std::string error;
    std::uint16_t machine{};
    std::uint16_t subsystem{};
    std::uint16_t characteristics{};
    std::uint64_t image_base{};
    std::uint32_t entry_rva{};
    std::uint32_t size_of_image{};
    std::uint32_t size_of_headers{};
    std::uint64_t stack_reserve{}, stack_commit{};
    std::uint32_t checksum{};
    std::vector<PeSection> sections;
    std::vector<PeImport> imports;
    std::vector<std::string> import_dlls;
    PeTls tls;
    std::map<std::string, std::uint32_t> exports_by_name;   // function RVAs (forwarders excluded)
    std::map<std::uint16_t, std::uint32_t> exports_by_ordinal;
    std::string module_name;
    std::uint32_t exception_rva{}, exception_size{};
    std::uint32_t reloc_rva{}, reloc_size{};
    std::vector<std::uint8_t> file;

    static std::string path_utf8(const std::filesystem::path& p) {
        const auto u = p.u8string();
        return std::string(u.begin(), u.end());
    }
    static std::optional<PeImage> load_file(const std::filesystem::path& path, std::string* err) {
        std::ifstream in(path, std::ios::binary);
        if (!in) { if (err) *err = "cannot open " + path_utf8(path); return std::nullopt; }
        in.seekg(0, std::ios::end);
        const auto size = static_cast<std::uint64_t>(in.tellg());
        in.seekg(0);
        if (size < 0x200 || size > (1ULL << 31)) { if (err) *err = "implausible PE file size"; return std::nullopt; }
        PeImage img;
        img.file.resize(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(img.file.data()), static_cast<std::streamsize>(size));
        if (!img.parse()) { if (err) *err = img.error; return std::nullopt; }
        return img;
    }

    bool parse() {
        const auto& f = file;
        if (f.size() < 0x40 || f[0] != 'M' || f[1] != 'Z') return fail("missing MZ header");
        const std::uint32_t pe = u32(0x3C);
        if (!in_file(pe, 24) || std::memcmp(&f[pe], "PE\0\0", 4) != 0) return fail("missing PE signature");
        machine = u16(pe + 4);
        if (machine != 0x8664) return fail("not an AMD64 PE image (machine=0x" + hex(machine) + ")");
        const std::uint16_t nsec = u16(pe + 6);
        const std::uint16_t opt_size = u16(pe + 20);
        characteristics = u16(pe + 22);
        const std::uint32_t opt = pe + 24;
        if (!in_file(opt, opt_size) || opt_size < 112 || u16(opt) != 0x20B) return fail("not a PE32+ optional header");
        entry_rva = u32(opt + 16);
        image_base = u64v(opt + 24);
        size_of_image = u32(opt + 56);
        size_of_headers = u32(opt + 60);
        checksum = u32(opt + 64);
        subsystem = u16(opt + 68);
        stack_reserve = u64v(opt + 72);
        stack_commit = u64v(opt + 80);
        const std::uint32_t ndirs = u32(opt + 108);
        if (size_of_image == 0 || size_of_image > 0x40000000u) return fail("implausible SizeOfImage");
        auto dir = [&](unsigned i, std::uint32_t& rva, std::uint32_t& sz) {
            rva = sz = 0;
            if (i < ndirs && 112 + (i + 1) * 8 <= opt_size) { rva = u32(opt + 112 + i * 8); sz = u32(opt + 116 + i * 8); }
        };
        std::uint32_t imp_rva, imp_sz, tls_rva, tls_sz;
        std::uint32_t exp_rva, exp_sz; dir(0, exp_rva, exp_sz);
        dir(1, imp_rva, imp_sz); dir(3, exception_rva, exception_size); dir(5, reloc_rva, reloc_size); dir(9, tls_rva, tls_sz);
        const std::uint32_t sec_off = opt + opt_size;
        if (!in_file(sec_off, static_cast<std::uint64_t>(nsec) * 40)) return fail("section table out of file");
        for (unsigned i = 0; i < nsec; ++i) {
            const std::uint32_t o = sec_off + i * 40;
            PeSection s;
            s.name.assign(reinterpret_cast<const char*>(&f[o]), strnlen(reinterpret_cast<const char*>(&f[o]), 8));
            s.virtual_size = u32(o + 8); s.virtual_address = u32(o + 12);
            s.raw_size = u32(o + 16); s.raw_offset = u32(o + 20); s.characteristics = u32(o + 36);
            if (s.raw_size && !in_file(s.raw_offset, s.raw_size)) return fail("section " + s.name + " raw data out of file");
            if (static_cast<std::uint64_t>(s.virtual_address) + (std::max)(s.virtual_size, s.raw_size) > size_of_image) return fail("section " + s.name + " exceeds SizeOfImage");
            sections.push_back(s);
        }
        if (exp_rva) parse_exports(exp_rva, exp_sz);
        if (imp_rva && !parse_imports(imp_rva)) return false;
        if (tls_rva) parse_tls(tls_rva);
        return true;
    }

    // Maps headers and sections at `base` and applies relocations if base != image_base.
    bool map_into(GuestMemory& mem, std::uint64_t base, std::string* err) const {
        if (!mem.map(base, size_of_image, "image")) { if (err) *err = "cannot map image (memory budget)"; return false; }
        mem.write_bytes(base, file.data(), (std::min<std::size_t>)(size_of_headers, file.size()));
        for (const auto& s : sections) {
            const std::uint32_t n = (std::min)(s.raw_size, s.virtual_size ? s.virtual_size : s.raw_size);
            if (n) mem.write_bytes(base + s.virtual_address, &file[s.raw_offset], n);
        }
        if (base != image_base && reloc_rva) {
            const std::int64_t delta = static_cast<std::int64_t>(base - image_base);
            std::uint32_t off = 0;
            while (off + 8 <= reloc_size) {
                const std::uint32_t page = mem.read<std::uint32_t>(base + reloc_rva + off);
                const std::uint32_t blk = mem.read<std::uint32_t>(base + reloc_rva + off + 4);
                if (blk < 8 || off + blk > reloc_size) break;
                for (std::uint32_t i = 8; i + 2 <= blk; i += 2) {
                    const std::uint16_t e = mem.read<std::uint16_t>(base + reloc_rva + off + i);
                    if ((e >> 12) == 10) { // IMAGE_REL_BASED_DIR64
                        const std::uint64_t a = base + page + (e & 0xFFF);
                        mem.write<std::uint64_t>(a, mem.read<std::uint64_t>(a) + static_cast<std::uint64_t>(delta));
                    }
                }
                off += blk;
            }
        }
        return true;
    }

    const PeSection* section_for_rva(std::uint32_t rva) const {
        for (const auto& s : sections) if (rva >= s.virtual_address && rva < s.virtual_address + (std::max)(s.virtual_size, s.raw_size)) return &s;
        return nullptr;
    }

    // Static analysis helper: file offset for an RVA (nullopt if not file-backed).
    std::optional<std::uint32_t> rva_to_offset(std::uint32_t rva) const {
        const auto* s = section_for_rva(rva);
        if (!s || rva - s->virtual_address >= s->raw_size) return std::nullopt;
        return s->raw_offset + (rva - s->virtual_address);
    }

private:
    bool fail(const std::string& m) { error = m; return false; }
    static std::string hex(std::uint64_t v) { char b[20]; std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(v)); return b; }
    bool in_file(std::uint64_t off, std::uint64_t n) const { return off <= file.size() && n <= file.size() - off; }
    std::uint16_t u16(std::size_t o) const { std::uint16_t v; std::memcpy(&v, &file[o], 2); return v; }
    std::uint32_t u32(std::size_t o) const { std::uint32_t v; std::memcpy(&v, &file[o], 4); return v; }
    std::uint64_t u64v(std::size_t o) const { std::uint64_t v; std::memcpy(&v, &file[o], 8); return v; }

    std::string cstr_at_rva(std::uint32_t rva, bool* ok) const {
        const auto off = rva_to_offset(rva);
        if (!off) { *ok = false; return {}; }
        std::string s;
        for (std::size_t i = *off; i < file.size() && file[i]; ++i) { s.push_back(static_cast<char>(file[i])); if (s.size() > 512) break; }
        *ok = true;
        return s;
    }

    bool parse_imports(std::uint32_t rva) {
        for (unsigned idx = 0; idx < 256; ++idx) {
            const auto off = rva_to_offset(rva + idx * 20);
            if (!off || !in_file(*off, 20)) return fail("import descriptor out of file");
            const std::uint32_t oft = u32(*off), name = u32(*off + 12), ft = u32(*off + 16);
            if (!name) break;
            bool ok;
            const std::string dll = cstr_at_rva(name, &ok);
            if (!ok) return fail("import DLL name unreadable");
            import_dlls.push_back(dll);
            const std::uint32_t thunk = oft ? oft : ft;
            for (unsigned i = 0; i < 20000; ++i) {
                const auto t = rva_to_offset(thunk + i * 8);
                if (!t || !in_file(*t, 8)) return fail("import thunk out of file");
                const std::uint64_t v = u64v(*t);
                if (!v) break;
                PeImport imp;
                imp.dll = dll; imp.iat_rva = ft + i * 8;
                if (v >> 63) { imp.by_ordinal = true; imp.ordinal = static_cast<std::uint16_t>(v & 0xFFFF); }
                else { imp.name = cstr_at_rva(static_cast<std::uint32_t>(v & 0x7FFFFFFF) + 2, &ok); if (!ok) return fail("import name unreadable"); }
                imports.push_back(std::move(imp));
            }
        }
        return true;
    }

    void parse_exports(std::uint32_t rva, std::uint32_t size) {
        const auto off = rva_to_offset(rva);
        if (!off || !in_file(*off, 40)) return;
        bool ok = false;
        module_name = cstr_at_rva(u32(*off + 12), &ok);
        const std::uint32_t ord_base = u32(*off + 16), nfunc = u32(*off + 20), nnames = u32(*off + 24);
        const auto funcs = rva_to_offset(u32(*off + 28)), names = rva_to_offset(u32(*off + 32)), ords = rva_to_offset(u32(*off + 36));
        if (!funcs || nfunc > 100000 || nnames > 100000) return;
        auto func_rva = [&](std::uint32_t idx) -> std::uint32_t { return (idx < nfunc && in_file(*funcs + idx * 4ULL, 4)) ? u32(*funcs + idx * 4ULL) : 0; };
        auto is_fwd = [&](std::uint32_t r) { return r >= rva && r < rva + size; };
        for (std::uint32_t i = 0; i < nfunc; ++i) { const auto r = func_rva(i); if (r && !is_fwd(r)) exports_by_ordinal[static_cast<std::uint16_t>(ord_base + i)] = r; }
        if (!names || !ords) return;
        for (std::uint32_t i = 0; i < nnames; ++i) {
            if (!in_file(*names + i * 4ULL, 4) || !in_file(*ords + i * 2ULL, 2)) break;
            const auto n = cstr_at_rva(u32(*names + i * 4ULL), &ok);
            const auto r = func_rva(u16(*ords + i * 2ULL));
            if (ok && r && !is_fwd(r)) exports_by_name[n] = r;
        }
    }

    void parse_tls(std::uint32_t rva) {
        const auto off = rva_to_offset(rva);
        if (!off || !in_file(*off, 40)) return;
        tls.present = true;
        tls.raw_start_va = u64v(*off); tls.raw_end_va = u64v(*off + 8);
        tls.index_va = u64v(*off + 16); tls.callbacks_va = u64v(*off + 24);
        tls.zero_fill = u32(*off + 32);
    }
};

} // namespace someboxone
