#include "g5_codec.h"

extern "C" {
#include "brotli/decode.h"
#include "brotli/encode.h"
#include "heatshrink_decoder.h"
#include "heatshrink_encoder.h"
#include "lz4.h"
#include "lz4hc.h"
}
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include "zlib.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct InputFile {
    fs::path path;
    std::string prefix;
    int width = 0;
    int height = 0;
};

struct Variant {
    std::string name;
    int level = 0;
    int window_bits = 0;
    int heat_window = 0;
    int heat_lookahead = 0;
    int brotli_quality = 0;
    int brotli_lgwin = 0;
    int zstd_level = 0;
    int zstd_window_log = 0;
    int lz4_block_size = 0;
    int lz4_hc_level = 0;
    int lzss_offset_bits = 0;
    int lzss_length_bits = 0;
    int lzss_raw_bits = 0;
};

struct CompressResult {
    bool ok = false;
    std::vector<uint8_t> data;
    std::string error;
};

struct Options {
    std::string algorithm;
    fs::path folder;
    int runs = 1;
    fs::path jsonl_path;
    bool jsonl_enabled = false;
    std::vector<std::string> variants;
};

static bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size()
        && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0x0f];
                    out += hex[c & 0x0f];
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

static std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("failed to open " + path.string());
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) throw std::runtime_error("failed to size " + path.string());
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!data.empty()) {
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return data;
}

static void write_file(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("failed to open output " + path.string());
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
}

static void append_u16le(std::vector<uint8_t>& out, size_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

static uint16_t read_u16le(const std::vector<uint8_t>& input, size_t offset) {
    return static_cast<uint16_t>(input[offset] | (static_cast<uint16_t>(input[offset + 1]) << 8u));
}

static void append_u16be(std::vector<uint8_t>& out, size_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    out.push_back(static_cast<uint8_t>(value & 0xffu));
}

static uint16_t read_u16be(const std::vector<uint8_t>& input, size_t offset) {
    return static_cast<uint16_t>((static_cast<uint16_t>(input[offset]) << 8u) | input[offset + 1]);
}

static bool parse_resolution_input(const fs::path& path, const std::string& suffix, InputFile& input) {
    const std::string filename = path.filename().string();
    if (!ends_with(filename, suffix)) return false;

    const std::string without_suffix = filename.substr(0, filename.size() - suffix.size());
    const size_t dot = without_suffix.rfind('.');
    if (dot == std::string::npos || dot + 1 >= without_suffix.size()) return false;

    const std::string resolution = without_suffix.substr(dot + 1);
    const size_t x_pos = resolution.find('x');
    if (x_pos == std::string::npos) return false;

    try {
        size_t used_w = 0;
        size_t used_h = 0;
        const int width = std::stoi(resolution.substr(0, x_pos), &used_w);
        const int height = std::stoi(resolution.substr(x_pos + 1), &used_h);
        if (used_w != x_pos || used_h != resolution.size() - x_pos - 1 || width <= 0 || height <= 0) {
            return false;
        }
        input.path = path;
        input.prefix = without_suffix;
        input.width = width;
        input.height = height;
        return true;
    } catch (...) {
        return false;
    }
}

static std::vector<InputFile> find_inputs(const fs::path& folder, const std::string& suffix) {
    std::vector<InputFile> inputs;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder)) {
        if (!entry.is_regular_file()) continue;
        if (ends_with(entry.path().filename().string(), ".dat")) continue;
        InputFile input;
        if (parse_resolution_input(entry.path(), suffix, input)) {
            inputs.push_back(std::move(input));
        }
    }
    std::sort(inputs.begin(), inputs.end(), [](const InputFile& a, const InputFile& b) {
        return a.path.filename().string() < b.path.filename().string();
    });
    return inputs;
}

static CompressResult zlib_compress(const std::vector<uint8_t>& input, int level, int window_bits) {
    CompressResult result;
    z_stream stream{};

    const int init = deflateInit2(&stream, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY);
    if (init != Z_OK) {
        result.error = "deflateInit2 failed: " + std::to_string(init);
        return result;
    }

    std::vector<uint8_t> out(deflateBound(&stream, static_cast<uLong>(input.size())));
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());

    const int rc = deflate(&stream, Z_FINISH);
    if (rc != Z_STREAM_END) {
        result.error = "deflate failed: " + std::to_string(rc);
        deflateEnd(&stream);
        return result;
    }

    out.resize(stream.total_out);
    deflateEnd(&stream);
    result.ok = true;
    result.data = std::move(out);
    return result;
}

static CompressResult zlib_decompress(const std::vector<uint8_t>& input, size_t expected_size, int window_bits) {
    CompressResult result;
    z_stream stream{};
    const int init = inflateInit2(&stream, window_bits);
    if (init != Z_OK) {
        result.error = "inflateInit2 failed: " + std::to_string(init);
        return result;
    }

    std::vector<uint8_t> out(expected_size);
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());

    const int rc = inflate(&stream, Z_FINISH);
    if (rc != Z_STREAM_END || stream.total_out != expected_size) {
        result.error = "inflate failed: " + std::to_string(rc);
        inflateEnd(&stream);
        return result;
    }

    inflateEnd(&stream);
    result.ok = true;
    result.data = std::move(out);
    return result;
}

static CompressResult brotli_compress(const std::vector<uint8_t>& input, int quality, int lgwin) {
    CompressResult result;
    const size_t bound = BrotliEncoderMaxCompressedSize(input.size());
    if (bound == 0 && !input.empty()) {
        result.error = "BrotliEncoderMaxCompressedSize failed";
        return result;
    }

    std::vector<uint8_t> out(bound);
    size_t encoded_size = out.size();
    const BROTLI_BOOL ok = BrotliEncoderCompress(
        quality,
        lgwin,
        BROTLI_MODE_GENERIC,
        input.size(),
        input.data(),
        &encoded_size,
        out.data());
    if (ok != BROTLI_TRUE) {
        result.error = "BrotliEncoderCompress failed";
        return result;
    }

    out.resize(encoded_size);
    result.ok = true;
    result.data = std::move(out);
    return result;
}

static CompressResult brotli_decompress(const std::vector<uint8_t>& input, size_t expected_size) {
    CompressResult result;
    std::vector<uint8_t> out(expected_size);
    size_t decoded_size = out.size();
    const BrotliDecoderResult rc = BrotliDecoderDecompress(
        input.size(),
        input.data(),
        &decoded_size,
        out.data());
    if (rc != BROTLI_DECODER_RESULT_SUCCESS || decoded_size != expected_size) {
        result.error = "BrotliDecoderDecompress failed: " + std::to_string(static_cast<int>(rc));
        return result;
    }

    result.ok = true;
    result.data = std::move(out);
    return result;
}

