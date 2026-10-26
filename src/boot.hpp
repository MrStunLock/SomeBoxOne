#pragma once

#include "inspector.hpp"
#include "image_source.hpp"
#include "xvd_storage.hpp"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace someboxone {

enum class BootStageStatus { Passed, Partial, Failed, NotChecked, NotImplemented, Blocked };

struct BootStage {
    std::string name;
    BootStageStatus status{BootStageStatus::NotChecked};
    std::string detail;
};

inline const char* boot_stage_status_name(BootStageStatus status) {
    switch (status) {
    case BootStageStatus::Passed: return "PASSED";
    case BootStageStatus::Partial: return "PARTIAL";
    case BootStageStatus::Failed: return "FAILED";
    case BootStageStatus::NotChecked: return "NOT CHECKED";
    case BootStageStatus::NotImplemented: return "NOT IMPLEMENTED";
    case BootStageStatus::Blocked: return "BLOCKED";
    }
    return "UNKNOWN";
}

// Experimental boot-stage interface. The stages make current readiness explicit,
// but this remains a preflight only: user image bytes are never executed.
struct BootPreflightResult {
    bool source_exists{};
    bool inspection_completed{};
    bool xvd_header_recognized{};
    bool vbi_marker_reported{};
    bool guest_execution_started{}; // remains false until a real boot path exists
    int inspection_exit_code{2};
    std::string inspection_report;
    std::string blocker;
    std::vector<BootStage> stages;
};

inline void append_unimplemented_boot_stages(BootPreflightResult& result, const std::string& reason) {
    result.stages.push_back({"XVD payload-to-guest-memory integration", BootStageStatus::NotImplemented,
        "A conservative read-only logical reader exists only for fixed plaintext XVDs, but no XVD volume is mapped into guest RAM; dynamic BAT translation, encrypted payload access and guest disk integration remain incomplete. " + reason});
    result.stages.push_back({"CR0/CR3/CR4/EFER control-state model", BootStageStatus::Partial,
        "A validated synthetic control-state model can activate long mode and feed CR3/NX/WP policy into the CPU subset tests. Limited guest LIDT and same-CPL #PF/#UD/#BP delivery are available only in synthetic runs; firmware register setup, MSR instructions, TLB behavior and privilege transitions remain incomplete."});
    result.stages.push_back({"CPU privileged startup and paging integration", BootStageStatus::Partial,
        "Four-level paging and CR0/CR3/CR4/EFER are connected for synthetic runs, but firmware register setup, TLB behavior, full architectural startup sequencing and privilege transitions remain incomplete."});
    result.stages.push_back({"Architectural page-fault diagnostics", BootStageStatus::Partial,
        "The software walker models Accessed/Dirty updates and x86 #PF error-code bits; the CPU records vector, PFEC and CR2. The synthetic CPU subset can execute a limited CPL-0 LIDT and route #PF, UD2/#UD, or INT3/#BP through that guest-loaded IDTR. Guest boot-time setup, broader exception routing and complete architectural behavior remain incomplete."});
    result.stages.push_back({"IDT gate and same-CPL exception delivery model", BootStageStatus::Partial,
        "Synthetic 64-bit interrupt/trap gate parsing and same-CPL frame construction are tested; CPL-0 guest LIDT, opt-in runtime #PF/#UD/#BP delivery and limited same-CPL IRETQ handler returns are connected for synthetic runs. GDT validation, IST/privilege stack switching, full architectural return checks, double-fault behavior and other exception vectors remain unsupported."});
    result.stages.push_back({"Synthetic CPU-to-IDT flow integration", BootStageStatus::Partial,
        "Generated guest code executes LIDT to load its IDTR. Supported synthetic paths include #PF with an error-code slot, UD2/#UD without one, and INT3/#BP with a saved RIP after the INT3 instruction; each is routed through a same-CPL IDT and exercised by limited IRETQ tests. This is not general exception routing or firmware startup."});
    result.stages.push_back({"Authenticated boot chain / hypervisor handoff", BootStageStatus::NotImplemented,
        "The platform boot chain and privileged handoff are not emulated; signatures are not authenticated by this preflight."});
    result.stages.push_back({"Interrupts, timers and platform devices", BootStageStatus::NotImplemented,
        "Interrupt delivery, timers and Xbox platform devices are not yet modelled."});
    result.stages.push_back({"GPU, display and OS services", BootStageStatus::NotImplemented,
        "No Xbox GPU/display path or required OS service environment is present."});
    result.stages.push_back({"System OS / dashboard entry", BootStageStatus::Blocked,
        "Guest execution was not started because required boot layers remain incomplete."});
}

