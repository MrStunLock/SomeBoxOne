#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace someboxone {

// Minimal validated x86-64 control-register/MSR state for synthetic tests.
// This models only the bits currently needed to enter long mode and configure
// the existing four-level page walker. It is not a full CPU control subsystem.
class X64ControlState {
public:
    static constexpr std::uint64_t cr0_pe = 1ULL << 0U;
    static constexpr std::uint64_t cr0_wp = 1ULL << 16U;
    static constexpr std::uint64_t cr0_pg = 1ULL << 31U;
    static constexpr std::uint64_t cr4_pae = 1ULL << 5U;
    static constexpr std::uint64_t cr4_pse = 1ULL << 4U;
    static constexpr std::uint64_t cr4_pge = 1ULL << 7U;
    static constexpr std::uint64_t efer_sce = 1ULL << 0U;
    static constexpr std::uint64_t efer_lme = 1ULL << 8U;
    static constexpr std::uint64_t efer_lma = 1ULL << 10U; // read-only, derived
    static constexpr std::uint64_t efer_nxe = 1ULL << 11U;

    X64ControlState() = default;

    bool write_cr0(std::uint64_t value) {
        constexpr std::uint64_t supported = (1ULL << 0U) | (1ULL << 1U) | (1ULL << 2U) |
            (1ULL << 3U) | (1ULL << 4U) | (1ULL << 5U) | (1ULL << 16U) |
            (1ULL << 18U) | (1ULL << 29U) | (1ULL << 30U) | (1ULL << 31U);
        if ((value & ~supported) != 0) return fail("CR0 contains unsupported/reserved bits.");
        if ((value & cr0_pg) != 0 && (value & cr0_pe) == 0)
            return fail("CR0.PG cannot be enabled while CR0.PE is clear.");
        if ((value & cr0_pg) != 0 && (efer_ & efer_lme) != 0 && (cr4_ & cr4_pae) == 0)
            return fail("Long-mode paging requires CR4.PAE before CR0.PG is enabled.");
        cr0_ = value;
        update_lma();
        last_error_.clear();
        return true;
    }

    bool write_cr3(std::uint64_t value) {
        // PCID is not modelled: only the page-table base and PWT/PCD are accepted.
        constexpr std::uint64_t address_mask = 0x000FFFFFFFFFF000ULL;
        constexpr std::uint64_t low_flags = (1ULL << 3U) | (1ULL << 4U);
        if ((value & ~(address_mask | low_flags)) != 0)
            return fail("CR3 contains unsupported/reserved bits (PCID is not implemented).");
        cr3_ = value;
        last_error_.clear();
        return true;
    }

    bool write_cr4(std::uint64_t value) {
        constexpr std::uint64_t supported = cr4_pse | cr4_pae | cr4_pge | (1ULL << 9U) | (1ULL << 10U);
        if ((value & ~supported) != 0)
            return fail("CR4 contains a feature bit not implemented by this model (for example LA57/PCID/SMEP/SMAP).");
        if (long_mode_active() && (value & cr4_pae) == 0)
            return fail("CR4.PAE cannot be cleared while long mode is active.");
        cr4_ = value;
        update_lma();
        last_error_.clear();
        return true;
    }

    bool write_efer(std::uint64_t value) {
        constexpr std::uint64_t writable = efer_sce | efer_lme | efer_nxe;
        if ((value & ~writable) != 0)
            return fail("EFER contains reserved bits or attempts to write read-only LMA.");
        if (long_mode_active() && ((value ^ efer_) & efer_lme) != 0)
            return fail("EFER.LME cannot be changed while EFER.LMA is active.");
        if ((value & efer_lme) != 0 && paging_enabled() && (cr4_ & cr4_pae) == 0)
            return fail("EFER.LME cannot activate long mode while paging is enabled without CR4.PAE.");
        efer_ = (efer_ & efer_lma) | value;
        update_lma();
        last_error_.clear();
        return true;
    }

    bool paging_enabled() const { return (cr0_ & cr0_pg) != 0; }
    bool write_protect_enabled() const { return (cr0_ & cr0_wp) != 0; }
    bool nx_enabled() const { return (efer_ & efer_nxe) != 0; }
    bool long_mode_active() const { return (efer_ & efer_lma) != 0; }
    std::uint64_t cr0() const { return cr0_; }
    std::uint64_t cr3() const { return cr3_; }
    std::uint64_t cr4() const { return cr4_; }
    std::uint64_t efer() const { return efer_; }
    const std::string& last_error() const { return last_error_; }

    bool set_cpl(std::uint8_t value) {
        if (value > 3U) return fail("CPL must be in the range 0..3.");
        cpl_ = value;
        last_error_.clear();
        return true;
    }
    std::uint8_t cpl() const { return cpl_; }

private:
    std::uint64_t cr0_{0x60000010ULL}; // Reset-style ET/CD/NW bits for this model.
    std::uint64_t cr3_{};
    std::uint64_t cr4_{};
    std::uint64_t efer_{};
    std::uint8_t cpl_{};
    std::string last_error_;

    bool fail(std::string message) {
        last_error_ = std::move(message);
        return false;
    }

    void update_lma() {
        const bool active = (efer_ & efer_lme) != 0 && paging_enabled() &&
            (cr0_ & cr0_pe) != 0 && (cr4_ & cr4_pae) != 0;
        if (active) efer_ |= efer_lma;
        else efer_ &= ~efer_lma;
    }
};

} // namespace someboxone
