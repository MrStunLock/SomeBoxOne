#pragma once

#include "cpu_x64.hpp"
#include "x64_idt.hpp"
#include "inspector.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace someboxone {

struct X64CpuGpLidtFlowResult {
    bool passed{};
    int exception_vector{-1};
    std::uint32_t error_code{};
    bool has_error_code{};
    std::uint64_t faulting_rip{};
    std::uint64_t resumed_rip{};
    std::uint64_t handler_rip{};
    std::uint64_t original_rsp{};
    std::uint64_t exception_frame_rsp{};
    std::uint64_t final_rsp{};
    std::uint64_t previous_idtr_base{};
    std::uint16_t previous_idtr_limit{};
    X64CpuSubset::RunResult cpu_run_result{X64CpuSubset::RunResult::Fault};
    std::string detail;
};

inline void append_gp_test_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<std::uint8_t>((value >> (i * 8U)) & 0xFFU));
    }
}

// Synthetic #GP(0) test: guest first loads a valid IDTR, then executes LIDT
// with a non-canonical base. The rejected LIDT must raise vector 13 with an
// error-code slot and leave the previous IDTR intact. The old vector-13 gate
// records the fault RIP, edits it to skip the rejected instruction, removes the
// error-code slot, and returns with limited same-CPL IRETQ. No firmware runs.
inline X64CpuGpLidtFlowResult run_synthetic_cpu_gp_lidt_flow_test() {
    X64CpuGpLidtFlowResult result;
    GuestAddressSpace memory(1024 * 1024);
    if (!memory.map_ram(0, 1024 * 1024, "synthetic CPU #GP/LIDT RAM")) {
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

    constexpr std::uint64_t guest_code_base = 0x400000;
    constexpr std::uint64_t handler_address = 0x9000;
    constexpr std::uint64_t initial_rsp = 0x1F000;
    constexpr std::uint64_t idt_base = 0x18000;
    constexpr std::uint64_t valid_descriptor_address = 0x19000;
    constexpr std::uint64_t invalid_descriptor_address = 0x19100;
    constexpr std::uint64_t invalid_idtr_base = 0x0000800000000000ULL;
    constexpr std::uint8_t gp_vector = 13;
    constexpr std::uint64_t faulting_rip = guest_code_base + 23ULL;
    constexpr std::uint64_t resumed_rip = guest_code_base + 26ULL;
    constexpr std::uint64_t exception_frame_rsp = initial_rsp - 32ULL;
    constexpr std::uint64_t saved_rip_slot = exception_frame_rsp + 8ULL;
    constexpr std::uint64_t captured_fault_rip_address = 0x1D000;

    // mov rax, valid_descriptor ; lidt [rax] ; mov rax, bad_descriptor ;
    // lidt [rax] ; hlt. The second LIDT is expected to raise #GP(0).
    std::vector<std::uint8_t> program{
        0x48, 0xB8, 0x00, 0x90, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0F, 0x01, 0x18,
        0x48, 0xB8, 0x00, 0x91, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0F, 0x01, 0x18,
        0xF4
    };
    for (std::size_t i = 0; i < program.size(); ++i) {
        if (!memory.write(0x10000 + static_cast<std::uint64_t>(i), 1, program[i])) {
            result.detail = "Unable to write synthetic guest #GP program.";
            return result;
        }
    }

    // Save RAX/RBX, capture the original saved RIP in a diagnostic slot, edit
    // that RIP to the HLT immediately after the rejected LIDT, restore registers,
    // discard #GP's error-code slot and IRETQ. Frame layout at entry:
    // [RSP]=error code 0, [RSP+8]=fault RIP, [RSP+16]=CS, [RSP+24]=RFLAGS.
    std::vector<std::uint8_t> handler{0x50, 0x53}; // push rax; push rbx
    handler.insert(handler.end(), {0x48, 0xB8});
    append_gp_test_u64(handler, saved_rip_slot); // mov rax, saved RIP slot
    handler.insert(handler.end(), {0x48, 0x8B, 0x18}); // mov rbx, [rax]
    handler.insert(handler.end(), {0x48, 0xB8});
    append_gp_test_u64(handler, captured_fault_rip_address); // mov rax, capture slot
    handler.insert(handler.end(), {0x48, 0x89, 0x18}); // mov [rax], rbx
    handler.insert(handler.end(), {0x48, 0xB8});
    append_gp_test_u64(handler, saved_rip_slot);
    handler.insert(handler.end(), {0x48, 0xBB});
    append_gp_test_u64(handler, resumed_rip); // mov rbx, resume address
    handler.insert(handler.end(), {0x48, 0x89, 0x18}); // mov [rax], rbx
    handler.push_back(0x5B); // pop rbx
    handler.push_back(0x58); // pop rax
    handler.insert(handler.end(), {0x48, 0x83, 0xC4, 0x08}); // add rsp, 8
    handler.insert(handler.end(), {0x48, 0xCF}); // iretq
    for (std::size_t i = 0; i < handler.size(); ++i) {
        if (!memory.write(handler_address + static_cast<std::uint64_t>(i), 1, handler[i])) {
            result.detail = "Unable to write synthetic #GP handler.";
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

    std::array<std::uint8_t, 16> gate{};
    gate[0] = static_cast<std::uint8_t>(handler_address & 0xFFU);
    gate[1] = static_cast<std::uint8_t>((handler_address >> 8U) & 0xFFU);
    gate[2] = 0x08;
    gate[5] = 0x8E; // Present 64-bit interrupt gate, no IST.
    gate[6] = static_cast<std::uint8_t>((handler_address >> 16U) & 0xFFU);
    gate[7] = static_cast<std::uint8_t>((handler_address >> 24U) & 0xFFU);
    for (unsigned i = 0; i < 4; ++i) {
        gate[8 + i] = static_cast<std::uint8_t>((handler_address >> (32U + i * 8U)) & 0xFFU);
    }
    const std::uint64_t gate_address = idt_base + static_cast<std::uint64_t>(gp_vector) * 16ULL;
    for (std::size_t i = 0; i < gate.size(); ++i) {
        if (!memory.write(gate_address + static_cast<std::uint64_t>(i), 1, gate[i])) {
            result.detail = "Unable to write the synthetic vector-13 IDT gate.";
            return result;
        }
    }

    if (!memory.write(valid_descriptor_address, 2, 0x0FFF) ||
        !memory.write(valid_descriptor_address + 2ULL, 8, idt_base) ||
        !memory.write(invalid_descriptor_address, 2, 0x0FFF) ||
        !memory.write(invalid_descriptor_address + 2ULL, 8, invalid_idtr_base)) {
        result.detail = "Unable to write the valid/invalid IDTR pseudo-descriptors.";
        return result;
    }

    result.faulting_rip = faulting_rip;
    result.resumed_rip = resumed_rip;
    result.handler_rip = handler_address;
    result.original_rsp = initial_rsp;
    result.exception_frame_rsp = exception_frame_rsp;

    X64CpuSubset cpu(memory, guest_code_base, control);
    cpu.state.registers[4] = initial_rsp;
    result.cpu_run_result = cpu.run(192);
    result.exception_vector = cpu.state.last_exception_vector;
    result.error_code = cpu.state.last_exception_error_code;
    result.has_error_code = cpu.state.last_exception_has_error_code;
    result.final_rsp = cpu.state.registers[4];

    const auto frame_error = memory.read(exception_frame_rsp, 8);
    const auto frame_rip = memory.read(saved_rip_slot, 8);
    const auto original_fault_rip = memory.read(captured_fault_rip_address, 8);
    const auto frame_cs = memory.read(exception_frame_rsp + 16ULL, 8);
    const auto frame_flags = memory.read(exception_frame_rsp + 24ULL, 8);
    const auto loaded_idtr = cpu.guest_idtr();

    const bool frame_ok = frame_error && *frame_error == 0 &&
        frame_rip && *frame_rip == resumed_rip &&
        original_fault_rip && *original_fault_rip == faulting_rip &&
        frame_cs && *frame_cs == 0x08ULL &&
        frame_flags && *frame_flags == 0x2ULL;
    const bool idtr_preserved = loaded_idtr && loaded_idtr->base == idt_base &&
        loaded_idtr->limit == 0x0FFF && cpu.automatic_exception_delivery_enabled();
    const bool execution_ok =
        result.cpu_run_result == X64CpuSubset::RunResult::Halted && cpu.state.halted &&
        cpu.state.fault.empty() && result.exception_vector == static_cast<int>(gp_vector) &&
        result.error_code == 0 && result.has_error_code &&
        !cpu.state.last_fault_was_page_fault && cpu.state.cr2 == 0 &&
        frame_ok && idtr_preserved && result.final_rsp == initial_rsp &&
        cpu.state.rip == guest_code_base + program.size();

    result.previous_idtr_base = loaded_idtr ? loaded_idtr->base : 0;
    result.previous_idtr_limit = loaded_idtr ? loaded_idtr->limit : 0;
    result.passed = execution_ok;
    result.detail = result.passed
        ? "Synthetic guest loaded a valid IDTR, attempted LIDT with a non-canonical base, delivered #GP(0) through the old vector-13 gate, verified the faulting RIP/error-code frame, preserved the old IDTR, skipped the rejected instruction, removed the error-code slot, returned via same-CPL IRETQ, restored RSP and reached HLT. Other #GP causes and general architectural delivery semantics remain unsupported."
        : "Synthetic #GP/LIDT test failed validation; vector=" +
            std::to_string(result.exception_vector) + "; error code=" + hex_value(result.error_code) +
            "; final RSP=" + hex_value(result.final_rsp) +
            "; loaded IDTR base=" + hex_value(result.previous_idtr_base) +
            "; halted=" + (cpu.state.halted ? "true" : "false") +
            "; fault=" + cpu.state.fault;
    return result;
}

} // namespace someboxone
