#include "cpu_x64.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    using someboxone::GuestAddressSpace;
    using someboxone::X64CpuSubset;

    GuestAddressSpace memory(4096);
    if (!memory.map_ram(0x1000, 0x100, "test code and data")) {
        std::cerr << "Could not map test memory\n";
        return 1;
    }

    // MOV RAX,42; MOV RBX,8; ADD RAX,RBX; XOR RCX,RCX;
    // JZ over MOV EAX,1; HLT. This is intentionally a synthetic test payload.
    const std::vector<std::uint8_t> program = {
        0x48, 0xB8, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0xBB, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x01, 0xD8,
        0x48, 0x31, 0xC9,
        0x74, 0x05,
        0xB8, 0x01, 0x00, 0x00, 0x00,
        0xF4
    };
    for (std::size_t i = 0; i < program.size(); ++i) {
        if (!memory.write(0x1000 + i, 1, program[i])) {
            std::cerr << "Could not load test payload\n";
            return 1;
        }
    }

    X64CpuSubset cpu(memory, 0x1000);
    if (cpu.run(32) != X64CpuSubset::RunResult::Halted) {
        std::cerr << "Synthetic CPU payload did not halt: " << cpu.state.fault << '\n';
        return 1;
    }
    if (cpu.state.registers[0] != 50 || cpu.state.registers[3] != 8 || cpu.state.registers[1] != 0) {
        std::cerr << "Register/arithmetic execution mismatch\n";
        return 1;
    }
    if ((cpu.state.rflags & (1ULL << 6U)) == 0) {
        std::cerr << "XOR should have set the zero flag\n";
        return 1;
    }

    // Unsupported opcodes must stop with an explicit fault instead of being skipped.
    if (!memory.write(0x1040, 1, 0xFF)) return 1;
    X64CpuSubset unsupported(memory, 0x1040);
    if (unsupported.step() != X64CpuSubset::StepResult::Fault || unsupported.state.fault.empty()) {
        std::cerr << "Unsupported instruction was not reported as a fault\n";
        return 1;
    }

    // Instruction fetch outside mapped memory should be a clean guest fault.
    X64CpuSubset out_of_range(memory, 0x9000);
    if (out_of_range.step() != X64CpuSubset::StepResult::Fault) {
        std::cerr << "Unmapped instruction fetch was not trapped\n";
        return 1;
    }

    // Step limit should stop an infinite NOP stream without hanging the host.
    for (std::uint64_t i = 0; i < 8; ++i) {
        if (!memory.write(0x1080 + i, 1, 0x90)) return 1;
    }
    X64CpuSubset limited(memory, 0x1080);
    if (limited.run(4) != X64CpuSubset::RunResult::StepLimit || limited.state.rip != 0x1084) {
        std::cerr << "CPU instruction step limit failed\n";
        return 1;
    }


    // Exercise common long-mode memory and stack forms on synthetic code:
    // - RIP-relative MOV to memory
    // - base+disp8 memory access
    // - SIB base+scaled-index+disp8 access
    // - CMP/JZ control flow
    // - PUSH/POP and ADD/SUB immediate group encodings (81/83 subset)
    GuestAddressSpace memory2(4096);
    if (!memory2.map_ram(0x2000, 0x400, "memory addressing and stack test")) {
        std::cerr << "Could not map memory-addressing test RAM\n";
        return 1;
    }
    constexpr std::uint64_t magic = 0x1122334455667788ULL;
    const std::vector<std::uint8_t> memory_program = {
        0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // mov rax, magic
        0x48, 0xBA, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rdx, 0x2200
        0x48, 0x89, 0x05, 0xE5, 0x01, 0x00, 0x00,                   // mov [rip+0x1e5], rax -> [0x2200]
        0x48, 0x8B, 0x5A, 0x00,                                     // mov rbx, [rdx+0]
        0x48, 0x39, 0xD8,                                           // cmp rax, rbx
        0x74, 0x05,                                                 // jz over failure assignment
        0xB8, 0x01, 0x00, 0x00, 0x00,                               // mov eax, 1 (failure path)
        0x48, 0xB9, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rcx, 1
        0x48, 0x89, 0x5C, 0x8A, 0x08,                               // mov [rdx+rcx*4+8], rbx
        0x48, 0x8B, 0x4C, 0x8A, 0x08,                               // mov rcx, [rdx+rcx*4+8]
        0x48, 0xBC, 0xF0, 0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rsp, 0x23f0
        0x48, 0x83, 0xEC, 0x08,                                     // sub rsp, 8
        0x50,                                                       // push rax
        0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rbx, 0
        0x5B,                                                       // pop rbx
        0x48, 0x83, 0xC4, 0x08,                                     // add rsp, 8
        0x48, 0x39, 0xD8,                                           // cmp rax, rbx
        0xF4                                                        // hlt
    };
    for (std::size_t i = 0; i < memory_program.size(); ++i) {
        if (!memory2.write(0x2000 + i, 1, memory_program[i])) {
            std::cerr << "Could not load memory-addressing test program\n";
            return 1;
        }
    }
    X64CpuSubset memory_cpu(memory2, 0x2000);
    if (memory_cpu.run(128) != X64CpuSubset::RunResult::Halted) {
        std::cerr << "Memory/stack instruction test did not halt: " << memory_cpu.state.fault << '\n';
        return 1;
    }
    if (memory_cpu.state.registers[0] != magic || memory_cpu.state.registers[3] != magic ||
        memory_cpu.state.registers[1] != magic || memory_cpu.state.registers[4] != 0x23F0 ||
        memory2.read(0x2200, 8) != magic || memory2.read(0x220C, 8) != magic ||
        (memory_cpu.state.rflags & (1ULL << 6U)) == 0) {
        std::cerr << "Memory operands, stack, or compare/branch execution mismatch\n";
        return 1;
    }

    // Data references outside mapped guest RAM must fault cleanly.
    const std::vector<std::uint8_t> bad_data_program{0x48, 0x8B, 0x00}; // mov rax, [rax]
    for (std::size_t i = 0; i < bad_data_program.size(); ++i) {
        if (!memory2.write(0x2100 + i, 1, bad_data_program[i])) return 1;
    }
    X64CpuSubset bad_data(memory2, 0x2100);
    bad_data.state.registers[0] = 0x90000000;
    if (bad_data.step() != X64CpuSubset::StepResult::Fault || bad_data.state.fault.empty()) {
        std::cerr << "Unmapped data operand was not trapped\n";
        return 1;
    }


    // End-to-end synthetic CPU execution with instruction fetches, data reads/writes,
    // and stack access translated through the new four-level page-table walker.
    GuestAddressSpace virtual_memory(1024 * 1024);
    if (!virtual_memory.map_ram(0, 1024 * 1024, "virtual CPU paging integration RAM")) {
        std::cerr << "Could not map virtual CPU integration RAM\n";
        return 1;
    }
    const bool page_tables_written =
        virtual_memory.write(0x1000, 8, 0x2007) &&       // PML4[0] -> PDPT
        virtual_memory.write(0x2000, 8, 0x3007) &&       // PDPT[0] -> PD
        virtual_memory.write(0x3010, 8, 0x4007) &&       // PD[2] -> PT
        virtual_memory.write(0x4000, 8, 0x10007) &&      // VA 0x400000 -> code PA 0x10000
        virtual_memory.write(0x4008, 8, 0x12007 | (1ULL << 63U)) && // VA 0x401000 -> data, NX
        virtual_memory.write(0x4010, 8, 0x15007 | (1ULL << 63U)) && // VA 0x402000 -> next half of cross-page data, NX
        virtual_memory.write(0x4018, 8, 0x14007 | (1ULL << 63U));   // VA 0x403000 -> stack, NX
    if (!page_tables_written) {
        std::cerr << "Could not create virtual CPU page tables\n";
        return 1;
    }
    const std::vector<std::uint8_t> virtual_program = {
        0x48, 0xB8, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rax, 42
        0x48, 0xBA, 0xFC, 0x1F, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rdx, 0x401ffc (cross-page data)
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
        0xF4                                                        // halt
    };
    for (std::size_t i = 0; i < virtual_program.size(); ++i) {
        if (!virtual_memory.write(0x10000 + i, 1, virtual_program[i])) {
            std::cerr << "Could not load virtual CPU program\n";
            return 1;
        }
    }
    X64CpuSubset virtual_cpu(virtual_memory, 0x400000, 0x1000, someboxone::X64PrivilegeLevel::User);
    if (virtual_cpu.run(128) != X64CpuSubset::RunResult::Halted) {
        std::cerr << "CPU with paging enabled failed: " << virtual_cpu.state.fault << '\n';
        return 1;
    }
    if (virtual_cpu.state.registers[0] != 42 || virtual_cpu.state.registers[3] != 42 ||
        virtual_cpu.state.registers[4] != 0x403FF0 || virtual_memory.read(0x12FFC, 4) != 42 ||
        virtual_memory.read(0x15000, 4) != 0) {
        std::cerr << "CPU virtual instruction/data/stack/cross-page translation mismatch\n";
        return 1;
    }

    // The same execution path must fault when instruction fetch hits an NX page.
    if (!virtual_memory.write(0x4000, 8, 0x10007 | (1ULL << 63U))) return 1;
    X64CpuSubset nx_code_cpu(virtual_memory, 0x400000, 0x1000, someboxone::X64PrivilegeLevel::User);
    if (nx_code_cpu.step() != X64CpuSubset::StepResult::Fault ||
        nx_code_cpu.state.fault.find("ExecuteAccessDenied") == std::string::npos ||
        nx_code_cpu.state.last_exception_vector != 14 ||
        nx_code_cpu.state.last_page_fault_error_code != 0x15U ||
        nx_code_cpu.state.cr2 != 0x400000ULL ||
        !nx_code_cpu.state.last_fault_was_page_fault) {
        std::cerr << "CPU instruction fetch did not honor NX/#PF metadata: " << nx_code_cpu.state.fault << '\n';
        return 1;
    }

    return 0;
}
