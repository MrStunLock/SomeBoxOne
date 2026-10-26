#include "boot.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path temp_root = fs::temp_directory_path() / ("someboxone-boot-test-" + std::to_string(stamp));
    fs::create_directories(temp_root);

    const auto missing = someboxone::run_boot_preflight(temp_root / "missing.xvd");
    assert(!missing.source_exists);
    assert(!missing.inspection_completed);
    assert(!missing.guest_execution_started);
    assert(!missing.blocker.empty());
    assert(!missing.stages.empty());
    assert(missing.stages.front().name == "Source file");
    assert(missing.stages.front().status == someboxone::BootStageStatus::Failed);

    const auto input_file = temp_root / "not-an-xbox-image.bin";
    {
        std::ofstream out(input_file, std::ios::binary);
        const char bytes[] = "synthetic test bytes only";
        out.write(bytes, static_cast<std::streamsize>(sizeof(bytes) - 1));
        assert(static_cast<bool>(out));
    }
    const auto result = someboxone::run_boot_preflight(input_file);
    assert(result.source_exists);
    assert(result.inspection_completed);
    assert(result.inspection_exit_code != 0 || !result.inspection_report.empty());
    assert(!result.xvd_header_recognized);
    assert(!result.vbi_marker_reported);
    assert(!result.guest_execution_started);
    assert(result.blocker.find("not implemented") != std::string::npos);
    assert(result.stages.size() >= 10);
    bool found_raw_read_stage = false;
    bool found_control_state_stage = false;
    bool found_paging_stage = false;
    bool found_page_fault_stage = false;
    bool found_blocked_dashboard = false;
    for (const auto& stage : result.stages) {
        if (stage.name == "Bounded raw image byte access" &&
            stage.status == someboxone::BootStageStatus::Passed) found_raw_read_stage = true;
        if (stage.name == "CR0/CR3/CR4/EFER control-state model" &&
            stage.status == someboxone::BootStageStatus::Partial) found_control_state_stage = true;
        if (stage.name == "CPU privileged startup and paging integration" &&
            stage.status == someboxone::BootStageStatus::Partial) found_paging_stage = true;
        if (stage.name == "Architectural page-fault diagnostics" &&
            stage.status == someboxone::BootStageStatus::Partial) found_page_fault_stage = true;
        if (stage.name == "System OS / dashboard entry" &&
            stage.status == someboxone::BootStageStatus::Blocked) found_blocked_dashboard = true;
    }
    assert(found_raw_read_stage);
    assert(found_control_state_stage);
    assert(found_paging_stage);
    assert(found_page_fault_stage);
    assert(found_blocked_dashboard);

    fs::remove_all(temp_root);
    std::cout << "SomeBoxOne boot preflight tests passed (preflight only; no payload executed).\n";
    return 0;
}