static CompressResult zstd_compress(const std::vector<uint8_t>& input, int level, int window_log) {
    CompressResult result;
    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (context == nullptr) {
        result.error = "ZSTD_createCCtx failed";
        return result;
    }

    auto cleanup = [&]() {
        ZSTD_freeCCtx(context);
    };

    size_t rc = ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level);
    if (ZSTD_isError(rc)) {
        result.error = "ZSTD_c_compressionLevel failed: " + std::string(ZSTD_getErrorName(rc));
        cleanup();
        return result;
    }

    rc = ZSTD_CCtx_setParameter(context, ZSTD_c_windowLog, window_log);
    if (ZSTD_isError(rc)) {
        result.error = "ZSTD_c_windowLog failed: " + std::string(ZSTD_getErrorName(rc));
        cleanup();
        return result;
    }

    std::vector<uint8_t> out(ZSTD_compressBound(input.size()));
    rc = ZSTD_compress2(context, out.data(), out.size(), input.data(), input.size());
    if (ZSTD_isError(rc)) {
        result.error = "ZSTD_compress2 failed: " + std::string(ZSTD_getErrorName(rc));
        cleanup();
        return result;
    }

    out.resize(rc);
    cleanup();
    result.ok = true;
    result.data = std::move(out);
    return result;
}

static CompressResult zstd_decompress(const std::vector<uint8_t>& input, size_t expected_size, int window_log) {
    CompressResult result;
    ZSTD_DCtx* context = ZSTD_createDCtx();
    if (context == nullptr) {
        result.error = "ZSTD_createDCtx failed";
        return result;
    }

    auto cleanup = [&]() {
        ZSTD_freeDCtx(context);
    };

    const size_t max_window_size = static_cast<size_t>(1) << window_log;
    size_t rc = ZSTD_DCtx_setMaxWindowSize(context, max_window_size);
    if (ZSTD_isError(rc)) {
        result.error = "ZSTD_DCtx_setMaxWindowSize failed: " + std::string(ZSTD_getErrorName(rc));
        cleanup();
        return result;
    }

    std::vector<uint8_t> out(expected_size);
    rc = ZSTD_decompressDCtx(context, out.data(), out.size(), input.data(), input.size());
    if (ZSTD_isError(rc) || rc != expected_size) {
        result.error = "ZSTD_decompressDCtx failed: "
            + std::string(ZSTD_isError(rc) ? ZSTD_getErrorName(rc) : "unexpected size");
        cleanup();
        return result;
    }

    cleanup();
    result.ok = true;
    result.data = std::move(out);
    return result;
}

static CompressResult lz4_compress_blocks(const std::vector<uint8_t>& input, int block_size, int hc_level) {
    CompressResult result;
    if (block_size <= 0 || block_size > 32768) {
        result.error = "invalid LZ4 block size";
        return result;
    }

    std::vector<uint8_t> out;
    out.reserve(input.size());
    out.push_back('O');
    out.push_back('D');
    out.push_back('L');
    out.push_back('4');
    append_u16le(out, static_cast<size_t>(block_size));

    size_t offset = 0;
    while (offset < input.size()) {
        const size_t remaining = input.size() - offset;
        const int chunk_size = static_cast<int>(std::min(remaining, static_cast<size_t>(block_size)));
        const int bound = LZ4_compressBound(chunk_size);
        if (bound <= 0 || bound > 65535) {
            result.error = "LZ4_compressBound failed";
            return result;
        }

        std::vector<char> compressed(static_cast<size_t>(bound));
        const char* src = reinterpret_cast<const char*>(input.data() + offset);
        const int compressed_size = hc_level > 0
            ? LZ4_compress_HC(src, compressed.data(), chunk_size, bound, hc_level)
            : LZ4_compress_default(src, compressed.data(), chunk_size, bound);
        if (compressed_size <= 0) {
            result.error = "LZ4 compression failed";
            return result;
        }

        append_u16le(out, static_cast<size_t>(chunk_size));
        if (compressed_size >= chunk_size) {
            append_u16le(out, 0);
            out.insert(out.end(), input.begin() + static_cast<std::ptrdiff_t>(offset),
                input.begin() + static_cast<std::ptrdiff_t>(offset + static_cast<size_t>(chunk_size)));
        } else {
            append_u16le(out, static_cast<size_t>(compressed_size));
            out.insert(out.end(), compressed.begin(), compressed.begin() + compressed_size);
        }
        offset += static_cast<size_t>(chunk_size);
    }

    result.ok = true;
    result.data = std::move(out);
    return result;
}

static CompressResult lz4_decompress_blocks(const std::vector<uint8_t>& input, size_t expected_size, int block_size) {
    CompressResult result;
    if (input.size() < 6 || input[0] != 'O' || input[1] != 'D' || input[2] != 'L' || input[3] != '4') {
        result.error = "invalid LZ4 block stream header";
        return result;
    }

    const uint16_t stored_block_size = read_u16le(input, 4);
    if (stored_block_size != static_cast<uint16_t>(block_size)) {
        result.error = "LZ4 block size mismatch";
        return result;
    }

    std::vector<uint8_t> out;
    out.reserve(expected_size);
    size_t offset = 6;
    while (offset < input.size()) {
        if (input.size() - offset < 4) {
            result.error = "truncated LZ4 block header";
            return result;
        }
        const uint16_t uncompressed_size = read_u16le(input, offset);
        const uint16_t compressed_size = read_u16le(input, offset + 2);
        offset += 4;
        if (uncompressed_size == 0 || uncompressed_size > stored_block_size) {
            result.error = "invalid LZ4 uncompressed block size";
            return result;
        }
        const size_t payload_size = compressed_size == 0 ? uncompressed_size : compressed_size;
        if (input.size() - offset < payload_size) {
            result.error = "truncated LZ4 block payload";
            return result;
        }

        const size_t output_offset = out.size();
        out.resize(output_offset + uncompressed_size);
        if (compressed_size == 0) {
            std::copy(input.begin() + static_cast<std::ptrdiff_t>(offset),
                input.begin() + static_cast<std::ptrdiff_t>(offset + payload_size),
                out.begin() + static_cast<std::ptrdiff_t>(output_offset));
        } else {
            const int decoded = LZ4_decompress_safe(
                reinterpret_cast<const char*>(input.data() + offset),
                reinterpret_cast<char*>(out.data() + output_offset),
                static_cast<int>(compressed_size),
                static_cast<int>(uncompressed_size));
            if (decoded != static_cast<int>(uncompressed_size)) {
                result.error = "LZ4 decompression failed";
                return result;
            }
        }
        offset += payload_size;
    }

    if (out.size() != expected_size) {
        result.error = "LZ4 decompressed size mismatch";
        return result;
    }

    result.ok = true;
    result.data = std::move(out);
    return result;
}

