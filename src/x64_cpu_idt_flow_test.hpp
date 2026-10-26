#pragma once

#include "x64_bootstrap.hpp"
#include "x64_idt.hpp"
#include "inspector.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace someboxone {

struct X64CpuIdtFlowResult {
    bool passed{};
    int exception_vector{-1};
    std::uint32_t page_fault_error_code{};
    std::uint64_t cr2{};
    std::uint64_t handler_rip{};
    std::uint64_t faulting_instruction_rip{};
    std::uint64_t original_rsp{};
    // RSP at handler entry: it points at the error-code slot.
    std::uint64_t exception_frame_rsp{};
    std::uint64_t final_rsp{};
    std::uint64_t resumed_value{};
    X64CpuSubset::RunResult cpu_run_result{X64CpuSubset::RunResult::Fault};
    std::string detail;
};

inline void append_u64_le(std::vector<std::uint8_t>& code, std::uint64_t value) {
    for (unsigned int i = 0; i < 8; ++i) {
        code.push_back(static_cast<std::uint8_t>((value >> (i * 8U)) & 0xFFU));
    }
}

inline void append_movabs(std::vector<std::uint8_t>& code, std::uint8_t opcode_register,
                          std::uint64_t value) {
    code.push_back(0x48); // REX.W
    code.push_back(opcode_register); // B8+rd; REX.B is deliberately not used here.
    append_u64_le(code, value);
}

