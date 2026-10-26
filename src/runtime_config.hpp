#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace someboxone {

enum class XboxLiveMode : std::uint8_t {
    OfflineStub,
    Disabled
};

struct RuntimeConfig {
    XboxLiveMode xbox_live_mode{XboxLiveMode::OfflineStub};
    // Offline WinRT metadata only. These values are not credentials and do not
    // enable Xbox Live authentication or network services.
    std::string xbox_live_sandbox{"RETAIL"};
    std::string xbox_live_title_id{"0"};
    std::string xbox_live_primary_service_config_id{};
    std::uint32_t graphics_width{1280};
    std::uint32_t graphics_height{720};
    bool open_graphics_window_on_launch{true};
    bool graphics_fullscreen{false};
    // User-facing settings. Master x game volume scales the XAudio2 software mixer output (audio_mixer.hpp).
    std::uint32_t master_volume_percent{100};
    std::uint32_t game_volume_percent{100};
    // Frame-rate lock applied at the title's main-loop boundary: exactly 30 or 60 (frame_pacer.hpp). Anything else in the file is reset to 60.
    std::uint32_t frame_rate_limit{60};
    bool show_fps_counter{true};   // draw the FPS counter on the game picture
    bool audio_enabled{true};      // false mutes the sound device; the mixer still runs so the title's audio callbacks keep firing
    bool keep_logs{true};
    // d3d11 is implemented; vulkan is a future backend selection and currently falls back to D3D11.
    std::string graphics_backend{"d3d11"};
};

namespace runtime_config_detail {
inline std::string trim(std::string value) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    while (!value.empty() && !not_space(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && !not_space(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

inline std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

inline bool parse_bool(const std::string& value, bool fallback) {
    const auto v = lower(trim(value));
    if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
    if (v == "false" || v == "0" || v == "no" || v == "off") return false;
    return fallback;
}

inline std::uint32_t parse_dimension(const std::string& value, std::uint32_t fallback) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(trim(value), &consumed, 10);
        if (consumed != trim(value).size() || parsed > std::numeric_limits<std::uint32_t>::max()) return fallback;
        return static_cast<std::uint32_t>(parsed);
    } catch (...) {
        return fallback;
    }
}
} // namespace runtime_config_detail

inline RuntimeConfig parse_runtime_config(std::string_view text) {
    RuntimeConfig config;
    std::istringstream input{std::string(text)};
    std::string line;
    while (std::getline(input, line)) {
        line = runtime_config_detail::trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';' || line.front() == '[') continue;
        const auto comment = line.find_first_of("#;");
        if (comment != std::string::npos) line.resize(comment);
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = runtime_config_detail::lower(runtime_config_detail::trim(line.substr(0, equals)));
        const std::string value = runtime_config_detail::trim(line.substr(equals + 1));
        if (key == "xboxlivemode") {
            const auto mode = runtime_config_detail::lower(value);
            config.xbox_live_mode = (mode == "disabled" || mode == "off")
                ? XboxLiveMode::Disabled : XboxLiveMode::OfflineStub;
        } else if (key == "xboxlivesandbox") {
            config.xbox_live_sandbox = value.substr(0, 128);
        } else if (key == "xboxlivetitleid") {
            config.xbox_live_title_id = value.substr(0, 32);
        } else if (key == "xboxliveprimaryserviceconfigid") {
            config.xbox_live_primary_service_config_id = value.substr(0, 128);
        } else if (key == "graphicswidth") {
            config.graphics_width = runtime_config_detail::parse_dimension(value, config.graphics_width);
        } else if (key == "graphicsheight") {
            config.graphics_height = runtime_config_detail::parse_dimension(value, config.graphics_height);
        } else if (key == "opengraphicswindowonlaunch") {
            config.open_graphics_window_on_launch = runtime_config_detail::parse_bool(value, config.open_graphics_window_on_launch);
        } else if (key == "graphicsfullscreen") {
            config.graphics_fullscreen = runtime_config_detail::parse_bool(value, config.graphics_fullscreen);
        } else if (key == "mastervolumepercent") {
            config.master_volume_percent = (std::min)(100U, runtime_config_detail::parse_dimension(value, config.master_volume_percent));
        } else if (key == "gamevolumepercent") {
            config.game_volume_percent = (std::min)(100U, runtime_config_detail::parse_dimension(value, config.game_volume_percent));
        } else if (key == "frameratelimit") {
            const auto v = runtime_config_detail::parse_dimension(value, config.frame_rate_limit);
            config.frame_rate_limit = (v == 30) ? 30U : 60U;
        } else if (key == "showfpscounter") {
            config.show_fps_counter = runtime_config_detail::parse_bool(value, config.show_fps_counter);
        } else if (key == "audioenabled") {
            config.audio_enabled = runtime_config_detail::parse_bool(value, config.audio_enabled);
        } else if (key == "keeplogs") {
            config.keep_logs = runtime_config_detail::parse_bool(value, config.keep_logs);
        } else if (key == "graphicsbackend") {
            const auto backend = runtime_config_detail::lower(value);
            config.graphics_backend = backend == "vulkan" ? "vulkan" : "d3d11";
        }
    }

    constexpr std::uint32_t default_width = 1280;
    constexpr std::uint32_t default_height = 720;
    if (config.graphics_width < 320 || config.graphics_width > 8192 ||
        config.graphics_height < 240 || config.graphics_height > 8192 ||
        static_cast<std::uint64_t>(config.graphics_width) * config.graphics_height > 16ULL * 1024ULL * 1024ULL) {
        config.graphics_width = default_width;
        config.graphics_height = default_height;
    }
    config.master_volume_percent = (std::min)(100U, config.master_volume_percent);
    config.game_volume_percent = (std::min)(100U, config.game_volume_percent);
    config.frame_rate_limit = config.frame_rate_limit == 30 ? 30U : 60U;
    return config;
}

