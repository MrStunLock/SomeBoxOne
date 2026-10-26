#include "logging.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int main() {
    namespace fs = std::filesystem;
    using namespace someboxone;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("SomeBoxOneSessionLogTest-" + std::to_string(stamp));
    const fs::path disabled_path = root / "disabled.log";
    const fs::path enabled_path = root / "enabled.log";
    const std::vector<std::string> lines{
        "SomeBoxOne game launch: Peggle 2.exe",
        "[guest] starting title",
        "Stopped because: [unsupported] unsupported VEX opcode"
    };

    std::string error;
    assert(!write_game_session_log_to_path(disabled_path, "Peggle 2", lines, false, &error));
    assert(!fs::exists(disabled_path));
    assert(!error.empty());

    assert(write_game_session_log_to_path(enabled_path, "Peggle 2", lines, true, &error));
    assert(fs::exists(enabled_path));
    std::ifstream in(enabled_path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(content.find("Title: Peggle 2") != std::string::npos);
    assert(content.find("SomeBoxOne game launch: Peggle 2.exe") != std::string::npos);
    assert(content.find("Stopped because: [unsupported] unsupported VEX opcode") != std::string::npos);

    const auto safe = logging_detail::sanitize_session_title("Peggle 2:/Bad? *Title.");
    assert(safe.find('/') == std::string::npos);
    assert(safe.find('\\') == std::string::npos);
    assert(safe.find(':') == std::string::npos);
    assert(safe.find('?') == std::string::npos);

    std::error_code ignored;
    fs::remove_all(root, ignored);
    return 0;
}
