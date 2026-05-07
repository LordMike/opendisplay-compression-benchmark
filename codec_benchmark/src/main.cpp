#include "g5_codec.h"

extern "C" {
#include "heatshrink_decoder.h"
#include "heatshrink_encoder.h"
}
#include "zlib.h"

#include <algorithm>
#include <chrono>
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
    std::cerr << "usage: " << argv0 << " [--runs N] [--jsonl path] (g5|zlib|heatshrink) <source_folder>\n";
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
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.size() != 2) return false;
    options.algorithm = positional[0];
    options.folder = positional[1];
    return options.algorithm == "zlib" || options.algorithm == "heatshrink" || options.algorithm == "g5";
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
            {"l9-ws9", 9, 9, 0, 0},
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
    } else {
        suffix = ".bs-1bppstreams";
        variants = {
            {"virtual-f64", 0, 0, 0, 0},
            {"perplane-f128", 0, 0, 0, 0},
            {"perplane-f256", 0, 0, 0, 0},
        };
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