inline std::string serialize_runtime_config(const RuntimeConfig& config) {
    std::ostringstream out;
    out << "# SomeBoxOne runtime configuration\n"
        << "# Xbox Live is a local offline compatibility stub, not an authenticated service.\n"
        << "XboxLiveMode=" << (config.xbox_live_mode == XboxLiveMode::Disabled ? "disabled" : "offline-stub") << "\n"
        << "XboxLiveSandbox=" << config.xbox_live_sandbox << "\n"
        << "XboxLiveTitleId=" << config.xbox_live_title_id << "\n"
        << "XboxLivePrimaryServiceConfigId=" << config.xbox_live_primary_service_config_id << "\n\n"
        << "GraphicsWidth=" << config.graphics_width << "\n"
        << "GraphicsHeight=" << config.graphics_height << "\n"
        << "OpenGraphicsWindowOnLaunch=" << (config.open_graphics_window_on_launch ? "true" : "false") << "\n"
        << "GraphicsFullscreen=" << (config.graphics_fullscreen ? "true" : "false") << "\n"
        << "GraphicsBackend=" << (config.graphics_backend == "vulkan" ? "vulkan" : "d3d11") << "\n\n"
        << "MasterVolumePercent=" << (std::min)(100U, config.master_volume_percent) << "\n"
        << "GameVolumePercent=" << (std::min)(100U, config.game_volume_percent) << "\n"
        << "AudioEnabled=" << (config.audio_enabled ? "true" : "false") << "\n\n"
        << "# Frame-rate lock: 30 or 60 FPS. The FPS counter is drawn on the game picture.\n"
        << "FrameRateLimit=" << (config.frame_rate_limit == 30 ? 30 : 60) << "\n"
        << "ShowFpsCounter=" << (config.show_fps_counter ? "true" : "false") << "\n\n"
        << "KeepLogs=" << (config.keep_logs ? "true" : "false") << "\n";
    return out.str();
}

inline std::filesystem::path runtime_config_path();

inline bool save_runtime_config(const RuntimeConfig& config, std::string* error = nullptr) {
    const auto path = runtime_config_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "Unable to create settings directory: " + ec.message();
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error) *error = "Unable to open settings file for writing: " + path.string();
        return false;
    }
    const std::string text = serialize_runtime_config(config);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) {
        if (error) *error = "Failed while writing settings file: " + path.string();
        return false;
    }
    return true;
}

inline std::string default_runtime_config_text() {
    return "# SomeBoxOne runtime configuration\n"
           "# Xbox Live is not implemented as an online service. offline-stub is a local compatibility placeholder only.\n"
           "# Set XboxLiveMode=disabled to make the probe reject XboxLiveConfiguration activation.\n"
           "XboxLiveMode=offline-stub\n"
           "# Offline XboxLiveConfiguration static-property defaults (not authenticated values).\n"
           "# Set TitleId and PrimaryServiceConfigId to values from the title package if known.\n"
           "XboxLiveSandbox=RETAIL\n"
           "XboxLiveTitleId=0\n"
           "XboxLivePrimaryServiceConfigId=\n"
           "\n"
           "# Host display settings (the window receives frames over shared memory when a renderer publishes them).\n"
           "GraphicsWidth=1280\n"
           "GraphicsHeight=720\n"
           "OpenGraphicsWindowOnLaunch=true\n"
           "GraphicsFullscreen=false\n"
           "GraphicsBackend=d3d11\n"
           "MasterVolumePercent=100\n"
           "GameVolumePercent=100\n"
           "AudioEnabled=true\n"
           "\n"
           "# Frame-rate lock: 30 or 60 FPS. The FPS counter is drawn on the game picture.\n"
           "FrameRateLimit=60\n"
           "ShowFpsCounter=true\n"
           "KeepLogs=true\n";
}

inline std::filesystem::path runtime_config_path() {
#ifdef _WIN32
    // Use the wide Windows environment API so non-ASCII user profile paths survive.
    if (const wchar_t* local_app_data = ::_wgetenv(L"LOCALAPPDATA")) {
        if (*local_app_data != L'\0') return std::filesystem::path(local_app_data) / L"SomeBoxOne" / L"SomeBoxOne.ini";
    }
    if (const wchar_t* profile = ::_wgetenv(L"USERPROFILE")) {
        if (*profile != L'\0') return std::filesystem::path(profile) / L"AppData" / L"Local" / L"SomeBoxOne" / L"SomeBoxOne.ini";
    }
#else
    if (const char* config_home = std::getenv("XDG_CONFIG_HOME")) {
        if (*config_home != '\0') return std::filesystem::path(config_home) / "SomeBoxOne" / "SomeBoxOne.ini";
    }
    if (const char* home = std::getenv("HOME")) {
        if (*home != '\0') return std::filesystem::path(home) / ".config" / "SomeBoxOne" / "SomeBoxOne.ini";
    }
#endif
    return std::filesystem::current_path() / "SomeBoxOne.ini";
}

inline RuntimeConfig load_runtime_config() {
    const auto path = runtime_config_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (output) output << default_runtime_config_text();
        return RuntimeConfig{};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return parse_runtime_config(contents.str());
}

inline const char* xbox_live_mode_name(XboxLiveMode mode) noexcept {
    return mode == XboxLiveMode::Disabled ? "disabled" : "offline-stub";
}

} // namespace someboxone