// Integration harness for opt-in runtime #PF delivery and same-CPL return.
// Generated guest code faults on a deliberately absent page. The ordinary run
// loop dispatches vector 14 to the configured IDT. The synthetic handler repairs
// the test page-table entry, writes the requested test value, restores registers,
// removes the pushed error code, and executes IRETQ. Guest execution then retries
// the faulting load and reaches HLT. This is not a general x86 exception subsystem.
inline X64CpuIdtFlowResult run_synthetic_cpu_idt_flow_test() {
    X64CpuIdtFlowResult result;
    GuestAddressSpace memory(1024 * 1024);
    if (!memory.map_ram(0, 1024 * 1024, "synthetic CPU-to-IDT flow RAM")) {
        result.detail = "Unable to map synthetic guest RAM.";
        return result;
    }

    // PML4 -> PDPT -> PD. The low 256 KiB is identity-mapped through PT 0x4000.
    // VA 0x400000 maps to code at PA 0x10000; VA 0x500000 has a missing PTE at 0x5800.
    const bool tables_ok =
        memory.write(0x1000, 8, 0x2007) &&
        memory.write(0x2000, 8, 0x3007) &&
        memory.write(0x3000, 8, 0x4007) &&
        memory.write(0x3010, 8, 0x5007);
    if (!tables_ok) {
        result.detail = "Unable to create synthetic page-table root.";
        return result;
    }
    for (std::uint64_t page = 0; page < 64; ++page) {
        if (!memory.write(0x4000 + page * 8ULL, 8, page * 0x1000ULL | 0x7ULL)) {
            result.detail = "Unable to populate low identity-mapped page table.";
            return result;
        }
    }
    if (!memory.write(0x5000, 8, 0x10007)) {
        result.detail = "Unable to map the synthetic guest code page.";
        return result;
    }

    // mov rax, 0x19000 ; lidt [rax] ; mov rax, 0x500000 ;
    // mov rbx, [rax] ; hlt. Guest code now loads its own IDTR before #PF.
    std::vector<std::uint8_t> program{
        0x48, 0xB8, 0x00, 0x90, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0F, 0x01, 0x18,
        0x48, 0xB8, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x8B, 0x18,
        0xF4
    };
    for (std::size_t i = 0; i < program.size(); ++i) {
        if (!memory.write(0x10000 + static_cast<std::uint64_t>(i), 1, program[i])) {
            result.detail = "Unable to write synthetic CPU program.";
            return result;
        }
    }

    constexpr std::uint64_t handler_address = 0x9000;
    std::vector<std::uint8_t> handler;
    handler.push_back(0x50); // push rax
    handler.push_back(0x53); // push rbx
    append_movabs(handler, 0xB8, 0x5800);  // rax = address of absent PTE (identity-mapped)
    append_movabs(handler, 0xBB, 0x11007); // rbx = present, writable, user page -> PA 0x11000
    handler.insert(handler.end(), {0x48, 0x89, 0x18}); // mov [rax], rbx
    append_movabs(handler, 0xB8, 0x11000); // rax = newly mapped test page
    append_movabs(handler, 0xBB, 0x2A);    // rbx = 42
    handler.insert(handler.end(), {0x48, 0x89, 0x18}); // mov [rax], rbx
    handler.push_back(0x5B); // pop rbx
    handler.push_back(0x58); // pop rax
    handler.insert(handler.end(), {0x48, 0x83, 0xC4, 0x08}); // add rsp, 8 (discard #PF error code)
    handler.insert(handler.end(), {0x48, 0xCF}); // iretq (same-CPL return)
    for (std::size_t i = 0; i < handler.size(); ++i) {
        if (!memory.write(handler_address + static_cast<std::uint64_t>(i), 1, handler[i])) {
            result.detail = "Unable to write synthetic page-fault handler.";
            return result;
        }
    }

    X64ControlState control;
    if (!control.write_cr4(X64ControlState::cr4_pae) ||
        !control.write_efer(X64ControlState::efer_lme | X64ControlState::efer_nxe) ||
        !control.write_cr3(0x1000) ||
        !control.write_cr0(X64ControlState::cr0_pe | X64ControlState::cr0_pg |
                           X64ControlState::cr0_wp | (1ULL << 4U)) ||
        !control.long_mode_active() || !control.set_cpl(0)) {
        result.detail = "Unable to activate synthetic long mode: " + control.last_error();
        return result;
    }

    constexpr std::uint64_t idt_base = 0x18000;
    constexpr std::uint8_t page_fault_vector = 14;
    std::array<std::uint8_t, 16> gate{};
    gate[0] = static_cast<std::uint8_t>(handler_address & 0xFFU);
    gate[1] = static_cast<std::uint8_t>((handler_address >> 8U) & 0xFFU);
    gate[2] = 0x08;
    gate[3] = 0x00;
    gate[4] = 0x00; // IST disabled.
    gate[5] = 0x8E; // Present 64-bit interrupt gate.
    gate[6] = static_cast<std::uint8_t>((handler_address >> 16U) & 0xFFU);
    gate[7] = static_cast<std::uint8_t>((handler_address >> 24U) & 0xFFU);
    for (unsigned int i = 0; i < 4; ++i) {
        gate[8 + i] = static_cast<std::uint8_t>((handler_address >> (32U + i * 8U)) & 0xFFU);
    }
    const std::uint64_t gate_address =
        idt_base + static_cast<std::uint64_t>(page_fault_vector) * 16ULL;
    for (std::size_t i = 0; i < gate.size(); ++i) {
        if (!memory.write(gate_address + static_cast<std::uint64_t>(i), 1, gate[i])) {
            result.detail = "Unable to write the synthetic page-fault IDT gate.";
            return result;
        }
    }
    // The 10-byte long-mode pseudo-descriptor consumed by guest LIDT.
    if (!memory.write(0x19000, 2, 0x0FFF) ||
        !memory.write(0x19002, 8, idt_base)) {
        result.detail = "Unable to write the synthetic guest IDTR pseudo-descriptor.";
        return result;
    }

    constexpr std::uint64_t guest_code_base = 0x400000;
    constexpr std::uint64_t faulting_instruction_rip = guest_code_base + 23ULL;
    constexpr std::uint64_t initial_rsp = 0x1F000;
    result.faulting_instruction_rip = faulting_instruction_rip;
    result.original_rsp = initial_rsp;

    X64CpuSubset cpu(memory, guest_code_base, control);
    cpu.state.registers[4] = initial_rsp;
    // Do not configure IDTR through the host API: the guest program must execute
    // LIDT and the CPU must use the descriptor it loaded itself.
    result.cpu_run_result = cpu.run(128);
    result.exception_vector = cpu.state.last_exception_vector;
    result.page_fault_error_code = cpu.state.last_page_fault_error_code;
    result.cr2 = cpu.state.cr2;
    result.handler_rip = handler_address;
    result.exception_frame_rsp = initial_rsp - 32ULL;
    result.final_rsp = cpu.state.registers[4];
    result.resumed_value = cpu.state.registers[3]; // RBX receives the retried memory load.

    const std::uint64_t frame_rsp = initial_rsp - 32ULL;
    const auto saved_error = memory.read(frame_rsp, 8);
    const auto saved_rip = memory.read(frame_rsp + 8, 8);
    const auto saved_cs = memory.read(frame_rsp + 16, 8);
    const auto saved_flags = memory.read(frame_rsp + 24, 8);
    const bool frame_ok =
        saved_error && *saved_error == cpu.state.last_page_fault_error_code &&
        saved_rip && *saved_rip == faulting_instruction_rip &&
        saved_cs && *saved_cs == 0x08ULL &&
        saved_flags && *saved_flags == 0x2ULL;

    const auto loaded_idtr = cpu.guest_idtr();
    const bool idtr_ok = loaded_idtr && loaded_idtr->base == idt_base &&
        loaded_idtr->limit == 0x0FFF && cpu.automatic_page_fault_delivery_enabled();
    const bool execution_ok =
        idtr_ok &&
        result.cpu_run_result == X64CpuSubset::RunResult::Halted && cpu.state.halted &&
        cpu.state.fault.empty() && cpu.state.last_fault_was_page_fault &&
        result.exception_vector == static_cast<int>(page_fault_vector) &&
        result.page_fault_error_code == 0 && result.cr2 == 0x500000ULL &&
        cpu.state.rip == guest_code_base + program.size() && frame_ok &&
        result.exception_frame_rsp == frame_rsp && result.final_rsp == initial_rsp &&
        cpu.state.registers[0] == 0x500000ULL && result.resumed_value == 42ULL;

    result.passed = execution_ok;
    result.detail = result.passed
        ? "Synthetic guest executed LIDT to load its own IDTR, raised #PF during normal execution, automatically delivered vector 14 through the same-CPL IDT, verified the exception frame, repaired the missing test PTE, returned with IRETQ, retried the faulting load (value=42), restored RSP, and reached HLT. Privilege transitions, other exception vectors, and full architectural return checks remain unsupported."
        : "Synthetic #PF/IDT/IRETQ round-trip failed validation; vector=" +
            std::to_string(result.exception_vector) + "; CR2=" + hex_value(result.cr2) +
            "; final RSP=" + hex_value(result.final_rsp) +
            "; loaded value=" + hex_value(result.resumed_value) +
            "; halted=" + (cpu.state.halted ? "true" : "false") +
            "; fault=" + cpu.state.fault;
    return result;
}

} // namespace someboxone
