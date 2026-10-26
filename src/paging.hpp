#pragma once

#include "machine.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace someboxone {

enum class X64AccessType { Read, Write, Execute };
enum class X64PrivilegeLevel { Supervisor, User };

enum class X64PageFault {
    None,
    NonCanonicalAddress,
    PageNotPresent,
    ReservedEntry,
    PageTableMemoryUnavailable,
    UserAccessDenied,
    WriteAccessDenied,
    ExecuteAccessDenied,
    InvalidConfiguration
};

inline const char* x64_page_fault_name(X64PageFault fault) {
    switch (fault) {
    case X64PageFault::None: return "None";
    case X64PageFault::NonCanonicalAddress: return "NonCanonicalAddress";
    case X64PageFault::PageNotPresent: return "PageNotPresent";
    case X64PageFault::ReservedEntry: return "ReservedEntry";
    case X64PageFault::PageTableMemoryUnavailable: return "PageTableMemoryUnavailable";
    case X64PageFault::UserAccessDenied: return "UserAccessDenied";
    case X64PageFault::WriteAccessDenied: return "WriteAccessDenied";
    case X64PageFault::ExecuteAccessDenied: return "ExecuteAccessDenied";
    case X64PageFault::InvalidConfiguration: return "InvalidConfiguration";
    }
    return "UnknownPageFault";
}

struct X64TranslationResult {
    bool success{};
    std::uint64_t virtual_address{};
    std::uint64_t physical_address{};
    std::uint64_t page_size{};
    std::uint8_t walk_level{};
    bool effective_user{};
    bool effective_writable{};
    bool effective_executable{};
    // Architectural fault metadata. Non-canonical addresses model #GP(0);
    // page-translation failures model #PF with the error code below.
    bool has_page_fault{};
    std::uint32_t page_fault_error_code{};
    int exception_vector{-1};
    bool accessed_bits_updated{};
    bool dirty_bit_updated{};
    X64PageFault fault{X64PageFault::None};
    std::string detail;
};

// A testable four-level x86-64 page walker for synthetic guest memory. It
// models 4 KiB, 2 MiB and 1 GiB pages, canonical addresses, U/S, R/W, NX,
// Accessed bits, and the leaf Dirty bit. It reports architectural #PF error
// codes, but the CPU scaffold records rather than delivers exceptions.
class X64PageTableWalker {
public:
    struct Configuration {
        std::uint8_t physical_address_bits{52};
        bool nx_enabled{true};
        bool supervisor_write_protect{true};
    };

    explicit X64PageTableWalker(GuestAddressSpace& physical_memory)
        : memory_(physical_memory) {}

    X64PageTableWalker(GuestAddressSpace& physical_memory, Configuration configuration)
        : memory_(physical_memory), configuration_(configuration) {}

