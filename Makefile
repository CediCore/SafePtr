# ============================
#  Minimal GNU Makefile for SafePtr
# ============================

CXX            := g++
CXXFLAGS       := -std=gnu++20 -O3 -march=native -mtune=native -pthread
WARNFLAGS      := -Wall -Wextra -Wpedantic

INCLUDES       := -Iinclude

BIN_DIR        := build/bin
OBJ_DIR        := build/obj

# Benchmark source
BENCH_SRC      := benchmark/bench_safeptr.cpp
BENCH_OBJ      := $(OBJ_DIR)/benchmark/bench_safeptr.o
BENCH_BIN      := $(BIN_DIR)/bench_safeptr.exe

all: dirs $(BENCH_BIN)

dirs:
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(OBJ_DIR)/benchmark

$(OBJ_DIR)/benchmark/%.o: benchmark/%.cpp
	$(CXX) $(CXXFLAGS) $(WARNFLAGS) $(INCLUDES) -c $< -o $@

$(BENCH_BIN): $(BENCH_OBJ)
	$(CXX) $^ -o $@

bench: $(BENCH_BIN)
	./$(BENCH_BIN)

clean:
	rm -rf build

.PHONY: all dirs bench clean
