#include "x64_cpu_ud_flow_test.hpp"
#include <iostream>

int main() {
    const auto result = someboxone::run_synthetic_cpu_ud_flow_test();
    if (!result.passed) {
        std::cerr << "SomeBoxOne CPU #UD flow test failed: " << result.detail << '\n';
        return 1;
    }
    if (result.exception_vector != 6 || result.faulting_rip != 0x40000DULL ||
        result.resumed_rip != 0x40000FULL || result.final_rsp != result.original_rsp ||
        result.exception_frame_rsp + 24ULL != result.original_rsp || result.cr2 != 0) {
        std::cerr << "Unexpected #UD flow metadata.\n";
        return 1;
    }
    std::cout << "SomeBoxOne synthetic CPU #UD flow test passed.\n" << result.detail << '\n';
    return 0;
}
