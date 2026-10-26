#pragma once

// SomeBoxOne v0.22.0 - sparse, flat, 64-bit guest virtual memory used by the
// user-mode title runner (PE loader + x64 interpreter + HLE).
//
// This is intentionally separate from GuestAddressSpace (machine.hpp), which models
// bounded *physical* RAM/MMIO for the firmware-research path. Titles are user-mode
// code that expects a private virtual address space, so this layer is demand-sparse:
// 4 KiB pages are allocated only for ranges that were explicitly mapped.

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace someboxone {

struct GuestFault {
    std::uint64_t address{};
    bool write{};
    bool fetch{};
};

class GuestMemory {
public:
    static constexpr std::uint64_t page_size = 0x1000;
    static constexpr std::uint64_t page_mask = page_size - 1;

    struct Mapping {
        std::uint64_t base{};
        std::uint64_t size{};
        std::string name;
    };

    explicit GuestMemory(std::uint64_t budget_bytes = 1536ULL * 1024ULL * 1024ULL)
        : budget_(budget_bytes) {
        tlb_page_.fill(~0ULL);
        tlb_ptr_.fill(nullptr);
    }

    // Map zero-filled pages. Existing pages are kept (so overlapping maps are tolerated),
    // but the call fails if the sparse-memory byte budget would be exceeded.
    bool map(std::uint64_t base, std::uint64_t size, const std::string& name) {
        if (size == 0) return false;
        const std::uint64_t first = base >> 12;
        const std::uint64_t last = (base + size - 1) >> 12;
        if (last < first) return false;
        std::uint64_t fresh = 0;
        for (std::uint64_t p = first; p <= last; ++p) if (!pages_.count(p)) ++fresh;
        if (used_ + fresh * page_size > budget_) return false;
        for (std::uint64_t p = first; p <= last; ++p) {
            if (pages_.count(p)) continue;
            auto page = std::make_unique<Page>();
            std::memset(page->data(), 0, page_size);
            pages_.emplace(p, std::move(page));
        }
        used_ += fresh * page_size;
        mappings_.push_back({base, size, name});
        return true;
    }

    // Releases whole pages in [base, base+size) (VirtualFree MEM_RELEASE/MEM_DECOMMIT). Returns the number of pages freed.
    std::uint64_t unmap(std::uint64_t base, std::uint64_t size) {
        if (size == 0) return 0;
        std::uint64_t freed = 0;
        const std::uint64_t first = base >> 12, last = (base + size - 1) >> 12;
        for (std::uint64_t p = first; p <= last; ++p) { if (pages_.erase(p)) ++freed; }
        used_ -= (std::min)(used_, freed * page_size);
        tlb_page_.fill(~0ULL); tlb_ptr_.fill(nullptr); tlb_code_.fill(0);
        for (std::uint64_t p = first; p <= last; ++p) code_pages_.erase(p);
        ++code_epoch_;
        return freed;
    }

    bool is_mapped(std::uint64_t address, std::uint64_t size = 1) const {
        if (size == 0) return true;
        const std::uint64_t first = address >> 12;
        const std::uint64_t last = (address + size - 1) >> 12;
        for (std::uint64_t p = first; p <= last; ++p) if (!pages_.count(p)) return false;
        return true;
    }

    std::uint64_t used_bytes() const { return used_; }
    const std::vector<Mapping>& mappings() const { return mappings_; }

    // Little-endian scalar access. Throws GuestFault on unmapped pages.
    template <typename T>
    T read(std::uint64_t address) const {
        T value;
        const std::uint64_t off = address & page_mask;
        if (off + sizeof(T) <= page_size) {
            std::memcpy(&value, page_for(address, false, false) + off, sizeof(T));
        } else {
            read_bytes_slow(address, &value, sizeof(T));
        }
        return value;
    }

    template <typename T>
    void write(std::uint64_t address, T value) {
        const std::uint64_t off = address & page_mask;
        if (off + sizeof(T) <= page_size) {
            std::uint8_t* const pg = page_for(address, true, false);
            if (tlb_code_[static_cast<std::size_t>((address >> 12) & (tlb_size - 1))]) note_code_write(address >> 12);
            std::memcpy(pg + off, &value, sizeof(T));
        } else {
            write_bytes_slow(address, &value, sizeof(T));
        }
    }

    std::uint64_t read_sized(std::uint64_t address, unsigned size) const {
        switch (size) {
        case 1: return read<std::uint8_t>(address);
        case 2: return read<std::uint16_t>(address);
        case 4: return read<std::uint32_t>(address);
        default: return read<std::uint64_t>(address);
        }
    }
    void write_sized(std::uint64_t address, unsigned size, std::uint64_t v) {
        switch (size) {
        case 1: write<std::uint8_t>(address, static_cast<std::uint8_t>(v)); break;
        case 2: write<std::uint16_t>(address, static_cast<std::uint16_t>(v)); break;
        case 4: write<std::uint32_t>(address, static_cast<std::uint32_t>(v)); break;
        default: write<std::uint64_t>(address, v); break;
        }
    }

    void read_bytes(std::uint64_t address, void* out, std::size_t n) const { read_bytes_slow(address, out, n); }
    void write_bytes(std::uint64_t address, const void* in, std::size_t n) { write_bytes_slow(address, in, n); }

    // Self-modifying-code guard for the CPU's decoded-instruction cache: pages whose bytes were decoded are flagged; a write to one
    // bumps code_epoch() so the CPU discards its cache. unmap() bumps it too.
    std::uint64_t code_epoch() const { return code_epoch_; }
    void mark_code_page(std::uint64_t page_number) {
        if (!code_pages_.insert(page_number).second) return;
        const std::size_t slot = static_cast<std::size_t>(page_number & (tlb_size - 1));
        if (tlb_page_[slot] == page_number) tlb_code_[slot] = 1;
    }

    // Direct pointer to the byte at `address` inside its page (valid up to the page end). Throws GuestFault if unmapped.
    const std::uint8_t* code_ptr(std::uint64_t address) const { return page_for(address, false, true) + (address & page_mask); }

    // Instruction fetch (kept separate only so faults can be labelled as fetches).
    std::uint8_t fetch8(std::uint64_t address) const {
        return page_for(address, false, true)[address & page_mask];
    }

    // Host-side helpers for HLE.
    std::string read_cstring(std::uint64_t address, std::size_t max = 4096) const {
        std::string s;
        for (std::size_t i = 0; i < max; ++i) {
            if (!is_mapped(address + i)) break;
            const char c = static_cast<char>(read<std::uint8_t>(address + i));
            if (!c) break;
            s.push_back(c);
        }
        return s;
    }
    std::u16string read_wstring(std::uint64_t address, std::size_t max = 4096) const {
        std::u16string s;
        for (std::size_t i = 0; i < max; ++i) {
            if (!is_mapped(address + i * 2, 2)) break;
            const auto c = read<std::uint16_t>(address + i * 2);
            if (!c) break;
            s.push_back(static_cast<char16_t>(c));
        }
        return s;
    }

private:
    using Page = std::array<std::uint8_t, page_size>;

    std::uint8_t* page_for(std::uint64_t address, bool write, bool fetch) const {
        const std::uint64_t pn = address >> 12;
        const std::size_t slot = static_cast<std::size_t>(pn & (tlb_size - 1));
        if (tlb_page_[slot] == pn) return tlb_ptr_[slot];
        const auto it = pages_.find(pn);
        if (it == pages_.end()) throw GuestFault{address, write, fetch};
        tlb_page_[slot] = pn;
        tlb_ptr_[slot] = it->second->data();
        tlb_code_[slot] = code_pages_.empty() ? 0 : static_cast<std::uint8_t>(code_pages_.count(pn));
        return tlb_ptr_[slot];
    }

    void read_bytes_slow(std::uint64_t address, void* out, std::size_t n) const {
        auto* dst = static_cast<std::uint8_t*>(out);
        while (n) {
            const std::uint64_t off = address & page_mask;
            const std::size_t chunk = static_cast<std::size_t>((std::min)(static_cast<std::uint64_t>(n), page_size - off));
            std::memcpy(dst, page_for(address, false, false) + off, chunk);
            address += chunk; dst += chunk; n -= chunk;
        }
    }
    void write_bytes_slow(std::uint64_t address, const void* in, std::size_t n) {
        const auto* src = static_cast<const std::uint8_t*>(in);
        while (n) {
            const std::uint64_t off = address & page_mask;
            const std::size_t chunk = static_cast<std::size_t>((std::min)(static_cast<std::uint64_t>(n), page_size - off));
            std::uint8_t* const pg = page_for(address, true, false);
            if (tlb_code_[static_cast<std::size_t>((address >> 12) & (tlb_size - 1))]) note_code_write(address >> 12);
            std::memcpy(pg + off, src, chunk);
            address += chunk; src += chunk; n -= chunk;
        }
    }

    void note_code_write(std::uint64_t page_number) {
        ++code_epoch_;
        code_pages_.erase(page_number);
        const std::size_t slot = static_cast<std::size_t>(page_number & (tlb_size - 1));
        if (tlb_page_[slot] == page_number) tlb_code_[slot] = 0;
    }

    static constexpr std::size_t tlb_size = 1024;
    std::unordered_map<std::uint64_t, std::unique_ptr<Page>> pages_;
    mutable std::array<std::uint64_t, tlb_size> tlb_page_{};
    mutable std::array<std::uint8_t*, tlb_size> tlb_ptr_{};
    mutable std::array<std::uint8_t, tlb_size> tlb_code_{};   // 1 = this TLB entry's page holds cached decoded instructions
    std::unordered_set<std::uint64_t> code_pages_;
    std::uint64_t code_epoch_{0};
    std::vector<Mapping> mappings_;
    std::uint64_t budget_;
    std::uint64_t used_{};
};

} // namespace someboxone
