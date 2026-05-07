#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct G5Result {
    bool ok = false;
    std::vector<uint8_t> data;
    std::string error;
};

G5Result g5_compress_virtual_f64(const std::vector<uint8_t>& input, int width, int height, int plane_count);
G5Result g5_compress_virtual_f128(const std::vector<uint8_t>& input, int width, int height, int plane_count);
G5Result g5_compress_virtual_f256(const std::vector<uint8_t>& input, int width, int height, int plane_count);

G5Result g5_compress_perplane_f64(const std::vector<uint8_t>& input, int width, int height, int plane_count);
G5Result g5_compress_perplane_f128(const std::vector<uint8_t>& input, int width, int height, int plane_count);
G5Result g5_compress_perplane_f256(const std::vector<uint8_t>& input, int width, int height, int plane_count);

G5Result g5_decompress_virtual_f64(const std::vector<uint8_t>& input, int width, int height, int plane_count);
G5Result g5_decompress_virtual_f128(const std::vector<uint8_t>& input, int width, int height, int plane_count);
G5Result g5_decompress_virtual_f256(const std::vector<uint8_t>& input, int width, int height, int plane_count);

G5Result g5_decompress_perplane_f64(const std::vector<uint8_t>& input, int width, int height, int plane_count);
G5Result g5_decompress_perplane_f128(const std::vector<uint8_t>& input, int width, int height, int plane_count);
G5Result g5_decompress_perplane_f256(const std::vector<uint8_t>& input, int width, int height, int plane_count);

