CXX ?= g++
CC ?= gcc

BUILD_DIR := build
DEPS_DIR := _deps
HEATSHRINK_DIR := $(DEPS_DIR)/heatshrink
G5_DIR := $(DEPS_DIR)/g5_imageconvert

CPPFLAGS += -I$(HEATSHRINK_DIR) -I$(G5_DIR)
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic
CFLAGS ?= -O2 -Wall -Wextra
HEATSHRINK_DEFS := -DHEATSHRINK_DYNAMIC_ALLOC=1 -DHEATSHRINK_USE_INDEX=1

BENCH_OBJS := \
	$(BUILD_DIR)/main.o \
	$(BUILD_DIR)/g5_f64.o \
	$(BUILD_DIR)/g5_f128.o \
	$(BUILD_DIR)/g5_f256.o \
	$(BUILD_DIR)/heatshrink_encoder.o \
	$(BUILD_DIR)/heatshrink_decoder.o

.PHONY: all deps fixtures check clean

all: deps $(BUILD_DIR)/compressor_benchmark $(BUILD_DIR)/fixture_generator

deps: $(HEATSHRINK_DIR) $(G5_DIR)

$(HEATSHRINK_DIR):
	git clone --depth 1 https://github.com/atomicobject/heatshrink.git $(HEATSHRINK_DIR)

$(G5_DIR):
	git clone --depth 1 https://github.com/bitbank2/g5_imageconvert.git $(G5_DIR)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/compressor_benchmark: $(BENCH_OBJS)
	$(CXX) $(CXXFLAGS) $^ -lz -o $@

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

$(BUILD_DIR)/fixture_generator.o: src/fixture_generator.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

fixtures: $(BUILD_DIR)/fixture_generator
	$(BUILD_DIR)/fixture_generator fixtures

check: all fixtures
	$(BUILD_DIR)/compressor_benchmark zlib fixtures
	$(BUILD_DIR)/compressor_benchmark heatshrink fixtures
	$(BUILD_DIR)/compressor_benchmark g5 fixtures

clean:
	rm -rf $(BUILD_DIR)