static size_t lzssraw_hash3(const std::vector<uint8_t>& input, size_t offset) {
    const uint32_t value = (static_cast<uint32_t>(input[offset]) << 16u)
        ^ (static_cast<uint32_t>(input[offset + 1]) << 8u)
        ^ static_cast<uint32_t>(input[offset + 2]);
    return (value * 2654435761u) >> 20u;
}

static bool lzssraw_valid_params(int offset_bits, int length_bits, int raw_bits) {
    return offset_bits >= 1
        && offset_bits <= 15
        && length_bits >= 1
        && length_bits <= 15
        && raw_bits == 7
        && offset_bits + length_bits <= 15;
}

static uint8_t lzssraw_header0(bool raw_mode, int offset_bits) {
    constexpr uint8_t version = 1;
    return static_cast<uint8_t>((raw_mode ? 0x80u : 0u) | (version << 4u) | static_cast<uint8_t>(offset_bits));
}

static uint8_t lzssraw_header1(int length_bits, int raw_bits) {
    return static_cast<uint8_t>((static_cast<uint8_t>(length_bits) << 4u) | static_cast<uint8_t>(raw_bits));
}

static void lzssraw_flush_raw(
    std::vector<uint8_t>& out,
    const std::vector<uint8_t>& input,
    size_t raw_start,
    size_t raw_length,
    int raw_bits) {

    const size_t max_raw = static_cast<size_t>(1) << raw_bits;
    size_t offset = 0;
    while (offset < raw_length) {
        const size_t chunk = std::min(max_raw, raw_length - offset);
        out.push_back(static_cast<uint8_t>(chunk - 1u));
        out.insert(out.end(),
            input.begin() + static_cast<std::ptrdiff_t>(raw_start + offset),
            input.begin() + static_cast<std::ptrdiff_t>(raw_start + offset + chunk));
        offset += chunk;
    }
}

static void lzssraw_insert_position(
    const std::vector<uint8_t>& input,
    std::vector<int>& head,
    std::vector<int>& previous,
    size_t position) {

    if (position + 2 >= input.size()) return;
    const size_t hash = lzssraw_hash3(input, position);
    previous[position] = head[hash];
    head[hash] = static_cast<int>(position);
}

static CompressResult lzssraw_compress(
    const std::vector<uint8_t>& input,
    int offset_bits,
    int length_bits,
    int raw_bits) {

    CompressResult result;
    if (!lzssraw_valid_params(offset_bits, length_bits, raw_bits)) {
        result.error = "invalid LZSS-Raw parameters";
        return result;
    }

    const size_t window_size = static_cast<size_t>(1) << offset_bits;
    const size_t max_match = (static_cast<size_t>(1) << length_bits) + 2u;
    const size_t max_raw = static_cast<size_t>(1) << raw_bits;
    constexpr size_t min_match = 3;
    constexpr int max_candidates = 64;

    std::vector<uint8_t> encoded;
    encoded.reserve(input.size());
    encoded.push_back(lzssraw_header0(false, offset_bits));
    encoded.push_back(lzssraw_header1(length_bits, raw_bits));

    std::vector<int> head(4096, -1);
    std::vector<int> previous(input.size(), -1);

    size_t raw_start = 0;
    size_t raw_length = 0;
    size_t position = 0;

    while (position < input.size()) {
        size_t best_length = 0;
        size_t best_offset = 0;

        if (position + min_match <= input.size()) {
            int candidate = head[lzssraw_hash3(input, position)];
            int candidate_count = 0;
            while (candidate >= 0 && candidate_count < max_candidates) {
                const size_t candidate_pos = static_cast<size_t>(candidate);
                const size_t offset = position - candidate_pos;
                if (offset == 0 || offset > window_size) break;

                size_t length = 0;
                const size_t length_limit = std::min(max_match, input.size() - position);
                while (length < length_limit && input[candidate_pos + length] == input[position + length]) {
                    ++length;
                }
                if (length > best_length) {
                    best_length = length;
                    best_offset = offset;
                    if (best_length == length_limit) break;
                }

                candidate = previous[candidate_pos];
                ++candidate_count;
            }
        }

        if (best_length >= min_match) {
            if (raw_length > 0) {
                lzssraw_flush_raw(encoded, input, raw_start, raw_length, raw_bits);
                raw_length = 0;
            }

            const int padding_bits = 15 - offset_bits - length_bits;
            const uint16_t offset_code = static_cast<uint16_t>(best_offset - 1u);
            const uint16_t length_code = static_cast<uint16_t>(best_length - min_match);
            const uint16_t token = static_cast<uint16_t>(
                0x8000u
                | (offset_code << (length_bits + padding_bits))
                | (length_code << padding_bits));
            append_u16be(encoded, token);

            for (size_t i = 0; i < best_length; ++i) {
                lzssraw_insert_position(input, head, previous, position + i);
            }
            position += best_length;
            raw_start = position;
        } else {
            if (raw_length == 0) raw_start = position;
            ++raw_length;
            lzssraw_insert_position(input, head, previous, position);
            ++position;
            if (raw_length == max_raw) {
                lzssraw_flush_raw(encoded, input, raw_start, raw_length, raw_bits);
                raw_length = 0;
                raw_start = position;
            }
        }
    }

    if (raw_length > 0) {
        lzssraw_flush_raw(encoded, input, raw_start, raw_length, raw_bits);
    }

    if (encoded.size() >= input.size() + 2u) {
        std::vector<uint8_t> raw;
        raw.reserve(input.size() + 2u);
        raw.push_back(lzssraw_header0(true, offset_bits));
        raw.push_back(lzssraw_header1(length_bits, raw_bits));
        raw.insert(raw.end(), input.begin(), input.end());
        result.data = std::move(raw);
    } else {
        result.data = std::move(encoded);
    }

    result.ok = true;
    return result;
}

