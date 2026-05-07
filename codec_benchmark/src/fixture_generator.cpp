#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void write_file(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to open " + path.string());
    }
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

static std::vector<uint8_t> pack_od(const std::vector<uint8_t>& pixels, int width, int height, int bpp) {
    const int pixels_per_byte = 8 / bpp;
    const size_t pitch = (static_cast<size_t>(width) + pixels_per_byte - 1u) / pixels_per_byte;
    std::vector<uint8_t> out(pitch * static_cast<size_t>(height), 0);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t value = pixels[static_cast<size_t>(y) * width + x] & ((1u << bpp) - 1u);
            const size_t byte_index = static_cast<size_t>(y) * pitch + static_cast<size_t>(x / pixels_per_byte);
            const int shift = (pixels_per_byte - 1 - (x % pixels_per_byte)) * bpp;
            out[byte_index] |= static_cast<uint8_t>(value << shift);
        }
    }

    return out;
}

static std::vector<uint8_t> pack_1bpp_streams(const std::vector<uint8_t>& pixels, int width, int height, int plane_count) {
    const size_t pitch = (static_cast<size_t>(width) + 7u) / 8u;
    const size_t plane_size = pitch * static_cast<size_t>(height);
    std::vector<uint8_t> out(plane_size * static_cast<size_t>(plane_count), 0);

    for (int plane = 0; plane < plane_count; ++plane) {
        uint8_t mask = static_cast<uint8_t>(1u << plane);
        uint8_t* dst_plane = out.data() + static_cast<size_t>(plane) * plane_size;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if ((pixels[static_cast<size_t>(y) * width + x] & mask) == 0) continue;
                const size_t byte_index = static_cast<size_t>(y) * pitch + static_cast<size_t>(x / 8);
                const int bit = 7 - (x % 8);
                dst_plane[byte_index] |= static_cast<uint8_t>(1u << bit);
            }
        }
    }

    return out;
}

static void make_fixture(const fs::path& folder, const std::string& name, int bpp) {
    constexpr int width = 16;
    constexpr int height = 16;
    std::vector<uint8_t> pixels(width * height, 0);
    const int colors = 1 << bpp;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int block_x = x / 4;
            const int block_y = y / 4;
            pixels[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>((block_x + block_y * 3) % colors);
        }
    }

    const std::string prefix = name + ".16x16";
    write_file(folder / (prefix + ".bs-od"), pack_od(pixels, width, height, bpp));
    write_file(folder / (prefix + ".bs-1bppstreams"), pack_1bpp_streams(pixels, width, height, bpp));
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: fixture_generator <output_folder>\n";
        return 2;
    }

    try {
        fs::path folder = argv[1];
        fs::create_directories(folder);
        make_fixture(folder, "sample-1bpp", 1);
        make_fixture(folder, "sample-2bpp", 2);
        make_fixture(folder, "sample-4bpp", 4);
    } catch (const std::exception& ex) {
        std::cerr << "fixture generation failed: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}

