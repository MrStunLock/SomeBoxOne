#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <objbase.h>

#include "boot.hpp"
#include "cpu_x64.hpp"
#include "x64_bootstrap.hpp"
#include "x64_idt_test.hpp"
#include "x64_cpu_idt_flow_test.hpp"
#include "x64_cpu_ud_flow_test.hpp"
#include "x64_cpu_bp_flow_test.hpp"
#include "x64_cpu_gp_lidt_flow_test.hpp"
#include "xvd_storage.hpp"
#include "inspector.hpp"
#include "logging.hpp"
#include "paging.hpp"
#include "title_launch.hpp"
#include "framebuffer_ipc.hpp"
#include "host_input_win32.hpp"
#include "runtime_config.hpp"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr int kOpenButton = 1001;
constexpr int kSaveButton = 1002;
constexpr int kPathLabel = 1003;
constexpr int kReportEdit = 1004;
constexpr int kHeaderLabel = 1005;
constexpr int kSubtitleLabel = 1006;
constexpr int kScanButton = 1007;
constexpr int kRefreshButton = 1008;
constexpr int kBootTab = 1010;
constexpr int kDebugTab = 1011;
constexpr int kBootSelectButton = 1012;
constexpr int kAttemptBootButton = 1013;
constexpr int kClearBootButton = 1014;
constexpr int kBootSourceLabel = 1015;
constexpr int kBootStatusLabel = 1016;
constexpr int kBootOutputEdit = 1017;
constexpr int kDebugStatusLabel = 1018;
constexpr int kVerboseCheck = 1019;
constexpr int kActivityLabel = 1020;
constexpr int kActivityEdit = 1021;
constexpr int kOpenLogFolderButton = 1022;
constexpr int kExportLogButton = 1023;
constexpr int kCpuTestButton = 1024;
constexpr int kPagingTestButton = 1025;
constexpr int kLongModeTestButton = 1026;
constexpr int kPageFaultTestButton = 1027;
constexpr int kIdtTestButton = 1031;
constexpr int kStorageProbeButton = 1032;
constexpr int kCpuIdtFlowButton = 1033;
constexpr int kCpuUdFlowButton = 1034;
constexpr int kCpuBpFlowButton = 1035;
constexpr int kCpuGpLidtFlowButton = 1036;
constexpr int kGamesTab = 1040;
constexpr int kGameOpenButton = 1041;
constexpr int kGameRefreshButton = 1042;
constexpr int kGameSaveButton = 1043;
constexpr int kGamePathLabel = 1044;
constexpr int kGameStatusLabel = 1045;
constexpr int kGameReportEdit = 1046;
constexpr int kGameNoticeLabel = 1047;
constexpr int kGameLaunchButton = 1048;
constexpr UINT kTitleLaunchDone = WM_APP + 1;
constexpr UINT kTitleLaunchProgressLine = WM_APP + 2;
constexpr int kGameStopButton = 1049;
constexpr int kGameFramesButton = 1050;
constexpr int kGameDisplayButton = 1051;

// Settings controls were added in v0.30.0 but their IDs were accidentally
// omitted from the Win32 control table, preventing MSVC from compiling the GUI.
constexpr int kSettingsTab = 1052;
constexpr int kMasterVolumeTrack = 1053;
constexpr int kMasterVolumeValue = 1054;
constexpr int kGameVolumeTrack = 1055;
constexpr int kGameVolumeValue = 1056;
constexpr int kKeepLogsCheck = 1057;
constexpr int kGraphicsBackendCombo = 1058;
constexpr int kSettingsSaveButton = 1059;
constexpr int kSettingsStatus = 1060;
// v0.32.0: frame-rate lock, FPS counter and audio switch (1061-1064 are static labels created inline).
constexpr int kFrameRateCombo = 1070;
constexpr int kShowFpsCheck = 1071;
constexpr int kAudioEnabledCheck = 1072;
constexpr int kFrameRateLabel = 1073;

HWND g_main = nullptr;
HWND g_open = nullptr;
HWND g_path = nullptr;
HWND g_report = nullptr;
HWND g_save = nullptr;
HWND g_scan = nullptr;
HWND g_refresh = nullptr;
HWND g_games_tab = nullptr;
HWND g_boot_tab = nullptr; // Firmware section; retains boot-specific control naming internally.
HWND g_debug_tab = nullptr;
HWND g_settings_tab = nullptr;
HWND g_master_volume_track = nullptr;
HWND g_master_volume_value = nullptr;
HWND g_game_volume_track = nullptr;
HWND g_game_volume_value = nullptr;
HWND g_keep_logs_check = nullptr;
HWND g_graphics_backend_combo = nullptr;
HWND g_frame_rate_combo = nullptr;
HWND g_show_fps_check = nullptr;
HWND g_audio_enabled_check = nullptr;
HWND g_settings_save = nullptr;
HWND g_settings_status = nullptr;
HWND g_game_open = nullptr;
HWND g_game_refresh = nullptr;
HWND g_game_save = nullptr;
HWND g_game_launch = nullptr;
HWND g_game_stop = nullptr;
HWND g_game_frames = nullptr;
HWND g_game_display = nullptr;
bool g_launch_running = false;
std::atomic<bool> g_launch_cancel{false};
std::atomic<bool> g_gui_shutting_down{false};
std::atomic<bool> g_game_console_ready{false};
bool g_game_console_allocated = false;
someboxone::RuntimeConfig g_settings;
std::thread g_launch_thread;
fs::path g_game_save_dir;
someboxone::graphics::SharedFrameMapping g_shared_frames;
HANDLE g_display_process = nullptr;
HANDLE g_game_console_output = INVALID_HANDLE_VALUE;
DWORD g_display_process_id = 0;
HWND g_game_path = nullptr;
HWND g_game_status = nullptr;
HWND g_game_report = nullptr;
HWND g_boot_select = nullptr;
HWND g_attempt_boot = nullptr;
HWND g_clear_boot = nullptr;
HWND g_boot_source = nullptr;
HWND g_boot_status = nullptr;
HWND g_boot_output = nullptr;
HWND g_debug_status = nullptr;
HWND g_verbose = nullptr;
HWND g_activity = nullptr;
HWND g_activity_label = nullptr;
HWND g_open_log_folder = nullptr;
HWND g_export_log = nullptr;
HWND g_cpu_test = nullptr;
HWND g_paging_test = nullptr;
HWND g_long_mode_test = nullptr;
HWND g_page_fault_test = nullptr;
HWND g_idt_test = nullptr;
HWND g_storage_probe = nullptr;
HWND g_cpu_idt_flow = nullptr;
HWND g_cpu_ud_flow = nullptr;
HWND g_cpu_bp_flow = nullptr;
HWND g_cpu_gp_lidt_flow = nullptr;
HFONT g_header_font = nullptr;
HFONT g_body_font = nullptr;
std::string g_last_report;
std::wstring g_current_path;
std::string g_game_last_report;
std::wstring g_game_current_path;
std::string g_game_session_title;
std::vector<std::string> g_game_session_lines;
bool g_game_session_truncated = false;
bool g_game_session_saved_for_current_launch = false;
constexpr std::size_t kMaxGameSessionTranscriptLines = 250000;
fs::path g_boot_source_path;
std::vector<HWND> g_games_controls;
std::vector<HWND> g_firmware_controls;
std::vector<HWND> g_debug_controls;
std::vector<HWND> g_settings_controls;
enum class CurrentView { None, SingleFile, Folder };
enum class Page { Games, Firmware, Debug, Settings };
CurrentView g_current_view = CurrentView::None;
Page g_current_page = Page::Games;

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        const int fallback = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (fallback <= 0) return L"(Unable to display text)";
        std::wstring value(static_cast<std::size_t>(fallback), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), value.data(), fallback);
        return value;
    }
    std::wstring value(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), value.data(), needed);
    return value;
}