    X64TranslationResult translate(std::uint64_t virtual_address,
                                   std::uint64_t cr3,
                                   X64AccessType access = X64AccessType::Read,
                                   X64PrivilegeLevel privilege = X64PrivilegeLevel::Supervisor) const {
        X64TranslationResult result;
        result.virtual_address = virtual_address;
        if (configuration_.physical_address_bits < 32 || configuration_.physical_address_bits > 52) {
            return fail(result, X64PageFault::InvalidConfiguration, 0,
                        "Physical address width must be between 32 and 52 bits.", access, privilege);
        }
        if (!is_canonical(virtual_address)) {
            // In 64-bit mode a non-canonical linear address produces #GP(0),
            // not a page fault. Keep it separate from #PF error-code reporting.
            return fail(result, X64PageFault::NonCanonicalAddress, 0,
                        "Virtual address is not canonical for 48-bit long-mode addressing.", access, privilege);
        }

        const std::uint64_t physical_mask = ((1ULL << configuration_.physical_address_bits) - 1ULL) & ~0xFFFULL;
        const std::uint64_t cr3_address_field = cr3 & architectural_address_field_mask;
        if ((cr3_address_field & ~physical_mask) != 0) {
            auto invalid_cr3 = fail(result, X64PageFault::InvalidConfiguration, 4,
                        "CR3 contains a page-table base address outside the configured physical width.", access, privilege);
            invalid_cr3.exception_vector = 13; // #GP(0) on an invalid control-register value.
            return invalid_cr3;
        }

        std::uint64_t table_base = cr3 & physical_mask;
        bool writable = true;
        bool user_accessible = true;
        bool executable = true;

        for (int level = 4; level >= 1; --level) {
            const unsigned shift = 12U + 9U * static_cast<unsigned>(level - 1);
            const std::uint64_t index = (virtual_address >> shift) & 0x1FFULL;
            const std::uint64_t entry_address = table_base + index * 8ULL;
            const auto entry_opt = memory_.read(entry_address, 8);
            if (!entry_opt) {
                return fail(result, X64PageFault::PageTableMemoryUnavailable,
                            static_cast<std::uint8_t>(level),
                            "Page-table entry could not be read from guest physical memory.", access, privilege);
            }
            std::uint64_t entry = *entry_opt;
            result.walk_level = static_cast<std::uint8_t>(level);
            if ((entry & present_bit) == 0) {
                return fail(result, X64PageFault::PageNotPresent,
                            static_cast<std::uint8_t>(level), "Present bit is clear.", access, privilege);
            }

            // x86 hardware sets Accessed in every present paging-structure
            // entry used by the walk, including walks that later fault on
            // permissions. The synthetic memory model updates it explicitly.
            if ((entry & accessed_bit) == 0) {
                if (!memory_.write(entry_address, 8, entry | accessed_bit)) {
                    return fail(result, X64PageFault::PageTableMemoryUnavailable,
                                static_cast<std::uint8_t>(level),
                                "Could not update Accessed bit in guest page-table memory.", access, privilege);
                }
                entry |= accessed_bit;
                result.accessed_bits_updated = true;
            }

            const std::uint64_t address_field = entry & architectural_address_field_mask;
            if ((address_field & ~physical_mask) != 0 || (!configuration_.nx_enabled && (entry & nx_bit) != 0)) {
                return fail(result, X64PageFault::ReservedEntry,
                            static_cast<std::uint8_t>(level),
                            "Entry contains an address bit outside the configured physical width or NX while NX is disabled.", access, privilege);
            }

            writable = writable && ((entry & writable_bit) != 0);
            user_accessible = user_accessible && ((entry & user_bit) != 0);
            if (configuration_.nx_enabled && (entry & nx_bit) != 0) executable = false;

            const bool large_page = (entry & page_size_bit) != 0;
            if (level == 4 && large_page) {
                return fail(result, X64PageFault::ReservedEntry, 4,
                            "Page-size bit is reserved in a PML4 entry.", access, privilege);
            }
            if ((level == 3 || level == 2) && large_page) {
                const std::uint64_t page_size = (level == 3) ? (1ULL << 30U) : (1ULL << 21U);
                const std::uint64_t reserved_alignment_mask = ((page_size - 1ULL) & ~0x1FFFULL);
                if ((entry & reserved_alignment_mask) != 0) {
                    return fail(result, X64PageFault::ReservedEntry,
                                static_cast<std::uint8_t>(level),
                                "Large-page entry has non-zero reserved alignment bits.", access, privilege);
                }
                result.page_size = page_size;
                const std::uint64_t base = entry & physical_mask & ~(page_size - 1ULL);
                result.physical_address = base | (virtual_address & (page_size - 1ULL));
                result.effective_user = user_accessible;
                result.effective_writable = writable;
                result.effective_executable = executable;
                return finalize_leaf(result, entry_address, entry, access, privilege);
            }

            if (level == 1) {
                result.page_size = 1ULL << 12U;
                result.physical_address = (entry & physical_mask) | (virtual_address & 0xFFFULL);
                result.effective_user = user_accessible;
                result.effective_writable = writable;
                result.effective_executable = executable;
                return finalize_leaf(result, entry_address, entry, access, privilege);
            }
            table_base = entry & physical_mask;
        }
        return fail(result, X64PageFault::ReservedEntry, 0,
                    "Page-table walk ended without a leaf entry.", access, privilege);
    }

private:
    static constexpr std::uint64_t present_bit = 1ULL << 0U;
    static constexpr std::uint64_t writable_bit = 1ULL << 1U;
    static constexpr std::uint64_t user_bit = 1ULL << 2U;
    static constexpr std::uint64_t accessed_bit = 1ULL << 5U;
    static constexpr std::uint64_t dirty_bit = 1ULL << 6U;
    static constexpr std::uint64_t page_size_bit = 1ULL << 7U;
    static constexpr std::uint64_t nx_bit = 1ULL << 63U;
    static constexpr std::uint64_t architectural_address_field_mask = 0x000FFFFFFFFFF000ULL;

