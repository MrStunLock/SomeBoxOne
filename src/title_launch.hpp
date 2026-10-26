#pragma once

// SomeBoxOne v0.22.0 - title package resolution and the single launch entry point shared
// by the CLI (`launch-title`, `title-info`) and the Win32 GUI (Games > Launch title).
//
// Scope: this launches *already-unpacked* Xbox One title folders (as dumped from an
// installed package: AppxManifest.xml + the title's PE + its data files). It does not
// decrypt .xvc packages; see xvc.hpp for the metadata-only XVC reader.

#include "xbox_runtime.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace someboxone {

struct TitlePackageInfo {
    bool ok{};
    std::string error;
    std::filesystem::path package_root; // folder that holds AppxManifest.xml (the "Mount" folder)
    std::filesystem::path exe_path;
    std::filesystem::path manifest_path;
    std::string identity_name, version, publisher, display_name, executable, title_id, os_name, app_environment;
    std::string splash_image;
    std::filesystem::path splash_path;
    std::filesystem::path system_root;// sibling EmbeddedXvd/Windows/System32 when present
    std::uint64_t exe_size{};
    bool has_data_pack{};
    std::string report() const {
        std::ostringstream o;
        o << "Title package\n";
        if (!ok) { o << "  NOT a launchable title folder: " << error << "\n"; return o.str(); }
        o << "  Name           : " << display_name << " (" << identity_name << " v" << version << ", publisher " << publisher << ")\n"
          << "  Xbox title ID  : " << title_id << "\n  OS / environment: " << os_name << " / " << app_environment << "\n"
          << "  Executable     : " << executable << " (" << exe_size << " bytes)\n  Package root   : " << PeImage::path_utf8(package_root) << "\n"
          << "  Splash screen  : " << (splash_path.empty() ? std::string("not declared or not found") : PeImage::path_utf8(splash_path)) << "\n"
          << "  Redistributables: " << (system_root.empty() ? std::string("none found (HLE only)") : PeImage::path_utf8(system_root)) << "\n";
        return o.str();
    }
};

inline std::string xml_attr(const std::string& xml, const std::string& element, const std::string& attr) {
    std::size_t e = 0;
    for (;;) { // match the whole element name (so <Application> is not confused with <Applications>)
        e = xml.find("<" + element, e);
        if (e == std::string::npos) return {};
        const char next = e + element.size() + 1 < xml.size() ? xml[e + element.size() + 1] : '\0';
        if (next == ' ' || next == '>' || next == '/' || next == '\r' || next == '\n' || next == '\t') break;
        ++e;
    }
    const auto end = xml.find('>', e);
    const auto a = xml.find(attr + "=\"", e);
    if (a == std::string::npos || (end != std::string::npos && a > end)) return {};
    const auto s = a + attr.size() + 2;
    const auto q = xml.find('"', s);
    return q == std::string::npos ? std::string() : xml.substr(s, q - s);
}