static CompressResult lzssraw_decompress(
    const std::vector<uint8_t>& input,
    size_t expected_size,
    int expected_offset_bits,
    int expected_length_bits,
    int expected_raw_bits) {

    CompressResult result;
    if (input.size() < 2) {
        result.error = "truncated LZSS-Raw header";
        return result;
    }

    const bool raw_mode = (input[0] & 0x80u) != 0;
    const int version = (input[0] >> 4u) & 0x07u;
    const int offset_bits = input[0] & 0x0fu;
    const int length_bits = (input[1] >> 4u) & 0x0fu;
    const int raw_bits = input[1] & 0x0fu;
    if (version != 1 || offset_bits != expected_offset_bits
        || length_bits != expected_length_bits || raw_bits != expected_raw_bits
        || !lzssraw_valid_params(offset_bits, length_bits, raw_bits)) {
        result.error = "invalid LZSS-Raw header";
        return result;
    }

    if (raw_mode) {
        if (input.size() - 2u != expected_size) {
            result.error = "LZSS-Raw raw payload size mismatch";
            return result;
        }
        result.data.assign(input.begin() + 2, input.end());
        result.ok = true;
        return result;
    }

    const size_t window_size = static_cast<size_t>(1) << offset_bits;
    const int padding_bits = 15 - offset_bits - length_bits;
    std::vector<uint8_t> window(window_size, 0);
    size_t window_pos = 0;
    std::vector<uint8_t> out;
    out.reserve(expected_size);

    auto append_byte = [&](uint8_t byte) {
        out.push_back(byte);
        window[window_pos] = byte;
        window_pos = (window_pos + 1u) & (window_size - 1u);
    };

    size_t offset = 2;
    while (offset < input.size()) {
        const uint8_t first = input[offset];
        if ((first & 0x80u) == 0) {
            const size_t raw_length = static_cast<size_t>(first) + 1u;
            ++offset;
            if (input.size() - offset < raw_length || out.size() + raw_length > expected_size) {
                result.error = "invalid LZSS-Raw raw run";
                return result;
            }
            for (size_t i = 0; i < raw_length; ++i) {
                append_byte(input[offset + i]);
            }
            offset += raw_length;
            continue;
        }

        if (input.size() - offset < 2) {
            result.error = "truncated LZSS-Raw match token";
            return result;
        }
        const uint16_t token = read_u16be(input, offset);
        offset += 2;
        const uint16_t payload = token & 0x7fffu;
        if (padding_bits > 0 && (payload & ((1u << padding_bits) - 1u)) != 0) {
            result.error = "non-zero LZSS-Raw match padding";
            return result;
        }

        const size_t length_code = (payload >> padding_bits) & ((1u << length_bits) - 1u);
        const size_t match_offset = (payload >> (length_bits + padding_bits)) + 1u;
        const size_t match_length = length_code + 3u;
        if (match_offset == 0 || match_offset > window_size || match_offset > out.size()
            || out.size() + match_length > expected_size) {
            result.error = "invalid LZSS-Raw match";
            return result;
        }

        for (size_t i = 0; i < match_length; ++i) {
            const size_t source = (window_pos + window_size - match_offset) & (window_size - 1u);
            append_byte(window[source]);
        }
    }

    if (out.size() != expected_size) {
        result.error = "LZSS-Raw decompressed size mismatch";
        return result;
    }

    result.ok = true;
    result.data = std::move(out);
    return result;
}

class HsRawBitWriter {
public:
    void push_bits(uint8_t count, uint32_t bits) {
        if (count == 8 && bit_index_ == 0x80u) {
            out_.push_back(static_cast<uint8_t>(bits));
            return;
        }
        for (int bit = static_cast<int>(count) - 1; bit >= 0; --bit) {
            if ((bits & (1u << bit)) != 0) current_byte_ |= bit_index_;
            bit_index_ >>= 1u;
            if (bit_index_ == 0) {
                out_.push_back(current_byte_);
                current_byte_ = 0;
                bit_index_ = 0x80u;
            }
        }
    }

    std::vector<uint8_t> finish() {
        if (bit_index_ != 0x80u) out_.push_back(current_byte_);
        return std::move(out_);
    }

private:
    std::vector<uint8_t> out_;
    uint8_t current_byte_ = 0;
    uint8_t bit_index_ = 0x80u;
};

class HsRawBitReader {
public:
    explicit HsRawBitReader(const std::vector<uint8_t>& input) : input_(input) {}

    bool read_bits(uint8_t count, uint32_t& bits) {
        bits = 0;
        for (uint8_t i = 0; i < count; ++i) {
            if (bit_index_ == 0) {
                if (offset_ >= input_.size()) return false;
                current_byte_ = input_[offset_++];
                bit_index_ = 0x80u;
            }
            bits <<= 1u;
            if ((current_byte_ & bit_index_) != 0) bits |= 1u;
            bit_index_ >>= 1u;
        }
        return true;
    }

private:
    const std::vector<uint8_t>& input_;
    size_t offset_ = 0;
    uint8_t current_byte_ = 0;
    uint8_t bit_index_ = 0;
};

static bool heatshrinkraw_valid_params(int window_bits, int lookahead_bits) {
    return window_bits >= 4
        && window_bits <= 15
        && lookahead_bits >= 3
        && lookahead_bits < window_bits;
}

static size_t heatshrinkraw_min_match(int window_bits, int lookahead_bits) {
    return (1u + static_cast<size_t>(window_bits) + static_cast<size_t>(lookahead_bits)) / 8u + 1u;
}

static void heatshrinkraw_emit_literal(HsRawBitWriter& writer, uint8_t byte) {
    writer.push_bits(1, 1);
    writer.push_bits(8, byte);
}

static void heatshrinkraw_emit_raw_escape(
    HsRawBitWriter& writer,
    const std::vector<uint8_t>& input,
    size_t raw_start,
    size_t raw_length,
    int window_bits,
    int lookahead_bits) {

    const size_t max_raw = static_cast<size_t>(1) << window_bits;
    size_t offset = 0;
    while (offset < raw_length) {
        const size_t chunk = std::min(max_raw, raw_length - offset);
        writer.push_bits(1, 0);
        writer.push_bits(static_cast<uint8_t>(window_bits), static_cast<uint32_t>(chunk - 1u));
        writer.push_bits(static_cast<uint8_t>(lookahead_bits), 0);
        for (size_t i = 0; i < chunk; ++i) {
            writer.push_bits(8, input[raw_start + offset + i]);
        }
        offset += chunk;
    }
}

static void heatshrinkraw_flush_raw(
    HsRawBitWriter& writer,
    const std::vector<uint8_t>& input,
    size_t raw_start,
    size_t raw_length,
    int window_bits,
    int lookahead_bits) {

    const size_t raw_escape_overhead_bits = 1u + static_cast<size_t>(window_bits) + static_cast<size_t>(lookahead_bits);
    if (raw_length <= raw_escape_overhead_bits) {
        for (size_t i = 0; i < raw_length; ++i) {
            heatshrinkraw_emit_literal(writer, input[raw_start + i]);
        }
        return;
    }

    heatshrinkraw_emit_raw_escape(writer, input, raw_start, raw_length, window_bits, lookahead_bits);
}