    GuestAddressSpace& memory_;
    Configuration configuration_{};

    static bool is_canonical(std::uint64_t address) {
        const bool sign = ((address >> 47U) & 1ULL) != 0;
        const std::uint64_t upper = address >> 48U;
        return sign ? upper == 0xFFFFULL : upper == 0;
    }

    static X64TranslationResult fail(X64TranslationResult result, X64PageFault fault,
                                     std::uint8_t level, std::string detail,
                                     X64AccessType access, X64PrivilegeLevel privilege) {
        result.success = false;
        result.fault = fault;
        result.walk_level = level;
        result.detail = std::move(detail);
        result.has_page_fault = false;
        result.page_fault_error_code = 0;
        result.exception_vector = -1;

        // Page-fault error code bits: P=0, W/R=1, U/S=2, RSVD=3, I/D=4.
        switch (fault) {
        case X64PageFault::PageNotPresent:
        case X64PageFault::ReservedEntry:
        case X64PageFault::UserAccessDenied:
        case X64PageFault::WriteAccessDenied:
        case X64PageFault::ExecuteAccessDenied: {
            result.has_page_fault = true;
            result.exception_vector = 14;
            std::uint32_t code = 0;
            if (fault != X64PageFault::PageNotPresent) code |= 1U; // protection/reserved-bit violation
            if (access == X64AccessType::Write) code |= 1U << 1U;
            if (privilege == X64PrivilegeLevel::User) code |= 1U << 2U;
            if (fault == X64PageFault::ReservedEntry) code |= 1U << 3U;
            if (access == X64AccessType::Execute) code |= 1U << 4U;
            result.page_fault_error_code = code;
            break;
        }
        case X64PageFault::NonCanonicalAddress:
            result.exception_vector = 13; // #GP(0), not #PF
            break;
        case X64PageFault::PageTableMemoryUnavailable:
        case X64PageFault::InvalidConfiguration:
        case X64PageFault::None:
            break; // Emulator/configuration failure, not an architectural guest exception.
        }
        return result;
    }

    X64TranslationResult finalize_leaf(X64TranslationResult result,
                                       std::uint64_t entry_address,
                                       std::uint64_t entry,
                                       X64AccessType access,
                                       X64PrivilegeLevel privilege) const {
        result = enforce_permissions(std::move(result), access, privilege,
                                     configuration_.supervisor_write_protect);
        if (!result.success) return result;

        if (access == X64AccessType::Write && (entry & dirty_bit) == 0) {
            if (!memory_.write(entry_address, 8, entry | dirty_bit)) {
                const auto level = result.walk_level;
                return fail(std::move(result), X64PageFault::PageTableMemoryUnavailable,
                            level, "Could not update Dirty bit in leaf entry.", access, privilege);
            }
            result.dirty_bit_updated = true;
        }
        return result;
    }

    static X64TranslationResult enforce_permissions(X64TranslationResult result,
                                                    X64AccessType access,
                                                    X64PrivilegeLevel privilege,
                                                    bool supervisor_write_protect) {
        if (privilege == X64PrivilegeLevel::User && !result.effective_user) {
            const auto level = result.walk_level;
            return fail(std::move(result), X64PageFault::UserAccessDenied,
                        level, "At least one paging level is supervisor-only.", access, privilege);
        }
        const bool enforce_write = privilege == X64PrivilegeLevel::User || supervisor_write_protect;
        if (access == X64AccessType::Write && !result.effective_writable && enforce_write) {
            const auto level = result.walk_level;
            return fail(std::move(result), X64PageFault::WriteAccessDenied,
                        level, "Write access is denied by the effective R/W permissions.", access, privilege);
        }
        if (access == X64AccessType::Execute && !result.effective_executable) {
            const auto level = result.walk_level;
            return fail(std::move(result), X64PageFault::ExecuteAccessDenied,
                        level, "NX is set on at least one paging level.", access, privilege);
        }
        result.success = true;
        result.fault = X64PageFault::None;
        result.detail = "Translation succeeded.";
        result.exception_vector = -1;
        result.has_page_fault = false;
        result.page_fault_error_code = 0;
        return result;
    }
};

} // namespace someboxone