inline TitlePackageInfo inspect_title_package(const std::filesystem::path& input) {
    namespace fs = std::filesystem;
    TitlePackageInfo info;
    std::error_code ec;
    fs::path p(input), root;
    if (fs::is_regular_file(p, ec)) root = p.parent_path();
    else if (fs::is_directory(p, ec)) root = p;
    else { info.error = "path does not exist: " + PeImage::path_utf8(input); return info; }
    // Accept: Mount folder, package folder (contains Mount), or a folder above it.
    fs::path manifest;
    for (const auto& cand : {root / "AppxManifest.xml", root / "Mount" / "AppxManifest.xml"})
        if (fs::exists(cand, ec)) { manifest = cand; break; }
    if (manifest.empty() && fs::is_directory(root, ec)) {
        for (const auto& d : fs::directory_iterator(root, ec)) {
            const auto cand = d.path() / "Mount" / "AppxManifest.xml";
            if (d.is_directory(ec) && fs::exists(cand, ec)) { manifest = cand; break; }
        }
    }
    if (manifest.empty()) { info.error = "no AppxManifest.xml found (expected <package>/Mount/AppxManifest.xml)"; return info; }
    std::ifstream in(manifest, std::ios::binary);
    std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    info.manifest_path = manifest;
    info.package_root = manifest.parent_path();
    info.identity_name = xml_attr(xml, "Identity", "Name");
    info.version = xml_attr(xml, "Identity", "Version");
    info.publisher = xml_attr(xml, "Identity", "Publisher");
    info.executable = xml_attr(xml, "Application", "Executable");
    info.display_name = xml_attr(xml, "VisualElements", "DisplayName");
    info.splash_image = xml_attr(xml, "SplashScreen", "Image");
    info.title_id = xml_attr(xml, "mx:XboxLive", "TitleId");
    const auto os = xml.find("<mx:OSName>");
    if (os != std::string::npos) { const auto e = xml.find("</mx:OSName>", os); if (e != std::string::npos) info.os_name = xml.substr(os + 11, e - os - 11); }
    const auto env = xml.find("<mx:ApplicationEnvironment>");
    if (env != std::string::npos) { const auto e = xml.find("</mx:ApplicationEnvironment>", env); if (e != std::string::npos) info.app_environment = xml.substr(env + 27, e - env - 27); }
    if (info.executable.empty()) { info.error = "manifest declares no Application Executable"; return info; }
    if (info.executable.find("..") != std::string::npos || info.executable.find(':') != std::string::npos) { info.error = "manifest executable path is not a plain relative file name"; return info; }
    const fs::path exe = manifest.parent_path() / info.executable;
    if (!fs::exists(exe, ec)) { info.error = "executable listed in manifest is missing: " + PeImage::path_utf8(exe); return info; }
    info.exe_path = exe;
    info.exe_size = static_cast<std::uint64_t>(fs::file_size(exe, ec));

    // The splash screen is package artwork, not a frame fabricated by the guest renderer.
    // Only honour a plain relative path that remains inside the package root.
    if (!info.splash_image.empty()) {
        std::u8string splash_utf8;
        splash_utf8.reserve(info.splash_image.size());
        for (const unsigned char ch : info.splash_image) splash_utf8.push_back(static_cast<char8_t>(ch));
        const fs::path splash_rel(splash_utf8);
        bool safe_relative = !splash_rel.empty() && !splash_rel.is_absolute();
        for (const auto& component : splash_rel) {
            if (component == ".." || component == "." || component.string().find(':') != std::string::npos) {
                safe_relative = false;
                break;
            }
        }
        if (safe_relative) {
            const fs::path candidate = manifest.parent_path() / splash_rel;
            if (fs::is_regular_file(candidate, ec) && !ec) info.splash_path = candidate;
            ec.clear();
        }
    }

    const fs::path sys = manifest.parent_path().parent_path() / "EmbeddedXvd" / "Windows" / "System32";
    if (fs::exists(sys, ec)) info.system_root = sys;
    for (const auto& d : fs::directory_iterator(manifest.parent_path(), ec)) if (d.path().extension() == ".rsdk") info.has_data_pack = true;
    info.ok = true;
    return info;
}

struct TitleLaunchResult {
    std::filesystem::path save_dir;   // where frames/saves for this run were written
    TitlePackageInfo package;
    TitleLaunchReport report;
    std::string text;
};

inline TitleLaunchResult launch_title(const std::filesystem::path& input, TitleLaunchOptions options = {}) {
    TitleLaunchResult res;
    auto emit_text = [&options](const std::string& text) {
        if (!options.progress_sink) return;
        std::size_t begin = 0;
        while (begin < text.size()) {
            const auto end = text.find('\n', begin);
            std::string line = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            options.progress_sink(line);
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    };
    if (options.progress_sink) options.progress_sink("SomeBoxOne game launch: " + PeImage::path_utf8(input));
    res.package = inspect_title_package(input);
    std::ostringstream o;
    o << res.package.report() << "\n";
    emit_text(res.package.report());
    if (!res.package.ok) { res.text = o.str(); emit_text("Title launch stopped: package is not launchable.\n"); return res; }
    std::string err;
    auto image = PeImage::load_file(res.package.exe_path, &err);
    if (!image) { res.report.load_error = err; o << "Could not parse the executable: " << err << "\n"; res.text = o.str(); emit_text("Could not parse the executable: " + err + "\n"); return res; }
    options.package_root = res.package.package_root;
    if (options.save_root.empty()) options.save_root = res.package.package_root.parent_path() / "SomeBoxOneSave";
    res.save_dir = options.save_root;
    options.system_root = res.package.system_root;
    options.command_line = "\"" + res.package.executable + "\"";
    options.exe_name = res.package.executable;
    if (options.progress_sink) options.progress_sink("Executable loaded; starting guest runtime.");
    XboxTitleRuntime runtime(*image, options);
    res.report = runtime.run();
    o << res.report.text;
    o << "\nWhat this means:\n";
    if (res.report.stop_kind == "exit") o << "  The title ran to a normal exit inside SomeBoxOne.\n";
    else if (res.report.frames > 0)
        o << "  The title's real main loop ran for " << res.report.frames << " iterations and its renderer produced frames, which were\n"
             "  saved as PNG files in:\n    " << PeImage::path_utf8(res.save_dir) << "\n"
             "  Graphics are a software capture of the engine's framebuffer (no GPU emulation) and audio is silent. The GUI\n"
             "  polls keyboard/XInput state and supplies it through Gamepad.GetCurrentReading; actual control depends on whether\n"
             "  the title polls that API and may need additional per-title input handling.\n";
    else o << "  The title did NOT reach its main loop. It executed real game/runtime code up to the point above and then stopped\n"
              "  at the first thing SomeBoxOne cannot yet provide.\n";
    res.text = o.str();
    emit_text("Guest runtime stopped; final report will be appended below in the main window.\n");
    return res;
}

} // namespace someboxone