static CompressResult heatshrinkraw_compress(
    const std::vector<uint8_t>& input,
    int window_bits,
    int lookahead_bits) {

    CompressResult result;
    if (!heatshrinkraw_valid_params(window_bits, lookahead_bits)) {
        result.error = "invalid heatshrinkraw parameters";
        return result;
    }

    const size_t window_size = static_cast<size_t>(1) << window_bits;
    const size_t max_match = static_cast<size_t>(1) << lookahead_bits;
    const size_t min_match = heatshrinkraw_min_match(window_bits, lookahead_bits);
    constexpr int max_candidates = 64;

    HsRawBitWriter writer;
    std::vector<int> head(4096, -1);
    std::vector<int> previous(input.size(), -1);

    size_t raw_start = 0;
    size_t raw_length = 0;
    size_t position = 0;

    while (position < input.size()) {
        size_t best_length = 0;
        size_t best_offset = 0;

        if (position + 2 < input.size()) {
            int candidate = head[lzssraw_hash3(input, position)];
            int candidate_count = 0;
            while (candidate >= 0 && candidate_count < max_candidates) {
                const size_t candidate_pos = static_cast<size_t>(candidate);
                const size_t offset = position - candidate_pos;
                if (offset == 0 || offset > window_size) break;

                size_t length = 0;
                const size_t length_limit = std::min(max_match, input.size() - position);
                while (length < length_limit && input[candidate_pos + length] == input[position + length]) {
                    ++length;
                }
                if (length > best_length) {
                    best_length = length;
                    best_offset = offset;
                    if (best_length == length_limit) break;
                }

                candidate = previous[candidate_pos];
                ++candidate_count;
            }
        }

        if (best_length >= min_match) {
            if (raw_length > 0) {
                heatshrinkraw_flush_raw(writer, input, raw_start, raw_length, window_bits, lookahead_bits);
                raw_length = 0;
            }

            writer.push_bits(1, 0);
            writer.push_bits(static_cast<uint8_t>(window_bits), static_cast<uint32_t>(best_offset - 1u));
            writer.push_bits(static_cast<uint8_t>(lookahead_bits), static_cast<uint32_t>(best_length - 1u));

            for (size_t i = 0; i < best_length; ++i) {
                lzssraw_insert_position(input, head, previous, position + i);
            }
            position += best_length;
            raw_start = position;
        } else {
            if (raw_length == 0) raw_start = position;
            ++raw_length;
            lzssraw_insert_position(input, head, previous, position);
            ++position;
            if (raw_length == window_size) {
                heatshrinkraw_flush_raw(writer, input, raw_start, raw_length, window_bits, lookahead_bits);
                raw_length = 0;
                raw_start = position;
            }
        }
    }

    if (raw_length > 0) {
        heatshrinkraw_flush_raw(writer, input, raw_start, raw_length, window_bits, lookahead_bits);
    }

    result.ok = true;
    result.data = writer.finish();
    return result;
}

static CompressResult heatshrinkraw_decompress(
    const std::vector<uint8_t>& input,
    size_t expected_size,
    int window_bits,
    int lookahead_bits) {

    CompressResult result;
    if (!heatshrinkraw_valid_params(window_bits, lookahead_bits)) {
        result.error = "invalid heatshrinkraw parameters";
        return result;
    }

    const size_t window_size = static_cast<size_t>(1) << window_bits;
    std::vector<uint8_t> window(window_size, 0);
    size_t window_pos = 0;
    std::vector<uint8_t> out;
    out.reserve(expected_size);
    HsRawBitReader reader(input);

    auto append_byte = [&](uint8_t byte) {
        out.push_back(byte);
        window[window_pos] = byte;
        window_pos = (window_pos + 1u) & (window_size - 1u);
    };

    while (out.size() < expected_size) {
        uint32_t tag = 0;
        if (!reader.read_bits(1, tag)) {
            result.error = "truncated heatshrinkraw tag";
            return result;
        }

        if (tag != 0) {
            uint32_t byte = 0;
            if (!reader.read_bits(8, byte)) {
                result.error = "truncated heatshrinkraw literal";
                return result;
            }
            append_byte(static_cast<uint8_t>(byte));
            continue;
        }

        uint32_t offset_code = 0;
        uint32_t length_code = 0;
        if (!reader.read_bits(static_cast<uint8_t>(window_bits), offset_code)
            || !reader.read_bits(static_cast<uint8_t>(lookahead_bits), length_code)) {
            result.error = "truncated heatshrinkraw backref";
            return result;
        }

        if (length_code == 0) {
            const size_t raw_length = static_cast<size_t>(offset_code) + 1u;
            if (out.size() + raw_length > expected_size) {
                result.error = "invalid heatshrinkraw raw run";
                return result;
            }
            for (size_t i = 0; i < raw_length; ++i) {
                uint32_t byte = 0;
                if (!reader.read_bits(8, byte)) {
                    result.error = "truncated heatshrinkraw raw run";
                    return result;
                }
                append_byte(static_cast<uint8_t>(byte));
            }
            continue;
        }

        const size_t match_offset = static_cast<size_t>(offset_code) + 1u;
        const size_t match_length = static_cast<size_t>(length_code) + 1u;
        if (match_offset > window_size || match_offset > out.size()
            || out.size() + match_length > expected_size) {
            result.error = "invalid heatshrinkraw backref";
            return result;
        }

        for (size_t i = 0; i < match_length; ++i) {
            const size_t source = (window_pos + window_size - match_offset) & (window_size - 1u);
            append_byte(window[source]);
        }
    }

    result.ok = true;
    result.data = std::move(out);
    return result;
}

static bool heatshrink_poll_encoder(heatshrink_encoder* encoder, std::vector<uint8_t>& out, std::string& error) {
    uint8_t buffer[4096];
    while (true) {
        size_t produced = 0;
        HSE_poll_res rc = heatshrink_encoder_poll(encoder, buffer, sizeof(buffer), &produced);
        if (produced > 0) out.insert(out.end(), buffer, buffer + produced);
        if (rc == HSER_POLL_EMPTY) return true;
        if (rc != HSER_POLL_MORE) {
            error = "heatshrink_encoder_poll failed";
            return false;
        }
    }
}

