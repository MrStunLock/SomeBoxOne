#include "x64_idt_test.hpp"

#include <iostream>

int main() {
    const auto result = someboxone::run_synthetic_idt_delivery_test();
    if (!result.passed) {
        std::cerr << "SomeBoxOne synthetic IDT delivery test failed: " << result.detail << '\n';
        return 1;
    }
    if (result.handler_rip != 0x9000ULL || result.old_rsp - result.new_rsp != 32ULL) {
        std::cerr << "Unexpected handler or exception-frame values\n";
        return 1;
    }
    std::cout << "SomeBoxOne synthetic IDT delivery tests passed (same-CPL only; no firmware execution).\n";
    return 0;
}
