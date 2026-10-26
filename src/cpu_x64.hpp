#pragma once

#include "paging.hpp"
#include "x64_control.hpp"
#include "x64_idt.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace someboxone {

// Phase 3 research scaffold: a deliberately small x86-64 interpreter subset
// for synthetic test payloads. It supports a limited set of register and memory
// operands, a minimal stack, and simple branches. It cannot boot Xbox firmware.
class X64CpuSubset {
public:
    enum class StepResult { Executed, Halted, Fault };
    enum class RunResult { Halted, Fault, StepLimit };

    struct State {
        std::array<std::uint64_t, 16> registers{}; // RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8-R15
        std::uint64_t rip{};
        std::uint64_t rflags{2};
        bool halted{};
        std::string fault;
        // Last modeled architectural translation exception. These diagnostic
        // fields are preserved after automatic delivery so tests can inspect it.
        int last_exception_vector{-1};
        // Generic exception metadata complements the historical #PF-only field.
        // Error-code presence is separate because #GP(0) has a zero-valued slot.
        std::uint32_t last_exception_error_code{};
        bool last_exception_has_error_code{};
        std::uint32_t last_page_fault_error_code{};
        std::uint64_t cr2{};
        bool last_fault_was_page_fault{};
    };

    explicit X64CpuSubset(GuestAddressSpace& memory, std::uint64_t entry_point = 0)
        : memory_(memory), page_walker_(memory) { state.rip = entry_point; }

    // Opt-in long-mode virtual-memory execution path for synthetic tests. The
    // default constructor remains flat/physical for existing micro-tests.
    X64CpuSubset(GuestAddressSpace& memory, std::uint64_t entry_point,
                 std::uint64_t cr3, X64PrivilegeLevel privilege)
        : memory_(memory), page_walker_(memory), paging_enabled_(true),
          cr3_(cr3), privilege_(privilege) { state.rip = entry_point; }

    // Architectural-control-state constructor. This path only supports the
    // interpreter's 64-bit subset after CR0/CR3/CR4/EFER activate long mode.
    X64CpuSubset(GuestAddressSpace& memory, std::uint64_t entry_point,
                 X64ControlState& control_state)
        : memory_(memory), page_walker_(memory), control_state_(&control_state) {
        state.rip = entry_point;
    }

    State state{};