static CompressResult heatshrink_compress(const std::vector<uint8_t>& input, int window_bits, int lookahead_bits) {
    CompressResult result;
    heatshrink_encoder* encoder = heatshrink_encoder_alloc(static_cast<uint8_t>(window_bits), static_cast<uint8_t>(lookahead_bits));
    if (!encoder) {
        result.error = "heatshrink_encoder_alloc failed";
        return result;
    }

    std::vector<uint8_t> out;
    size_t offset = 0;
    while (offset < input.size()) {
        size_t sunk = 0;
        HSE_sink_res sink_rc = heatshrink_encoder_sink(
            encoder,
            const_cast<uint8_t*>(input.data() + offset),
            input.size() - offset,
            &sunk);
        if (sink_rc != HSER_SINK_OK) {
            result.error = "heatshrink_encoder_sink failed";
            heatshrink_encoder_free(encoder);
            return result;
        }
        offset += sunk;
        if (!heatshrink_poll_encoder(encoder, out, result.error)) {
            heatshrink_encoder_free(encoder);
            return result;
        }
        if (sunk == 0 && offset < input.size()) {
            result.error = "heatshrink encoder made no progress";
            heatshrink_encoder_free(encoder);
            return result;
        }
    }

    while (true) {
        HSE_finish_res finish_rc = heatshrink_encoder_finish(encoder);
        if (!heatshrink_poll_encoder(encoder, out, result.error)) {
            heatshrink_encoder_free(encoder);
            return result;
        }
        if (finish_rc == HSER_FINISH_DONE) break;
        if (finish_rc != HSER_FINISH_MORE) {
            result.error = "heatshrink_encoder_finish failed";
            heatshrink_encoder_free(encoder);
            return result;
        }
    }

    heatshrink_encoder_free(encoder);
    result.ok = true;
    result.data = std::move(out);
    return result;
}

static bool heatshrink_poll_decoder(heatshrink_decoder* decoder, std::vector<uint8_t>& out, std::string& error) {
    uint8_t buffer[4096];
    while (true) {
        size_t produced = 0;
        HSD_poll_res rc = heatshrink_decoder_poll(decoder, buffer, sizeof(buffer), &produced);
        if (produced > 0) out.insert(out.end(), buffer, buffer + produced);
        if (rc == HSDR_POLL_EMPTY) return true;
        if (rc != HSDR_POLL_MORE) {
            error = "heatshrink_decoder_poll failed";
            return false;
        }
    }
}

static CompressResult heatshrink_decompress(const std::vector<uint8_t>& input, int window_bits, int lookahead_bits) {
    CompressResult result;
    heatshrink_decoder* decoder = heatshrink_decoder_alloc(256, static_cast<uint8_t>(window_bits), static_cast<uint8_t>(lookahead_bits));
    if (!decoder) {
        result.error = "heatshrink_decoder_alloc failed";
        return result;
    }

    std::vector<uint8_t> out;
    size_t offset = 0;
    while (offset < input.size()) {
        size_t sunk = 0;
        HSD_sink_res sink_rc = heatshrink_decoder_sink(
            decoder,
            const_cast<uint8_t*>(input.data() + offset),
            input.size() - offset,
            &sunk);
        if (sink_rc != HSDR_SINK_OK && sink_rc != HSDR_SINK_FULL) {
            result.error = "heatshrink_decoder_sink failed";
            heatshrink_decoder_free(decoder);
            return result;
        }
        offset += sunk;
        if (!heatshrink_poll_decoder(decoder, out, result.error)) {
            heatshrink_decoder_free(decoder);
            return result;
        }
        if (sunk == 0 && sink_rc == HSDR_SINK_FULL) continue;
        if (sunk == 0 && offset < input.size()) {
            result.error = "heatshrink decoder made no progress";
            heatshrink_decoder_free(decoder);
            return result;
        }
    }

    while (true) {
        HSD_finish_res finish_rc = heatshrink_decoder_finish(decoder);
        if (!heatshrink_poll_decoder(decoder, out, result.error)) {
            heatshrink_decoder_free(decoder);
            return result;
        }
        if (finish_rc == HSDR_FINISH_DONE) break;
        if (finish_rc != HSDR_FINISH_MORE) {
            result.error = "heatshrink_decoder_finish failed";
            heatshrink_decoder_free(decoder);
            return result;
        }
    }

    heatshrink_decoder_free(decoder);
    result.ok = true;
    result.data = std::move(out);
    return result;
}

static G5Result g5_compress_variant(const std::string& name, const std::vector<uint8_t>& input, int width, int height, int planes) {
    if (name == "virtual-f64") return g5_compress_virtual_f64(input, width, height, planes);
    if (name == "perplane-f128") return g5_compress_perplane_f128(input, width, height, planes);
    if (name == "perplane-f256") return g5_compress_perplane_f256(input, width, height, planes);
    return {false, {}, "unknown g5 variant"};
}

static G5Result g5_decompress_variant(const std::string& name, const std::vector<uint8_t>& input, int width, int height, int planes) {
    if (name == "virtual-f64") return g5_decompress_virtual_f64(input, width, height, planes);
    if (name == "perplane-f128") return g5_decompress_perplane_f128(input, width, height, planes);
    if (name == "perplane-f256") return g5_decompress_perplane_f256(input, width, height, planes);
    return {false, {}, "unknown g5 variant"};
}

static fs::path output_path_for(const InputFile& input, const std::string& algorithm, const std::string& variant) {
    return input.path.parent_path() / (input.prefix + "." + algorithm + "." + variant + ".dat");
}

static void print_usage(const char* argv0) {
    std::cerr << "usage: " << argv0
              << " [--runs N] [--jsonl path] [--variant name] "
              << "(brotli|g5|heatshrink|heatshrinkraw|lz4|lz4hc|lzssraw|zlib|zstd) <source_folder>\n";
}

static bool parse_options(int argc, char** argv, Options& options) {
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--runs") {
            if (++i >= argc) return false;
            try {
                options.runs = std::stoi(argv[i]);
            } catch (...) {
                return false;
            }
            if (options.runs <= 0) return false;
        } else if (arg == "--jsonl") {
            if (++i >= argc) return false;
            options.jsonl_path = argv[i];
            options.jsonl_enabled = true;
        } else if (arg == "--variant") {
            if (++i >= argc) return false;
            options.variants.push_back(argv[i]);
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.size() != 2) return false;
    options.algorithm = positional[0];
    options.folder = positional[1];
    return options.algorithm == "zlib"
        || options.algorithm == "heatshrink"
        || options.algorithm == "heatshrinkraw"
        || options.algorithm == "g5"
        || options.algorithm == "brotli"
        || options.algorithm == "lz4"
        || options.algorithm == "lz4hc"
        || options.algorithm == "lzssraw"
        || options.algorithm == "zstd";
}

