#include "image_source.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using someboxone::ReadOnlyImageSource;

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path path = fs::temp_directory_path() / ("someboxone-image-source-" + std::to_string(stamp) + ".bin");
    const std::string content = "read-only image source test data";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        assert(static_cast<bool>(out));
    }

    std::string error;
    auto source = ReadOnlyImageSource::open(path, &error);
    assert(source.has_value());
    assert(source->size() == content.size());
    auto slice = source->read_at(10, 5, &error);
    assert(slice.has_value() && slice->size() == 5);
    assert(std::string(slice->begin(), slice->end()) == "image");
    assert(!source->read_at(source->size() - 2, 3, &error).has_value());
    assert(!error.empty());
    assert(!source->read_at(0, ReadOnlyImageSource::max_read_bytes + 1, &error).has_value());
    assert(!ReadOnlyImageSource::open(path.string() + ".missing", &error).has_value());

    fs::remove(path);
    std::cout << "SomeBoxOne bounded read-only image-source tests passed (no XVD decryption or writes).\n";
    return 0;
}