inline BootPreflightResult run_boot_preflight(const fs::path& source) {
    BootPreflightResult result;
    std::error_code ec;
    result.source_exists = fs::is_regular_file(source, ec) && !ec;
    if (!result.source_exists) {
        result.blocker = "Selected source is missing, inaccessible, or not a regular file.";
        result.stages.push_back({"Source file", BootStageStatus::Failed, result.blocker});
        result.stages.push_back({"Bounded raw image byte access", BootStageStatus::Blocked,
            "Not run because source validation failed."});
        result.stages.push_back({"Read-only image inspection", BootStageStatus::Blocked,
            "Not run because source validation failed."});
        result.stages.push_back({"Image classification", BootStageStatus::NotChecked,
            "No source bytes were read."});
        result.stages.push_back({"First hash-tree page digest", BootStageStatus::NotChecked,
            "No source bytes were read."});
        append_unimplemented_boot_stages(result, "Source validation failed before data access.");
        return result;
    }

    result.stages.push_back({"Source file", BootStageStatus::Passed,
        "A regular file exists and is accessible for read-only inspection."});
    std::string source_error;
    const auto raw_source = ReadOnlyImageSource::open(source, &source_error);
    if (!raw_source) {
        result.stages.push_back({"Bounded raw image byte access", BootStageStatus::Failed,
            "Read-only byte source could not open the file: " + source_error});
    } else if (raw_source->size() == 0) {
        result.stages.push_back({"Bounded raw image byte access", BootStageStatus::Failed,
            "Source is an empty file; there are no physical bytes to read."});
    } else {
        const auto probe_size = raw_source->size() < 4096ULL ? raw_source->size() : 4096ULL;
        const auto probe = raw_source->read_at(0, probe_size, &source_error);
        if (probe) {
            result.stages.push_back({"Bounded raw image byte access", BootStageStatus::Passed,
                "Read " + std::to_string(probe->size()) + " physical file bytes through the bounded read-only source. This is not decrypted/logical XVD payload access."});
        } else {
            result.stages.push_back({"Bounded raw image byte access", BootStageStatus::Failed,
                "Read-only byte probe failed: " + source_error});
        }
    }
    try {
        const auto inspected = inspect_file(source);
        result.inspection_completed = true;
        result.inspection_exit_code = inspected.exit_code;
        result.inspection_report = inspected.report;
    } catch (const std::exception& ex) {
        result.blocker = std::string("Read-only image inspection threw an exception: ") + ex.what();
        result.stages.push_back({"Read-only image inspection", BootStageStatus::Failed, result.blocker});
        result.stages.push_back({"Image classification", BootStageStatus::NotChecked,
            "Classification could not be established because inspection failed."});
        result.stages.push_back({"First hash-tree page digest", BootStageStatus::NotChecked,
            "Integrity probe could not run because inspection failed."});
        append_unimplemented_boot_stages(result, "Image inspection did not complete.");
        return result;
    }

    result.stages.push_back({"Read-only image inspection", BootStageStatus::Passed,
        "Inspector completed; exit_code=" + std::to_string(result.inspection_exit_code) + "."});
    result.xvd_header_recognized = result.inspection_report.find("XVD-compatible container header ('msft-xvd')") != std::string::npos;
    result.vbi_marker_reported = result.inspection_report.find("Classification: Virtual Boot Image (VBI) signature") != std::string::npos;
    if (result.xvd_header_recognized) {
        result.stages.push_back({"Image classification", BootStageStatus::Passed,
            "XVD-compatible header marker detected; authenticity and bootability are not established."});
    } else if (result.vbi_marker_reported) {
        result.stages.push_back({"Image classification", BootStageStatus::Partial,
            "VBI-related signature was reported; executable VBI support is not implemented."});
    } else {
        result.stages.push_back({"Image classification", BootStageStatus::Blocked,
            "No currently recognized bootable image marker was established."});
    }

    if (result.inspection_report.find("Top hash-tree page SHA-256 check: MATCH") != std::string::npos) {
        result.stages.push_back({"First hash-tree page digest", BootStageStatus::Passed,
            "The inspector reports a single-page SHA-256 comparison match; this is not full tree or signature verification."});
    } else if (result.inspection_report.find("Top hash-tree page SHA-256 check: MISMATCH") != std::string::npos) {
        result.stages.push_back({"First hash-tree page digest", BootStageStatus::Failed,
            "The inspector reports a mismatch for the sampled hash-tree page. No guest execution was attempted."});
    } else {
        result.stages.push_back({"First hash-tree page digest", BootStageStatus::NotChecked,
            "No applicable result was emitted by the current image inspector."});
    }

    if (result.xvd_header_recognized) {
        const auto storage = ReadOnlyXvdVolume::probe(source);
        if (storage.logical_read_supported) {
            result.stages.push_back({"Read-only XVD logical-volume probe",
                storage.data_integrity_enabled ? BootStageStatus::Partial : BootStageStatus::Passed,
                storage.detail});
        } else {
            result.stages.push_back({"Read-only XVD logical-volume probe",
                storage.header_recognized ? BootStageStatus::NotImplemented : BootStageStatus::Failed,
                storage.detail});
        }
    } else {
        result.stages.push_back({"Read-only XVD logical-volume probe", BootStageStatus::NotChecked,
            "The selected image did not have a recognised XVD header; no XVD logical-volume read was attempted."});
    }

    result.guest_execution_started = false;
    result.blocker = "Firmware execution is not implemented. The CPU/paging path is only an opt-in synthetic-test mode and is not connected to guest memory backed by a validated XVD logical volume; authenticated boot-chain, hypervisor, interrupts/timers, system services, and GPU/display support are incomplete.";
    append_unimplemented_boot_stages(result, "Image metadata was inspected only; payload access is not implemented.");
    return result;
}

} // namespace someboxone