static void write_jsonl(
    std::ofstream* jsonl,
    const std::string& status,
    const std::string& algorithm,
    const std::string& variant,
    const InputFile& input,
    const fs::path& output_path,
    size_t input_bytes,
    size_t compressed_bytes,
    double ratio,
    double avg_ms,
    int runs,
    const std::string& error) {

    if (jsonl == nullptr) return;

    *jsonl << "{"
           << "\"status\":\"" << json_escape(status) << "\","
           << "\"algorithm\":\"" << json_escape(algorithm) << "\","
           << "\"variant\":\"" << json_escape(variant) << "\","
           << "\"input_file\":\"" << json_escape(input.path.string()) << "\",";
    if (!output_path.empty()) {
        *jsonl << "\"output_file\":\"" << json_escape(output_path.string()) << "\",";
    }
    *jsonl << "\"width\":" << input.width << ","
           << "\"height\":" << input.height << ","
           << "\"input_bytes\":" << input_bytes << ",";
    if (status == "ok") {
        *jsonl << "\"compressed_bytes\":" << compressed_bytes << ","
               << "\"ratio\":" << std::fixed << std::setprecision(8) << ratio << ",";
    }
    *jsonl << "\"avg_ms\":" << std::fixed << std::setprecision(6) << avg_ms << ","
           << "\"runs\":" << runs;
    if (!error.empty()) {
        *jsonl << ",\"error\":\"" << json_escape(error) << "\"";
    }
    *jsonl << "}\n";
}

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage(argv[0]);
        return 2;
    }
    if (!fs::is_directory(options.folder)) {
        std::cerr << "source folder does not exist: " << options.folder << "\n";
        return 2;
    }

    std::ofstream jsonl_stream;
    std::ofstream* jsonl = nullptr;
    if (options.jsonl_enabled) {
        jsonl_stream.open(options.jsonl_path, std::ios::app);
        if (!jsonl_stream) {
            std::cerr << "failed to open jsonl output: " << options.jsonl_path << "\n";
            return 2;
        }
        jsonl = &jsonl_stream;
    }

    std::vector<Variant> variants;
    std::string suffix;
    if (options.algorithm == "zlib") {
        suffix = ".bs-od";
        variants = {
            {"current", 6, 15, 0, 0},
            {"l1-ws9", 1, 9, 0, 0},
            {"l6-ws9", 6, 9, 0, 0},
            {"l9-ws9", 9, 9, 0, 0},
            {"l1-ws10", 1, 10, 0, 0},
            {"l6-ws10", 6, 10, 0, 0},
            {"l9-ws10", 9, 10, 0, 0},
            {"l9-ws12", 9, 12, 0, 0},
            {"l9-ws15", 9, 15, 0, 0},
        };
    } else if (options.algorithm == "heatshrink") {
        suffix = ".bs-od";
        variants = {
            {"w9-l4", 0, 0, 9, 4},
            {"w11-l5", 0, 0, 11, 5},
            {"w13-l6", 0, 0, 13, 6},
        };
    } else if (options.algorithm == "heatshrinkraw") {
        suffix = ".bs-od";
        variants = {
            {"w9-l4", 0, 0, 9, 4},
            {"w11-l5", 0, 0, 11, 5},
            {"w13-l6", 0, 0, 13, 6},
        };
    } else if (options.algorithm == "brotli") {
        suffix = ".bs-od";
        variants = {
            {"q9-w10", 0, 0, 0, 0, 9, 10},
            {"q9-w12", 0, 0, 0, 0, 9, 12},
            {"q9-w15", 0, 0, 0, 0, 9, 15},
        };
    } else if (options.algorithm == "zstd") {
        suffix = ".bs-od";
        variants = {
            {"l19-w10", 0, 0, 0, 0, 0, 0, 19, 10},
            {"l19-w12", 0, 0, 0, 0, 0, 0, 19, 12},
            {"l19-w15", 0, 0, 0, 0, 0, 0, 19, 15},
        };
    } else if (options.algorithm == "lz4") {
        suffix = ".bs-od";
        variants = {
            {"b512", 0, 0, 0, 0, 0, 0, 0, 0, 512, 0},
            {"b1024", 0, 0, 0, 0, 0, 0, 0, 0, 1024, 0},
            {"b16384", 0, 0, 0, 0, 0, 0, 0, 0, 16384, 0},
            {"b32768", 0, 0, 0, 0, 0, 0, 0, 0, 32768, 0},
        };
    } else if (options.algorithm == "lz4hc") {
        suffix = ".bs-od";
        variants = {
            {"b512-l9", 0, 0, 0, 0, 0, 0, 0, 0, 512, 9},
            {"b1024-l9", 0, 0, 0, 0, 0, 0, 0, 0, 1024, 9},
            {"b16384-l9", 0, 0, 0, 0, 0, 0, 0, 0, 16384, 9},
            {"b32768-l9", 0, 0, 0, 0, 0, 0, 0, 0, 32768, 9},
        };
    } else if (options.algorithm == "lzssraw") {
        suffix = ".bs-od";
        variants = {
            {"w6-l4-r7", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 4, 7},
            {"w7-l4-r7", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 4, 7},
            {"w8-l4-r7", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 4, 7},
            {"w9-l4-r7", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 4, 7},
            {"w10-l5-r7", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10, 5, 7},
            {"w11-l4-r7", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 11, 4, 7},
        };
    } else {
        suffix = ".bs-1bppstreams";
        variants = {
            {"virtual-f64", 0, 0, 0, 0},
            {"perplane-f128", 0, 0, 0, 0},
            {"perplane-f256", 0, 0, 0, 0},
        };
    }

    if (!options.variants.empty()) {
        std::vector<Variant> filtered;
        for (const std::string& requested : options.variants) {
            const auto found = std::find_if(variants.begin(), variants.end(), [&](const Variant& variant) {
                return variant.name == requested;
            });
            if (found == variants.end()) {
                std::cerr << "unknown " << options.algorithm << " variant: " << requested << "\n";
                return 2;
            }
            filtered.push_back(*found);
        }
        variants = std::move(filtered);
    }

    const std::vector<InputFile> inputs = find_inputs(options.folder, suffix);
    if (inputs.empty()) {
        std::cerr << "no inputs matching *" << suffix << " found in " << options.folder << "\n";
        return 1;
    }

    double ratio_sum = 0.0;
    size_t success_count = 0;

    std::cout << "algorithm variant file resolution input_bytes compressed_bytes ratio avg_ms\n";

    for (const InputFile& input : inputs) {
        std::vector<uint8_t> source;
        try {
            source = read_file(input.path);
        } catch (const std::exception& ex) {
            std::cerr << input.path << ": " << ex.what() << "\n";
            continue;
        }

        int plane_count = 0;
        if (options.algorithm == "g5") {
            const size_t pitch = (static_cast<size_t>(input.width) + 7u) / 8u;
            const size_t plane_size = pitch * static_cast<size_t>(input.height);
            if (plane_size == 0 || source.size() % plane_size != 0) {
                std::cerr << input.path << ": G5 input size is not divisible by plane size\n";
                continue;
            }
            plane_count = static_cast<int>(source.size() / plane_size);
            if (plane_count != 1 && plane_count != 2 && plane_count != 4) {
                std::cerr << input.path << ": unsupported G5 plane count " << plane_count << "\n";
                continue;
            }
        }

        for (const Variant& variant : variants) {
            CompressResult compressed;
            G5Result g5_compressed;
            std::chrono::duration<double, std::milli> elapsed{0};
            bool ok = true;
            std::string error;
            int completed_runs = 0;

            for (int run = 0; run < options.runs; ++run) {
                const auto start = std::chrono::steady_clock::now();
                if (options.algorithm == "zlib") {
                    compressed = zlib_compress(source, variant.level, variant.window_bits);
                    ok = compressed.ok;
                    error = compressed.error;
                } else if (options.algorithm == "heatshrink") {
                    compressed = heatshrink_compress(source, variant.heat_window, variant.heat_lookahead);
                    ok = compressed.ok;
                    error = compressed.error;
                } else if (options.algorithm == "heatshrinkraw") {
                    compressed = heatshrinkraw_compress(source, variant.heat_window, variant.heat_lookahead);
                    ok = compressed.ok;
                    error = compressed.error;
                } else if (options.algorithm == "brotli") {
                    compressed = brotli_compress(source, variant.brotli_quality, variant.brotli_lgwin);
                    ok = compressed.ok;
                    error = compressed.error;
                } else if (options.algorithm == "zstd") {
                    compressed = zstd_compress(source, variant.zstd_level, variant.zstd_window_log);
                    ok = compressed.ok;
                    error = compressed.error;
                } else if (options.algorithm == "lz4" || options.algorithm == "lz4hc") {
                    compressed = lz4_compress_blocks(source, variant.lz4_block_size, variant.lz4_hc_level);
                    ok = compressed.ok;
                    error = compressed.error;
                } else if (options.algorithm == "lzssraw") {
                    compressed = lzssraw_compress(source,
                        variant.lzss_offset_bits,
                        variant.lzss_length_bits,
                        variant.lzss_raw_bits);
                    ok = compressed.ok;
                    error = compressed.error;
                } else {
                    g5_compressed = g5_compress_variant(variant.name, source, input.width, input.height, plane_count);
                    ok = g5_compressed.ok;
                    error = g5_compressed.error;
                }
                const auto end = std::chrono::steady_clock::now();
                elapsed += end - start;
                ++completed_runs;
                if (!ok) break;
            }

            const double avg_ms = completed_runs > 0 ? elapsed.count() / static_cast<double>(completed_runs) : 0.0;
            const fs::path output_path = output_path_for(input, options.algorithm, variant.name);

            if (!ok) {
                std::cerr << input.path.filename().string() << " " << options.algorithm << "." << variant.name
                          << ": compression failed: " << error << "\n";
                write_jsonl(jsonl, "failed", options.algorithm, variant.name, input, output_path,
                    source.size(), 0, 0.0, avg_ms, options.runs, error);
                continue;
            }

            const std::vector<uint8_t>& output = (options.algorithm == "g5") ? g5_compressed.data : compressed.data;

            bool verified = false;
            if (options.algorithm == "zlib") {
                CompressResult decoded = zlib_decompress(output, source.size(), variant.window_bits);
                verified = decoded.ok && decoded.data == source;
                error = decoded.error;
            } else if (options.algorithm == "heatshrink") {
                CompressResult decoded = heatshrink_decompress(output, variant.heat_window, variant.heat_lookahead);
                verified = decoded.ok && decoded.data == source;
                error = decoded.error;
            } else if (options.algorithm == "heatshrinkraw") {
                CompressResult decoded = heatshrinkraw_decompress(output, source.size(), variant.heat_window, variant.heat_lookahead);
                verified = decoded.ok && decoded.data == source;
                error = decoded.error;
            } else if (options.algorithm == "brotli") {
                CompressResult decoded = brotli_decompress(output, source.size());
                verified = decoded.ok && decoded.data == source;
                error = decoded.error;
            } else if (options.algorithm == "zstd") {
                CompressResult decoded = zstd_decompress(output, source.size(), variant.zstd_window_log);
                verified = decoded.ok && decoded.data == source;
                error = decoded.error;
            } else if (options.algorithm == "lz4" || options.algorithm == "lz4hc") {
                CompressResult decoded = lz4_decompress_blocks(output, source.size(), variant.lz4_block_size);
                verified = decoded.ok && decoded.data == source;
                error = decoded.error;
            } else if (options.algorithm == "lzssraw") {
                CompressResult decoded = lzssraw_decompress(output,
                    source.size(),
                    variant.lzss_offset_bits,
                    variant.lzss_length_bits,
                    variant.lzss_raw_bits);
                verified = decoded.ok && decoded.data == source;
                error = decoded.error;
            } else {
                G5Result decoded = g5_decompress_variant(variant.name, output, input.width, input.height, plane_count);
                verified = decoded.ok && decoded.data == source;
                error = decoded.error;
            }

            if (!verified) {
                std::cerr << input.path.filename().string() << " " << options.algorithm << "." << variant.name
                          << ": verification failed: " << error << "\n";
                write_jsonl(jsonl, "failed", options.algorithm, variant.name, input, output_path,
                    source.size(), output.size(), 0.0, avg_ms, options.runs, "verification failed: " + error);
                continue;
            }

            try {
                write_file(output_path, output);
            } catch (const std::exception& ex) {
                std::cerr << input.path.filename().string() << ": " << ex.what() << "\n";
                write_jsonl(jsonl, "failed", options.algorithm, variant.name, input, output_path,
                    source.size(), output.size(), 0.0, avg_ms, options.runs, ex.what());
                continue;
            }

            const double ratio = source.empty() ? 0.0 : (static_cast<double>(output.size()) / static_cast<double>(source.size())) * 100.0;
            ratio_sum += ratio;
            ++success_count;
            write_jsonl(jsonl, "ok", options.algorithm, variant.name, input, output_path,
                source.size(), output.size(), ratio / 100.0, avg_ms, options.runs, "");

            std::cout << options.algorithm << ' '
                      << variant.name << ' '
                      << input.path.filename().string() << ' '
                      << input.width << 'x' << input.height << ' '
                      << source.size() << ' '
                      << output.size() << ' '
                      << std::fixed << std::setprecision(2) << ratio << "% "
                      << std::setprecision(3) << avg_ms << "\n";
        }
    }

    if (success_count > 0) {
        std::cout << "average_ratio=" << std::fixed << std::setprecision(2)
                  << (ratio_sum / static_cast<double>(success_count)) << "%\n";
    }

    return 0;
}