std::string path_utf8(const fs::path& path) {
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

void append_edit(HWND edit, const std::wstring& text) {
    if (!edit) return;
    const int current = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(current), static_cast<LPARAM>(current));
    SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

void log_ui(const std::string& action, const std::string& detail = {}, const std::string& level = "INFO") {
    const auto line = someboxone::log_event(level, action, detail);
    append_edit(g_activity, utf8_to_wide(line + "\r\n"));
}

void load_recent_activity() {
    if (!g_activity) return;
    const auto recent = someboxone::read_recent_log(100000);
    if (recent.empty()) {
        SetWindowTextW(g_activity, L"No previous log entries. Actions appear here; disk persistence is configurable on Settings.\r\n");
        return;
    }
    SetWindowTextW(g_activity, utf8_to_wide(recent).c_str());
    const int length = GetWindowTextLengthW(g_activity);
    SendMessageW(g_activity, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
    SendMessageW(g_activity, EM_SCROLLCARET, 0, 0);
}

void set_edit_text(HWND edit, const std::string& text) {
    if (edit) SetWindowTextW(edit, utf8_to_wide(text).c_str());
}

void append_console_line(const std::string& line) {
    if (!g_game_console_ready.load(std::memory_order_relaxed)) return;
    HANDLE output = g_game_console_output;
    if (!output || output == INVALID_HANDLE_VALUE) return;
    std::wstring wide = utf8_to_wide(line + "\r\n");
    DWORD written = 0;
    (void)WriteConsoleW(output, wide.c_str(), static_cast<DWORD>(wide.size()), &written, nullptr);
}

void append_game_session_line(const std::string& line) {
    if (g_game_session_lines.size() < kMaxGameSessionTranscriptLines) {
        g_game_session_lines.push_back(line);
    } else if (!g_game_session_truncated) {
        g_game_session_lines.push_back("[SomeBoxOne] Session transcript reached its 250000-line safety limit; later lines are not stored.");
        g_game_session_truncated = true;
    }
    append_console_line(line);
}

void open_game_console() {
    if (g_game_console_ready.load(std::memory_order_relaxed)) return;
    if (GetConsoleWindow() == nullptr) {
        if (!AllocConsole()) {
            log_ui("GAME_CONSOLE_CREATE_FAILED", "AllocConsole failed with error " + std::to_string(GetLastError()), "WARN");
            return;
        }
        g_game_console_allocated = true;
    }
    if (g_game_console_allocated) SetConsoleTitleW(L"SomeBoxOne - Xbox One Emulator - Live Game Output");
    // Open CONOUT$ directly rather than trusting stdout, which may be redirected or unset
    // for a Windows-subsystem GUI process.
    g_game_console_output = CreateFileW(L"CONOUT$", GENERIC_WRITE | GENERIC_READ,
        FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    g_game_console_ready.store(g_game_console_output != nullptr && g_game_console_output != INVALID_HANDLE_VALUE,
                               std::memory_order_relaxed);
    if (g_game_console_ready.load(std::memory_order_relaxed)) {
        append_game_session_line("SomeBoxOne live game output");
        append_game_session_line("Output is streamed during guest execution; the final report remains in the main window.");
    }
}

void update_volume_labels() {
    if (g_master_volume_track && g_master_volume_value) {
        const auto value = static_cast<unsigned>(SendMessageW(g_master_volume_track, TBM_GETPOS, 0, 0));
        const std::wstring label = std::to_wstring(value) + L"%";
        SetWindowTextW(g_master_volume_value, label.c_str());
    }
    if (g_game_volume_track && g_game_volume_value) {
        const auto value = static_cast<unsigned>(SendMessageW(g_game_volume_track, TBM_GETPOS, 0, 0));
        const std::wstring label = std::to_wstring(value) + L"%";
        SetWindowTextW(g_game_volume_value, label.c_str());
    }
}

void load_settings_controls() {
    if (g_master_volume_track) SendMessageW(g_master_volume_track, TBM_SETPOS, TRUE, g_settings.master_volume_percent);
    if (g_game_volume_track) SendMessageW(g_game_volume_track, TBM_SETPOS, TRUE, g_settings.game_volume_percent);
    if (g_keep_logs_check) SendMessageW(g_keep_logs_check, BM_SETCHECK, g_settings.keep_logs ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_frame_rate_combo) SendMessageW(g_frame_rate_combo, CB_SETCURSEL, g_settings.frame_rate_limit == 30 ? 1 : 0, 0);   // 0 = 60 FPS, 1 = 30 FPS
    if (g_show_fps_check) SendMessageW(g_show_fps_check, BM_SETCHECK, g_settings.show_fps_counter ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_audio_enabled_check) SendMessageW(g_audio_enabled_check, BM_SETCHECK, g_settings.audio_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_graphics_backend_combo) SendMessageW(g_graphics_backend_combo, CB_SETCURSEL, g_settings.graphics_backend == "vulkan" ? 1 : 0, 0);
    update_volume_labels();
}

void save_settings_from_controls() {
    if (g_master_volume_track) g_settings.master_volume_percent = static_cast<std::uint32_t>(SendMessageW(g_master_volume_track, TBM_GETPOS, 0, 0));
    if (g_game_volume_track) g_settings.game_volume_percent = static_cast<std::uint32_t>(SendMessageW(g_game_volume_track, TBM_GETPOS, 0, 0));
    g_settings.keep_logs = g_keep_logs_check && SendMessageW(g_keep_logs_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_settings.frame_rate_limit = (g_frame_rate_combo && SendMessageW(g_frame_rate_combo, CB_GETCURSEL, 0, 0) == 1) ? 30U : 60U;
    g_settings.show_fps_counter = g_show_fps_check && SendMessageW(g_show_fps_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_settings.audio_enabled = g_audio_enabled_check && SendMessageW(g_audio_enabled_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_settings.graphics_backend = g_graphics_backend_combo && SendMessageW(g_graphics_backend_combo, CB_GETCURSEL, 0, 0) == 1 ? "vulkan" : "d3d11";
    std::string error;
    if (!someboxone::save_runtime_config(g_settings, &error)) {
        SetWindowTextW(g_settings_status, L"Could not save settings. See the activity log.");
        log_ui("SETTINGS_SAVE_FAILED", error, "ERROR");
        MessageBoxW(g_main, utf8_to_wide(error).c_str(), L"SomeBoxOne - Settings", MB_ICONERROR | MB_OK);
        return;
    }
    someboxone::set_persistent_logging_enabled(g_settings.keep_logs);
    update_volume_labels();
    if (g_settings.graphics_backend == "vulkan") {
        SetWindowTextW(g_settings_status, L"Saved. Vulkan is selected, but its presenter is not implemented yet; games currently fall back to D3D11/DXGI.");
    } else if (g_settings.master_volume_percent != 100 || g_settings.game_volume_percent != 100) {
        SetWindowTextW(g_settings_status, L"Saved. Volume, audio, frame-rate and FPS-counter changes apply the next time a game is launched.");
    } else {
        SetWindowTextW(g_settings_status, L"Saved. Frame-rate, FPS-counter and audio changes apply the next time a game is launched.");
    }
    log_ui("SETTINGS_SAVED", "master_volume=" + std::to_string(g_settings.master_volume_percent) +
        "; game_volume=" + std::to_string(g_settings.game_volume_percent) +
        "; keep_logs=" + std::string(g_settings.keep_logs ? "true" : "false") +
        "; frame_rate_limit=" + std::to_string(g_settings.frame_rate_limit) +
        "; show_fps=" + std::string(g_settings.show_fps_counter ? "true" : "false") +
        "; audio_enabled=" + std::string(g_settings.audio_enabled ? "true" : "false") +
        "; graphics_backend=" + g_settings.graphics_backend);
}

void show_report(const fs::path& path, const someboxone::InspectionResult& result, CurrentView view) {
    g_last_report = result.report;
    g_current_path = path.wstring();
    g_current_view = view;
    SetWindowTextW(g_path, g_current_path.c_str());
    set_edit_text(g_report, result.report);
    EnableWindow(g_save, TRUE);
    EnableWindow(g_refresh, TRUE);
}

void inspect_path(const fs::path& path) {
    log_ui("INSPECT_BEGIN", path_utf8(path));
    try {
        const auto result = someboxone::inspect_file(path);
        show_report(path, result, CurrentView::SingleFile);
        log_ui("INSPECT_COMPLETE", "path=" + path_utf8(path) + "; exit_code=" + std::to_string(result.exit_code),
               result.exit_code == 0 ? "INFO" : "WARN");
    } catch (const std::exception& ex) {
        log_ui("INSPECT_EXCEPTION", "path=" + path_utf8(path) + "; error=" + ex.what(), "ERROR");
        MessageBoxW(g_main, L"The image inspection failed. See the activity log for details.", L"SomeBoxOne - Inspect", MB_ICONERROR | MB_OK);
    }
}

void scan_path(const fs::path& path) {
    log_ui("SCAN_BEGIN", path_utf8(path));
    try {
        const auto result = someboxone::scan_directory(path);
        show_report(path, result, CurrentView::Folder);
        log_ui("SCAN_COMPLETE", "root=" + path_utf8(path) + "; exit_code=" + std::to_string(result.exit_code),
               result.exit_code == 0 ? "INFO" : "WARN");
    } catch (const std::exception& ex) {
        log_ui("SCAN_EXCEPTION", "root=" + path_utf8(path) + "; error=" + ex.what(), "ERROR");
        MessageBoxW(g_main, L"The folder scan failed. See the activity log for details.", L"SomeBoxOne - Scan", MB_ICONERROR | MB_OK);
    }
}

void refresh_current() {
    log_ui("REFRESH_REQUEST", g_current_path.empty() ? "no previous path" : path_utf8(fs::path(g_current_path)));
    if (g_current_view == CurrentView::None || g_current_path.empty()) {
        log_ui("REFRESH_SKIPPED", "No previous inspection or scan is available.", "WARN");
        return;
    }
    const fs::path path(g_current_path);
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec || !exists) {
        log_ui("REFRESH_PATH_MISSING", path_utf8(path), "ERROR");
        MessageBoxW(g_main,
            L"The previously selected file or folder cannot be found. It may have been moved or disconnected.\r\n\r\n"
            L"Use Open image... or Scan folder... to choose a new location.",
            L"SomeBoxOne - Refresh", MB_ICONWARNING | MB_OK);
        return;
    }
    if (g_current_view == CurrentView::SingleFile) inspect_path(path);
    else if (g_current_view == CurrentView::Folder) scan_path(path);
}

bool choose_image_path(fs::path& output_path, const wchar_t* title) {
    std::vector<wchar_t> filename(32768, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_main;
    dialog.lpstrFilter = L"Xbox/firmware images\0*.bin;*.vbi;*.xvd;*.xvc;*.img;*.dat\0All files\0*.*\0\0";
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    dialog.lpstrTitle = title;
    if (!GetOpenFileNameW(&dialog)) return false;
    output_path = fs::path(filename.data());
    return true;
}

void open_image() {
    fs::path path;
    if (!choose_image_path(path, L"Select a firmware or image file to inspect")) {
        log_ui("OPEN_IMAGE_CANCELLED", "File picker was closed without choosing a file.");
        return;
    }
    inspect_path(path);
}

bool choose_game_package_path(fs::path& output_path) {
    std::vector<wchar_t> filename(32768, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_main;
    dialog.lpstrFilter = L"Xbox game packages (*.xvc)\0*.xvc\0All files\0*.*\0\0";
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    dialog.lpstrTitle = L"Select an Xbox Virtual Container (.xvc) game package";
    if (!GetOpenFileNameW(&dialog)) return false;
    output_path = fs::path(filename.data());
    return true;
}

void inspect_game_path(const fs::path& path) {
    log_ui("GAME_PACKAGE_INSPECT_BEGIN", path_utf8(path));
    try {
        const auto result = someboxone::inspect_file(path);
        g_game_last_report = result.report;
        g_game_current_path = path.wstring();
        SetWindowTextW(g_game_path, g_game_current_path.c_str());
        set_edit_text(g_game_report, result.report);
        EnableWindow(g_game_save, TRUE);
        EnableWindow(g_game_refresh, TRUE);
        SetWindowTextW(g_game_status, result.exit_code == 0
            ? L"Package inspected - metadata only; use Launch title for an unpacked folder"
            : L"Inspection completed with warnings - review the report");
        log_ui("GAME_PACKAGE_INSPECT_COMPLETE", "path=" + path_utf8(path) + "; exit_code=" + std::to_string(result.exit_code),
               result.exit_code == 0 ? "INFO" : "WARN");
    } catch (const std::exception& ex) {
        SetWindowTextW(g_game_status, L"Package inspection failed - see activity log");
        log_ui("GAME_PACKAGE_INSPECT_EXCEPTION", "path=" + path_utf8(path) + "; error=" + ex.what(), "ERROR");
        MessageBoxW(g_main, L"The game package inspection failed. See the activity log for details.",
                    L"SomeBoxOne - Games", MB_ICONERROR | MB_OK);
    }
}

void open_game_package() {
    fs::path path;
    if (!choose_game_package_path(path)) {
        log_ui("GAME_PACKAGE_PICKER_CANCELLED", "No XVC package selected.");
        return;
    }
    inspect_game_path(path);
}

void refresh_game_package() {
    if (g_game_current_path.empty()) {
        log_ui("GAME_PACKAGE_REFRESH_SKIPPED", "No game package has been inspected yet.", "WARN");
        return;
    }
    const fs::path path(g_game_current_path);
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        SetWindowTextW(g_game_status, L"Selected package is unavailable; choose it again");
        log_ui("GAME_PACKAGE_REFRESH_PATH_MISSING", path_utf8(path), "ERROR");
        return;
    }
    inspect_game_path(path);
}

bool choose_title_path(fs::path& output_path) {
    std::vector<wchar_t> filename(32768, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_main;
    dialog.lpstrFilter = L"Unpacked Xbox title (*.exe;AppxManifest.xml)\0*.exe;AppxManifest.xml\0All files\0*.*\0\0";
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    dialog.lpstrTitle = L"Select the title's AppxManifest.xml or executable inside its unpacked Mount folder";
    if (!GetOpenFileNameW(&dialog)) return false;
    output_path = fs::path(filename.data());
    return true;
}

BOOL CALLBACK post_close_to_process_window(HWND hwnd, LPARAM parameter) {
    const DWORD target_pid = static_cast<DWORD>(parameter);
    DWORD window_pid = 0;
    GetWindowThreadProcessId(hwnd, &window_pid);
    if (window_pid == target_pid && IsWindowVisible(hwnd)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

bool ensure_graphics_display_window() {
    if (g_display_process && WaitForSingleObject(g_display_process, 0) == WAIT_TIMEOUT) {
        log_ui("DISPLAY_WINDOW_ALREADY_RUNNING", "The separate D3D11/DXGI display process is already running.");
        return true;
    }
    if (g_display_process) {
        CloseHandle(g_display_process);
        g_display_process = nullptr;
        g_display_process_id = 0;
    }

    std::vector<wchar_t> module_path(32768, L'\0');
    const DWORD module_len = GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (module_len == 0 || module_len >= module_path.size()) {
        log_ui("DISPLAY_WINDOW_PATH_FAILED", "Could not resolve the GUI executable directory.", "ERROR");
        return false;
    }
    const fs::path gui_path(module_path.data());
    const fs::path display_path = gui_path.parent_path() / L"SomeBoxOneDisplay.exe";
    if (!fs::exists(display_path)) {
        log_ui("DISPLAY_WINDOW_EXECUTABLE_MISSING", "Expected separate D3D11/DXGI window at " + path_utf8(display_path), "ERROR");
        SetWindowTextW(g_game_status, L"Display executable missing. Rebuild the full Windows solution to create SomeBoxOneDisplay.exe.");
        return false;
    }
    if (!g_shared_frames.open()) {
        log_ui("DISPLAY_FRAME_TRANSPORT_FAILED", "Could not create/open the v3 shared-frame mapping.", "ERROR");
        return false;
    }
    if (g_settings.graphics_backend == "vulkan") {
        (void)g_shared_frames.publish_status("Vulkan was requested but is not implemented yet; using D3D11/DXGI presenter.");
        log_ui("GRAPHICS_BACKEND_FALLBACK", "Vulkan requested, but no Vulkan presenter is available; using D3D11/DXGI.", "WARN");
    } else {
        (void)g_shared_frames.publish_status("Graphics display connected; waiting for title frame capture.");
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    std::wstring command_line = L"\"" + display_path.wstring() + L"\"";
    const std::wstring working_directory = display_path.parent_path().wstring();
    if (!CreateProcessW(display_path.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, working_directory.c_str(), &startup, &process)) {
        const DWORD error = GetLastError();
        log_ui("DISPLAY_WINDOW_START_FAILED", "CreateProcessW failed with error " + std::to_string(error) + "; path=" + path_utf8(display_path), "ERROR");
        return false;
    }
    CloseHandle(process.hThread);
    g_display_process = process.hProcess;
    g_display_process_id = process.dwProcessId;
    log_ui("DISPLAY_WINDOW_STARTED", "Started separate SomeBoxOneDisplay.exe (D3D11/DXGI), pid=" + std::to_string(g_display_process_id) +
        "; configured_backend=" + g_settings.graphics_backend);
    return true;
}

void close_graphics_display_window() {
    if (!g_display_process) return;
    if (WaitForSingleObject(g_display_process, 0) == WAIT_TIMEOUT) {
        EnumWindows(post_close_to_process_window, static_cast<LPARAM>(g_display_process_id));
        if (WaitForSingleObject(g_display_process, 1200) == WAIT_TIMEOUT) {
            // Do not leave the standalone renderer orphaned if its window fails to close.
            TerminateProcess(g_display_process, 0);
            (void)WaitForSingleObject(g_display_process, 1200);
        }
    }
    CloseHandle(g_display_process);
    g_display_process = nullptr;
    g_display_process_id = 0;
}

void launch_title_from_gui() {
    if (g_launch_running) {
        log_ui("GAME_TITLE_LAUNCH_SKIPPED", "A title launch is already running.", "WARN");
        return;
    }
    fs::path path;
    if (!choose_title_path(path)) {
        log_ui("GAME_TITLE_PICKER_CANCELLED", "No title selected.");
        return;
    }
    if (g_launch_thread.joinable()) g_launch_thread.join();

    // Read package metadata before the display process is created (or reused), so it
    // can show the package splash while the guest is still booting.
    const auto launch_package = someboxone::inspect_title_package(path);
    std::string active_title = launch_package.display_name;
    if (active_title.empty() || active_title.rfind("ms-resource:", 0) == 0)
        active_title = !launch_package.identity_name.empty() ? launch_package.identity_name : path_utf8(path.stem());
    if (active_title.empty()) active_title = "Game";
    const std::string splash_path = launch_package.splash_path.empty() ? std::string() : path_utf8(launch_package.splash_path);

    g_launch_running = true;
    g_launch_cancel = false;
    g_game_session_lines.clear();
    g_game_session_truncated = false;
    g_game_session_saved_for_current_launch = false;
    g_game_session_title = active_title;
    if (!g_shared_frames.open()) {
        log_ui("DISPLAY_FRAME_TRANSPORT_FAILED", "Could not create/open the v3 shared-frame mapping.", "ERROR");
    } else if (!g_shared_frames.publish_game_info(active_title, splash_path)) {
        log_ui("DISPLAY_GAME_INFO_PUBLISH_FAILED", "Could not publish the game title and splash metadata.", "WARN");
    }
    open_game_console();
    (void)ensure_graphics_display_window();
    if (g_shared_frames.is_open()) {
        (void)g_shared_frames.publish_status(g_settings.graphics_backend == "vulkan"
            ? "Vulkan requested but unavailable; running with D3D11/DXGI display presenter."
            : "Title execution started; waiting for software-captured D3D texture frames.");
    }
    EnableWindow(g_game_launch, FALSE);
    EnableWindow(g_game_stop, TRUE);
    SetWindowTextW(g_game_path, path.wstring().c_str());
    SetWindowTextW(g_game_status, g_settings.graphics_backend == "vulkan"
        ? L"Launching with D3D11/DXGI fallback (Vulkan presenter is not implemented yet)..."
        : L"Launching title; live output is shown in the console and here...");
    g_game_last_report.clear();
    set_edit_text(g_game_report, "Launching " + path_utf8(path) + "\r\nLive guest-runtime output follows below.\r\n\r\n");
    const std::string backend_line = g_settings.graphics_backend == "vulkan"
        ? "Host graphics backend: Vulkan requested but unavailable; active presenter is D3D11/DXGI."
        : "Host graphics backend: D3D11/DXGI.";
    append_edit(g_game_report, utf8_to_wide(backend_line + "\r\n"));
    const std::string input_line = "Input: keyboard WASD/arrows; A=Space/Z, B=X, X=C, Y=V, Menu=Enter, View=Esc, shoulders=Q/E, triggers=Shift/Ctrl, right stick=I/J/K/L; XInput controller is polled when connected.";
    append_edit(g_game_report, utf8_to_wide(input_line + "\r\n"));
    append_game_session_line("SomeBoxOne game launch: " + path_utf8(path));
    append_game_session_line(backend_line);
    append_game_session_line(input_line);
    log_ui("GRAPHICS_BACKEND_USED", backend_line, g_settings.graphics_backend == "vulkan" ? "WARN" : "INFO");
    log_ui("GAME_TITLE_LAUNCH_BEGIN", path_utf8(path));
    const HWND target = g_main;
    // Capture the display PID before the worker starts; this avoids reading the GUI's
    // mutable process-handle state concurrently from the emulation thread.
    const DWORD input_display_pid = g_display_process_id;
    const someboxone::RuntimeConfig launch_settings = g_settings;   // snapshot: the worker thread must not read GUI state
    g_launch_thread = std::thread([path, target, input_display_pid, launch_settings]() {
        auto* text = new std::string();
        try {
            someboxone::TitleLaunchOptions options;
            // Zero disables artificial instruction/frame caps; Stop run and shutdown still cancel it.
            options.max_instructions = 0;
            options.max_frames = 0;
            options.save_every = 0;              // disable periodic PNG writes on the emulation thread; the runtime still saves its final frame
            options.fps_limit = launch_settings.frame_rate_limit == 30 ? 30U : 60U;
            options.show_fps = launch_settings.show_fps_counter;
            options.audio_enabled = launch_settings.audio_enabled;
            options.audio_volume = static_cast<float>((std::min)(100U, launch_settings.master_volume_percent)) * static_cast<float>((std::min)(100U, launch_settings.game_volume_percent)) / 10000.0f;
            options.cancel = &g_launch_cancel;
            options.input_provider = [input_display_pid]() { return someboxone::poll_host_gamepad_input(input_display_pid); };
            options.progress_sink = [target](const std::string& line) {
                if (g_gui_shutting_down.load(std::memory_order_relaxed)) return;
                auto* text = new std::string(line);
                if (!PostMessageW(target, kTitleLaunchProgressLine, 0, reinterpret_cast<LPARAM>(text))) delete text;
            };
            options.sink_every = 1;              // present every frame (every 4th made a 60 FPS game look like 15 FPS)
            options.frame_sink = [](const std::vector<std::uint8_t>& bgra, unsigned width, unsigned height, std::uint64_t frame) {
                (void)frame;
                if (!g_shared_frames.publish_bgra(width, height, bgra.data(), bgra.size())) {
                    static std::atomic<bool> logged{false};
                    if (!logged.exchange(true)) {
                        someboxone::log_event("WARN", "DISPLAY_FRAME_PUBLISH_FAILED",
                            "A captured frame could not be committed to the v3 shared framebuffer mapping.");
                    }
                }
            };
            const auto result = someboxone::launch_title(path, options);
            g_game_save_dir = result.save_dir;
            *text = result.text;
        } catch (const std::exception& ex) {
            *text = std::string("Title launch failed: ") + ex.what() + "\n";
        }
        if (g_gui_shutting_down.load()) {
            delete text;
            return;
        }
        if (!PostMessageW(target, kTitleLaunchDone, 0, reinterpret_cast<LPARAM>(text))) delete text;
    });
}

void stop_title_launch() {
    if (!g_launch_running) return;
    g_launch_cancel = true;
    SetWindowTextW(g_game_status, L"Stopping after the current frame...");
    log_ui("GAME_TITLE_LAUNCH_STOP_REQUESTED", "User requested the title run to stop.");
}

void open_game_frames_folder() {
    if (g_game_save_dir.empty()) {
        log_ui("GAME_FRAMES_FOLDER_UNAVAILABLE", "No title run has produced a frames folder yet.", "WARN");
        return;
    }
    ShellExecuteW(g_main, L"open", g_game_save_dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    log_ui("GAME_FRAMES_FOLDER_OPENED", path_utf8(g_game_save_dir));
}

void finish_title_launch(std::string* text) {
    if (g_launch_thread.joinable()) g_launch_thread.join();
    g_launch_running = false;
    if (g_shared_frames.is_open())
        (void)g_shared_frames.publish_status("Title run finished; the graphics window retains the last captured frame. See the main report for the stop reason.");
    EnableWindow(g_game_launch, TRUE);
    EnableWindow(g_game_stop, FALSE);
    if (text != nullptr) {
        g_game_last_report = *text;
        // Keep the live console transcript and append the original complete report afterwards.
        append_edit(g_game_report, L"\r\n================ FINAL GAME REPORT ================\r\n");
        append_game_session_line("================ FINAL GAME REPORT ================");
        std::string crlf;
        std::size_t begin = 0;
        while (begin < text->size()) {
            const auto end = text->find('\n', begin);
            const std::string line = text->substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            append_edit(g_game_report, utf8_to_wide(line + "\r\n"));
            append_game_session_line(line);
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        delete text;
    }
    EnableWindow(g_game_save, TRUE);
    if (!g_game_session_saved_for_current_launch) {
        std::filesystem::path session_path;
        std::string session_error;
        if (someboxone::save_game_session_log(g_game_session_title, g_game_session_lines, &session_path, &session_error)) {
            const std::string saved_line = "Game session log saved: " + path_utf8(session_path);
            append_edit(g_game_report, utf8_to_wide(saved_line + "\r\n"));
            append_console_line(saved_line);
            log_ui("GAME_SESSION_LOG_SAVED", path_utf8(session_path));
        } else if (!someboxone::persistent_logging_enabled()) {
            append_edit(g_game_report, L"Disk logging is disabled in Settings; the game-session transcript was not saved.\r\n");
        } else {
            append_edit(g_game_report, utf8_to_wide("Game session log save failed: " + session_error + "\r\n"));
            log_ui("GAME_SESSION_LOG_SAVE_FAILED", session_error, "ERROR");
        }
        g_game_session_saved_for_current_launch = true;
    }
    SetWindowTextW(g_game_status, L"Title run finished - see the report for the stop reason; session output was saved when logging is enabled");
    log_ui("GAME_TITLE_LAUNCH_COMPLETE", "Title runner finished; report is shown on the Games page.");
}

void save_game_report() {
    if (g_game_last_report.empty()) {
        log_ui("GAME_REPORT_SAVE_SKIPPED", "No game package report is available.", "WARN");
        return;
    }
    std::vector<wchar_t> filename(32768, L'\0');
    const wchar_t* suggested = L"someboxone-game-report.txt";
    for (std::size_t i = 0; suggested[i] != L'\0' && i + 1 < filename.size(); ++i) filename[i] = suggested[i];
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_main;
    dialog.lpstrFilter = L"Text report\0*.txt\0All files\0*.*\0\0";
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    dialog.lpstrTitle = L"Save SomeBoxOne game package report";
    if (!GetSaveFileNameW(&dialog)) {
        log_ui("GAME_REPORT_SAVE_CANCELLED", "Save report dialog was cancelled.");
        return;
    }
    const fs::path destination(filename.data());
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
        log_ui("GAME_REPORT_SAVE_FAILED", path_utf8(destination), "ERROR");
        MessageBoxW(g_main, L"Could not create the game report file.", L"SomeBoxOne - Games", MB_ICONERROR | MB_OK);
        return;
    }
    out.write(g_game_last_report.data(), static_cast<std::streamsize>(g_game_last_report.size()));
    if (!out) {
        log_ui("GAME_REPORT_SAVE_FAILED", "Write error: " + path_utf8(destination), "ERROR");
        MessageBoxW(g_main, L"An error occurred while writing the game report.", L"SomeBoxOne - Games", MB_ICONERROR | MB_OK);
    } else {
        log_ui("GAME_REPORT_SAVE_COMPLETE", path_utf8(destination));
    }
}

void select_boot_image() {
    fs::path path;
    if (!choose_image_path(path, L"Select an Xbox system image for boot preflight")) {
        log_ui("BOOT_SOURCE_PICKER_CANCELLED", "No boot source selected.");
        return;
    }
    g_boot_source_path = path;
    SetWindowTextW(g_boot_source, path.wstring().c_str());
    SetWindowTextW(g_boot_status, L"Source selected - not started");
    append_edit(g_boot_output, L"\r\n[BOOT SOURCE] Selected: ");
    append_edit(g_boot_output, path.wstring());
    append_edit(g_boot_output, L"\r\nUse Attempt boot to run read-only preflight checks. Actual Xbox firmware execution is not implemented yet.\r\n");
    log_ui("BOOT_SOURCE_SELECTED", path_utf8(path));
}

void scan_folder() {
    BROWSEINFOW dialog{};
    dialog.hwndOwner = g_main;
    dialog.lpszTitle = L"Select an extracted Xbox OSU/restore folder to scan";
    dialog.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST selected = SHBrowseForFolderW(&dialog);
    if (!selected) {
        log_ui("SCAN_FOLDER_PICKER_CANCELLED", "Folder picker was closed without choosing a folder.");
        return;
    }
    wchar_t folder[MAX_PATH]{};
    const BOOL ok = SHGetPathFromIDListW(selected, folder);
    CoTaskMemFree(selected);
    if (!ok) {
        log_ui("SCAN_FOLDER_PATH_ERROR", "Could not read the selected folder path.", "ERROR");
        MessageBoxW(g_main, L"Could not read the selected folder path.", L"SomeBoxOne", MB_ICONERROR | MB_OK);
        return;
    }
    scan_path(fs::path(folder));
}

void save_report() {
    if (g_last_report.empty()) {
        log_ui("SAVE_REPORT_SKIPPED", "There is no inspection report to save.", "WARN");
        return;
    }
    std::vector<wchar_t> filename(32768, L'\0');
    const wchar_t* suggested = L"someboxone-report.txt";
    for (std::size_t i = 0; suggested[i] != L'\0' && i + 1 < filename.size(); ++i) filename[i] = suggested[i];
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_main;
    dialog.lpstrFilter = L"Text report\0*.txt\0All files\0*.*\0\0";
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    dialog.lpstrTitle = L"Save SomeBoxOne inspection report";
    if (!GetSaveFileNameW(&dialog)) {
        log_ui("SAVE_REPORT_CANCELLED", "Save report dialog was cancelled.");
        return;
    }

    const fs::path path(filename.data());
    log_ui("SAVE_REPORT_BEGIN", path_utf8(path));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        log_ui("SAVE_REPORT_FAILED", path_utf8(path), "ERROR");
        MessageBoxW(g_main, L"Could not create the report file.", L"SomeBoxOne", MB_ICONERROR | MB_OK);
        return;
    }
    out.write(g_last_report.data(), static_cast<std::streamsize>(g_last_report.size()));
    if (!out) {
        log_ui("SAVE_REPORT_FAILED", "Write error: " + path_utf8(path), "ERROR");
        MessageBoxW(g_main, L"An error occurred while writing the report.", L"SomeBoxOne", MB_ICONERROR | MB_OK);
    } else {
        log_ui("SAVE_REPORT_COMPLETE", path_utf8(path));
    }
}

void append_boot_output(const std::string& text) {
    append_edit(g_boot_output, utf8_to_wide(text));
}

void append_boot_stage_trace(const someboxone::BootPreflightResult& preflight) {
    append_boot_output("\r\n[BOOT STAGE TRACE]\r\n");
    for (const auto& stage : preflight.stages) {
        const std::string status = someboxone::boot_stage_status_name(stage.status);
        append_boot_output(" - [" + status + "] " + stage.name + ": " + stage.detail + "\r\n");
        log_ui("BOOT_STAGE_RESULT", stage.name + "; status=" + status + "; detail=" + stage.detail,
               stage.status == someboxone::BootStageStatus::Failed ? "ERROR" :
               (stage.status == someboxone::BootStageStatus::Blocked || stage.status == someboxone::BootStageStatus::NotImplemented ? "WARN" : "INFO"));
    }
}

void attempt_boot() {
    log_ui("BOOT_ATTEMPT_REQUESTED", g_boot_source_path.empty() ? "no source selected" : path_utf8(g_boot_source_path));
    if (g_boot_source_path.empty()) {
        SetWindowTextW(g_boot_status, L"Blocked - choose a source first");
        append_boot_output("\r\n[BOOT] BLOCKED: no source image has been selected.\r\n");
        log_ui("BOOT_ATTEMPT_BLOCKED", "No source image selected.", "WARN");
        MessageBoxW(g_main, L"Choose a boot image first. The current boot action performs safe preflight checks only; it does not execute Xbox firmware.",
                    L"SomeBoxOne - Boot", MB_ICONINFORMATION | MB_OK);
        return;
    }

    const fs::path source = g_boot_source_path;
    append_boot_output("\r\n============================================================\r\n");
    append_boot_output("[BOOT] Experimental boot request for: " + path_utf8(source) + "\r\n");
    append_boot_output("[1/4] Checking source file... ");
    log_ui("BOOT_PREFLIGHT_BEGIN", path_utf8(source));

    const auto preflight = someboxone::run_boot_preflight(source);
    if (!preflight.source_exists) {
        SetWindowTextW(g_boot_status, L"Failed - source file unavailable");
        append_boot_output("FAILED\r\n" + preflight.blocker + "\r\n");
        log_ui("BOOT_PREFLIGHT_FILE_CHECK_FAILED", path_utf8(source) + "; " + preflight.blocker, "ERROR");
        append_boot_stage_trace(preflight);
        return;
    }
    append_boot_output("OK\r\n");
    log_ui("BOOT_PREFLIGHT_FILE_CHECK", "regular file exists: " + path_utf8(source));

    append_boot_output("[2/4] Inspecting known image metadata... ");
    if (!preflight.inspection_completed) {
        append_boot_output("FAILED\r\n" + preflight.blocker + "\r\n");
        SetWindowTextW(g_boot_status, L"Failed - preflight inspection error");
        log_ui("BOOT_PREFLIGHT_INSPECT_FAILED", preflight.blocker, "ERROR");
        append_boot_stage_trace(preflight);
        return;
    }
    show_report(source, someboxone::InspectionResult{preflight.inspection_exit_code, preflight.inspection_report}, CurrentView::SingleFile);
    append_boot_output(preflight.inspection_exit_code == 0 ? "finished (no inspector error)\r\n" : "finished with inspector diagnostics\r\n");
    log_ui("BOOT_PREFLIGHT_INSPECT_COMPLETE", "exit_code=" + std::to_string(preflight.inspection_exit_code));
    const auto& image_report = preflight.inspection_report;
    log_ui("BOOT_IMAGE_METADATA",
        "path=" + path_utf8(source) +
        "; classification=" + someboxone::report_value(image_report, "Classification: ") +
        "; xvd_type=" + someboxone::report_value(image_report, "XVD type (decoded): ") +
        "; content_type=" + someboxone::report_value(image_report, "Content type (decoded): ") +
        "; encryption=" + someboxone::report_value(image_report, "Data encryption status: ") +
        "; package_version=" + someboxone::report_value(image_report, "Package version: ") +
        "; hash_sample=" + someboxone::report_value(image_report, "Top hash-tree page SHA-256 check: "));

    append_boot_output("[3/4] Checking current emulator support... BLOCKED\r\n");
    if (preflight.xvd_header_recognized) append_boot_output("Input classification: XVD-compatible header detected. This is not an authenticity or bootability check.\r\n");
    else if (preflight.vbi_marker_reported) append_boot_output("Input classification: VBI-related metadata reported; executable boot support is unavailable.\r\n");
    else append_boot_output("Input classification: no currently recognized bootable image type established.\r\n");
    append_boot_output("[4/4] Guest execution: NOT STARTED\r\n\r\n");
    append_boot_output("Reason: " + preflight.blocker + "\r\n");
    append_boot_output("Safety: no source bytes were executed, modified, decrypted, or extracted. This was a read-only preflight attempt.\r\n");
    append_boot_stage_trace(preflight);
    SetWindowTextW(g_boot_status, L"Blocked - preflight only; firmware not booted");
    log_ui("BOOT_ATTEMPT_BLOCKED", preflight.xvd_header_recognized ?
           "XVD recognized but required boot layers are not implemented; no guest execution." :
           "Input was inspected, but required boot layers are not implemented; no guest execution.", "WARN");
}
void clear_boot_output() {
    SetWindowTextW(g_boot_output, L"Boot-attempt output cleared. No emulator state was reset.\r\n");
    SetWindowTextW(g_boot_status, L"Idle - no boot session running");
    log_ui("BOOT_OUTPUT_CLEARED", "Only the visible boot output was cleared; no emulator state was reset.");
}

void run_cpu_smoke_test() {
    log_ui("CPU_SMOKE_TEST_BEGIN", "Synthetic x86-64 program with four-level paging; no user firmware bytes are executed.");
    SetWindowTextW(g_debug_status, L"Running paged CPU subset test...");
    someboxone::GuestAddressSpace memory(1024 * 1024);
    if (!memory.map_ram(0, 1024 * 1024, "synthetic paged CPU test RAM")) {
        SetWindowTextW(g_debug_status, L"FAIL - unable to map test RAM");
        log_ui("CPU_SMOKE_TEST_FAILED", "RAM mapping failed.", "ERROR");
        return;
    }
    const bool page_tables_written =
        memory.write(0x1000, 8, 0x2007) &&
        memory.write(0x2000, 8, 0x3007) &&
        memory.write(0x3010, 8, 0x4007) &&
        memory.write(0x4000, 8, 0x10007) &&
        memory.write(0x4008, 8, 0x12007 | (1ULL << 63U)) &&
        memory.write(0x4010, 8, 0x15007 | (1ULL << 63U)) &&
        memory.write(0x4018, 8, 0x14007 | (1ULL << 63U));
    if (!page_tables_written) {
        SetWindowTextW(g_debug_status, L"FAIL - unable to create test page tables");
        log_ui("CPU_SMOKE_TEST_FAILED", "Writing synthetic page-table entries failed.", "ERROR");
        return;
    }
    // The CPU executes these generated bytes from VA 0x400000. Its data and stack
    // accesses use separate virtual pages translated by the synthetic page tables.
    const std::vector<std::uint8_t> program{
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
        0xF4                                                        // test-harness halt
    };
    for (std::size_t i = 0; i < program.size(); ++i) {
        if (!memory.write(0x10000 + i, 1, program[i])) {
            SetWindowTextW(g_debug_status, L"FAIL - unable to write test program");
            log_ui("CPU_SMOKE_TEST_FAILED", "Writing synthetic test program failed.", "ERROR");
            return;
        }
    }
    someboxone::X64CpuSubset cpu(memory, 0x400000, 0x1000, someboxone::X64PrivilegeLevel::User);
    const auto result = cpu.run(128);
    const auto low_data_value = memory.read(0x12FFC, 4);
    const auto high_data_value = memory.read(0x15000, 4);
    const bool passed = result == someboxone::X64CpuSubset::RunResult::Halted &&
                        cpu.state.registers[0] == 42 && cpu.state.registers[3] == 42 &&
                        cpu.state.registers[4] == 0x403FF0 && low_data_value == 42 &&
                        high_data_value == 0 && cpu.state.fault.empty();
    if (passed) {
        SetWindowTextW(g_debug_status, L"PASS - paged CPU memory, branch and stack test; RAX = 42");
        log_ui("CPU_SMOKE_TEST_PASSED", "Halted=1; virtual RIP=" + someboxone::hex_value(cpu.state.rip) +
               "; 8-byte data access crossed into a non-contiguous physical page; stack restored; synthetic bytes only.");
        if (SendMessageW(g_verbose, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            log_ui("CPU_SMOKE_TEST_DETAIL", "RIP=" + someboxone::hex_value(cpu.state.rip) +
                   "; RSP=" + someboxone::hex_value(cpu.state.registers[4]) +
                   "; RFLAGS=" + someboxone::hex_value(cpu.state.rflags) +
                   "; physical_data_low=" + someboxone::hex_value(*low_data_value) +
                   "; physical_data_high=" + someboxone::hex_value(*high_data_value));
        }
    } else {
        const std::string detail = "result=" + std::to_string(static_cast<int>(result)) +
            "; RAX=" + someboxone::hex_value(cpu.state.registers[0]) +
            "; fault=" + cpu.state.fault;
        SetWindowTextW(g_debug_status, L"FAIL - see activity log");
        log_ui("CPU_SMOKE_TEST_FAILED", detail, "ERROR");
    }
}

void run_long_mode_control_test() {
    log_ui("LONG_MODE_CONTROL_TEST_BEGIN", "Synthetic CR0/CR3/CR4/EFER sequence and x86-64 code only; no firmware bytes are executed.");
    SetWindowTextW(g_debug_status, L"Running long-mode control-state test...");
    const auto result = someboxone::run_synthetic_long_mode_test();
    if (result.passed) {
        SetWindowTextW(g_debug_status, L"PASS - long-mode control state + paged CPU test; RAX = 42");
        log_ui("LONG_MODE_CONTROL_TEST_PASSED", result.detail + "; RAX=" + someboxone::hex_value(result.rax) +
               "; RIP=" + someboxone::hex_value(result.rip) + "; RSP=" + someboxone::hex_value(result.rsp));
    } else {
        SetWindowTextW(g_debug_status, L"FAIL - see activity log");
        log_ui("LONG_MODE_CONTROL_TEST_FAILED", result.detail, "ERROR");
    }
}

void run_cpu_idt_flow_test() {
    log_ui("CPU_IDT_FLOW_TEST_BEGIN",
        "Synthetic CPU run-loop #PF -> same-CPL IDT -> PTE repair -> IRETQ retry; no firmware bytes are executed.");
    SetWindowTextW(g_debug_status, L"Running CPU-to-IDT flow test...");
    const auto result = someboxone::run_synthetic_cpu_idt_flow_test();
    if (result.passed) {
        SetWindowTextW(g_debug_status, L"PASS - #PF delivered, repaired, returned with IRETQ");
        log_ui("CPU_IDT_FLOW_TEST_PASSED", result.detail +
            "; vector=" + std::to_string(result.exception_vector) +
            "; PFEC=" + someboxone::hex_value(result.page_fault_error_code) +
            "; CR2=" + someboxone::hex_value(result.cr2) +
            "; handler=" + someboxone::hex_value(result.handler_rip) +
            "; final_rsp=" + someboxone::hex_value(result.final_rsp) +
            "; resumed_value=" + someboxone::hex_value(result.resumed_value));
    } else {
        SetWindowTextW(g_debug_status, L"FAIL - see activity log");
        log_ui("CPU_IDT_FLOW_TEST_FAILED", result.detail, "ERROR");
    }
}


void run_cpu_ud_flow_test() {
    log_ui("CPU_UD_FLOW_TEST_BEGIN",
        "Synthetic guest LIDT -> UD2/#UD (vector 6, no error code) -> same-CPL IDT -> IRETQ skip; no firmware bytes are executed.");
    SetWindowTextW(g_debug_status, L"Running CPU #UD flow test...");
    const auto result = someboxone::run_synthetic_cpu_ud_flow_test();
    if (result.passed) {
        SetWindowTextW(g_debug_status, L"PASS - #UD delivered and returned with IRETQ");
        log_ui("CPU_UD_FLOW_TEST_PASSED", result.detail +
            "; vector=" + std::to_string(result.exception_vector) +
            "; fault_rip=" + someboxone::hex_value(result.faulting_rip) +
            "; resumed_rip=" + someboxone::hex_value(result.resumed_rip) +
            "; final_rsp=" + someboxone::hex_value(result.final_rsp) +
            "; CR2=" + someboxone::hex_value(result.cr2));
    } else {
        SetWindowTextW(g_debug_status, L"FAIL - see activity log");
        log_ui("CPU_UD_FLOW_TEST_FAILED", result.detail, "ERROR");
    }
}

void run_cpu_gp_lidt_flow_test() {
    log_ui("CPU_GP_LIDT_FLOW_TEST_BEGIN",
        "Synthetic guest LIDT with non-canonical base -> #GP(0) through previous IDT -> same-CPL IRETQ; no firmware executed.");
    SetWindowTextW(g_debug_status, L"Running CPU #GP/LIDT flow test...");
    const auto result = someboxone::run_synthetic_cpu_gp_lidt_flow_test();
    if (result.passed) {
        SetWindowTextW(g_debug_status, L"PASS - #GP(0) delivered; previous IDTR preserved");
        log_ui("CPU_GP_LIDT_FLOW_TEST_PASSED", result.detail +
            "; vector=" + std::to_string(result.exception_vector) +
            "; error_code=" + someboxone::hex_value(result.error_code) +
            "; has_error_code=" + (result.has_error_code ? "true" : "false") +
            "; fault_rip=" + someboxone::hex_value(result.faulting_rip) +
            "; resumed_rip=" + someboxone::hex_value(result.resumed_rip) +
            "; idtr_base=" + someboxone::hex_value(result.previous_idtr_base) +
            "; final_rsp=" + someboxone::hex_value(result.final_rsp));
    } else {
        SetWindowTextW(g_debug_status, L"FAIL - see activity log");
        log_ui("CPU_GP_LIDT_FLOW_TEST_FAILED", result.detail, "ERROR");
    }
}

void run_cpu_bp_flow_test() {
    log_ui("CPU_BP_FLOW_TEST_BEGIN",
        "Synthetic guest LIDT -> INT3/#BP (vector 3, no error code) -> same-CPL IDT -> IRETQ; no firmware bytes are executed.");
    SetWindowTextW(g_debug_status, L"Running CPU #BP flow test...");
    const auto result = someboxone::run_synthetic_cpu_bp_flow_test();
    if (result.passed) {
        SetWindowTextW(g_debug_status, L"PASS - #BP saved next RIP and returned with IRETQ");
        log_ui("CPU_BP_FLOW_TEST_PASSED", result.detail +
            "; vector=" + std::to_string(result.exception_vector) +
            "; int3_rip=" + someboxone::hex_value(result.int3_rip) +
            "; saved_rip=" + someboxone::hex_value(result.saved_rip) +
            "; final_rsp=" + someboxone::hex_value(result.final_rsp) +
            "; CR2=" + someboxone::hex_value(result.cr2));
    } else {
        SetWindowTextW(g_debug_status, L"FAIL - see activity log");
        log_ui("CPU_BP_FLOW_TEST_FAILED", result.detail, "ERROR");
    }
}

void run_idt_delivery_test() {
    log_ui("IDT_DELIVERY_TEST_BEGIN",
        "Synthetic 64-bit IDT gate and same-CPL exception frame; no firmware bytes are executed.");
    SetWindowTextW(g_debug_status, L"Running synthetic IDT delivery test...");
    const auto result = someboxone::run_synthetic_idt_delivery_test();
    if (result.passed) {
        SetWindowTextW(g_debug_status, L"PASS - IDT gate delivered a synthetic exception");
        log_ui("IDT_DELIVERY_TEST_PASSED", result.detail +
            "; handler=" + someboxone::hex_value(result.handler_rip) +
            "; old_rsp=" + someboxone::hex_value(result.old_rsp) +
            "; new_rsp=" + someboxone::hex_value(result.new_rsp));
    } else {
        SetWindowTextW(g_debug_status, L"FAIL - see activity log");
        log_ui("IDT_DELIVERY_TEST_FAILED", result.detail, "ERROR");
    }
}

void run_xvd_storage_probe() {
    fs::path source;
    if (!g_boot_source_path.empty()) source = g_boot_source_path;
    else if (!g_current_path.empty()) source = fs::path(g_current_path);
    if (source.empty()) {
        SetWindowTextW(g_debug_status, L"Select an image before probing XVD storage");
        log_ui("XVD_STORAGE_PROBE_BLOCKED", "No image selected.", "WARN");
        return;
    }
    log_ui("XVD_STORAGE_PROBE_BEGIN", path_utf8(source));
    const auto probe = someboxone::ReadOnlyXvdVolume::probe(source);
    if (probe.logical_read_supported) {
        SetWindowTextW(g_debug_status, probe.data_integrity_enabled
            ? L"PARTIAL - fixed volume readable; per-block hash verification absent"
            : L"PASS - read-only fixed plaintext XVD access available");
    } else {
        SetWindowTextW(g_debug_status, probe.header_recognized
            ? L"BLOCKED - logical XVD storage unavailable; see activity log"
            : L"Not an XVD image or header could not be parsed");
    }
    log_ui(probe.logical_read_supported ? "XVD_STORAGE_PROBE_SUPPORTED" :
        (probe.header_recognized ? "XVD_STORAGE_PROBE_UNAVAILABLE" : "XVD_STORAGE_PROBE_FAILED"),
        "path=" + path_utf8(source) +
        "; header_recognized=" + (probe.header_recognized ? "true" : "false") +
        "; type=" + std::to_string(probe.xvd_type) +
        "; encrypted=" + (probe.data_encrypted ? "true" : "false") +
        "; logical_read_supported=" + (probe.logical_read_supported ? "true" : "false") +
        "; detail=" + probe.detail,
        probe.logical_read_supported ? "INFO" : "WARN");
}

void run_page_fault_diagnostic_test() {
    log_ui("PAGE_FAULT_TEST_BEGIN",
        "Synthetic long-mode user read from a non-present page; no firmware bytes are executed.");
    SetWindowTextW(g_debug_status, L"Running architectural page-fault diagnostic...");
    const auto result = someboxone::run_synthetic_page_fault_test();
    if (result.passed) {
        SetWindowTextW(g_debug_status, L"PASS - synthetic #PF recorded; vector 14, PFEC 0x4");
        log_ui("PAGE_FAULT_TEST_PASSED", result.detail +
            "; vector=" + std::to_string(result.exception_vector) +
            "; PFEC=" + someboxone::hex_value(result.error_code) +
            "; CR2=" + someboxone::hex_value(result.cr2));
        if (SendMessageW(g_verbose, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            log_ui("PAGE_FAULT_TEST_DETAIL",
                "P=0 (not present), W/R=0 (read), U/S=1 (user), RSVD=0, I/D=0; "
                "this diagnostic-only run does not configure an IDTR; opt-in same-CPL runtime #PF delivery is exercised by the CPU-to-IDT flow test.");
        }
    } else {
        SetWindowTextW(g_debug_status, L"FAIL - see activity log");
        log_ui("PAGE_FAULT_TEST_FAILED", result.detail, "ERROR");
    }
}

void run_paging_smoke_test() {
    log_ui("PAGING_SMOKE_TEST_BEGIN", "Synthetic four-level page tables; no firmware memory is read or executed.");
    SetWindowTextW(g_debug_status, L"Running x86-64 paging test...");
    someboxone::GuestAddressSpace memory(1024 * 1024);
    if (!memory.map_ram(0, 1024 * 1024, "synthetic paging-test RAM")) {
        SetWindowTextW(g_debug_status, L"FAIL - unable to map paging-test RAM");
        log_ui("PAGING_SMOKE_TEST_FAILED", "RAM mapping failed.", "ERROR");
        return;
    }
    const bool tables_ok = memory.write(0x1000, 8, 0x2007) &&
                           memory.write(0x2000, 8, 0x3007) &&
                           memory.write(0x3010, 8, 0x4007) &&
                           memory.write(0x4008, 8, 0x12007);
    if (!tables_ok) {
        SetWindowTextW(g_debug_status, L"FAIL - could not create test page tables");
        log_ui("PAGING_SMOKE_TEST_FAILED", "Writing synthetic page-table entries failed.", "ERROR");
        return;
    }
    const someboxone::X64PageTableWalker walker(memory);
    const auto basic = walker.translate(0x401234, 0x1000, someboxone::X64AccessType::Read,
                                        someboxone::X64PrivilegeLevel::User);
    const bool nx_entry_written = memory.write(0x4008, 8, 0x12007 | (1ULL << 63U));
    const auto nx = walker.translate(0x401234, 0x1000, someboxone::X64AccessType::Execute,
                                     someboxone::X64PrivilegeLevel::User);
    const bool passed = basic.success && basic.physical_address == 0x12234 && basic.page_size == 4096 &&
                        nx_entry_written && !nx.success && nx.fault == someboxone::X64PageFault::ExecuteAccessDenied;
    if (passed) {
        SetWindowTextW(g_debug_status, L"PASS - 4-level paging translation and NX protection");
        log_ui("PAGING_SMOKE_TEST_PASSED", "VA 0x401234 -> PA 0x12234; 4 KiB page; user read allowed; NX execute denied.");
        if (SendMessageW(g_verbose, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            log_ui("PAGING_SMOKE_TEST_DETAIL", "Walk levels=4; page_size=4096; execute_fault=" +
                   std::string(someboxone::x64_page_fault_name(nx.fault)) + "; synthetic table entries only.");
        }
    } else {
        const std::string detail = "translate_success=" + std::to_string(basic.success) +
            "; physical=" + someboxone::hex_value(basic.physical_address) +
            "; NX fault=" + someboxone::x64_page_fault_name(nx.fault) + "; " + basic.detail;
        SetWindowTextW(g_debug_status, L"FAIL - see activity log");
        log_ui("PAGING_SMOKE_TEST_FAILED", detail, "ERROR");
    }
}

void open_log_folder() {
    const auto path = someboxone::log_file_path().parent_path();
    log_ui("OPEN_LOG_FOLDER", path_utf8(path));
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(g_main, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) log_ui("OPEN_LOG_FOLDER_FAILED", "ShellExecute error=" + std::to_string(result), "ERROR");
}

void export_log() {
    std::vector<wchar_t> filename(32768, L'\0');
    const wchar_t* suggested = L"SomeBoxOne.log";
    for (std::size_t i = 0; suggested[i] != L'\0' && i + 1 < filename.size(); ++i) filename[i] = suggested[i];
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_main;
    dialog.lpstrFilter = L"Log files\0*.log;*.txt\0All files\0*.*\0\0";
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.lpstrDefExt = L"log";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    dialog.lpstrTitle = L"Export SomeBoxOne activity log";
    if (!GetSaveFileNameW(&dialog)) {
        log_ui("EXPORT_LOG_CANCELLED", "Export log dialog was cancelled.");
        return;
    }
    const fs::path destination(filename.data());
    log_ui("EXPORT_LOG_BEGIN", path_utf8(destination));
    const auto source = someboxone::log_file_path();
    std::ifstream in(source, std::ios::binary);
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!in || !out) {
        log_ui("EXPORT_LOG_FAILED", "source=" + path_utf8(source) + "; destination=" + path_utf8(destination), "ERROR");
        MessageBoxW(g_main, L"Could not read the current log or create the export file.", L"SomeBoxOne - Export Log", MB_ICONERROR | MB_OK);
        return;
    }
    out << in.rdbuf();
    if (!out) log_ui("EXPORT_LOG_FAILED", "Write error: " + path_utf8(destination), "ERROR");
    else log_ui("EXPORT_LOG_COMPLETE", path_utf8(destination));
}

HWND create_control(HINSTANCE instance, const wchar_t* class_name, const wchar_t* text, DWORD style,
                    int id, std::vector<HWND>* page_controls = nullptr, DWORD ex_style = 0) {
    HWND control = CreateWindowExW(ex_style, class_name, text, style, 0, 0, 0, 0, g_main,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (page_controls && control) page_controls->push_back(control);
    if (control && g_body_font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_body_font), TRUE);
    return control;
}

void show_page(Page page) {
    g_current_page = page;
    for (HWND control : g_games_controls) ShowWindow(control, page == Page::Games ? SW_SHOW : SW_HIDE);
    for (HWND control : g_firmware_controls) ShowWindow(control, page == Page::Firmware ? SW_SHOW : SW_HIDE);
    for (HWND control : g_debug_controls) ShowWindow(control, page == Page::Debug ? SW_SHOW : SW_HIDE);
    for (HWND control : g_settings_controls) ShowWindow(control, page == Page::Settings ? SW_SHOW : SW_HIDE);
    SetWindowTextW(g_games_tab, page == Page::Games ? L"> Games" : L"Games");
    SetWindowTextW(g_boot_tab, page == Page::Firmware ? L"> Firmware" : L"Firmware");
    SetWindowTextW(g_debug_tab, page == Page::Debug ? L"> Debug / Scan" : L"Debug / Scan");
    SetWindowTextW(g_settings_tab, page == Page::Settings ? L"> Settings" : L"Settings");
    const char* section = page == Page::Games ? "Games" : (page == Page::Firmware ? "Firmware" : (page == Page::Debug ? "Debug / Scan" : "Settings"));
    log_ui("SECTION_CHANGED", section);
    InvalidateRect(g_main, nullptr, TRUE);
}

void layout_controls(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int margin = 18;
    const int header_y = 15;
    const int nav_y = 80;
    const int content_y = 123;
    const int button_h = 32;
    const int log_y = (std::max)(content_y + (g_current_page == Page::Settings ? 580 : 290), height - 190);
    const int page_bottom = log_y - 12;
    const int content_w = (std::max)(200, width - margin * 2);

    MoveWindow(GetDlgItem(hwnd, kHeaderLabel), margin, header_y, content_w, 31, TRUE);
    MoveWindow(GetDlgItem(hwnd, kSubtitleLabel), margin, 48, content_w, 23, TRUE);
    MoveWindow(g_games_tab, margin, nav_y, 110, button_h, TRUE);
    MoveWindow(g_boot_tab, margin + 118, nav_y, 126, button_h, TRUE);
    MoveWindow(g_debug_tab, margin + 252, nav_y, 150, button_h, TRUE);
    MoveWindow(g_settings_tab, margin + 410, nav_y, 112, button_h, TRUE);

    // Settings page: volume sliders, audio switch, frame-rate lock + FPS counter, logging, graphics backend.
    MoveWindow(GetDlgItem(hwnd, 1061), margin, content_y + 8, 330, 26, TRUE);
    MoveWindow(g_master_volume_track, margin, content_y + 38, 420, 34, TRUE);
    MoveWindow(g_master_volume_value, margin + 430, content_y + 42, 70, 25, TRUE);
    MoveWindow(GetDlgItem(hwnd, 1062), margin, content_y + 86, 330, 26, TRUE);
    MoveWindow(g_game_volume_track, margin, content_y + 116, 420, 34, TRUE);
    MoveWindow(g_game_volume_value, margin + 430, content_y + 120, 70, 25, TRUE);
    MoveWindow(g_audio_enabled_check, margin, content_y + 160, 500, 28, TRUE);
    MoveWindow(GetDlgItem(hwnd, kFrameRateLabel), margin, content_y + 204, 330, 26, TRUE);
    MoveWindow(g_frame_rate_combo, margin, content_y + 232, 220, 200, TRUE);
    MoveWindow(g_show_fps_check, margin + 240, content_y + 234, 260, 28, TRUE);
    MoveWindow(g_keep_logs_check, margin, content_y + 280, 500, 28, TRUE);
    MoveWindow(GetDlgItem(hwnd, 1063), margin, content_y + 322, 300, 26, TRUE);
    MoveWindow(g_graphics_backend_combo, margin, content_y + 350, 470, 260, TRUE);
    MoveWindow(GetDlgItem(hwnd, 1064), margin, content_y + 398, content_w, 95, TRUE);
    MoveWindow(g_settings_save, margin, content_y + 504, 150, button_h, TRUE);
    MoveWindow(g_settings_status, margin + 164, content_y + 508, content_w - 164, 50, TRUE);

    // Games page: two button rows keep the separate-display controls usable when
    // the user resizes the main window down to its minimum width.
    int gx = margin;
    MoveWindow(g_game_open, gx, content_y + 8, 174, button_h, TRUE); gx += 182;
    MoveWindow(g_game_refresh, gx, content_y + 8, 82, button_h, TRUE); gx += 90;
    MoveWindow(g_game_save, gx, content_y + 8, 128, button_h, TRUE); gx += 136;
    MoveWindow(g_game_launch, gx, content_y + 8, 140, button_h, TRUE);
    MoveWindow(g_game_stop, margin, content_y + 46, 88, button_h, TRUE);
    MoveWindow(g_game_frames, margin + 100, content_y + 46, 160, button_h, TRUE);
    MoveWindow(g_game_display, margin + 272, content_y + 46, 160, button_h, TRUE);
    MoveWindow(g_game_status, margin, content_y + 83, content_w, 24, TRUE);
    MoveWindow(g_game_path, margin, content_y + 110, content_w, 24, TRUE);
    MoveWindow(GetDlgItem(hwnd, kGameNoticeLabel), margin, content_y + 138, content_w, 43, TRUE);
    MoveWindow(g_game_report, margin, content_y + 186, content_w, (std::max)(80, page_bottom - (content_y + 186)), TRUE);

    // Firmware page: source selection, preflight attempt and a persistent attempt transcript.
    MoveWindow(g_boot_select, margin, content_y + 8, 158, button_h, TRUE);
    MoveWindow(g_attempt_boot, margin + 168, content_y + 8, 145, button_h, TRUE);
    MoveWindow(g_clear_boot, margin + 323, content_y + 8, 118, button_h, TRUE);
    MoveWindow(g_boot_source, margin, content_y + 48, content_w, 24, TRUE);
    MoveWindow(g_boot_status, margin, content_y + 75, content_w, 23, TRUE);
    MoveWindow(GetDlgItem(hwnd, 1030), margin, content_y + 101, content_w, 51, TRUE);
    MoveWindow(g_boot_output, margin, content_y + 157, content_w, (std::max)(80, page_bottom - (content_y + 157)), TRUE);

    // Debug / Scan page: general image/folder inspection, storage probes and synthetic CPU diagnostics.
    int x = margin;
    const int y = content_y + 8;
    MoveWindow(g_open, x, y, 118, button_h, TRUE); x += 126;
    MoveWindow(g_scan, x, y, 118, button_h, TRUE); x += 126;
    MoveWindow(g_refresh, x, y, 82, button_h, TRUE); x += 90;
    MoveWindow(g_save, x, y, 122, button_h, TRUE); x += 130;
    MoveWindow(g_cpu_test, x, y, 154, button_h, TRUE); x += 162;
    MoveWindow(g_paging_test, x, y, 138, button_h, TRUE);
    MoveWindow(g_long_mode_test, margin, content_y + 47, 170, button_h, TRUE);
    MoveWindow(g_verbose, margin + 180, content_y + 51, 260, 24, TRUE);
    MoveWindow(g_debug_status, margin + 450, content_y + 51, content_w - 450, 24, TRUE);
    MoveWindow(g_page_fault_test, margin, content_y + 83, 202, button_h, TRUE);
    MoveWindow(g_idt_test, margin + 212, content_y + 83, 190, button_h, TRUE);
    MoveWindow(g_storage_probe, margin + 410, content_y + 83, 190, button_h, TRUE);
    MoveWindow(g_cpu_idt_flow, margin + 608, content_y + 83, 220, button_h, TRUE);
    MoveWindow(g_cpu_ud_flow, margin, content_y + 120, 190, button_h, TRUE);
    MoveWindow(g_cpu_bp_flow, margin + 200, content_y + 120, 190, button_h, TRUE);
    MoveWindow(g_cpu_gp_lidt_flow, margin + 400, content_y + 120, 235, button_h, TRUE);
    MoveWindow(g_path, margin, content_y + 156, content_w, 23, TRUE);
    const int report_y = content_y + 184;
    MoveWindow(g_report, margin, report_y, content_w, (std::max)(80, page_bottom - report_y), TRUE);

    // Activity log is shared across all three sections and saved automatically to disk.
    MoveWindow(g_activity_label, margin, log_y, 350, 22, TRUE);
    MoveWindow(g_open_log_folder, width - margin - 248, log_y - 2, 122, 25, TRUE);
    MoveWindow(g_export_log, width - margin - 120, log_y - 2, 120, 25, TRUE);
    MoveWindow(g_activity, margin, log_y + 24, content_w, (std::max)(78, height - (log_y + 24) - 14), TRUE);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_main = hwnd;
        const HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance;
        HWND header = create_control(instance, L"STATIC", L"SomeBoxOne", WS_CHILD | WS_VISIBLE, kHeaderLabel);
        HWND subtitle = create_control(instance, L"STATIC",
            L"Xbox One Emulator  |  Games  |  Firmware  |  Debug / Scan  |  Settings",
            WS_CHILD | WS_VISIBLE, kSubtitleLabel);
        g_header_font = CreateFontW(24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_body_font = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        if (g_header_font) SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(g_header_font), TRUE);
        if (g_body_font) SendMessageW(subtitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_body_font), TRUE);

        g_games_tab = create_control(instance, L"BUTTON", L"> Games", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kGamesTab);
        g_boot_tab = create_control(instance, L"BUTTON", L"Firmware", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kBootTab);
        g_debug_tab = create_control(instance, L"BUTTON", L"Debug / Scan", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kDebugTab);
        g_settings_tab = create_control(instance, L"BUTTON", L"Settings", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kSettingsTab);

        g_game_open = create_control(instance, L"BUTTON", L"Open game package (.xvc)...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     kGameOpenButton, &g_games_controls);
        g_game_refresh = create_control(instance, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                        kGameRefreshButton, &g_games_controls);
        EnableWindow(g_game_refresh, FALSE);
        g_game_save = create_control(instance, L"BUTTON", L"Save game report...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     kGameSaveButton, &g_games_controls);
        EnableWindow(g_game_save, FALSE);
        g_game_stop = create_control(instance, L"BUTTON", L"Stop run", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     kGameStopButton, &g_games_controls);
        EnableWindow(g_game_stop, FALSE);
        g_game_frames = create_control(instance, L"BUTTON", L"Open frames folder", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       kGameFramesButton, &g_games_controls);
        g_game_display = create_control(instance, L"BUTTON", L"Open graphics window", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                        kGameDisplayButton, &g_games_controls);
        g_game_launch = create_control(instance, L"BUTTON", L"Launch title...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       kGameLaunchButton, &g_games_controls);
        g_game_status = create_control(instance, L"STATIC", L"No game package inspected this session.", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       kGameStatusLabel, &g_games_controls);
        g_game_path = create_control(instance, L"STATIC", L"No .xvc package selected", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                     kGamePathLabel, &g_games_controls, WS_EX_CLIENTEDGE);
        create_control(instance, L"STATIC",
            L"Game package (.xvc) inspection is read-only. Launch title runs an already-unpacked folder; captured CPU-backed D3D frames appear in a separate D3D11/DXGI graphics window (no GPU rasterisation or audio emulation yet).",
            WS_CHILD | WS_VISIBLE | SS_LEFT, kGameNoticeLabel, &g_games_controls);
        g_game_report = create_control(instance, L"EDIT",
            L"Games section\r\n\r\nSelect an .xvc Xbox game package to inspect its available descriptor and region metadata.\r\n",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            kGameReportEdit, &g_games_controls, WS_EX_CLIENTEDGE);

        g_boot_select = create_control(instance, L"BUTTON", L"Select boot image...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       kBootSelectButton, &g_firmware_controls);
        g_attempt_boot = create_control(instance, L"BUTTON", L"Attempt boot", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                        kAttemptBootButton, &g_firmware_controls);
        g_clear_boot = create_control(instance, L"BUTTON", L"Clear output", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      kClearBootButton, &g_firmware_controls);
        g_boot_source = create_control(instance, L"STATIC", L"Boot source: (none selected)", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                       kBootSourceLabel, &g_firmware_controls, WS_EX_CLIENTEDGE);
        g_boot_status = create_control(instance, L"STATIC", L"Idle - no boot session running", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        kBootStatusLabel, &g_firmware_controls);
        create_control(instance, L"STATIC",
            L"Experimental boot workflow: the button below runs file and metadata preflight only. It will not execute Xbox firmware until CPU architectural startup, storage, authenticated boot-chain, hypervisor and device layers are implemented.",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 1030, &g_firmware_controls);
        g_boot_output = create_control(instance, L"EDIT",
            L"SomeBoxOne v0.23.0 firmware preflight console\r\n\r\n"
            L"Choose an OS image such as system.xvd, host.xvd, or uERA.xvd, then select Attempt boot.\r\n"
            L"Current stage: read-only preflight. The source image is never modified and its bytes are not executed.\r\n",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            kBootOutputEdit, &g_firmware_controls, WS_EX_CLIENTEDGE);

        g_open = create_control(instance, L"BUTTON", L"Open image...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                kOpenButton, &g_debug_controls);
        g_scan = create_control(instance, L"BUTTON", L"Scan folder...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                kScanButton, &g_debug_controls);
        g_refresh = create_control(instance, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   kRefreshButton, &g_debug_controls);
        EnableWindow(g_refresh, FALSE);
        g_save = create_control(instance, L"BUTTON", L"Save report...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                kSaveButton, &g_debug_controls);
        EnableWindow(g_save, FALSE);
        g_cpu_test = create_control(instance, L"BUTTON", L"Run CPU micro-test", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                    kCpuTestButton, &g_debug_controls);
        g_paging_test = create_control(instance, L"BUTTON", L"Run paging test", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       kPagingTestButton, &g_debug_controls);
        g_long_mode_test = create_control(instance, L"BUTTON", L"Run long-mode test", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                          kLongModeTestButton, &g_debug_controls);
        g_page_fault_test = create_control(instance, L"BUTTON", L"Run page-fault test", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                           kPageFaultTestButton, &g_debug_controls);
        g_idt_test = create_control(instance, L"BUTTON", L"Run IDT delivery test", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                    kIdtTestButton, &g_debug_controls);
        g_storage_probe = create_control(instance, L"BUTTON", L"Probe XVD storage", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                         kStorageProbeButton, &g_debug_controls);
        g_cpu_idt_flow = create_control(instance, L"BUTTON", L"Run CPU-to-IDT flow test", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                           kCpuIdtFlowButton, &g_debug_controls);
        g_cpu_ud_flow = create_control(instance, L"BUTTON", L"Run CPU #UD flow test", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       kCpuUdFlowButton, &g_debug_controls);
        g_cpu_bp_flow = create_control(instance, L"BUTTON", L"Run CPU #BP flow test", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       kCpuBpFlowButton, &g_debug_controls);
        g_cpu_gp_lidt_flow = create_control(instance, L"BUTTON", L"Run CPU #GP/LIDT test", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                            kCpuGpLidtFlowButton, &g_debug_controls);
        g_verbose = create_control(instance, L"BUTTON", L"Verbose debug details", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                   kVerboseCheck, &g_debug_controls);
        SendMessageW(g_verbose, BM_SETCHECK, BST_CHECKED, 0);
        g_debug_status = create_control(instance, L"STATIC", L"CPU test has not run this session.", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        kDebugStatusLabel, &g_debug_controls);
        g_path = create_control(instance, L"STATIC", L"No image or folder selected", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                kPathLabel, &g_debug_controls, WS_EX_CLIENTEDGE);
        g_report = create_control(instance, L"EDIT",
            L"Debug / Scan section\r\n\r\nOpen a firmware/system image or scan an extracted OSU/restore folder.\r\n"
            L"CPU, paging, long-mode and exception-flow tests use synthetic bytes and tables created by SomeBoxOne; they never execute firmware.",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            kReportEdit, &g_debug_controls, WS_EX_CLIENTEDGE);

        create_control(instance, L"STATIC", L"Master volume", WS_CHILD | WS_VISIBLE | SS_LEFT, 1061, &g_settings_controls);
        g_master_volume_track = create_control(instance, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
                                                kMasterVolumeTrack, &g_settings_controls);
        g_master_volume_value = create_control(instance, L"STATIC", L"100%", WS_CHILD | WS_VISIBLE | SS_LEFT, kMasterVolumeValue, &g_settings_controls);
        create_control(instance, L"STATIC", L"Game volume", WS_CHILD | WS_VISIBLE | SS_LEFT, 1062, &g_settings_controls);
        g_game_volume_track = create_control(instance, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
                                             kGameVolumeTrack, &g_settings_controls);
        g_game_volume_value = create_control(instance, L"STATIC", L"100%", WS_CHILD | WS_VISIBLE | SS_LEFT, kGameVolumeValue, &g_settings_controls);
        g_audio_enabled_check = create_control(instance, L"BUTTON", L"Play game audio (software-mixed XAudio2)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                               kAudioEnabledCheck, &g_settings_controls);
        create_control(instance, L"STATIC", L"Frame-rate lock", WS_CHILD | WS_VISIBLE | SS_LEFT, kFrameRateLabel, &g_settings_controls);
        g_frame_rate_combo = create_control(instance, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                            kFrameRateCombo, &g_settings_controls);
        SendMessageW(g_frame_rate_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"60 FPS"));
        SendMessageW(g_frame_rate_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"30 FPS"));
        g_show_fps_check = create_control(instance, L"BUTTON", L"Show FPS counter on screen", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                          kShowFpsCheck, &g_settings_controls);
        g_keep_logs_check = create_control(instance, L"BUTTON", L"Keep activity and game-session logs on disk", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                           kKeepLogsCheck, &g_settings_controls);
        create_control(instance, L"STATIC", L"Graphics backend", WS_CHILD | WS_VISIBLE | SS_LEFT, 1063, &g_settings_controls);
        g_graphics_backend_combo = create_control(instance, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                                  kGraphicsBackendCombo, &g_settings_controls);
        SendMessageW(g_graphics_backend_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Direct3D 11 / DXGI (available)"));
        SendMessageW(g_graphics_backend_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Vulkan (not implemented; falls back to D3D11)"));
        create_control(instance, L"STATIC",
            L"Audio note: XAudio2 voices are mixed in software (PCM 8/16/24/32-bit and float) and played through the default Windows output device; compressed XMA/ADPCM voices stay silent. Frame-rate note: the lock only holds the game at 30 or 60 FPS - it cannot make a slow emulator faster, and the counter always shows the real measured rate.\r\n"
            L"Graphics note: D3D11/DXGI is the only implemented display backend. Selecting Vulkan stores that preference but currently falls back to D3D11.\r\n"
            L"Logging note: this checkbox controls both the activity log and separate per-game session transcripts. When off, live output remains in the GUI/console but no new log files are written; existing files are not deleted.",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 1064, &g_settings_controls);
        g_settings_save = create_control(instance, L"BUTTON", L"Save settings", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                         kSettingsSaveButton, &g_settings_controls);
        g_settings_status = create_control(instance, L"STATIC", L"Changes are applied when you save.", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                           kSettingsStatus, &g_settings_controls);
        SendMessageW(g_master_volume_track, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessageW(g_game_volume_track, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        load_settings_controls();

        g_activity_label = create_control(instance, L"STATIC", L"Activity log (separate game-session files are also saved when enabled)", WS_CHILD | WS_VISIBLE | SS_LEFT, kActivityLabel);
        g_open_log_folder = create_control(instance, L"BUTTON", L"Open log folder", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                           kOpenLogFolderButton);
        g_export_log = create_control(instance, L"BUTTON", L"Export log...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      kExportLogButton);
        g_activity = create_control(instance, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            kActivityEdit, nullptr, WS_EX_CLIENTEDGE);
        SendMessageW(g_activity, EM_SETLIMITTEXT, 200000, 0);
        SendMessageW(g_report, EM_SETLIMITTEXT, 2000000, 0);
        SendMessageW(g_game_report, EM_SETLIMITTEXT, 2000000, 0);
        SendMessageW(g_boot_output, EM_SETLIMITTEXT, 500000, 0);
        SendMessageW(g_activity, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(ANSI_FIXED_FONT)), TRUE);
        SendMessageW(g_report, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(ANSI_FIXED_FONT)), TRUE);
        SendMessageW(g_game_report, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(ANSI_FIXED_FONT)), TRUE);
        SendMessageW(g_boot_output, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(ANSI_FIXED_FONT)), TRUE);
        DragAcceptFiles(hwnd, TRUE);
        load_recent_activity();
        show_page(Page::Games);
        layout_controls(hwnd);
        return 0;
    }
    case kTitleLaunchDone:
        finish_title_launch(reinterpret_cast<std::string*>(lParam));
        return 0;
    case kTitleLaunchProgressLine: {
        auto* line = reinterpret_cast<std::string*>(lParam);
        if (line) {
            append_edit(g_game_report, utf8_to_wide(*line + "\r\n"));
            append_game_session_line(*line);
            log_ui("GAME_CONSOLE_OUTPUT", *line);
            delete line;
        }
        return 0;
    }
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam) == g_master_volume_track || reinterpret_cast<HWND>(lParam) == g_game_volume_track) {
            update_volume_labels();
            return 0;
        }
        break;
    case WM_SIZE:
        layout_controls(hwnd);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 760;
        info->ptMinTrackSize.y = 640;
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kGamesTab: show_page(Page::Games); return 0;
        case kBootTab: show_page(Page::Firmware); return 0;
        case kDebugTab: show_page(Page::Debug); return 0;
        case kSettingsTab: show_page(Page::Settings); return 0;
        case kSettingsSaveButton: save_settings_from_controls(); return 0;
        case kOpenButton: open_image(); return 0;
        case kSaveButton: save_report(); return 0;
        case kScanButton: scan_folder(); return 0;
        case kRefreshButton: refresh_current(); return 0;
        case kGameOpenButton: open_game_package(); return 0;
        case kGameRefreshButton: refresh_game_package(); return 0;
        case kGameSaveButton: save_game_report(); return 0;
        case kGameLaunchButton: launch_title_from_gui(); return 0;
        case kGameStopButton: stop_title_launch(); return 0;
        case kGameFramesButton: open_game_frames_folder(); return 0;
        case kGameDisplayButton: (void)ensure_graphics_display_window(); return 0;
        case kBootSelectButton: select_boot_image(); return 0;
        case kAttemptBootButton: attempt_boot(); return 0;
        case kClearBootButton: clear_boot_output(); return 0;
        case kCpuTestButton: run_cpu_smoke_test(); return 0;
        case kPagingTestButton: run_paging_smoke_test(); return 0;
        case kLongModeTestButton: run_long_mode_control_test(); return 0;
        case kPageFaultTestButton: run_page_fault_diagnostic_test(); return 0;
        case kIdtTestButton: run_idt_delivery_test(); return 0;
        case kStorageProbeButton: run_xvd_storage_probe(); return 0;
        case kCpuIdtFlowButton: run_cpu_idt_flow_test(); return 0;
        case kCpuUdFlowButton: run_cpu_ud_flow_test(); return 0;
        case kCpuBpFlowButton: run_cpu_bp_flow_test(); return 0;
        case kCpuGpLidtFlowButton: run_cpu_gp_lidt_flow_test(); return 0;
        case kOpenLogFolderButton: open_log_folder(); return 0;
        case kExportLogButton: export_log(); return 0;
        case kVerboseCheck:
            log_ui("VERBOSE_LOGGING_TOGGLED", SendMessageW(g_verbose, BM_GETCHECK, 0, 0) == BST_CHECKED ? "enabled" : "disabled");
            return 0;
        default: break;
        }
        break;
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (count > 0) {
            const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
            std::vector<wchar_t> path(static_cast<std::size_t>(length) + 1, L'\0');
            if (DragQueryFileW(drop, 0, path.data(), length + 1) > 0) {
                const fs::path dropped(path.data());
                std::error_code ec;
                if (fs::is_directory(dropped, ec) && !ec) {
                    log_ui("DRAG_DROP_FOLDER", path_utf8(dropped));
                    show_page(Page::Debug);
                    scan_path(dropped);
                } else {
                    log_ui("DRAG_DROP_FILE", path_utf8(dropped));
                    std::wstring extension = dropped.extension().wstring();
                    std::transform(extension.begin(), extension.end(), extension.begin(),
                        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
                    if (extension == L".xvc") {
                        show_page(Page::Games);
                        inspect_game_path(dropped);
                    } else {
                        show_page(Page::Debug);
                        inspect_path(dropped);
                    }
                }
                if (count > 1) log_ui("DRAG_DROP_EXTRA_ITEMS_IGNORED", "Only the first dropped item was processed.", "WARN");
            }
        } else {
            log_ui("DRAG_DROP_EMPTY", "No files or folders were provided.", "WARN");
        }
        DragFinish(drop);
        return 0;
    }
    case WM_DESTROY: {
        log_ui("APPLICATION_SHUTDOWN", "GUI closing.");
        g_gui_shutting_down = true;
        g_launch_cancel = true;
        if (g_launch_thread.joinable()) g_launch_thread.join();
        MSG pending{};
        while (PeekMessageW(&pending, hwnd, kTitleLaunchProgressLine, kTitleLaunchProgressLine, PM_REMOVE)) {
            auto* line = reinterpret_cast<std::string*>(pending.lParam);
            if (line) { append_game_session_line(*line); delete line; }
        }
        while (PeekMessageW(&pending, hwnd, kTitleLaunchDone, kTitleLaunchDone, PM_REMOVE))
            delete reinterpret_cast<std::string*>(pending.lParam);
        if (g_launch_running && !g_game_session_saved_for_current_launch && !g_game_session_lines.empty()) {
            append_game_session_line("[SomeBoxOne] GUI shutdown requested while the game was running; this transcript may be incomplete.");
            std::filesystem::path session_path;
            std::string session_error;
            if (someboxone::save_game_session_log(g_game_session_title, g_game_session_lines, &session_path, &session_error))
                log_ui("GAME_SESSION_LOG_SAVED_ON_SHUTDOWN", path_utf8(session_path));
            else if (someboxone::persistent_logging_enabled())
                log_ui("GAME_SESSION_LOG_SAVE_FAILED_ON_SHUTDOWN", session_error, "ERROR");
            g_game_session_saved_for_current_launch = true;
        }
        close_graphics_display_window();
        g_shared_frames.close();
        if (g_game_console_output && g_game_console_output != INVALID_HANDLE_VALUE) CloseHandle(g_game_console_output);
        g_game_console_output = INVALID_HANDLE_VALUE;
        if (g_game_console_allocated) FreeConsole();
        g_game_console_ready.store(false, std::memory_order_relaxed);
        if (g_header_font) DeleteObject(g_header_font);
        if (g_body_font) DeleteObject(g_body_font);
        PostQuitMessage(0);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    INITCOMMONCONTROLSEX common_controls{};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&common_controls);
    g_settings = someboxone::load_runtime_config();
    someboxone::set_persistent_logging_enabled(g_settings.keep_logs);
    someboxone::log_event("INFO", "APPLICATION_START", std::string("SomeBoxOne v") + someboxone::version_string);
    const wchar_t* class_name = L"SomeBoxOneMainWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = class_name;
    if (!RegisterClassExW(&wc)) {
        someboxone::log_event("ERROR", "WINDOW_CLASS_REGISTER_FAILED", std::to_string(GetLastError()));
        MessageBoxW(nullptr, L"Could not register the SomeBoxOne window class.", L"SomeBoxOne", MB_ICONERROR | MB_OK);
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, class_name, L"SomeBoxOne - Xbox One Emulator",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1080, 800,
        nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        someboxone::log_event("ERROR", "WINDOW_CREATE_FAILED", std::to_string(GetLastError()));
        MessageBoxW(nullptr, L"Could not create the SomeBoxOne window.", L"SomeBoxOne", MB_ICONERROR | MB_OK);
        return 1;
    }
    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
