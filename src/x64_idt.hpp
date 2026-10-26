#pragma once

#include "machine.hpp"
#include "paging.hpp"
#include "x64_control.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace someboxone {

struct X64Idtr {
    std::uint64_t base{};
    std::uint16_t limit{};
};

struct X64InterruptFrame {
    std::uint64_t rip{};
    std::uint64_t rsp{};
    std::uint64_t rflags{2};
    std::uint16_t cs{0x8};
    std::uint8_t cpl{};
};

struct X64InterruptDeliveryResult {
    bool success{};
    std::uint8_t vector{};
    std::uint64_t handler_rip{};
    std::uint64_t old_rsp{};
    std::uint64_t new_rsp{};
    bool error_code_pushed{};
    bool interrupt_gate{};
    std::string detail;
};

// A deliberately limited long-mode IDT model. It validates 64-bit interrupt
// and trap gates and delivers an exception to the same privilege level. It
// uses the guest page tables for IDT reads and stack writes, but does not model
// GDT descriptor validation, IST/privilege stack switching, double faults, or
// delivery for every exception vector. The CPU subset can load an IDTR with a
// limited guest LIDT instruction and has a narrow same-CPL IRETQ path for a
// synthetic #PF repair/return test.
class X64IdtDispatcher {
public:
    X64IdtDispatcher(GuestAddressSpace& memory, X64ControlState& control)
        : memory_(memory), control_(control) {}

    X64InterruptDeliveryResult deliver_exception(
        std::uint8_t vector,
        const X64Idtr& idtr,
        X64InterruptFrame& frame,
        std::optional<std::uint64_t> error_code = std::nullopt) {
        X64InterruptDeliveryResult result;
        result.vector = vector;
        result.old_rsp = frame.rsp;
        result.new_rsp = frame.rsp;
        result.error_code_pushed = error_code.has_value();

        if (!control_.long_mode_active() || !control_.paging_enabled()) {
            result.detail = "IDT delivery requires the modeled x86-64 long-mode and paging state.";
            return result;
        }
        if (frame.cpl > 3 || frame.cpl != control_.cpl() ||
            (frame.cs & 3U) != frame.cpl) {
            result.detail = "Frame CPL, control-state CPL and current CS RPL must agree.";
            return result;
        }
        const std::uint64_t descriptor_offset = static_cast<std::uint64_t>(vector) * 16ULL;
        if (descriptor_offset + 15ULL > idtr.limit) {
            result.detail = "IDT limit does not contain the requested 16-byte gate.";
            return result;
        }
        if (!is_canonical(idtr.base) || idtr.base > (UINT64_MAX - descriptor_offset)) {
            result.detail = "IDTR base or gate address is non-canonical or overflows.";
            return result;
        }

        std::array<std::uint8_t, 16> gate{};
        for (std::size_t i = 0; i < gate.size(); ++i) {
            const auto value = read_linear_byte(idtr.base + descriptor_offset + i);
            if (!value) {
                result.detail = "Could not read the IDT gate through guest virtual memory.";
                return result;
            }
            gate[i] = *value;
        }

        const std::uint8_t attributes = gate[5];
        const std::uint8_t gate_type = attributes & 0x0FU;
        const bool present = (attributes & 0x80U) != 0;
        if (!present) {
            result.detail = "IDT gate Present bit is clear.";
            return result;
        }
        if ((attributes & 0x10U) != 0 || (gate[4] & 0xF8U) != 0 ||
            gate[12] != 0 || gate[13] != 0 || gate[14] != 0 || gate[15] != 0) {
            result.detail = "IDT gate contains non-zero reserved bits.";
            return result;
        }
        if (gate_type != 0x0EU && gate_type != 0x0FU) {
            result.detail = "Only 64-bit interrupt gates (type 0xE) and trap gates (type 0xF) are supported.";
            return result;
        }
        if ((gate[4] & 0x07U) != 0) {
            result.detail = "IST stack switching is not implemented by this same-CPL delivery model.";
            return result;
        }

        const std::uint16_t selector = static_cast<std::uint16_t>(gate[2]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(gate[3]) << 8U);
        if (selector == 0 || (selector & 3U) != frame.cpl) {
            result.detail = "Gate selector is null or requests a privilege-level transition, which is not implemented.";
            return result;
        }

        const std::uint64_t handler =
            static_cast<std::uint64_t>(gate[0]) |
            (static_cast<std::uint64_t>(gate[1]) << 8U) |
            (static_cast<std::uint64_t>(gate[6]) << 16U) |
            (static_cast<std::uint64_t>(gate[7]) << 24U) |
            (static_cast<std::uint64_t>(gate[8]) << 32U) |
            (static_cast<std::uint64_t>(gate[9]) << 40U) |
            (static_cast<std::uint64_t>(gate[10]) << 48U) |
            (static_cast<std::uint64_t>(gate[11]) << 56U);
        if (!is_canonical(handler)) {
            result.detail = "IDT gate target is not a canonical 48-bit linear address.";
            return result;
        }
        result.handler_rip = handler;
        result.interrupt_gate = gate_type == 0x0EU;

        // Same-CPL long-mode frame. Push sequence yields [RSP] = error code
        // when one is supplied, then RIP, CS and RFLAGS. A handler must remove
        // an error code before IRETQ. The dispatcher itself only constructs the
        // entry frame; the CPU subset models a limited same-CPL return separately.
        std::uint64_t new_rsp = frame.rsp;
        if (!push64(new_rsp, frame.rflags, result.detail) ||
            !push64(new_rsp, frame.cs, result.detail) ||
            !push64(new_rsp, frame.rip, result.detail) ||
            (error_code && !push64(new_rsp, *error_code, result.detail))) {
            if (result.detail.empty()) result.detail = "Could not construct the exception stack frame.";
            return result;
        }

        frame.rsp = new_rsp;
        frame.rip = handler;
        frame.cs = selector;
        constexpr std::uint64_t flag_tf = 1ULL << 8U;
        constexpr std::uint64_t flag_if = 1ULL << 9U;
        constexpr std::uint64_t flag_nt = 1ULL << 14U;
        constexpr std::uint64_t flag_rf = 1ULL << 16U;
        frame.rflags &= ~(flag_tf | flag_nt | flag_rf);
        if (result.interrupt_gate) frame.rflags &= ~flag_if;

        result.success = true;
        result.new_rsp = frame.rsp;
        result.detail = result.interrupt_gate
            ? "Same-CPL interrupt-gate delivery completed; IF/TF/NT/RF clear policy applied."
            : "Same-CPL trap-gate delivery completed; TF/NT/RF clear policy applied.";
        return result;
    }

private:
    GuestAddressSpace& memory_;
    X64ControlState& control_;

