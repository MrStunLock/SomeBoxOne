#include "x64_cpu_idt_flow_test.hpp"

#include <iostream>
#include <string>
#include <vector>

static bool test_lidt_rejects_cpl3() {
    using namespace someboxone;
    GuestAddressSpace memory(1024 * 1024);
    if (!memory.map_ram(0, 1024 * 1024, "synthetic LIDT privilege test RAM")) return false;

    if (!memory.write(0x1000, 8, 0x2007) ||
        !memory.write(0x2000, 8, 0x3007) ||
        !memory.write(0x3000, 8, 0x4007) ||
        !memory.write(0x3010, 8, 0x5007)) return false;

    for (std::uint64_t page = 0; page < 64; ++page) {
        if (!memory.write(0x4000 + page * 8ULL, 8, page * 0x1000ULL | 0x7ULL)) return false;
    }
    if (!memory.write(0x5000, 8, 0x10007)) return false;

    const std::vector<std::uint8_t> code{
        0x48, 0xB8, 0x00, 0x90, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rax, 0x19000
        0x0F, 0x01, 0x18,                                           // lidt [rax]
        0xF4                                                        // hlt (must not execute)
    };
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (!memory.write(0x10000 + static_cast<std::uint64_t>(i), 1, code[i])) return false;
    }

    X64ControlState control;
    if (!control.write_cr4(X64ControlState::cr4_pae) ||
        !control.write_efer(X64ControlState::efer_lme | X64ControlState::efer_nxe) ||
        !control.write_cr3(0x1000) ||
        !control.write_cr0(X64ControlState::cr0_pe | X64ControlState::cr0_pg |
                           X64ControlState::cr0_wp | (1ULL << 4U)) ||
        !control.long_mode_active() || !control.set_cpl(3)) return false;

    X64CpuSubset cpu(memory, 0x400000, control);
    const auto run_result = cpu.run(32);
    return run_result == X64CpuSubset::RunResult::Fault &&
        !cpu.state.halted &&
        cpu.state.fault.find("privileged") != std::string::npos &&
        !cpu.automatic_page_fault_delivery_enabled() &&
        !cpu.guest_idtr().has_value();
}

int main() {
    const auto result = someboxone::run_synthetic_cpu_idt_flow_test();
    if (!result.passed) {
        std::cerr << "SomeBoxOne CPU-to-IDT flow test failed: " << result.detail << '\n';
        return 1;
    }
    if (result.exception_vector != 14 || result.page_fault_error_code != 0 ||
        result.cpu_run_result != someboxone::X64CpuSubset::RunResult::Halted ||
        result.detail.empty() ||
        result.cr2 != 0x500000ULL ||
        result.faulting_instruction_rip != 0x400017ULL ||
        result.handler_rip != 0x9000ULL ||
        result.original_rsp - result.exception_frame_rsp != 32ULL ||
        result.final_rsp != result.original_rsp ||
        result.resumed_value != 42ULL) {
        std::cerr << "Unexpected CPU-to-IDT integration metadata\n";
        return 1;
    }
    if (!test_lidt_rejects_cpl3()) {
        std::cerr << "LIDT must be rejected at CPL 3 without installing an IDTR.\n";
        return 1;
    }
    std::cout << "SomeBoxOne synthetic CPU-to-IDT flow test passed.\n"
              << result.detail << '\n'
              << "CPL-3 LIDT privilege rejection passed.\n";
    return 0;
}
