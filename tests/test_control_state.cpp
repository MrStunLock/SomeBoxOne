#include "x64_control.hpp"
#include "x64_bootstrap.hpp"

#include <cassert>
#include <iostream>

using namespace someboxone;

int main() {
    X64ControlState state;
    assert(!state.long_mode_active());
    assert(state.write_efer(X64ControlState::efer_lme | X64ControlState::efer_nxe));
    assert(!state.write_cr0(X64ControlState::cr0_pe | X64ControlState::cr0_pg | X64ControlState::cr0_wp));
    assert(state.last_error().find("CR4.PAE") != std::string::npos);

    assert(state.write_cr4(X64ControlState::cr4_pae));
    assert(state.write_cr3(0x1000));
    assert(state.write_cr0(X64ControlState::cr0_pe | X64ControlState::cr0_pg | X64ControlState::cr0_wp | (1ULL << 4U)));
    assert(state.long_mode_active());
    assert(state.paging_enabled());
    assert(state.nx_enabled());
    assert(state.write_protect_enabled());

    assert(!state.write_cr4(0)); // PAE cannot be cleared while LMA is set.
    assert(!state.write_efer(X64ControlState::efer_nxe)); // LME cannot be cleared while LMA is set.
    assert(!state.write_cr3(0x1001)); // unsupported low CR3 bits (no PCID).
    assert(!state.write_cr4(1ULL << 12U)); // LA57 is not implemented.
    assert(!state.set_cpl(4));
    assert(state.set_cpl(3));
    assert(state.cpl() == 3);

    // Disabling paging clears LMA, after which LME may be changed.
    assert(state.write_cr0(X64ControlState::cr0_pe | X64ControlState::cr0_wp | (1ULL << 4U)));
    assert(!state.long_mode_active());
    assert(state.write_efer(X64ControlState::efer_nxe));

    const auto smoke = run_synthetic_long_mode_test();
    if (!smoke.passed) {
        std::cerr << "Synthetic long-mode integration test failed: " << smoke.detail << '\n';
        return 1;
    }
    std::cout << "SomeBoxOne control-state and integrated long-mode tests passed. " << smoke.detail << '\n';
    return 0;
}
