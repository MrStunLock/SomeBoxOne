#pragma once

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace someboxone {
inline std::atomic<bool>& persistent_logging_setting() {
    static std::atomic<bool> enabled{true};
    return enabled;
}
inline void set_persistent_logging_enabled(bool enabled) noexcept {
    persistent_logging_setting().store(enabled, std::memory_order_relaxed);
}
inline bool persistent_logging_enabled() noexcept {
    return persistent_logging_setting().load(std::memory_order_relaxed);
}

namespace logging_detail {
namespace fs = std::filesystem;

inline std::string one_line(std::string value) {
    for (char& ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '\0') ch = ' ';
    }
    return value;
}

inline std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

inline fs::path choose_log_path() {
    fs::path root;
#ifdef _WIN32
#ifdef _MSC_VER
    // Avoid the deprecated _wgetenv CRT call (C4996) in Visual Studio builds.
    wchar_t* local_app_data = nullptr;
    std::size_t local_app_data_length = 0;
    const errno_t env_result = ::_wdupenv_s(&local_app_data, &local_app_data_length, L"LOCALAPPDATA");
    if (env_result == 0 && local_app_data && *local_app_data) {
        root = fs::path(local_app_data) / L"SomeBoxOne" / L"logs";
    }
    std::free(local_app_data);
#else
    const wchar_t* local_app_data = ::_wgetenv(L"LOCALAPPDATA");
    if (local_app_data && *local_app_data) root = fs::path(local_app_data) / L"SomeBoxOne" / L"logs";
#endif
#else
    const char* state_home = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    if (state_home && *state_home) root = fs::path(state_home) / "SomeBoxOne" / "logs";
    else if (home && *home) root = fs::path(home) / ".local" / "state" / "SomeBoxOne" / "logs";
#endif
    if (root.empty()) {
        std::error_code ec;
        root = fs::current_path(ec);
        if (ec) root = fs::path(".");
        root /= "SomeBoxOneLogs";
    }
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        std::error_code cwd_error;
        auto fallback = fs::current_path(cwd_error);
        if (cwd_error) fallback = fs::path(".");
        fallback /= "SomeBoxOneLogs";
        ec.clear();
        fs::create_directories(fallback, ec);
        if (!ec) root = fallback;
    }
    return root / "SomeBoxOne.log";
}

inline bool append_to(const fs::path& path, const std::string& line) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return false;
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    out.put('\n');
    return static_cast<bool>(out);
}
} // namespace logging_detail

inline std::string format_log_line(const std::string& level,
                                   const std::string& action,
                                   const std::string& detail = {}) {
    std::ostringstream out;
    out << logging_detail::timestamp() << " [" << logging_detail::one_line(level) << "] "
        << logging_detail::one_line(action);
    if (!detail.empty()) out << " | " << logging_detail::one_line(detail);
    return out.str();
}

// Writes a timestamped, single-line event to a per-user persistent log. A
// working-directory fallback is attempted if the preferred location fails.
inline std::string log_event(const std::string& level,
                             const std::string& action,
                             const std::string& detail = {}) {
    static std::mutex mutex;
    const std::lock_guard<std::mutex> lock(mutex);
    const std::string line = format_log_line(level, action, detail);
    if (!persistent_logging_enabled()) return line;
    const auto preferred = logging_detail::choose_log_path();
    if (logging_detail::append_to(preferred, line)) return line;

    std::error_code ec;
    auto fallback = std::filesystem::current_path(ec);
    if (ec) fallback = std::filesystem::path(".");
    fallback /= "SomeBoxOne.log";
    logging_detail::append_to(fallback, line);
    return line;
}

inline std::filesystem::path log_file_path() {
    return logging_detail::choose_log_path();
}

inline std::string read_recent_log(std::size_t max_bytes = 120000) {
    const auto path = log_file_path();
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end <= 0) return {};
    const auto size = static_cast<std::size_t>(end);
    const auto start = size > max_bytes ? size - max_bytes : 0;
    in.seekg(static_cast<std::streamoff>(start), std::ios::beg);
    std::string text;
    text.resize(size - start);
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!in && !in.eof()) return {};
    if (start != 0) {
        const auto newline = text.find('\n');
        if (newline != std::string::npos) text.erase(0, newline + 1);
    }
    return text;
}


// A completed title launch gets its own transcript file, separate from the
// high-level activity log. The existing KeepLogs setting gates both channels.
namespace logging_detail {
inline std::string sanitize_session_title(std::string title) {
    for (char& ch : title) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20U || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
            ch = '-';
        }
    }
    while (!title.empty() && (title.back() == ' ' || title.back() == '.')) title.pop_back();
    while (!title.empty() && title.front() == ' ') title.erase(title.begin());
    if (title.empty() || title == "." || title == "..") title = "Title";
    if (title.size() > 72) title.resize(72);
    return title;
}

inline std::string filename_timestamp(std::time_t when) {
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &when);
#else
    localtime_r(&when, &local);
#endif
    char stamp[32]{};
    if (std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local) == 0) return "unknown-time";
    return stamp;
}

inline std::string readable_timestamp(std::time_t when) {
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &when);
#else
    localtime_r(&when, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

inline std::filesystem::path make_game_session_log_path(const std::filesystem::path& folder,
                                                         const std::string& title,
                                                         std::time_t when = std::time(nullptr)) {
    return folder / ("GameSession-" + filename_timestamp(when) + "-" + sanitize_session_title(title) + ".log");
}
} // namespace logging_detail

inline bool write_game_session_log_to_path(const std::filesystem::path& destination,
                                           const std::string& title,
                                           const std::vector<std::string>& transcript,
                                           bool enabled,
                                           std::string* error = nullptr) {
    if (!enabled) {
        if (error) *error = "Persistent logging is disabled in Settings.";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
        if (error) *error = "Unable to create game-session log folder: " + ec.message();
        return false;
    }
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "Unable to open game-session log for writing: " + destination.string();
        return false;
    }
    const auto now = std::time(nullptr);
    out << "SomeBoxOne game-session transcript\n"
        << "Title: " << logging_detail::one_line(title) << "\n"
        << "Saved: " << logging_detail::readable_timestamp(now) << "\n"
        << "Transcript lines: " << transcript.size() << "\n"
        << "============================================================\n";
    for (const auto& line : transcript) out << line << '\n';
    out.flush();
    if (!out) {
        if (error) *error = "Write failed for game-session log: " + destination.string();
        return false;
    }
    return true;
}

inline bool save_game_session_log(const std::string& title,
                                 const std::vector<std::string>& transcript,
                                 std::filesystem::path* saved_path = nullptr,
                                 std::string* error = nullptr) {
    if (!persistent_logging_enabled()) {
        if (error) *error = "Persistent logging is disabled in Settings.";
        return false;
    }
    const auto folder = log_file_path().parent_path();
    auto path = logging_detail::make_game_session_log_path(folder, title);
    // Avoid overwriting an earlier launch of the same title in the same second.
    for (unsigned suffix = 2; std::filesystem::exists(path) && suffix < 10000; ++suffix) {
        path = folder / ("GameSession-" + logging_detail::filename_timestamp(std::time(nullptr)) + "-" +
                         logging_detail::sanitize_session_title(title) + "-" + std::to_string(suffix) + ".log");
    }
    if (!write_game_session_log_to_path(path, title, transcript, true, error)) return false;
    if (saved_path) *saved_path = path;
    return true;
}

} // namespace someboxone
