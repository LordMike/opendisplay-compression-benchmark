CXX ?= g++
CC ?= gcc

BUILD_DIR := build
DEPS_DIR := _deps
HEATSHRINK_DIR := $(DEPS_DIR)/heatshrink
G5_DIR := $(DEPS_DIR)/g5_imageconvert
BROTLI_DIR := $(DEPS_DIR)/brotli
ZSTD_DIR := $(DEPS_DIR)/zstd
LZ4_DIR := $(DEPS_DIR)/lz4

CPPFLAGS += -I$(HEATSHRINK_DIR) -I$(G5_DIR) -I$(BROTLI_DIR)/c/include -I$(ZSTD_DIR)/lib -I$(LZ4_DIR)/lib
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic
CFLAGS ?= -O2 -Wall -Wextra
LIBDIR ?= /usr/lib/x86_64-linux-gnu
LDFLAGS += -L$(LIBDIR)
LDLIBS += -lz -l:libbrotlienc.so.1 -l:libbrotlidec.so.1 -l:libbrotlicommon.so.1 -l:libzstd.so.1
HEATSHRINK_DEFS := -DHEATSHRINK_DYNAMIC_ALLOC=1 -DHEATSHRINK_USE_INDEX=1

BENCH_OBJS := \
	$(BUILD_DIR)/main.o \
	$(BUILD_DIR)/g5_f64.o \
	$(BUILD_DIR)/g5_f128.o \
	$(BUILD_DIR)/g5_f256.o \
	$(BUILD_DIR)/heatshrink_encoder.o \
	$(BUILD_DIR)/heatshrink_decoder.o \
	$(BUILD_DIR)/lz4.o \
	$(BUILD_DIR)/lz4hc.o

.PHONY: all deps fixtures check clean

all: deps $(BUILD_DIR)/compressor_benchmark $(BUILD_DIR)/fixture_generator

deps: $(HEATSHRINK_DIR) $(G5_DIR) $(BROTLI_DIR) $(ZSTD_DIR) $(LZ4_DIR)

$(HEATSHRINK_DIR):
	git clone --depth 1 https://github.com/atomicobject/heatshrink.git $(HEATSHRINK_DIR)

$(G5_DIR):
	git clone --depth 1 https://github.com/bitbank2/g5_imageconvert.git $(G5_DIR)

$(BROTLI_DIR):
	git clone --depth 1 https://github.com/google/brotli.git $(BROTLI_DIR)

$(ZSTD_DIR):
	git clone --depth 1 https://github.com/facebook/zstd.git $(ZSTD_DIR)

$(LZ4_DIR):
	git clone --depth 1 https://github.com/lz4/lz4.git $(LZ4_DIR)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/compressor_benchmark: $(BENCH_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/fixture_generator: $(BUILD_DIR)/fixture_generator.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD_DIR)/main.o: src/main.cpp src/g5_codec.h | $(BUILD_DIR) deps
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/g5_f64.o: src/g5_f64.cpp src/g5_wrapper_impl.inl src/g5_codec.h | $(BUILD_DIR) deps
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/g5_f128.o: src/g5_f128.cpp src/g5_wrapper_impl.inl src/g5_codec.h | $(BUILD_DIR) deps
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/g5_f256.o: src/g5_f256.cpp src/g5_wrapper_impl.inl src/g5_codec.h | $(BUILD_DIR) deps
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/heatshrink_encoder.o: $(HEATSHRINK_DIR)/heatshrink_encoder.c | $(BUILD_DIR) deps
	$(CC) $(CPPFLAGS) $(CFLAGS) $(HEATSHRINK_DEFS) -c $< -o $@

$(BUILD_DIR)/heatshrink_decoder.o: $(HEATSHRINK_DIR)/heatshrink_decoder.c | $(BUILD_DIR) deps
	$(CC) $(CPPFLAGS) $(CFLAGS) $(HEATSHRINK_DEFS) -c $< -o $@

$(BUILD_DIR)/lz4.o: $(LZ4_DIR)/lib/lz4.c | $(BUILD_DIR) deps
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/lz4hc.o: $(LZ4_DIR)/lib/lz4hc.c | $(BUILD_DIR) deps
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fixture_generator.o: src/fixture_generator.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

fixtures: $(BUILD_DIR)/fixture_generator
	$(BUILD_DIR)/fixture_generator fixtures

check: all fixtures
	$(BUILD_DIR)/compressor_benchmark zlib fixtures
	$(BUILD_DIR)/compressor_benchmark heatshrink fixtures
	$(BUILD_DIR)/compressor_benchmark g5 fixtures
	$(BUILD_DIR)/compressor_benchmark brotli fixtures
	$(BUILD_DIR)/compressor_benchmark zstd fixtures
	$(BUILD_DIR)/compressor_benchmark lz4 fixtures
	$(BUILD_DIR)/compressor_benchmark lz4hc fixtures
	$(BUILD_DIR)/compressor_benchmark lzssraw fixtures

clean:
	rm -rf $(BUILD_DIR)
