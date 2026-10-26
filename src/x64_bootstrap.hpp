#pragma once

#include "cpu_x64.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace someboxone {

struct LongModeSmokeResult {
    bool passed{};
    std::uint64_t rax{};
    std::uint64_t rip{};
    std::uint64_t rsp{};
    std::string detail;
};

// Runs only bytes and page tables built below for testing. It validates that
// the control-register model can activate long mode and that the CPU subset
// uses CR3/EFER/CR0-derived policy. It never reads or executes user image bytes.
inline LongModeSmokeResult run_synthetic_long_mode_test() {
    LongModeSmokeResult result;
    GuestAddressSpace memory(1024 * 1024);
    if (!memory.map_ram(0, 1024 * 1024, "synthetic long-mode test RAM")) {
        result.detail = "Unable to map synthetic RAM.";
        return result;
    }

    // Four-level page tables: 0x400000 virtual region -> code/data/stack pages.
    const bool tables_ok = memory.write(0x1000, 8, 0x2007) &&
        memory.write(0x2000, 8, 0x3007) &&
        memory.write(0x3010, 8, 0x4007) &&
        memory.write(0x4000, 8, 0x10007) &&
        memory.write(0x4008, 8, 0x12007 | (1ULL << 63U)) &&
        memory.write(0x4010, 8, 0x15007 | (1ULL << 63U)) &&
        memory.write(0x4018, 8, 0x14007 | (1ULL << 63U));
    if (!tables_ok) {
        result.detail = "Unable to populate synthetic page tables.";
        return result;
    }

    const std::vector<std::uint8_t> program{
        0x48, 0xB8, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rax, 42
        0x48, 0xBA, 0xFC, 0x1F, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rdx, 0x401ffc
        0x48, 0x89, 0x02,                                           // mov [rdx], rax
        0x48, 0x8B, 0x1A,                                           // mov rbx, [rdx]
        0x48, 0x39, 0xD8,                                           // cmp rax, rbx
        0x74, 0x05,                                                 // jz past failure assignment
        0xB8, 0x01, 0x00, 0x00, 0x00,                               // failure: mov eax, 1
        0x48, 0xBC, 0xF0, 0x3F, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rsp, 0x403ff0
        0x50,                                                       // push rax
        0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rbx, 0
        0x5B,                                                       // pop rbx
        0x48, 0x39, 0xD8,                                           // cmp rax, rbx
        0xF4                                                        // synthetic harness halt
    };
    for (std::size_t i = 0; i < program.size(); ++i) {
        if (!memory.write(0x10000 + static_cast<std::uint64_t>(i), 1, program[i])) {
            result.detail = "Unable to write synthetic test program.";
            return result;
        }
    }

    X64ControlState controls;
    if (!controls.write_cr4(X64ControlState::cr4_pae)) {
        result.detail = "CR4 setup failed: " + controls.last_error();
        return result;
    }
    if (!controls.write_efer(X64ControlState::efer_lme | X64ControlState::efer_nxe)) {
        result.detail = "EFER setup failed: " + controls.last_error();
        return result;
    }
    if (!controls.write_cr3(0x1000)) {
        result.detail = "CR3 setup failed: " + controls.last_error();
        return result;
    }
    const std::uint64_t cr0 = X64ControlState::cr0_pe | X64ControlState::cr0_pg |
        X64ControlState::cr0_wp | (1ULL << 4U);
    if (!controls.write_cr0(cr0) || !controls.long_mode_active()) {
        result.detail = "Long-mode activation failed: " + controls.last_error();
        return result;
    }

    X64CpuSubset cpu(memory, 0x400000, controls);
    const auto run_result = cpu.run(128);
    result.rax = cpu.state.registers[0];
    result.rip = cpu.state.rip;
    result.rsp = cpu.state.registers[4];
    const auto low = memory.read(0x12FFC, 4);
    const auto high = memory.read(0x15000, 4);
    result.passed = run_result == X64CpuSubset::RunResult::Halted && cpu.state.fault.empty() &&
        controls.long_mode_active() && controls.paging_enabled() && controls.nx_enabled() &&
        result.rax == 42 && cpu.state.registers[3] == 42 && result.rsp == 0x403FF0 &&
        low && *low == 42 && high && *high == 0;
    if (result.passed) {
        result.detail = "Long mode active via CR0/CR3/CR4/EFER; virtual instruction fetch, cross-page data, stack and NX mappings passed using synthetic bytes.";
    } else {
        result.detail = "run_result=" + std::to_string(static_cast<int>(run_result)) +
            "; long_mode=" + std::to_string(controls.long_mode_active()) +
            "; RAX=" + std::to_string(result.rax) +
            "; fault=" + cpu.state.fault;
    }
    return result;
}


