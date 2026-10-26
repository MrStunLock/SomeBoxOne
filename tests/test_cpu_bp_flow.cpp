#include "x64_cpu_bp_flow_test.hpp"
#include <iostream>

int main() {
    const auto result = someboxone::run_synthetic_cpu_bp_flow_test();
    if (!result.passed) {
        std::cerr << "SomeBoxOne CPU #BP flow test failed: " << result.detail << '\n';
        return 1;
    }
    if (result.exception_vector != 3 || result.int3_rip != 0x40000DULL ||
        result.saved_rip != 0x40000EULL || result.final_rsp != result.original_rsp ||
        result.exception_frame_rsp + 24ULL != result.original_rsp || result.cr2 != 0) {
        std::cerr << "Unexpected #BP flow metadata.\n";
        return 1;
    }
    std::cout << "SomeBoxOne synthetic CPU #BP flow test passed.\n" << result.detail << '\n';
    return 0;
}