    static bool is_canonical(std::uint64_t address) {
        const bool sign = ((address >> 47U) & 1ULL) != 0;
        const std::uint64_t upper = address >> 48U;
        return sign ? upper == 0xFFFFULL : upper == 0;
    }

    std::optional<std::uint8_t> read_linear_byte(std::uint64_t address) {
        X64PageTableWalker::Configuration cfg;
        cfg.nx_enabled = control_.nx_enabled();
        cfg.supervisor_write_protect = control_.write_protect_enabled();
        X64PageTableWalker walker(memory_, cfg);
        const auto translated = walker.translate(address, control_.cr3(), X64AccessType::Read,
                                                  X64PrivilegeLevel::Supervisor);
        if (!translated.success) return std::nullopt;
        const auto value = memory_.read(translated.physical_address, 1);
        if (!value) return std::nullopt;
        return static_cast<std::uint8_t>(*value);
    }

    bool write_linear_byte(std::uint64_t address, std::uint8_t value) {
        X64PageTableWalker::Configuration cfg;
        cfg.nx_enabled = control_.nx_enabled();
        cfg.supervisor_write_protect = control_.write_protect_enabled();
        X64PageTableWalker walker(memory_, cfg);
        const auto translated = walker.translate(address, control_.cr3(), X64AccessType::Write,
                                                  X64PrivilegeLevel::Supervisor);
        if (!translated.success) return false;
        return memory_.write(translated.physical_address, 1, value);
    }

    bool push64(std::uint64_t& rsp, std::uint64_t value, std::string& error) {
        if (rsp < 8ULL) {
            error = "RSP underflow while building the exception frame.";
            return false;
        }
        const std::uint64_t destination = rsp - 8ULL;
        if (!is_canonical(destination)) {
            error = "Stack destination is non-canonical.";
            return false;
        }
        for (unsigned int i = 0; i < 8; ++i) {
            if (!write_linear_byte(destination + i,
                    static_cast<std::uint8_t>((value >> (i * 8U)) & 0xFFU))) {
                error = "Stack write failed while building the exception frame.";
                return false;
            }
        }
        rsp = destination;
        return true;
    }
};

} // namespace someboxone
