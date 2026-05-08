#include "g5_codec.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#define MAX_IMAGE_FLIPS G5_FLIPS
#include "Group5.h"
#include "g5enc.inl"
#include "g5dec.inl"

#define G5_JOIN2(a, b) a##b
#define G5_JOIN(a, b) G5_JOIN2(a, b)
#define G5_NAME(name) G5_JOIN(name, G5_SUFFIX)

namespace {

static void append_u32le(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
}

static bool read_u32le(const std::vector<uint8_t>& input, size_t& offset, uint32_t& value) {
    if (offset + 4 > input.size()) return false;
    value = static_cast<uint32_t>(input[offset])
        | (static_cast<uint32_t>(input[offset + 1]) << 8)
        | (static_cast<uint32_t>(input[offset + 2]) << 16)
        | (static_cast<uint32_t>(input[offset + 3]) << 24);
    offset += 4;
    return true;
}

static const char* g5_error_name(int rc) {
    switch (rc) {
        case G5_SUCCESS: return "success";
        case G5_INVALID_PARAMETER: return "invalid parameter";
        case G5_DECODE_ERROR: return "decode error";
        case G5_UNSUPPORTED_FEATURE: return "unsupported feature";
        case G5_ENCODE_COMPLETE: return "encode complete";
        case G5_DECODE_COMPLETE: return "decode complete";
        case G5_NOT_INITIALIZED: return "not initialized";
        case G5_DATA_OVERFLOW: return "data overflow";
        case G5_MAX_FLIPS_EXCEEDED: return "max flips exceeded";
        default: return "unknown error";
    }
}

static bool line_exceeds_flip_limit(const uint8_t* row, int width, size_t pitch) {
    int flips = 0;
    int previous = 0;
    for (int x = 0; x < width; ++x) {
        const uint8_t byte = row[static_cast<size_t>(x) / 8u];
        const int bit = (byte >> (7 - (x & 7))) & 1;
        if (bit != previous) {
            ++flips;
            previous = bit;
            if (flips >= G5_FLIPS - 4) return true;
        }
    }
    (void)pitch;
    return false;
}

static G5Result compress_rows(const uint8_t* rows, int width, int row_count, size_t pitch) {
    G5Result result;
    if (width <= 0 || row_count <= 0 || pitch == 0) {
        result.error = "invalid geometry";
        return result;
    }

    for (int row = 0; row < row_count; ++row) {
        if (line_exceeds_flip_limit(rows + static_cast<size_t>(row) * pitch, width, pitch)) {
            result.error = "row " + std::to_string(row) + " exceeds MAX_IMAGE_FLIPS="
                + std::to_string(G5_FLIPS);
            return result;
        }
    }

    const size_t source_size = pitch * static_cast<size_t>(row_count);
    const size_t out_capacity = source_size * 4u + 4096u;
    std::vector<uint8_t> out(out_capacity);

    G5ENCIMAGE enc;
    std::memset(&enc, 0, sizeof(enc));
    int rc = g5_encode_init(&enc, width, row_count, out.data(), static_cast<int>(out.size()));
    if (rc != G5_SUCCESS) {
        result.error = std::string("g5_encode_init failed: ") + g5_error_name(rc);
        return result;
    }

    std::vector<uint8_t> row_buffer(pitch + 1u, 0);
    for (int row = 0; row < row_count; ++row) {
        std::memcpy(row_buffer.data(), rows + static_cast<size_t>(row) * pitch, pitch);
        row_buffer[pitch] = 0;
        rc = g5_encode_encodeLine(&enc, row_buffer.data());
        const bool last = row == row_count - 1;
        if ((!last && rc != G5_SUCCESS) || (last && rc != G5_ENCODE_COMPLETE)) {
            result.error = "g5_encode_encodeLine failed at row " + std::to_string(row)
                + ": " + g5_error_name(rc);
            return result;
        }
    }

    out.resize(static_cast<size_t>(g5_encode_getOutSize(&enc)));
    result.ok = true;
    result.data = std::move(out);
    return result;
}

static G5Result decompress_rows(const uint8_t* compressed, size_t compressed_size, int width, int row_count, size_t pitch) {
    G5Result result;
    if (compressed_size == 0 || width <= 0 || row_count <= 0 || pitch == 0) {
        result.error = "invalid compressed data or geometry";
        return result;
    }

    std::vector<uint8_t> out(pitch * static_cast<size_t>(row_count));
    G5DECIMAGE dec;
    std::memset(&dec, 0, sizeof(dec));
    int rc = g5_decode_init(&dec, width, row_count, const_cast<uint8_t*>(compressed), static_cast<int>(compressed_size));
    if (rc != G5_SUCCESS) {
        result.error = std::string("g5_decode_init failed: ") + g5_error_name(rc);
        return result;
    }

    std::vector<uint8_t> row_buffer(pitch + 1u, 0);
    for (int row = 0; row < row_count; ++row) {
        std::fill(row_buffer.begin(), row_buffer.end(), 0);
        rc = g5_decode_line(&dec, row_buffer.data());
        const bool last = row == row_count - 1;
        if (rc != G5_SUCCESS && !(last && rc == G5_DECODE_COMPLETE)) {
            result.error = "g5_decode_line failed at row " + std::to_string(row)
                + ": " + g5_error_name(rc);
            return result;
        }
        std::memcpy(out.data() + static_cast<size_t>(row) * pitch, row_buffer.data(), pitch);
    }

    result.ok = true;
    result.data = std::move(out);
    return result;
}

} // namespace

