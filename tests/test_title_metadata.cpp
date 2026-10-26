// Package title/splash metadata regression test. Uses tiny placeholder files only.
#include "title_launch.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace someboxone;

static void write_text(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary);
    out << s;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "SomeBoxOneTitleMetadataTests";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Mount");
    const fs::path mount = root / "Mount";
    write_text(mount / "Game.exe", "placeholder executable");
    write_text(mount / "SplashScreen.png", "placeholder image");
    const std::string manifest =
        "<?xml version=\"1.0\"?><Package><Identity Name=\"Test.Identity\" Publisher=\"CN=Test\" Version=\"1.0.0.0\" />"
        "<Applications><Application Executable=\"Game.exe\"><VisualElements DisplayName=\"Test Game\" />"
        "<SplashScreen Image=\"SplashScreen.png\" /></Application></Applications></Package>";
    write_text(mount / "AppxManifest.xml", manifest);

    const TitlePackageInfo good = inspect_title_package(mount);
    assert(good.ok);
    assert(good.display_name == "Test Game");
    assert(good.splash_image == "SplashScreen.png");
    assert(good.splash_path == mount / "SplashScreen.png");

    // A manifest may not use parent traversal to display an unrelated file.
    write_text(root / "Outside.png", "not in package");
    write_text(mount / "AppxManifest.xml",
        "<?xml version=\"1.0\"?><Package><Identity Name=\"Test.Identity\" Publisher=\"CN=Test\" Version=\"1.0.0.0\" />"
        "<Applications><Application Executable=\"Game.exe\"><VisualElements DisplayName=\"Test Game\" />"
        "<SplashScreen Image=\"../Outside.png\" /></Application></Applications></Package>");
    const TitlePackageInfo unsafe = inspect_title_package(mount);
    assert(unsafe.ok);
    assert(unsafe.splash_path.empty());

    fs::remove_all(root, ec);
    std::cout << "Title metadata tests passed.\n";
    return 0;
}
