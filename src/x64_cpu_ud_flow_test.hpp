#pragma once

#include "cpu_x64.hpp"
#include "x64_idt.hpp"
#include "inspector.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace someboxone {

struct X64CpuUdFlowResult {
    bool passed{};
    int exception_vector{-1};
    std::uint64_t faulting_rip{};
    std::uint64_t resumed_rip{};
    std::uint64_t original_rsp{};
    std::uint64_t exception_frame_rsp{};
    std::uint64_t final_rsp{};
    std::uint64_t cr2{};
    X64CpuSubset::RunResult cpu_run_result{X64CpuSubset::RunResult::Fault};
    std::string detail;
};

inline void append_ud_test_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<std::uint8_t>((value >> (i * 8U)) & 0xFFU));
    }
}

inline void append_ud_test_movabs(std::vector<std::uint8_t>& code, std::uint8_t opcode_register,
                                  std::uint64_t value) {
    code.push_back(0x48); // REX.W
    code.push_back(opcode_register);
    append_ud_test_u64(code, value);
}

// Synthetic #UD path: guest LIDT -> UD2 (#UD/vector 6, no error code) -> IDT
// handler edits saved RIP to skip UD2 -> same-CPL IRETQ -> HLT. This deliberately
// models only #UD and #PF, not a complete architectural exception subsystem.
inline X64CpuUdFlowResult run_synthetic_cpu_ud_flow_test() {
    X64CpuUdFlowResult result;
    GuestAddressSpace memory(1024 * 1024);
    if (!memory.map_ram(0, 1024 * 1024, "synthetic CPU #UD flow RAM")) {
        result.detail = "Unable to map synthetic guest RAM.";
        return result;
    }

    const bool tables_ok = memory.write(0x1000, 8, 0x2007) &&
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

    // mov rax, 0x19000 ; lidt [rax] ; ud2 ; hlt
    const std::vector<std::uint8_t> program{
        0x48, 0xB8, 0x00, 0x90, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0F, 0x01, 0x18,
        0x0F, 0x0B,
        0xF4
    };
    for (std::size_t i = 0; i < program.size(); ++i) {
        if (!memory.write(0x10000 + static_cast<std::uint64_t>(i), 1, program[i])) {
            result.detail = "Unable to write synthetic guest #UD program.";
            return result;
        }
    }

    constexpr std::uint64_t handler_address = 0x9000;
    constexpr std::uint64_t guest_code_base = 0x400000;
    constexpr std::uint64_t faulting_rip = guest_code_base + 13ULL;
    constexpr std::uint64_t resumed_rip = guest_code_base + 15ULL;
    constexpr std::uint64_t initial_rsp = 0x1F000;
    constexpr std::uint64_t frame_rsp = initial_rsp - 24ULL;
    result.faulting_rip = faulting_rip;
    result.resumed_rip = resumed_rip;
    result.original_rsp = initial_rsp;
    result.exception_frame_rsp = frame_rsp;

    // No-error-code exception frame: [RSP]=RIP, [RSP+8]=CS, [RSP+16]=RFLAGS.
    std::vector<std::uint8_t> handler{0x50, 0x53}; // push rax; push rbx
    append_ud_test_movabs(handler, 0xB8, frame_rsp); // rax = saved RIP slot
    append_ud_test_movabs(handler, 0xBB, resumed_rip); // skip the two-byte UD2
    handler.insert(handler.end(), {0x48, 0x89, 0x18}); // mov [rax], rbx
    handler.insert(handler.end(), {0x5B, 0x58, 0x48, 0xCF}); // pop rbx; pop rax; iretq
    for (std::size_t i = 0; i < handler.size(); ++i) {
        if (!memory.write(handler_address + static_cast<std::uint64_t>(i), 1, handler[i])) {
            result.detail = "Unable to write synthetic #UD handler.";
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
    constexpr std::uint8_t vector_ud = 6;
    std::array<std::uint8_t, 16> gate{};
    gate[0] = static_cast<std::uint8_t>(handler_address & 0xFFU);
    gate[1] = static_cast<std::uint8_t>((handler_address >> 8U) & 0xFFU);
    gate[2] = 0x08;
    gate[5] = 0x8E; // Present 64-bit interrupt gate; IST disabled.
    gate[6] = static_cast<std::uint8_t>((handler_address >> 16U) & 0xFFU);
    gate[7] = static_cast<std::uint8_t>((handler_address >> 24U) & 0xFFU);
    for (unsigned i = 0; i < 4; ++i) {
        gate[8 + i] = static_cast<std::uint8_t>((handler_address >> (32U + i * 8U)) & 0xFFU);
    }
    const std::uint64_t gate_address = idt_base + static_cast<std::uint64_t>(vector_ud) * 16ULL;
    for (std::size_t i = 0; i < gate.size(); ++i) {
        if (!memory.write(gate_address + static_cast<std::uint64_t>(i), 1, gate[i])) {
            result.detail = "Unable to write synthetic #UD IDT gate.";
            return result;
        }
    }
    if (!memory.write(0x19000, 2, 0x0FFF) || !memory.write(0x19002, 8, idt_base)) {
        result.detail = "Unable to write the synthetic guest IDTR pseudo-descriptor.";
        return result;
    }

    X64CpuSubset cpu(memory, guest_code_base, control);
    cpu.state.registers[4] = initial_rsp;
    result.cpu_run_result = cpu.run(96);
    result.exception_vector = cpu.state.last_exception_vector;
    result.cr2 = cpu.state.cr2;
    result.final_rsp = cpu.state.registers[4];

    const auto saved_rip = memory.read(frame_rsp, 8);
    const auto saved_cs = memory.read(frame_rsp + 8ULL, 8);
    const auto saved_flags = memory.read(frame_rsp + 16ULL, 8);
    const auto loaded_idtr = cpu.guest_idtr();
    const bool frame_ok = saved_rip && *saved_rip == resumed_rip &&
        saved_cs && *saved_cs == 0x08ULL && saved_flags && *saved_flags == 0x2ULL;
    const bool pass = loaded_idtr && loaded_idtr->base == idt_base && loaded_idtr->limit == 0x0FFF &&
        cpu.automatic_page_fault_delivery_enabled() &&
        result.cpu_run_result == X64CpuSubset::RunResult::Halted && cpu.state.halted &&
        cpu.state.fault.empty() && result.exception_vector == static_cast<int>(vector_ud) &&
        !cpu.state.last_fault_was_page_fault && cpu.state.last_page_fault_error_code == 0 &&
        result.cr2 == 0 && result.final_rsp == initial_rsp && cpu.state.rip == guest_code_base + program.size() &&
        frame_ok;

    result.passed = pass;
    result.detail = pass
        ? "Synthetic guest executed LIDT, raised #UD via UD2 (vector 6, no error code), automatically delivered it through the guest-loaded same-CPL IDT, adjusted the saved RIP to skip UD2, returned with IRETQ, restored RSP, and reached HLT. Other exception vectors and privilege transitions remain unsupported."
        : "Synthetic #UD/IDT/IRETQ flow failed validation; vector=" + std::to_string(result.exception_vector) +
          "; frame_rsp=" + hex_value(frame_rsp) + "; final_rsp=" + hex_value(result.final_rsp) +
          "; RIP=" + hex_value(cpu.state.rip) + "; CR2=" + hex_value(result.cr2) + "; fault=" + cpu.state.fault;
    return result;
}

} // namespace someboxone