struct PageFaultSmokeResult {
    bool passed{};
    int exception_vector{-1};
    std::uint32_t error_code{};
    std::uint64_t cr2{};
    std::string detail;
};

// Runs generated instructions through synthetic long-mode page tables and
// verifies the CPU captures a non-present user data #PF (vector 14, error
// code U/S=1, P=0) and CR2. This records fault metadata; it does not deliver
// an IDT exception or execute any firmware.
inline PageFaultSmokeResult run_synthetic_page_fault_test() {
    PageFaultSmokeResult result;
    GuestAddressSpace memory(1024 * 1024);
    if (!memory.map_ram(0, 1024 * 1024, "synthetic page-fault test RAM")) {
        result.detail = "Unable to map synthetic RAM.";
        return result;
    }

    const bool tables_ok =
        memory.write(0x1000, 8, 0x2007) &&       // PML4[0] -> PDPT
        memory.write(0x2000, 8, 0x3007) &&       // PDPT[0] -> PD
        memory.write(0x3010, 8, 0x4007) &&       // PD[2] -> PT, VA 0x400000-0x5fffff
        memory.write(0x4000, 8, 0x10007);        // VA 0x400000 -> code PA 0x10000
    if (!tables_ok) {
        result.detail = "Unable to create synthetic page tables.";
        return result;
    }

    const std::vector<std::uint8_t> program{
        0x48, 0xB8, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rax, 0x500000
        0x48, 0x8B, 0x18                                             // mov rbx, [rax]; PTE is not present
    };
    for (std::size_t i = 0; i < program.size(); ++i) {
        if (!memory.write(0x10000 + static_cast<std::uint64_t>(i), 1, program[i])) {
            result.detail = "Unable to write synthetic program.";
            return result;
        }
    }

    X64ControlState controls;
    if (!controls.write_cr4(X64ControlState::cr4_pae) ||
        !controls.write_efer(X64ControlState::efer_lme | X64ControlState::efer_nxe) ||
        !controls.write_cr3(0x1000) ||
        !controls.write_cr0(X64ControlState::cr0_pe | X64ControlState::cr0_pg |
                            X64ControlState::cr0_wp | (1ULL << 4U)) ||
        !controls.long_mode_active() || !controls.set_cpl(3)) {
        result.detail = "Could not activate synthetic long-mode control state: " + controls.last_error();
        return result;
    }

    X64CpuSubset cpu(memory, 0x400000, controls);
    const auto run_result = cpu.run(16);
    result.exception_vector = cpu.state.last_exception_vector;
    result.error_code = cpu.state.last_page_fault_error_code;
    result.cr2 = cpu.state.cr2;
    result.passed = run_result == X64CpuSubset::RunResult::Fault &&
        cpu.state.last_fault_was_page_fault &&
        result.exception_vector == 14 &&
        result.error_code == 0x4U &&
        result.cr2 == 0x500000ULL &&
        cpu.state.fault.find("PageNotPresent") != std::string::npos;
    if (result.passed) {
        result.detail = "Diagnostic-only synthetic non-present user read recorded #PF vector 14, error code 0x4, CR2=0x500000; this test does not configure an IDTR or request runtime delivery.";
    } else {
        result.detail = "run_result=" + std::to_string(static_cast<int>(run_result)) +
            "; vector=" + std::to_string(result.exception_vector) +
            "; error_code=" + std::to_string(result.error_code) +
            "; CR2=" + std::to_string(result.cr2) +
            "; fault=" + cpu.state.fault;
    }
    return result;
}

} // namespace someboxone
