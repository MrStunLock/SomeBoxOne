#include "x64_bootstrap.hpp"

#include <iostream>

int main() {
    const auto result = someboxone::run_synthetic_page_fault_test();
    if (!result.passed) {
        std::cerr << "SomeBoxOne page-fault recording test failed: " << result.detail << '\n';
        return 1;
    }
    if (result.exception_vector != 14 || result.error_code != 0x4U || result.cr2 != 0x500000ULL) {
        std::cerr << "Unexpected architectural fault metadata\n";
        return 1;
    }
    std::cout << "SomeBoxOne synthetic #PF/#GP metadata tests passed (fault recorded, not delivered).\n";
    return 0;
}
