#include "x64_cpu_gp_lidt_flow_test.hpp"

#include <iostream>

int main() {
    const auto result = someboxone::run_synthetic_cpu_gp_lidt_flow_test();
    if (!result.passed) {
        std::cerr << "SomeBoxOne CPU #GP/LIDT flow test failed: " << result.detail << '\n';
        return 1;
    }
    if (result.exception_vector != 13 || result.error_code != 0 || !result.has_error_code ||
        result.faulting_rip != 0x400017ULL || result.resumed_rip != 0x40001AULL ||
        result.handler_rip != 0x9000ULL || result.original_rsp - result.exception_frame_rsp != 32ULL ||
        result.final_rsp != result.original_rsp || result.previous_idtr_base != 0x18000ULL ||
        result.previous_idtr_limit != 0x0FFFU ||
        result.cpu_run_result != someboxone::X64CpuSubset::RunResult::Halted) {
        std::cerr << "Unexpected CPU #GP/LIDT integration metadata\n";
        return 1;
    }
    std::cout << "SomeBoxOne synthetic CPU #GP/LIDT flow test passed.\n"
              << result.detail << '\n';
    return 0;
}