G5Result G5_NAME(g5_compress_virtual_)(const std::vector<uint8_t>& input, int width, int height, int plane_count) {
    const size_t pitch = (static_cast<size_t>(width) + 7u) / 8u;
    return compress_rows(input.data(), width, height * plane_count, pitch);
}

G5Result G5_NAME(g5_decompress_virtual_)(const std::vector<uint8_t>& input, int width, int height, int plane_count) {
    const size_t pitch = (static_cast<size_t>(width) + 7u) / 8u;
    return decompress_rows(input.data(), input.size(), width, height * plane_count, pitch);
}

G5Result G5_NAME(g5_compress_perplane_)(const std::vector<uint8_t>& input, int width, int height, int plane_count) {
    G5Result result;
    const size_t pitch = (static_cast<size_t>(width) + 7u) / 8u;
    const size_t plane_size = pitch * static_cast<size_t>(height);
    std::vector<uint8_t> out;

    for (int plane = 0; plane < plane_count; ++plane) {
        const uint8_t* plane_data = input.data() + static_cast<size_t>(plane) * plane_size;
        G5Result part = compress_rows(plane_data, width, height, pitch);
        if (!part.ok) {
            result.error = "plane " + std::to_string(plane) + ": " + part.error;
            return result;
        }
        append_u32le(out, static_cast<uint32_t>(part.data.size()));
        out.insert(out.end(), part.data.begin(), part.data.end());
    }

    result.ok = true;
    result.data = std::move(out);
    return result;
}

G5Result G5_NAME(g5_decompress_perplane_)(const std::vector<uint8_t>& input, int width, int height, int plane_count) {
    G5Result result;
    const size_t pitch = (static_cast<size_t>(width) + 7u) / 8u;
    const size_t plane_size = pitch * static_cast<size_t>(height);
    std::vector<uint8_t> out(plane_size * static_cast<size_t>(plane_count));
    size_t offset = 0;

    for (int plane = 0; plane < plane_count; ++plane) {
        uint32_t compressed_size = 0;
        if (!read_u32le(input, offset, compressed_size) || offset + compressed_size > input.size()) {
            result.error = "invalid per-plane frame";
            return result;
        }
        G5Result part = decompress_rows(input.data() + offset, compressed_size, width, height, pitch);
        if (!part.ok) {
            result.error = "plane " + std::to_string(plane) + ": " + part.error;
            return result;
        }
        if (part.data.size() != plane_size) {
            result.error = "decoded plane size mismatch";
            return result;
        }
        std::memcpy(out.data() + static_cast<size_t>(plane) * plane_size, part.data.data(), plane_size);
        offset += compressed_size;
    }

    if (offset != input.size()) {
        result.error = "trailing bytes after per-plane frames";
        return result;
    }

    result.ok = true;
    result.data = std::move(out);
    return result;
}

#undef G5_NAME
#undef G5_JOIN
#undef G5_JOIN2
#undef MAX_IMAGE_FLIPS
