#include "inspector.hpp"
#include "logging.hpp"
#include "x64_bootstrap.hpp"
#include "x64_idt_test.hpp"
#include "x64_cpu_idt_flow_test.hpp"
#include "x64_cpu_ud_flow_test.hpp"
#include "x64_cpu_bp_flow_test.hpp"
#include "x64_cpu_gp_lidt_flow_test.hpp"
#include "xvd_storage.hpp"
#include "image_source.hpp"
#include "title_launch.hpp"
#include "runtime_config.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <fstream>
#include <iostream>
#include <string>

static void usage(const char* exe) {
    std::cout << "Usage:\n  " << exe << " inspect <firmware-or-image-file>\n  "
              << exe << " scan <OSU-or-restore-folder>\n  " << exe << " long-mode-test\n  "
              << exe << " page-fault-test\n  "
              << exe << " idt-test\n  "
              << exe << " cpu-idt-flow-test\n  "
              << exe << " cpu-ud-flow-test\n  "
              << exe << " cpu-bp-flow-test\n  "
              << exe << " cpu-gp-lidt-flow-test\n  "
              << exe << " title-info <title-folder-or-exe>\n  "
              << exe << " launch-title <title-folder-or-exe> [--max-instructions N] [--max-frames N] [--save-every N] [--input SCRIPT] [--no-gamepad] [--fps-limit 30|60] [--show-fps] [--no-audio] [--audio-volume 0..1] [--audio-wav FILE] [--strict-winrt] [--trace-hle] [--report FILE]\n  "
              << exe << " xvd-probe <image-file>\n  "
              << exe << " xvd-read <image-file> <logical-offset> <byte-count>\n  "
              << exe << " read-raw <image-file> <offset> <byte-count>\n  " << exe << " --help\n\n"
              << "SomeBoxOne v" << someboxone::version_string << " is a read-only image-inspection research tool for an eventual\n"
              << "Xbox One OS-only emulation project. The GUI separates Games metadata, Firmware preflight and Debug / Scan; this build cannot boot firmware. launch-title runs an already-unpacked title through an early user-mode HLE runner (see docs/TITLE_LAUNCH_v0.22.0.md).\n"
              << "CLI invocations and operations are recorded in the persistent SomeBoxOne activity log.\n";
}

static std::string join_arguments(int argc, char** argv) {
    std::string command;
    for (int i = 1; i < argc; ++i) {
        if (!command.empty()) command += ' ';
        command += argv[i];
    }
    return command.empty() ? "(no arguments)" : command;
}