    StepResult step() {
        page_fault_raised_this_step_ = false;
        invalid_opcode_raised_this_step_ = false;
        breakpoint_raised_this_step_ = false;
        general_protection_raised_this_step_ = false;
        if (state.halted) return StepResult::Halted;
        if (!state.fault.empty()) return StepResult::Fault;
        instruction_start_rip_ = state.rip;
        if (control_state_ && !control_state_->long_mode_active()) {
            return fail("x86-64 subset requires CR0.PE+PG, CR4.PAE, and EFER.LME to activate long mode.");
        }

        std::uint8_t rex = 0;
        auto opcode_value = fetch8();
        if (!opcode_value) return StepResult::Fault;
        std::uint8_t opcode = *opcode_value;
        if (opcode >= 0x40 && opcode <= 0x4F) {
            rex = opcode;
            const auto next = fetch8();
            if (!next) return StepResult::Fault;
            opcode = *next;
        }

        if (opcode == 0x90) return StepResult::Executed; // NOP
        if (opcode == 0xF4) { state.halted = true; return StepResult::Halted; } // HLT in this test harness
        if (opcode == 0xCC) { // INT3 raises #BP (vector 3); saved RIP points after INT3.
            state.last_exception_vector = 3;
            state.last_exception_error_code = 0;
            state.last_exception_has_error_code = false;
            state.last_page_fault_error_code = 0;
            state.last_fault_was_page_fault = false;
            breakpoint_raised_this_step_ = true;
            return fail("Breakpoint exception (#BP) raised by INT3.");
        }
        if (opcode == 0xCF) {
            if ((rex & 0x08U) == 0) return fail("Only 64-bit same-CPL IRETQ (REX.W + CF) is modeled.");
            return execute_iretq();
        }

        // PUSH/POP general-purpose registers use 64-bit stack slots in long mode.
        if (opcode >= 0x50 && opcode <= 0x57) {
            const unsigned reg = static_cast<unsigned>(opcode - 0x50) + ((rex & 0x01U) ? 8U : 0U);
            const std::uint64_t value = state.registers[reg];
            const std::uint64_t new_rsp = state.registers[4] - 8U;
            if (!write_guest_memory(new_rsp, 8, value)) return fail(memory_fault_or("PUSH target outside mapped guest memory"));
            state.registers[4] = new_rsp;
            return StepResult::Executed;
        }
        if (opcode >= 0x58 && opcode <= 0x5F) {
            const unsigned reg = static_cast<unsigned>(opcode - 0x58) + ((rex & 0x01U) ? 8U : 0U);
            const std::uint64_t old_rsp = state.registers[4];
            const auto value = read_guest_memory(old_rsp, 8, X64AccessType::Read);
            if (!value) return fail(memory_fault_or("POP source outside mapped guest memory"));
            if (reg == 4U) state.registers[4] = *value; // POP RSP replaces RSP with the popped value.
            else {
                state.registers[reg] = *value;
                state.registers[4] = old_rsp + 8U;
            }
            return StepResult::Executed;
        }

        if (opcode == 0x81 || opcode == 0x83) return execute_group_immediate(opcode, rex);

        if (opcode >= 0xB8 && opcode <= 0xBF) { // MOV r32/r64, immediate
            const unsigned reg = static_cast<unsigned>(opcode - 0xB8) + ((rex & 0x01U) ? 8U : 0U);
            if ((rex & 0x08U) != 0) {
                const auto immediate = fetch64();
                if (!immediate) return StepResult::Fault;
                state.registers[reg] = *immediate;
            } else {
                const auto immediate = fetch32();
                if (!immediate) return StepResult::Fault;
                state.registers[reg] = *immediate; // 32-bit writes zero-extend in x86-64
            }
            return StepResult::Executed;
        }

        if (opcode == 0xEB || opcode == 0xE9) { // JMP rel8 / rel32
            if (opcode == 0xEB) {
                const auto raw = fetch8();
                if (!raw) return StepResult::Fault;
                state.rip += static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(*raw)));
            } else {
                const auto raw = fetch32();
                if (!raw) return StepResult::Fault;
                state.rip += static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(*raw)));
            }
            return StepResult::Executed;
        }

        if (opcode == 0x74 || opcode == 0x75) { // JZ/JNZ rel8
            const auto raw = fetch8();
            if (!raw) return StepResult::Fault;
            const bool zero = (state.rflags & flag_zero) != 0;
            if ((opcode == 0x74 && zero) || (opcode == 0x75 && !zero)) {
                state.rip += static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(*raw)));
            }
            return StepResult::Executed;
        }

        if (opcode == 0x0F) {
            const auto second = fetch8();
            if (!second) return StepResult::Fault;
            if (*second == 0x01) return execute_group_0f01(rex); // LIDT m16&64 subset
            if (*second == 0x0B) { // UD2 raises #UD (vector 6), which has no error-code slot.
                state.last_exception_vector = 6;
                state.last_exception_error_code = 0;
                state.last_exception_has_error_code = false;
                state.last_page_fault_error_code = 0;
                state.last_fault_was_page_fault = false;
                invalid_opcode_raised_this_step_ = true;
                return fail("Invalid opcode exception (#UD) raised by UD2.");
            }
            if (*second != 0x84 && *second != 0x85) return fail("Unsupported 0F opcode in x86-64 subset");
            const auto raw = fetch32();
            if (!raw) return StepResult::Fault;
            const bool zero = (state.rflags & flag_zero) != 0;
            if ((*second == 0x84 && zero) || (*second == 0x85 && !zero)) {
                state.rip += static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(*raw)));
            }
            return StepResult::Executed;
        }

        switch (opcode) {
        case 0x89: case 0x8B: // MOV r/m,reg or reg,r/m
        case 0x31: case 0x33: // XOR
        case 0x01: case 0x03: // ADD
        case 0x29: case 0x2B: // SUB
        case 0x39: case 0x3B: // CMP
        case 0x85:             // TEST
            return execute_modrm(opcode, rex);
        default:
            return fail("Unsupported opcode in x86-64 subset: 0x" + hex_byte(opcode));
        }
    }

    RunResult run(std::uint64_t max_steps) {
        for (std::uint64_t count = 0; count < max_steps; ++count) {
            const auto result = step();
            if (result == StepResult::Halted) return RunResult::Halted;
            if (result == StepResult::Fault) {
                // Deliver only explicitly modeled architectural exceptions.
                // Ordinary decode failures and physical-memory errors remain terminal.
                if (try_deliver_runtime_exception()) continue;
                return RunResult::Fault;
            }
        }
        return state.halted ? RunResult::Halted : (state.fault.empty() ? RunResult::StepLimit : RunResult::Fault);
    }

    // Host/test harness override retained for focused tests. Guest LIDT can also
    // load an IDTR and enables limited same-CPL runtime #PF/#UD/#BP delivery in CPL 0.
    bool enable_automatic_exception_delivery(const X64Idtr& idtr) {
        if (!control_state_ || !control_state_->long_mode_active() ||
            !control_state_->paging_enabled()) {
            return false;
        }
        idtr_ = idtr;
        automatic_exception_delivery_ = true;
        return true;
    }

    // Backward-compatible name for older focused page-fault tests.
    bool enable_automatic_page_fault_delivery(const X64Idtr& idtr) {
        return enable_automatic_exception_delivery(idtr);
    }

    void disable_automatic_exception_delivery() {
        automatic_exception_delivery_ = false;
        idtr_.reset();
    }

    void disable_automatic_page_fault_delivery() {
        disable_automatic_exception_delivery();
    }

    bool automatic_exception_delivery_enabled() const {
        return automatic_exception_delivery_ && idtr_.has_value();
    }

    bool automatic_page_fault_delivery_enabled() const {
        return automatic_exception_delivery_enabled();
    }

    std::optional<X64Idtr> guest_idtr() const {
        return idtr_;
    }

