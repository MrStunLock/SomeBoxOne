#include "paging.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>

namespace {
bool put64(someboxone::GuestAddressSpace& memory, std::uint64_t address, std::uint64_t value) {
    return memory.write(address, 8, value);
}
}

int main() {
    using namespace someboxone;
    GuestAddressSpace memory(1U << 20U);
    if (!memory.map_ram(0, 1U << 20U, "page table and target RAM")) {
        std::cerr << "RAM map failed\n";
        return 1;
    }

    // Four-level mapping: VA 0x401234 -> PA 0x12234. All levels initially
    // permit user read/write and execute. Entries are generated test data.
    if (!put64(memory, 0x1000, 0x2007) ||   // PML4[0] -> PDPT
        !put64(memory, 0x2000, 0x3007) ||   // PDPT[0] -> PD
        !put64(memory, 0x3010, 0x4007) ||   // PD[2] -> PT
        !put64(memory, 0x4008, 0x12007)) {  // PT[1] -> physical page 0x12000
        std::cerr << "Could not create synthetic page tables\n";
        return 1;
    }

    X64PageTableWalker walker(memory);
    auto translated = walker.translate(0x401234, 0x1000, X64AccessType::Read, X64PrivilegeLevel::User);
    if (!translated.success || translated.physical_address != 0x12234 || translated.page_size != 4096) {
        std::cerr << "4 KiB translation failed: " << x64_page_fault_name(translated.fault)
                  << " - " << translated.detail << '\n';
        return 1;
    }
    // The walk updates Accessed bits in all present paging levels.
    for (const std::uint64_t entry_address : {0x1000ULL, 0x2000ULL, 0x3010ULL, 0x4008ULL}) {
        const auto entry = memory.read(entry_address, 8);
        if (!entry || ((*entry & (1ULL << 5U)) == 0)) {
            std::cerr << "Accessed bit was not set at entry " << entry_address << '\n';
            return 1;
        }
    }
    if (!translated.accessed_bits_updated || translated.has_page_fault || translated.exception_vector != -1) {
        std::cerr << "Successful translation returned incorrect fault metadata\n";
        return 1;
    }
    translated = walker.translate(0x401234, 0x1000, X64AccessType::Write, X64PrivilegeLevel::User);
    if (!translated.success || !translated.dirty_bit_updated) {
        std::cerr << "Writable user translation did not set the Dirty bit\n";
        return 1;
    }
    const auto dirty_leaf = memory.read(0x4008, 8);
    if (!dirty_leaf || ((*dirty_leaf & (1ULL << 6U)) == 0)) {
        std::cerr << "Leaf Dirty bit was not written back\n";
        return 1;
    }

    // With CR0.WP conceptually clear, supervisor writes to a read-only page are
    // allowed by this configurable model; user writes remain denied.
    if (!put64(memory, 0x4008, 0x12005)) return 1;
    const X64PageTableWalker supervisor_wp_off(memory, X64PageTableWalker::Configuration{52, true, false});
    translated = supervisor_wp_off.translate(0x401234, 0x1000, X64AccessType::Write, X64PrivilegeLevel::Supervisor);
    if (!translated.success) {
        std::cerr << "Supervisor write-protect configuration was not respected\n";
        return 1;
    }
    translated = supervisor_wp_off.translate(0x401234, 0x1000, X64AccessType::Write, X64PrivilegeLevel::User);
    if (translated.success || translated.fault != X64PageFault::WriteAccessDenied ||
        translated.page_fault_error_code != 0x7U || translated.exception_vector != 14) {
        std::cerr << "User write error code is incorrect when supervisor WP was disabled\n";
        return 1;
    }

    // NX is aggregated from every paging level.
    if (!put64(memory, 0x4008, 0x12007 | (1ULL << 63U))) return 1;
    translated = walker.translate(0x401234, 0x1000, X64AccessType::Execute, X64PrivilegeLevel::User);
    if (translated.success || translated.fault != X64PageFault::ExecuteAccessDenied ||
        !translated.has_page_fault || translated.exception_vector != 14 ||
        translated.page_fault_error_code != 0x15U) {
        std::cerr << "NX execute protection or #PF error-code reporting failed; code="
                  << translated.page_fault_error_code << '\n';
        return 1;
    }
    translated = walker.translate(0x401234, 0x1000, X64AccessType::Read, X64PrivilegeLevel::User);
    if (!translated.success) {
        std::cerr << "NX page should remain readable\n";
        return 1;
    }

    // Read-only mappings and supervisor-only mappings must block user writes/access.
    if (!put64(memory, 0x4008, 0x12005)) return 1;
    translated = walker.translate(0x401234, 0x1000, X64AccessType::Write, X64PrivilegeLevel::User);
    if (translated.success || translated.fault != X64PageFault::WriteAccessDenied) {
        std::cerr << "Read-only page write was not blocked\n";
        return 1;
    }
    if (!put64(memory, 0x4008, 0x12003)) return 1; // present+writable, supervisor-only
    translated = walker.translate(0x401234, 0x1000, X64AccessType::Read, X64PrivilegeLevel::User);
    if (translated.success || translated.fault != X64PageFault::UserAccessDenied ||
        translated.page_fault_error_code != 0x5U || translated.exception_vector != 14) {
        std::cerr << "Supervisor-only user read has incorrect #PF error code\n";
        return 1;
    }

    if (!put64(memory, 0x4008, 0)) return 1;
    translated = walker.translate(0x401234, 0x1000, X64AccessType::Read, X64PrivilegeLevel::User);
    if (translated.success || translated.fault != X64PageFault::PageNotPresent ||
        translated.page_fault_error_code != 0x4U || translated.exception_vector != 14) {
        std::cerr << "Not-present PTE or non-present #PF error code was not detected\n";
        return 1;
    }

    translated = walker.translate(0x0000800000000000ULL, 0x1000);
    if (translated.success || translated.fault != X64PageFault::NonCanonicalAddress ||
        translated.has_page_fault || translated.exception_vector != 13) {
        std::cerr << "Non-canonical address was not reported as #GP(0)\n";
        return 1;
    }

    const X64PageTableWalker narrow_physical(memory, X64PageTableWalker::Configuration{36, true, true});
    translated = narrow_physical.translate(0x401234, (1ULL << 40U) | 0x1000ULL);
    if (translated.success || translated.fault != X64PageFault::InvalidConfiguration ||
        translated.has_page_fault || translated.exception_vector != 13) {
        std::cerr << "Invalid CR3 address was not reported as a control-state #GP\n";
        return 1;
    }

    // 2 MiB leaf in PD[4]: 0x00801234 -> 0x00801234.
    if (!put64(memory, 0x3020, 0x00800000ULL | 0x87ULL)) return 1;
    translated = walker.translate(0x00801234, 0x1000, X64AccessType::Read, X64PrivilegeLevel::Supervisor);
    if (!translated.success || translated.page_size != (1ULL << 21U) ||
        translated.physical_address != 0x00801234) {
        std::cerr << "2 MiB page translation failed: " << x64_page_fault_name(translated.fault) << '\n';
        return 1;
    }

    // 1 GiB leaf in PDPT[1]: 0x40001234 -> 0x40001234.
    if (!put64(memory, 0x2008, 0x40000000ULL | 0x87ULL)) return 1;
    translated = walker.translate(0x40001234, 0x1000, X64AccessType::Read, X64PrivilegeLevel::Supervisor);
    if (!translated.success || translated.page_size != (1ULL << 30U) ||
        translated.physical_address != 0x40001234) {
        std::cerr << "1 GiB page translation failed: " << x64_page_fault_name(translated.fault)
                  << " - " << translated.detail << '\n';
        return 1;
    }

    // Large-page reserved alignment bits are rejected rather than masked away.
    if (!put64(memory, 0x2008, 0x40000000ULL | 0x87ULL | (1ULL << 13U))) return 1;
    translated = walker.translate(0x40001234, 0x1000, X64AccessType::Read, X64PrivilegeLevel::Supervisor);
    if (translated.success || translated.fault != X64PageFault::ReservedEntry) {
        std::cerr << "Reserved large-page alignment bit was not rejected\n";
        return 1;
    }

    std::cout << "SomeBoxOne x86-64 paging tests passed (synthetic page tables only).\n";
    return 0;
}
