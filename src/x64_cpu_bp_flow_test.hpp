#pragma once

#include "cpu_x64.hpp"
#include "x64_idt.hpp"
#include "inspector.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace someboxone {

struct X64CpuBpFlowResult {
    bool passed{};
    int exception_vector{-1};
    std::uint64_t int3_rip{};
    std::uint64_t saved_rip{};
    std::uint64_t original_rsp{};
    std::uint64_t exception_frame_rsp{};
    std::uint64_t final_rsp{};
    std::uint64_t cr2{};
    X64CpuSubset::RunResult cpu_run_result{X64CpuSubset::RunResult::Fault};
    std::string detail;
};

inline void append_bp_test_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<std::uint8_t>((value >> (i * 8U)) & 0xFFU));
    }
}

// Synthetic #BP path: guest LIDT -> INT3 -> same-CPL #BP frame (no error
// code; saved RIP is the instruction after INT3) -> IRETQ -> HLT. This remains
// a narrow same-CPL test and is not a general interrupt or firmware subsystem.
inline X64CpuBpFlowResult run_synthetic_cpu_bp_flow_test() {
    X64CpuBpFlowResult result;
    GuestAddressSpace memory(1024 * 1024);
    if (!memory.map_ram(0, 1024 * 1024, "synthetic CPU #BP flow RAM")) {
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

    // mov rax, 0x19000 ; lidt [rax] ; int3 ; hlt
    const std::vector<std::uint8_t> program{
        0x48, 0xB8, 0x00, 0x90, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0F, 0x01, 0x18,
        0xCC,
        0xF4
    };
    for (std::size_t i = 0; i < program.size(); ++i) {
        if (!memory.write(0x10000 + static_cast<std::uint64_t>(i), 1, program[i])) {
            result.detail = "Unable to write synthetic guest #BP program.";
            return result;
        }
    }

    constexpr std::uint64_t handler_address = 0x9000;
    constexpr std::uint64_t guest_code_base = 0x400000;
    constexpr std::uint64_t int3_rip = guest_code_base + 13ULL;
    constexpr std::uint64_t saved_rip = guest_code_base + 14ULL;
    constexpr std::uint64_t initial_rsp = 0x1F000;
    constexpr std::uint64_t frame_rsp = initial_rsp - 24ULL;
    result.int3_rip = int3_rip;
    result.saved_rip = saved_rip;
    result.original_rsp = initial_rsp;
    result.exception_frame_rsp = frame_rsp;

    // #BP has no error-code slot: [RSP]=RIP, [RSP+8]=CS, [RSP+16]=RFLAGS.
    // A bare IRETQ returns to the instruction immediately after INT3.
    const std::vector<std::uint8_t> handler{0x48, 0xCF};
    for (std::size_t i = 0; i < handler.size(); ++i) {
        if (!memory.write(handler_address + static_cast<std::uint64_t>(i), 1, handler[i])) {
            result.detail = "Unable to write synthetic #BP handler.";
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
    constexpr std::uint8_t vector_bp = 3;
    constexpr std::uint64_t gate_address = idt_base + static_cast<std::uint64_t>(vector_bp) * 16ULL;
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
    for (std::size_t i = 0; i < gate.size(); ++i) {
        if (!memory.write(gate_address + static_cast<std::uint64_t>(i), 1, gate[i])) {
            result.detail = "Unable to write synthetic guest #BP IDT gate.";
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

    const auto saved_rip_mem = memory.read(frame_rsp, 8);
    const auto saved_cs = memory.read(frame_rsp + 8ULL, 8);
    const auto saved_flags = memory.read(frame_rsp + 16ULL, 8);
    const auto loaded_idtr = cpu.guest_idtr();
    const bool frame_ok = saved_rip_mem && *saved_rip_mem == saved_rip &&
        saved_cs && *saved_cs == 0x08ULL && saved_flags && *saved_flags == 0x2ULL;
    const bool pass = loaded_idtr && loaded_idtr->base == idt_base && loaded_idtr->limit == 0x0FFF &&
        cpu.automatic_exception_delivery_enabled() &&
        result.cpu_run_result == X64CpuSubset::RunResult::Halted && cpu.state.halted &&
        cpu.state.fault.empty() && result.exception_vector == static_cast<int>(vector_bp) &&
        !cpu.state.last_fault_was_page_fault && cpu.state.last_page_fault_error_code == 0 &&
        result.cr2 == 0 && result.final_rsp == initial_rsp && cpu.state.rip == guest_code_base + program.size() &&
        frame_ok && result.saved_rip == result.int3_rip + 1ULL && result.exception_frame_rsp + 24ULL == initial_rsp;

    result.passed = pass;
    result.detail = pass
        ? "Synthetic guest executed LIDT, raised #BP via INT3 (vector 3, no error code), received a same-CPL frame whose saved RIP points after INT3, returned with IRETQ without editing RIP, restored RSP, and reached HLT. Other exception vectors and privilege transitions remain unsupported."
        : "Synthetic #BP/IDT/IRETQ flow failed validation; vector=" + std::to_string(result.exception_vector) +
          "; int3_rip=" + hex_value(result.int3_rip) + "; saved_rip=" + hex_value(result.saved_rip) +
          "; frame_rsp=" + hex_value(frame_rsp) + "; final_rsp=" + hex_value(result.final_rsp) +
          "; RIP=" + hex_value(cpu.state.rip) + "; CR2=" + hex_value(result.cr2) + "; fault=" + cpu.state.fault;
    return result;
}

} // namespace someboxone