private:
    static constexpr std::uint64_t flag_carry = 1ULL << 0;
    static constexpr std::uint64_t flag_parity = 1ULL << 2;
    static constexpr std::uint64_t flag_zero = 1ULL << 6;
    static constexpr std::uint64_t flag_sign = 1ULL << 7;
    static constexpr std::uint64_t flag_overflow = 1ULL << 11;
    static constexpr std::uint64_t arithmetic_flags = flag_carry | flag_parity | flag_zero | flag_sign | flag_overflow;

    GuestAddressSpace& memory_;
    X64PageTableWalker page_walker_;
    bool paging_enabled_{};
    std::uint64_t cr3_{};
    X64PrivilegeLevel privilege_{X64PrivilegeLevel::Supervisor};
    X64ControlState* control_state_{};
    std::string last_memory_fault_;
    std::uint64_t instruction_start_rip_{};
    bool page_fault_raised_this_step_{};
    bool invalid_opcode_raised_this_step_{};
    bool breakpoint_raised_this_step_{};
    bool general_protection_raised_this_step_{};
    bool automatic_exception_delivery_{};
    std::optional<X64Idtr> idtr_;

    bool effective_paging_enabled() const {
        return control_state_ ? control_state_->paging_enabled() : paging_enabled_;
    }

    X64TranslationResult translate_guest_address(std::uint64_t address, X64AccessType access) const {
        if (control_state_) {
            X64PageTableWalker::Configuration config;
            config.nx_enabled = control_state_->nx_enabled();
            config.supervisor_write_protect = control_state_->write_protect_enabled();
            X64PageTableWalker walker(memory_, config);
            const auto level = control_state_->cpl() == 0 ? X64PrivilegeLevel::Supervisor : X64PrivilegeLevel::User;
            return walker.translate(address, control_state_->cr3(), access, level);
        }
        return page_walker_.translate(address, cr3_, access, privilege_);
    }

    std::string memory_fault_or(const std::string& fallback) const {
        return last_memory_fault_.empty() ? fallback : last_memory_fault_;
    }

    void record_translation_exception(const X64TranslationResult& translated,
                                     std::uint64_t linear_address) {
        if (translated.has_page_fault) {
            state.last_exception_vector = 14; // #PF
            state.last_exception_error_code = translated.page_fault_error_code;
            state.last_exception_has_error_code = true;
            state.last_page_fault_error_code = translated.page_fault_error_code;
            state.cr2 = linear_address;
            state.last_fault_was_page_fault = true;
            page_fault_raised_this_step_ = true;
        } else if (translated.exception_vector >= 0) {
            state.last_exception_vector = translated.exception_vector;
            state.last_exception_error_code = 0;
            state.last_exception_has_error_code = false;
            state.last_page_fault_error_code = 0;
            state.last_fault_was_page_fault = false;
            // CR2 is only updated by a page fault; leave it unchanged for #GP.
        }
    }

    std::optional<std::uint64_t> read_guest_memory(std::uint64_t address, std::uint8_t width,
                                                   X64AccessType access) {
        last_memory_fault_.clear();
        if (!effective_paging_enabled()) return memory_.read(address, width);
        if (address > (std::numeric_limits<std::uint64_t>::max)() - (static_cast<std::uint64_t>(width) - 1ULL)) {
            last_memory_fault_ = "Virtual memory read address overflows.";
            return std::nullopt;
        }
        const std::uint64_t bytes_remaining = 4096ULL - (address & 0xFFFULL);
        if (width <= bytes_remaining) {
            const auto translated = translate_guest_address(address, access);
            if (!translated.success) {
                record_translation_exception(translated, address);
                last_memory_fault_ = std::string("Virtual read translation fault: ") +
                    x64_page_fault_name(translated.fault) + " - " + translated.detail;
                if (translated.has_page_fault) {
                    last_memory_fault_ += " (#PF error code=" + std::to_string(translated.page_fault_error_code) + ")";
                } else if (translated.exception_vector >= 0) {
                    last_memory_fault_ += " (exception vector=" + std::to_string(translated.exception_vector) + ")";
                }
                return std::nullopt;
            }
            const auto value = memory_.read(translated.physical_address, width);
            if (!value) last_memory_fault_ = "Translated physical read is outside mapped guest memory.";
            return value;
        }
        std::uint64_t value = 0;
        for (std::uint8_t i = 0; i < width; ++i) {
            const auto translated = translate_guest_address(address + i, access);
            if (!translated.success) {
                record_translation_exception(translated, address + i);
                last_memory_fault_ = std::string("Cross-page virtual read fault: ") +
                    x64_page_fault_name(translated.fault) + " - " + translated.detail;
                if (translated.has_page_fault) {
                    last_memory_fault_ += " (#PF error code=" + std::to_string(translated.page_fault_error_code) + ")";
                } else if (translated.exception_vector >= 0) {
                    last_memory_fault_ += " (exception vector=" + std::to_string(translated.exception_vector) + ")";
                }
                return std::nullopt;
            }
            const auto byte = memory_.read(translated.physical_address, 1);
            if (!byte) {
                last_memory_fault_ = "Translated physical read is outside mapped guest memory.";
                return std::nullopt;
            }
            value |= (*byte & 0xFFULL) << (static_cast<unsigned>(i) * 8U);
        }
        return value;
    }

    bool write_guest_memory(std::uint64_t address, std::uint8_t width, std::uint64_t value) {
        last_memory_fault_.clear();
        if (!effective_paging_enabled()) return memory_.write(address, width, value);
        if (address > (std::numeric_limits<std::uint64_t>::max)() - (static_cast<std::uint64_t>(width) - 1ULL)) {
            last_memory_fault_ = "Virtual memory write address overflows.";
            return false;
        }
        const std::uint64_t bytes_remaining = 4096ULL - (address & 0xFFFULL);
        if (width <= bytes_remaining) {
            const auto translated = translate_guest_address(address, X64AccessType::Write);
            if (!translated.success) {
                record_translation_exception(translated, address);
                last_memory_fault_ = std::string("Virtual write translation fault: ") +
                    x64_page_fault_name(translated.fault) + " - " + translated.detail;
                if (translated.has_page_fault) {
                    last_memory_fault_ += " (#PF error code=" + std::to_string(translated.page_fault_error_code) + ")";
                } else if (translated.exception_vector >= 0) {
                    last_memory_fault_ += " (exception vector=" + std::to_string(translated.exception_vector) + ")";
                }
                return false;
            }
            if (!memory_.write(translated.physical_address, width, value)) {
                last_memory_fault_ = "Translated physical write is outside mapped guest memory.";
                return false;
            }
            return true;
        }
        for (std::uint8_t i = 0; i < width; ++i) {
            const auto translated = translate_guest_address(address + i, X64AccessType::Write);
            if (!translated.success) {
                record_translation_exception(translated, address + i);
                last_memory_fault_ = std::string("Cross-page virtual write fault: ") +
                    x64_page_fault_name(translated.fault) + " - " + translated.detail;
                if (translated.has_page_fault) {
                    last_memory_fault_ += " (#PF error code=" + std::to_string(translated.page_fault_error_code) + ")";
                } else if (translated.exception_vector >= 0) {
                    last_memory_fault_ += " (exception vector=" + std::to_string(translated.exception_vector) + ")";
                }
                return false;
            }
            if (!memory_.write(translated.physical_address, 1, (value >> (static_cast<unsigned>(i) * 8U)) & 0xFFULL)) {
                last_memory_fault_ = "Translated physical write is outside mapped guest memory.";
                return false;
            }
        }
        return true;
    }

    static std::string hex_byte(std::uint8_t value) {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string result;
        result.push_back(digits[(value >> 4U) & 0xFU]);
        result.push_back(digits[value & 0xFU]);
        return result;
    }

    StepResult fail(std::string message) {
        state.fault = std::move(message);
        return StepResult::Fault;
    }

    StepResult raise_general_protection(std::string message) {
        state.last_exception_vector = 13; // #GP
        state.last_exception_error_code = 0; // LIDT's non-canonical base uses selector/index 0.
        state.last_exception_has_error_code = true;
        state.last_page_fault_error_code = 0;
        state.last_fault_was_page_fault = false;
        general_protection_raised_this_step_ = true;
        return fail(std::move(message));
    }

    static bool is_canonical_linear_address(std::uint64_t address) {
        const bool sign = ((address >> 47U) & 1ULL) != 0;
        const std::uint64_t upper = address >> 48U;
        return sign ? upper == 0xFFFFULL : upper == 0;
    }

    StepResult execute_iretq() {
        if (!control_state_ || !control_state_->long_mode_active() || !control_state_->paging_enabled()) {
            return fail("IRETQ requires the modeled long-mode control state and paging path.");
        }
        const std::uint64_t old_rsp = state.registers[4];
        if (old_rsp > (std::numeric_limits<std::uint64_t>::max)() - 24ULL) {
            return fail("IRETQ stack frame address overflows.");
        }
        const auto saved_rip = read_guest_memory(old_rsp, 8, X64AccessType::Read);
        if (!saved_rip) return fail(memory_fault_or("IRETQ could not read saved RIP."));
        const auto saved_cs = read_guest_memory(old_rsp + 8ULL, 8, X64AccessType::Read);
        if (!saved_cs) return fail(memory_fault_or("IRETQ could not read saved CS."));
        const auto saved_flags = read_guest_memory(old_rsp + 16ULL, 8, X64AccessType::Read);
        if (!saved_flags) return fail(memory_fault_or("IRETQ could not read saved RFLAGS."));

        const auto cpl = control_state_->cpl();
        const std::uint64_t expected_cs = 0x08ULL | static_cast<std::uint64_t>(cpl & 3U);
        if (*saved_cs != expected_cs) {
            return fail("IRETQ only supports a same-CPL return to the modeled code selector.");
        }
        if (!is_canonical_linear_address(*saved_rip)) {
            return fail("IRETQ rejected a non-canonical saved RIP.");
        }

        // This phase intentionally models only the same-CPL long-mode return
        // shape (RIP, CS, RFLAGS). It does not validate GDT descriptors, perform
        // privilege changes, or emulate all architectural RFLAGS restrictions.
        state.rip = *saved_rip;
        state.rflags = *saved_flags | 0x2ULL; // Reserved bit 1 reads as one.
        state.registers[4] = old_rsp + 24ULL;
        return StepResult::Executed;
    }

    bool try_deliver_runtime_exception() {
        const bool page_fault_pending =
            page_fault_raised_this_step_ && state.last_fault_was_page_fault &&
            state.last_exception_vector == 14;
        const bool invalid_opcode_pending =
            invalid_opcode_raised_this_step_ && !state.last_fault_was_page_fault &&
            state.last_exception_vector == 6;
        const bool breakpoint_pending =
            breakpoint_raised_this_step_ && !state.last_fault_was_page_fault &&
            state.last_exception_vector == 3;
        const bool general_protection_pending =
            general_protection_raised_this_step_ && !state.last_fault_was_page_fault &&
            state.last_exception_vector == 13 && state.last_exception_has_error_code;
        if ((!page_fault_pending && !invalid_opcode_pending && !breakpoint_pending &&
             !general_protection_pending) ||
            !automatic_exception_delivery_ || !idtr_.has_value() ||
            !control_state_ || !control_state_->long_mode_active() ||
            !control_state_->paging_enabled()) {
            return false;
        }

        const std::uint8_t vector = page_fault_pending ? 14U :
            invalid_opcode_pending ? 6U : breakpoint_pending ? 3U : 13U;
        X64IdtDispatcher dispatcher(memory_, *control_state_);
        X64InterruptFrame frame;
        // #PF/#UD save the faulting instruction address. #BP from INT3 saves
        // the next instruction, allowing a handler to resume without editing RIP.
        frame.rip = breakpoint_pending ? state.rip : instruction_start_rip_;
        frame.rsp = state.registers[4];
        frame.rflags = state.rflags;
        frame.cs = static_cast<std::uint16_t>(0x08U | (control_state_->cpl() & 3U));
        frame.cpl = control_state_->cpl();

        std::optional<std::uint64_t> error_code;
        if (page_fault_pending || general_protection_pending) {
            error_code = static_cast<std::uint64_t>(state.last_exception_error_code);
        }
        const auto delivered = dispatcher.deliver_exception(vector, *idtr_, frame, error_code);
        page_fault_raised_this_step_ = false;
        invalid_opcode_raised_this_step_ = false;
        breakpoint_raised_this_step_ = false;
        general_protection_raised_this_step_ = false;
        if (!delivered.success) {
            const std::string reason = delivered.detail.empty()
                ? "unknown IDT-delivery failure" : delivered.detail;
            const std::string name = page_fault_pending ? "#PF" :
                invalid_opcode_pending ? "#UD" : breakpoint_pending ? "#BP" : "#GP";
            fail("Automatic " + name + " delivery failed: " + reason);
            return false;
        }

        // Apply same-CPL exception entry to the interpreter, then allow the
        // handler to execute through the regular run() loop.
        state.fault.clear();
        state.rip = frame.rip;
        state.registers[4] = frame.rsp;
        state.rflags = frame.rflags;
        return true;
    }

    std::optional<std::uint8_t> fetch8() {
        const auto value = read_guest_memory(state.rip, 1, X64AccessType::Execute);
        if (!value) {
            fail(memory_fault_or("Instruction fetch outside mapped memory at RIP"));
            return std::nullopt;
        }
        ++state.rip;
        return static_cast<std::uint8_t>(*value);
    }

    std::optional<std::uint32_t> fetch32() {
        const auto value = read_guest_memory(state.rip, 4, X64AccessType::Execute);
        if (!value) {
            fail(memory_fault_or("Instruction fetch outside mapped memory at RIP"));
            return std::nullopt;
        }
        state.rip += 4;
        return static_cast<std::uint32_t>(*value);
    }

    std::optional<std::uint64_t> fetch64() {
        const auto low = fetch32();
        if (!low) return std::nullopt;
        const auto high = fetch32();
        if (!high) return std::nullopt;
        return static_cast<std::uint64_t>(*low) | (static_cast<std::uint64_t>(*high) << 32U);
    }

    static std::uint64_t width_mask(bool wide) { return wide ? UINT64_MAX : 0xFFFFFFFFULL; }

    std::uint64_t read_register(unsigned index, bool wide) const {
        const auto value = state.registers[index & 0xFU];
        return wide ? value : (value & 0xFFFFFFFFULL);
    }

    void write_register(unsigned index, bool wide, std::uint64_t value) {
        state.registers[index & 0xFU] = wide ? value : (value & 0xFFFFFFFFULL);
    }

    void set_logic_flags(std::uint64_t value, bool wide) {
        const auto result = value & width_mask(wide);
        state.rflags &= ~arithmetic_flags;
        if (result == 0) state.rflags |= flag_zero;
        if ((result & (wide ? (1ULL << 63U) : (1ULL << 31U))) != 0) state.rflags |= flag_sign;
        std::uint8_t low = static_cast<std::uint8_t>(result);
        bool odd = false;
        for (unsigned i = 0; i < 8; ++i) { odd ^= (low & 1U) != 0; low >>= 1U; }
        if (!odd) state.rflags |= flag_parity;
    }

    std::uint64_t add(std::uint64_t lhs, std::uint64_t rhs, bool wide) {
        const auto mask = width_mask(wide);
        const auto sign = wide ? (1ULL << 63U) : (1ULL << 31U);
        lhs &= mask; rhs &= mask;
        const std::uint64_t raw_sum = lhs + rhs;
        const auto result = raw_sum & mask;
        set_logic_flags(result, wide);
        const bool carry = wide ? (result < lhs) : (raw_sum > mask);
        if (carry) state.rflags |= flag_carry;
        if (((~(lhs ^ rhs) & (lhs ^ result)) & sign) != 0) state.rflags |= flag_overflow;
        return result;
    }

    std::uint64_t sub(std::uint64_t lhs, std::uint64_t rhs, bool wide) {
        const auto mask = width_mask(wide);
        const auto sign = wide ? (1ULL << 63U) : (1ULL << 31U);
        lhs &= mask; rhs &= mask;
        const auto result = (lhs - rhs) & mask;
        set_logic_flags(result, wide);
        if (lhs < rhs) state.rflags |= flag_carry;
        if ((((lhs ^ rhs) & (lhs ^ result)) & sign) != 0) state.rflags |= flag_overflow;
        return result;
    }

    struct RmOperand {
        bool is_register{};
        unsigned register_index{};
        std::uint64_t address{};
    };

    std::optional<RmOperand> decode_rm_operand(std::uint8_t modrm, std::uint8_t rex) {
        const unsigned mod = static_cast<unsigned>(modrm >> 6U);
        const unsigned raw_rm = static_cast<unsigned>(modrm & 7U);
        if (mod == 3U) {
            return RmOperand{true, raw_rm + ((rex & 0x01U) ? 8U : 0U), 0};
        }

        std::uint64_t address = 0;
        if (raw_rm == 4U) {
            const auto sib_raw = fetch8();
            if (!sib_raw) return std::nullopt;
            const unsigned sib = *sib_raw;
            const unsigned scale = sib >> 6U;
            const unsigned raw_index = (sib >> 3U) & 7U;
            const unsigned raw_base = sib & 7U;
            const bool rex_x = (rex & 0x02U) != 0;
            if (raw_index != 4U || rex_x) {
                const unsigned index = raw_index + (rex_x ? 8U : 0U);
                address += state.registers[index] << scale;
            }
            if (mod == 0U && raw_base == 5U) {
                const auto displacement = fetch32();
                if (!displacement) return std::nullopt;
                address += static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(*displacement)));
            } else {
                const unsigned base = raw_base + ((rex & 0x01U) ? 8U : 0U);
                address += state.registers[base];
            }
        } else if (mod == 0U && raw_rm == 5U) {
            // In 64-bit addressing mode, ModRM r/m=101 with mod=00 is RIP-relative.
            const auto displacement = fetch32();
            if (!displacement) return std::nullopt;
            address = state.rip + static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(*displacement)));
            return RmOperand{false, 0, address};
        } else {
            const unsigned base = raw_rm + ((rex & 0x01U) ? 8U : 0U);
            address = state.registers[base];
        }

        if (mod == 1U) {
            const auto displacement = fetch8();
            if (!displacement) return std::nullopt;
            address += static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(*displacement)));
        } else if (mod == 2U) {
            const auto displacement = fetch32();
            if (!displacement) return std::nullopt;
            address += static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(*displacement)));
        }
        return RmOperand{false, 0, address};
    }

    std::optional<std::uint64_t> read_rm(const RmOperand& operand, bool wide) {
        if (operand.is_register) return read_register(operand.register_index, wide);
        const auto value = read_guest_memory(operand.address, static_cast<std::uint8_t>(wide ? 8 : 4), X64AccessType::Read);
        if (!value) {
            fail(memory_fault_or("Data read outside mapped guest memory or rejected by MMIO device"));
            return std::nullopt;
        }
        return value;
    }

    bool write_rm(const RmOperand& operand, bool wide, std::uint64_t value) {
        if (operand.is_register) {
            write_register(operand.register_index, wide, value);
            return true;
        }
        if (!write_guest_memory(operand.address, static_cast<std::uint8_t>(wide ? 8 : 4), value)) {
            fail(memory_fault_or("Data write outside mapped guest memory or rejected by MMIO device"));
            return false;
        }
        return true;
    }

    StepResult execute_group_0f01(std::uint8_t rex) {
        const auto raw = fetch8();
        if (!raw) return StepResult::Fault;
        const std::uint8_t modrm = *raw;
        const unsigned group = static_cast<unsigned>((modrm >> 3U) & 7U);
        const unsigned mod = static_cast<unsigned>(modrm >> 6U);
        if (group != 3U) {
            return fail("Unsupported 0F 01 group extension; only LIDT /3 is modeled.");
        }
        if (mod == 3U) {
            return fail("LIDT requires a memory operand, not a register operand.");
        }
        if (!control_state_ || !control_state_->long_mode_active() ||
            !control_state_->paging_enabled()) {
            return fail("LIDT requires modeled long-mode control state and paging.");
        }
        if (control_state_->cpl() != 0U) {
            return fail("LIDT is privileged and is only modeled at CPL 0.");
        }

        const auto operand = decode_rm_operand(modrm, rex);
        if (!operand) return StepResult::Fault;
        if (operand->is_register) {
            return fail("LIDT requires a memory operand.");
        }
        if (operand->address > (std::numeric_limits<std::uint64_t>::max)() - 9ULL) {
            return fail("LIDT descriptor address overflows.");
        }
        const auto limit = read_guest_memory(operand->address, 2, X64AccessType::Read);
        if (!limit) return fail(memory_fault_or("LIDT could not read the descriptor limit."));
        const auto base = read_guest_memory(operand->address + 2ULL, 8, X64AccessType::Read);
        if (!base) return fail(memory_fault_or("LIDT could not read the descriptor base."));
        if (!is_canonical_linear_address(*base)) {
            // A non-canonical IDTR base is an explicitly modeled #GP(0) in this
            // subset. Keep the previously loaded IDTR active for exception entry.
            return raise_general_protection("General-protection exception (#GP, vector 13, error code 0): LIDT rejected a non-canonical IDTR base.");
        }

        idtr_ = X64Idtr{*base, static_cast<std::uint16_t>(*limit & 0xFFFFULL)};
        automatic_exception_delivery_ = true;
        return StepResult::Executed;
    }

    StepResult execute_modrm(std::uint8_t opcode, std::uint8_t rex) {
        const auto raw = fetch8();
        if (!raw) return StepResult::Fault;
        const std::uint8_t modrm = *raw;
        const bool wide = (rex & 0x08U) != 0;
        const unsigned reg = static_cast<unsigned>((modrm >> 3U) & 7U) + ((rex & 0x04U) ? 8U : 0U);
        const auto operand = decode_rm_operand(modrm, rex);
        if (!operand) return StepResult::Fault;
        const auto reg_value = read_register(reg, wide);

        switch (opcode) {
        case 0x89: // MOV r/m,reg - do not perform a needless read from the destination.
            return write_rm(*operand, wide, reg_value) ? StepResult::Executed : StepResult::Fault;
        case 0x8B: {
            const auto value = read_rm(*operand, wide);
            if (!value) return StepResult::Fault;
            write_register(reg, wide, *value);
            return StepResult::Executed;
        }
        case 0x31: case 0x01: case 0x29: {
            const auto value = read_rm(*operand, wide);
            if (!value) return StepResult::Fault;
            std::uint64_t result = 0;
            if (opcode == 0x31) { result = *value ^ reg_value; set_logic_flags(result, wide); }
            else if (opcode == 0x01) result = add(*value, reg_value, wide);
            else result = sub(*value, reg_value, wide);
            return write_rm(*operand, wide, result) ? StepResult::Executed : StepResult::Fault;
        }
        case 0x33: case 0x03: case 0x2B: {
            const auto value = read_rm(*operand, wide);
            if (!value) return StepResult::Fault;
            std::uint64_t result = 0;
            if (opcode == 0x33) { result = reg_value ^ *value; set_logic_flags(result, wide); }
            else if (opcode == 0x03) result = add(reg_value, *value, wide);
            else result = sub(reg_value, *value, wide);
            write_register(reg, wide, result);
            return StepResult::Executed;
        }
        case 0x39: case 0x3B: {
            const auto value = read_rm(*operand, wide);
            if (!value) return StepResult::Fault;
            if (opcode == 0x39) (void)sub(*value, reg_value, wide);
            else (void)sub(reg_value, *value, wide);
            return StepResult::Executed;
        }
        case 0x85: {
            const auto value = read_rm(*operand, wide);
            if (!value) return StepResult::Fault;
            set_logic_flags(*value & reg_value, wide);
            return StepResult::Executed;
        }
        default:
            return fail("Internal ModRM decoder error");
        }
    }

    StepResult execute_group_immediate(std::uint8_t opcode, std::uint8_t rex) {
        const auto raw = fetch8();
        if (!raw) return StepResult::Fault;
        const std::uint8_t modrm = *raw;
        const unsigned group = static_cast<unsigned>((modrm >> 3U) & 7U);
        if (group != 0U && group != 5U && group != 7U) {
            return fail("Unsupported 81/83 immediate arithmetic group in x86-64 subset");
        }
        const bool wide = (rex & 0x08U) != 0;
        const auto operand = decode_rm_operand(modrm, rex);
        if (!operand) return StepResult::Fault;

        std::uint64_t immediate = 0;
        if (opcode == 0x83) {
            const auto value = fetch8();
            if (!value) return StepResult::Fault;
            immediate = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(*value)));
        } else {
            const auto value = fetch32();
            if (!value) return StepResult::Fault;
            immediate = wide
                ? static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(*value)))
                : static_cast<std::uint64_t>(*value);
        }

        const auto lhs = read_rm(*operand, wide);
        if (!lhs) return StepResult::Fault;
        if (group == 0U) {
            const auto result = add(*lhs, immediate, wide);
            return write_rm(*operand, wide, result) ? StepResult::Executed : StepResult::Fault;
        }
        const auto result = sub(*lhs, immediate, wide);
        if (group == 7U) return StepResult::Executed; // CMP updates flags but does not store the result.
        return write_rm(*operand, wide, result) ? StepResult::Executed : StepResult::Fault;
    }
};

} // namespace someboxone
