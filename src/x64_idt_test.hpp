#pragma once

#include "x64_idt.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace someboxone {

struct X64IdtSmokeResult {
    bool passed{};
    std::uint64_t handler_rip{};
    std::uint64_t old_rsp{};
    std::uint64_t new_rsp{};
    std::string detail;
};

inline X64IdtSmokeResult run_synthetic_idt_delivery_test() {
    X64IdtSmokeResult result;
    GuestAddressSpace memory(0x40000);
    if (!memory.map_ram(0, 0x40000, "synthetic IDT delivery RAM")) {
        result.detail = "Unable to map synthetic RAM.";
        return result;
    }

    // Identity-map the low 256 KiB using synthetic 4-level page tables.
    bool tables_ok =
        memory.write(0x1000, 8, 0x2007) &&
        memory.write(0x2000, 8, 0x3007) &&
        memory.write(0x3000, 8, 0x4007);
    for (std::uint64_t i = 0; tables_ok && i < 64; ++i) {
        tables_ok = memory.write(0x4000 + i * 8ULL, 8, (i * 0x1000ULL) | 0x7ULL);
    }
    if (!tables_ok) {
        result.detail = "Unable to create synthetic identity page tables.";
        return result;
    }

    X64ControlState control;
    if (!control.write_cr0(X64ControlState::cr0_pe | X64ControlState::cr0_wp | (1ULL << 4U)) ||
        !control.write_cr4(X64ControlState::cr4_pae) ||
        !control.write_efer(X64ControlState::efer_lme | X64ControlState::efer_nxe) ||
        !control.write_cr3(0x1000) ||
        !control.write_cr0(X64ControlState::cr0_pe | X64ControlState::cr0_pg |
                           X64ControlState::cr0_wp | (1ULL << 4U)) ||
        !control.long_mode_active() || !control.set_cpl(0)) {
        result.detail = "Unable to configure synthetic long-mode state: " + control.last_error();
        return result;
    }

    constexpr std::uint64_t idt_base = 0x10000;
    const auto write_gate = [&memory](std::uint8_t vector, std::uint64_t handler,
                                                std::uint8_t attributes) {
        std::array<std::uint8_t, 16> gate{};
        gate[0] = static_cast<std::uint8_t>(handler & 0xFFU);
        gate[1] = static_cast<std::uint8_t>((handler >> 8U) & 0xFFU);
        gate[2] = 0x08; // same-ring kernel code selector
        gate[3] = 0x00;
        gate[4] = 0x00; // IST not used
        gate[5] = attributes;
        gate[6] = static_cast<std::uint8_t>((handler >> 16U) & 0xFFU);
        gate[7] = static_cast<std::uint8_t>((handler >> 24U) & 0xFFU);
        for (unsigned int i = 0; i < 4; ++i) {
            gate[8 + i] = static_cast<std::uint8_t>((handler >> (32U + i * 8U)) & 0xFFU);
        }
        for (std::size_t i = 0; i < gate.size(); ++i) {
            if (!memory.write(idt_base + static_cast<std::uint64_t>(vector) * 16ULL + i, 1, gate[i]))
                return false;
        }
        return true;
    };

    constexpr std::uint8_t page_fault_vector = 14;
    constexpr std::uint64_t interrupt_handler = 0x9000;
    constexpr std::uint64_t trap_handler = 0xA000;
    if (!write_gate(page_fault_vector, interrupt_handler, 0x8E) || // present interrupt gate
        !write_gate(3, trap_handler, 0x8F) ||                       // present trap gate
        !write_gate(13, 0xB000, 0x0E)) {                            // not present
        result.detail = "Unable to write synthetic IDT gates.";
        return result;
    }

    X64Idtr idtr{idt_base, 0x0FFF};
    X64IdtDispatcher dispatcher(memory, control);
    X64InterruptFrame frame;
    frame.rip = 0x5555;
    frame.rsp = 0x1F000;
    frame.rflags = 2ULL | (1ULL << 8U) | (1ULL << 9U) | (1ULL << 14U) | (1ULL << 16U);
    frame.cs = 0x08;
    frame.cpl = 0;
    const auto original_rflags = frame.rflags;
    result.old_rsp = frame.rsp;

    const auto delivered = dispatcher.deliver_exception(page_fault_vector, idtr, frame, 0x4);
    result.handler_rip = frame.rip;
    result.new_rsp = frame.rsp;
    if (!delivered.success) {
        result.detail = "Interrupt-gate delivery failed: " + delivered.detail;
        return result;
    }

    const auto error = memory.read(frame.rsp, 8);
    const auto saved_rip = memory.read(frame.rsp + 8, 8);
    const auto saved_cs = memory.read(frame.rsp + 16, 8);
    const auto saved_flags = memory.read(frame.rsp + 24, 8);
    const std::uint64_t clear_mask = (1ULL << 8U) | (1ULL << 9U) |
                                     (1ULL << 14U) | (1ULL << 16U);
    const bool interrupt_frame_ok = frame.rip == interrupt_handler && frame.cs == 0x08 &&
        frame.rsp == result.old_rsp - 32ULL && error == 0x4ULL &&
        saved_rip == 0x5555ULL && saved_cs == 0x08ULL &&
        saved_flags == original_rflags && (frame.rflags & clear_mask) == 0;
    if (!interrupt_frame_ok) {
        result.detail = "Interrupt-gate target, error-code stack frame, or saved flags did not match expectations.";
        return result;
    }

    // Trap gates preserve IF. With no error code the same-CPL frame is 24 bytes.
    X64InterruptFrame trap_frame;
    trap_frame.rip = 0x6666;
    trap_frame.rsp = 0x1E000;
    trap_frame.rflags = 2ULL | (1ULL << 9U) | (1ULL << 8U);
    trap_frame.cs = 0x08;
    trap_frame.cpl = 0;
    const auto trap_result = dispatcher.deliver_exception(3, idtr, trap_frame);
    const auto trap_saved_rip = memory.read(trap_frame.rsp, 8);
    const auto trap_saved_cs = memory.read(trap_frame.rsp + 8, 8);
    const auto trap_saved_flags = memory.read(trap_frame.rsp + 16, 8);
    const bool trap_frame_ok = trap_result.success && trap_frame.rip == trap_handler &&
        trap_frame.rsp == 0x1E000ULL - 24ULL &&
        (trap_frame.rflags & (1ULL << 9U)) != 0 &&
        (trap_frame.rflags & (1ULL << 8U)) == 0 &&
        trap_saved_rip == 0x6666ULL && trap_saved_cs == 0x08ULL &&
        trap_saved_flags == (2ULL | (1ULL << 9U) | (1ULL << 8U));
    if (!trap_frame_ok) {
        result.detail = "Trap-gate delivery did not preserve IF or construct the expected frame.";
        return result;
    }

    X64InterruptFrame absent_frame;
    absent_frame.rip = 0x7777;
    absent_frame.rsp = 0x1D000;
    absent_frame.cs = 0x08;
    absent_frame.cpl = 0;
    const auto absent_result = dispatcher.deliver_exception(13, idtr, absent_frame);
    if (absent_result.success || absent_result.detail.find("Present bit is clear") == std::string::npos ||
        absent_frame.rip != 0x7777 || absent_frame.rsp != 0x1D000) {
        result.detail = "A not-present gate was not rejected cleanly.";
        return result;
    }

    result.passed = true;
    result.detail = "Synthetic IDT tests passed: #PF-style interrupt-gate frame/error code, trap-gate IF preservation, and not-present gate rejection. Same CPL only; no firmware executed.";
    return result;
}

} // namespace someboxone