int main(int argc, char** argv) {
    const auto runtime_config = someboxone::load_runtime_config();
    someboxone::set_persistent_logging_enabled(runtime_config.keep_logs);
    someboxone::log_event("INFO", "CLI_INVOCATION", join_arguments(argc, argv));
    int exit_code = 0;
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        usage(argv[0]);
    } else if (argc == 3 && std::string(argv[1]) == "inspect") {
        someboxone::log_event("INFO", "CLI_INSPECT_BEGIN", argv[2]);
        try {
            const auto result = someboxone::inspect_file(argv[2]);
            std::cout << result.report;
            exit_code = result.exit_code;
            someboxone::log_event(exit_code == 0 ? "INFO" : "WARN", "CLI_INSPECT_COMPLETE",
                                  std::string("path=") + argv[2] + "; exit_code=" + std::to_string(exit_code));
        } catch (const std::exception& ex) {
            std::cerr << "Inspection failed: " << ex.what() << '\n';
            someboxone::log_event("ERROR", "CLI_INSPECT_EXCEPTION", std::string("path=") + argv[2] + "; error=" + ex.what());
            exit_code = 1;
        }
    } else if (argc == 2 && std::string(argv[1]) == "long-mode-test") {
        someboxone::log_event("INFO", "CLI_LONG_MODE_TEST_BEGIN", "Synthetic code and page tables only; no firmware bytes executed.");
        const auto result = someboxone::run_synthetic_long_mode_test();
        std::cout << (result.passed ? "PASS - " : "FAIL - ") << result.detail << '\n';
        std::cout << "RAX=" << someboxone::hex_value(result.rax) << "; RIP=" << someboxone::hex_value(result.rip)
                  << "; RSP=" << someboxone::hex_value(result.rsp) << '\n';
        someboxone::log_event(result.passed ? "INFO" : "ERROR",
            result.passed ? "CLI_LONG_MODE_TEST_PASSED" : "CLI_LONG_MODE_TEST_FAILED", result.detail);
        exit_code = result.passed ? 0 : 1;
    } else if (argc == 2 && std::string(argv[1]) == "page-fault-test") {
        someboxone::log_event("INFO", "CLI_PAGE_FAULT_TEST_BEGIN",
            "Synthetic user read from a non-present page; no firmware bytes executed.");
        const auto result = someboxone::run_synthetic_page_fault_test();
        std::cout << (result.passed ? "PASS - " : "FAIL - ") << result.detail << '\n';
        std::cout << "Vector=" << result.exception_vector
                  << "; #PFEC=" << someboxone::hex_value(result.error_code)
                  << "; CR2=" << someboxone::hex_value(result.cr2) << '\n';
        someboxone::log_event(result.passed ? "INFO" : "ERROR",
            result.passed ? "CLI_PAGE_FAULT_TEST_PASSED" : "CLI_PAGE_FAULT_TEST_FAILED",
            result.detail + "; vector=" + std::to_string(result.exception_vector) +
            "; PFEC=" + someboxone::hex_value(result.error_code) +
            "; CR2=" + someboxone::hex_value(result.cr2));
        exit_code = result.passed ? 0 : 1;
    } else if (argc == 2 && std::string(argv[1]) == "idt-test") {
        someboxone::log_event("INFO", "CLI_IDT_TEST_BEGIN",
            "Synthetic 64-bit IDT gate and same-CPL exception frame; no firmware executed.");
        const auto result = someboxone::run_synthetic_idt_delivery_test();
        std::cout << (result.passed ? "PASS - " : "FAIL - ") << result.detail << '\n';
        std::cout << "Handler RIP=" << someboxone::hex_value(result.handler_rip)
                  << "; old RSP=" << someboxone::hex_value(result.old_rsp)
                  << "; new RSP=" << someboxone::hex_value(result.new_rsp) << '\n';
        someboxone::log_event(result.passed ? "INFO" : "ERROR",
            result.passed ? "CLI_IDT_TEST_PASSED" : "CLI_IDT_TEST_FAILED",
            result.detail + "; handler=" + someboxone::hex_value(result.handler_rip) +
            "; stack_delta=" + std::to_string(result.old_rsp - result.new_rsp));
        exit_code = result.passed ? 0 : 1;
    } else if (argc == 2 && std::string(argv[1]) == "cpu-idt-flow-test") {
        someboxone::log_event("INFO", "CLI_CPU_IDT_FLOW_TEST_BEGIN",
            "Synthetic runtime #PF -> configured same-CPL IDT -> PTE repair -> IRETQ retry; no firmware executed.");
        const auto result = someboxone::run_synthetic_cpu_idt_flow_test();
        std::cout << (result.passed ? "PASS - " : "FAIL - ") << result.detail << '\n';
        std::cout << "Vector=" << result.exception_vector
                  << "; PFEC=" << someboxone::hex_value(result.page_fault_error_code)
                  << "; CR2=" << someboxone::hex_value(result.cr2)
                  << "; handler=" << someboxone::hex_value(result.handler_rip)
                  << "; final RSP=" << someboxone::hex_value(result.final_rsp)
                  << "; resumed value=" << someboxone::hex_value(result.resumed_value) << '\n';
        someboxone::log_event(result.passed ? "INFO" : "ERROR",
            result.passed ? "CLI_CPU_IDT_FLOW_TEST_PASSED" : "CLI_CPU_IDT_FLOW_TEST_FAILED",
            result.detail + "; vector=" + std::to_string(result.exception_vector) +
            "; PFEC=" + someboxone::hex_value(result.page_fault_error_code) +
            "; CR2=" + someboxone::hex_value(result.cr2));
        exit_code = result.passed ? 0 : 1;
    } else if (argc == 2 && std::string(argv[1]) == "cpu-ud-flow-test") {
        someboxone::log_event("INFO", "CLI_CPU_UD_FLOW_TEST_BEGIN",
            "Synthetic guest LIDT -> UD2/#UD -> same-CPL IDT -> IRETQ skip -> HLT; no firmware executed.");
        const auto result = someboxone::run_synthetic_cpu_ud_flow_test();
        std::cout << (result.passed ? "PASS - " : "FAIL - ") << result.detail << '\n';
        std::cout << "Vector=" << result.exception_vector
                  << "; fault RIP=" << someboxone::hex_value(result.faulting_rip)
                  << "; resumed RIP=" << someboxone::hex_value(result.resumed_rip)
                  << "; final RSP=" << someboxone::hex_value(result.final_rsp)
                  << "; CR2=" << someboxone::hex_value(result.cr2) << '\n';
        someboxone::log_event(result.passed ? "INFO" : "ERROR",
            result.passed ? "CLI_CPU_UD_FLOW_TEST_PASSED" : "CLI_CPU_UD_FLOW_TEST_FAILED",
            result.detail + "; vector=" + std::to_string(result.exception_vector) +
            "; fault_rip=" + someboxone::hex_value(result.faulting_rip) +
            "; resumed_rip=" + someboxone::hex_value(result.resumed_rip) +
            "; final_rsp=" + someboxone::hex_value(result.final_rsp));
        exit_code = result.passed ? 0 : 1;
    } else if (argc == 2 && std::string(argv[1]) == "cpu-bp-flow-test") {
        someboxone::log_event("INFO", "CLI_CPU_BP_FLOW_TEST_BEGIN",
            "Synthetic guest LIDT -> INT3/#BP (vector 3, no error code) -> same-CPL IDT -> IRETQ; no firmware executed.");
        const auto result = someboxone::run_synthetic_cpu_bp_flow_test();
        std::cout << (result.passed ? "PASS - " : "FAIL - ") << result.detail << '\n';
        std::cout << "Vector=" << result.exception_vector
                  << "; INT3 RIP=" << someboxone::hex_value(result.int3_rip)
                  << "; saved RIP=" << someboxone::hex_value(result.saved_rip)
                  << "; final RSP=" << someboxone::hex_value(result.final_rsp)
                  << "; CR2=" << someboxone::hex_value(result.cr2) << '\n';
        someboxone::log_event(result.passed ? "INFO" : "ERROR",
            result.passed ? "CLI_CPU_BP_FLOW_TEST_PASSED" : "CLI_CPU_BP_FLOW_TEST_FAILED",
            result.detail + "; vector=" + std::to_string(result.exception_vector) +
            "; int3_rip=" + someboxone::hex_value(result.int3_rip) +
            "; saved_rip=" + someboxone::hex_value(result.saved_rip) +
            "; final_rsp=" + someboxone::hex_value(result.final_rsp));
        exit_code = result.passed ? 0 : 1;
    } else if (argc == 2 && std::string(argv[1]) == "cpu-gp-lidt-flow-test") {
        someboxone::log_event("INFO", "CLI_CPU_GP_LIDT_FLOW_TEST_BEGIN",
            "Synthetic guest LIDT with non-canonical base -> #GP(0) through previous IDT -> same-CPL IRETQ; no firmware executed.");
        const auto result = someboxone::run_synthetic_cpu_gp_lidt_flow_test();
        std::cout << (result.passed ? "PASS - " : "FAIL - ") << result.detail << '\n';
        std::cout << "Vector=" << result.exception_vector
                  << "; error code=" << someboxone::hex_value(result.error_code)
                  << "; error slot=" << (result.has_error_code ? "present" : "absent")
                  << "; fault RIP=" << someboxone::hex_value(result.faulting_rip)
                  << "; resumed RIP=" << someboxone::hex_value(result.resumed_rip)
                  << "; previous IDTR base=" << someboxone::hex_value(result.previous_idtr_base)
                  << "; final RSP=" << someboxone::hex_value(result.final_rsp) << '\n';
        someboxone::log_event(result.passed ? "INFO" : "ERROR",
            result.passed ? "CLI_CPU_GP_LIDT_FLOW_TEST_PASSED" : "CLI_CPU_GP_LIDT_FLOW_TEST_FAILED",
            result.detail + "; vector=" + std::to_string(result.exception_vector) +
            "; error_code=" + someboxone::hex_value(result.error_code) +
            "; fault_rip=" + someboxone::hex_value(result.faulting_rip) +
            "; resumed_rip=" + someboxone::hex_value(result.resumed_rip));
        exit_code = result.passed ? 0 : 1;
    } else if ((argc == 3) && std::string(argv[1]) == "title-info") {
        const auto info = someboxone::inspect_title_package(argv[2]);
        std::cout << info.report();
        someboxone::log_event(info.ok ? "INFO" : "WARN", "CLI_TITLE_INFO", std::string("path=") + argv[2] + "; ok=" + (info.ok ? "true" : "false"));
        exit_code = info.ok ? 0 : 2;
    } else if (argc >= 3 && std::string(argv[1]) == "launch-title") {
        someboxone::TitleLaunchOptions options;
        std::string report_path;
        bool bad = false;
        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--max-instructions" && i + 1 < argc) options.max_instructions = std::stoull(argv[++i], nullptr, 0);
            else if (a == "--max-frames" && i + 1 < argc) options.max_frames = std::stoull(argv[++i], nullptr, 0);
            else if (a == "--save-every" && i + 1 < argc) options.save_every = std::stoull(argv[++i], nullptr, 0);
            else if (a == "--input" && i + 1 < argc) options.input_script = argv[++i];
            else if (a == "--serve" && i + 1 < argc) {
                // Interactive control: after each max-frames pause the process writes DIR/status.txt (and current.png into the save
                // folder) and waits for DIR/cmd.txt with lines: until=<absolute frame>, input=<script>, quit.
                const std::filesystem::path dir = argv[++i];
                std::error_code mk; std::filesystem::create_directories(dir, mk);
                options.pause_hook = [dir](std::uint64_t frames, std::string& more) -> std::uint64_t {
                    { std::ofstream f(dir / "status.txt", std::ios::trunc); f << "frame=" << frames << "\n"; }
                    for (;;) {
                        std::error_code ec;
                        if (std::filesystem::exists(dir / "cmd.txt", ec)) {
                            std::ifstream in(dir / "cmd.txt"); std::string line; std::uint64_t next = 0; bool quit = false;
                            while (std::getline(in, line)) {
                                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                                if (line.rfind("until=", 0) == 0) next = std::strtoull(line.c_str() + 6, nullptr, 10);
                                else if (line.rfind("input=", 0) == 0) more += (more.empty() ? "" : ",") + line.substr(6);
                                else if (line == "quit") quit = true;
                            }
                            in.close(); std::filesystem::remove(dir / "cmd.txt", ec);
                            if (quit) return 0;
                            if (next > frames) return next;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    }
                };
            }
            else if (a == "--no-gamepad") options.virtual_gamepad = false;
            else if (a == "--trace-hle") options.trace_hle = true;
            else if (a == "--strict-winrt") options.permissive_xbox_services = false;
            else if (a == "--fps-limit" && i + 1 < argc) { const auto v = std::stoul(argv[++i], nullptr, 0); options.fps_limit = (v == 30 || v == 60) ? static_cast<unsigned>(v) : 0; }
            else if (a == "--show-fps") options.show_fps = true;
            else if (a == "--no-audio") options.audio_enabled = false;
            else if (a == "--audio-volume" && i + 1 < argc) options.audio_volume = (std::max)(0.0f, (std::min)(1.0f, std::stof(argv[++i])));
            else if (a == "--audio-wav" && i + 1 < argc) options.audio_wav_path = argv[++i];
            else if (a == "--report" && i + 1 < argc) report_path = argv[++i];
            else { std::cerr << "Unknown launch-title option: " << a << '\n'; bad = true; }
        }
        if (bad) { usage(argv[0]); exit_code = 2; }
        else {
            someboxone::log_event("INFO", "CLI_LAUNCH_TITLE_BEGIN", std::string("path=") + argv[2] + "; max_instructions=" + std::to_string(options.max_instructions));
            try {
                // Stream launch milestones to stdout so terminal and Linux GUI callers
                // can display progress while the guest runtime is still executing.
                options.progress_sink = [](const std::string& line) {
                    std::cout << line;
                    if (line.empty() || line.back() != '\n') std::cout << '\n';
                    std::cout.flush();
                };
                const auto result = someboxone::launch_title(argv[2], options);
                std::cout << result.text;
                if (!report_path.empty()) { std::ofstream out(report_path, std::ios::binary); out << result.text; }
                someboxone::log_event("INFO", "CLI_LAUNCH_TITLE_COMPLETE", std::string("path=") + argv[2] + "; stop=" + result.report.stop_kind +
                    "; instructions=" + std::to_string(result.report.instructions) + "; detail=" + result.report.stop_detail);
                exit_code = result.package.ok && result.report.image_loaded ? 0 : 2;
            } catch (const std::exception& ex) {
                std::cerr << "Title launch failed: " << ex.what() << '\n';
                someboxone::log_event("ERROR", "CLI_LAUNCH_TITLE_EXCEPTION", std::string("path=") + argv[2] + "; error=" + ex.what());
                exit_code = 1;
            }
        }
    } else if (argc == 3 && std::string(argv[1]) == "xvd-probe") {
        someboxone::log_event("INFO", "CLI_XVD_STORAGE_PROBE_BEGIN", argv[2]);
        try {
            const auto probe = someboxone::ReadOnlyXvdVolume::probe(argv[2]);
            std::cout << "XVD header recognized: " << (probe.header_recognized ? "yes" : "no") << '\n';
            if (probe.header_recognized) {
                std::cout << "XVD type: " << probe.xvd_type
                          << " (" << (probe.xvd_type == 0 ? "Fixed" : probe.xvd_type == 1 ? "Dynamic" : "Unknown") << ")\n";
                std::cout << "Declared drive size: " << probe.drive_size << " bytes\n";
                std::cout << "Calculated data offset: " << someboxone::hex_value(probe.data_offset) << '\n';
                std::cout << "Encryption: " << (probe.data_encrypted ? "enabled" : "disabled") << '\n';
                std::cout << "Logical reads supported: " << (probe.logical_read_supported ? "yes" : "no") << '\n';
            }
            std::cout << "Details: " << probe.detail << '\n';
            someboxone::log_event(probe.header_recognized ? "INFO" : "WARN", "CLI_XVD_STORAGE_PROBE_COMPLETE",
                std::string("path=") + argv[2] + "; logical_read_supported=" +
                (probe.logical_read_supported ? "true" : "false") + "; detail=" + probe.detail);
            exit_code = probe.header_recognized ? 0 : 2;
        } catch (const std::exception& ex) {
            std::cerr << "XVD storage probe failed: " << ex.what() << '\n';
            someboxone::log_event("ERROR", "CLI_XVD_STORAGE_PROBE_FAILED",
                std::string("path=") + argv[2] + "; error=" + ex.what());
            exit_code = 1;
        }
    } else if (argc == 5 && std::string(argv[1]) == "xvd-read") {
        someboxone::log_event("INFO", "CLI_XVD_READ_BEGIN",
            std::string("path=") + argv[2] + "; offset=" + argv[3] + "; count=" + argv[4]);
        try {
            const auto offset = std::stoull(argv[3], nullptr, 0);
            const auto count = std::stoull(argv[4], nullptr, 0);
            std::string error;
            const auto volume = someboxone::ReadOnlyXvdVolume::open(argv[2], &error);
            if (!volume) throw std::runtime_error(error);
            const auto bytes = volume->read_at(offset, count, &error);
            if (!bytes) throw std::runtime_error(error);
            std::cout << "Read " << bytes->size() << " logical bytes (read-only; hash verification is not performed by this reader).\n";
            const std::size_t preview = (std::min)(bytes->size(), static_cast<std::size_t>(256));
            for (std::size_t i = 0; i < preview; ++i) {
                if ((i % 16U) == 0) std::cout << someboxone::hex_value(offset + i, 8) << ":";
                constexpr char hex[] = "0123456789ABCDEF";
                std::cout << ' ' << hex[((*bytes)[i] >> 4U) & 0x0FU] << hex[(*bytes)[i] & 0x0FU];
                if ((i % 16U) == 15U || i + 1 == preview) std::cout << '\n';
            }
            if (bytes->size() > preview) std::cout << "... preview capped at 256 bytes\n";
            someboxone::log_event("INFO", "CLI_XVD_READ_COMPLETE",
                std::string("path=") + argv[2] + "; logical_offset=" + someboxone::hex_value(offset) +
                "; bytes=" + std::to_string(bytes->size()) + "; preview_bytes=" + std::to_string(preview));
        } catch (const std::exception& ex) {
            std::cerr << "XVD logical read failed: " << ex.what() << '\n';
            someboxone::log_event("ERROR", "CLI_XVD_READ_FAILED",
                std::string("path=") + argv[2] + "; error=" + ex.what());
            exit_code = 1;
        }
    } else if (argc == 5 && std::string(argv[1]) == "read-raw") {
        someboxone::log_event("INFO", "CLI_RAW_READ_BEGIN", std::string("path=") + argv[2] + "; offset=" + argv[3] + "; count=" + argv[4]);
        try {
            const auto offset = std::stoull(argv[3], nullptr, 0);
            const auto count = std::stoull(argv[4], nullptr, 0);
            std::string error;
            const auto source = someboxone::ReadOnlyImageSource::open(argv[2], &error);
            if (!source) throw std::runtime_error(error);
            const auto bytes = source->read_at(offset, count, &error);
            if (!bytes) throw std::runtime_error(error);
            std::cout << "Read " << bytes->size() << " raw physical bytes (not decrypted XVD payload):";
            for (const auto byte : *bytes) {
                constexpr char hex[] = "0123456789ABCDEF";
                std::cout << ' ' << hex[(byte >> 4U) & 0x0FU] << hex[byte & 0x0FU];
            }
            std::cout << '\n';
            someboxone::log_event("INFO", "CLI_RAW_READ_COMPLETE", std::string("path=") + argv[2] + "; bytes=" + std::to_string(bytes->size()));
        } catch (const std::exception& ex) {
            std::cerr << "Raw read failed: " << ex.what() << '\n';
            someboxone::log_event("ERROR", "CLI_RAW_READ_FAILED", std::string("path=") + argv[2] + "; error=" + ex.what());
            exit_code = 1;
        }
    } else if (argc == 3 && std::string(argv[1]) == "scan") {
        someboxone::log_event("INFO", "CLI_SCAN_BEGIN", argv[2]);
        try {
            const auto result = someboxone::scan_directory(argv[2]);
            std::cout << result.report;
            exit_code = result.exit_code;
            someboxone::log_event(exit_code == 0 ? "INFO" : "WARN", "CLI_SCAN_COMPLETE",
                                  std::string("root=") + argv[2] + "; exit_code=" + std::to_string(exit_code));
        } catch (const std::exception& ex) {
            std::cerr << "Directory scan failed: " << ex.what() << '\n';
            someboxone::log_event("ERROR", "CLI_SCAN_EXCEPTION", std::string("root=") + argv[2] + "; error=" + ex.what());
            exit_code = 1;
        }
    } else {
        usage(argv[0]);
        exit_code = argc == 1 ? 0 : 2;
    }
    someboxone::log_event(exit_code == 0 ? "INFO" : "WARN", "CLI_EXIT", "exit_code=" + std::to_string(exit_code));
    return exit_code;
}
