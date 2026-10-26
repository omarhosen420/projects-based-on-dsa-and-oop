# MMF Engine — plain Makefile (no external dependencies).
#
#   make            build demo, tests and benchmark
#   make run        run the concurrent demo
#   make test       build and run the test suite
#   make bench      build and run the benchmark
#   make asan       test build with AddressSanitizer + UBSan
#   make tsan       test build with ThreadSanitizer
#   make clean

CXX      ?= g++
CXXSTD   ?= -std=c++20
OPT      ?= -O2
WARN     ?= -Wall -Wextra -Wpedantic -Wshadow
CXXFLAGS ?= $(CXXSTD) $(OPT) -g $(WARN)
CPPFLAGS ?= -Iinclude -Itests
LDLIBS   ?= -pthread

BUILD    := build
SAN      := $(BUILD)/sanitizers

CORE_SRC := src/config.cpp src/lobby.cpp src/matchmaker.cpp src/observers.cpp src/strategy.cpp
TEST_SRC := $(wildcard tests/*.cpp)
DEMO_SRC := src/main.cpp
BENCH_SRC := bench/bench_mmf.cpp
HEADERS  := $(wildcard include/mmf/*.hpp) $(wildcard tests/*.hpp)

DEMO     := $(BUILD)/mmf_demo
TESTS    := $(BUILD)/mmf_tests
BENCH    := $(BUILD)/mmf_bench

.PHONY: all run test bench asan tsan clean help

all: $(DEMO) $(TESTS) $(BENCH)

$(BUILD):
	@mkdir -p $(BUILD)

$(DEMO): $(CORE_SRC) $(DEMO_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(CORE_SRC) $(DEMO_SRC) -o $@ $(LDLIBS)

$(TESTS): $(CORE_SRC) $(TEST_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(CORE_SRC) $(TEST_SRC) -o $@ $(LDLIBS)

$(BENCH): $(CORE_SRC) $(BENCH_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(CORE_SRC) $(BENCH_SRC) -o $@ $(LDLIBS)

run: $(DEMO)
	./$(DEMO)

test: $(TESTS)
	./$(TESTS)

bench: $(BENCH)
	./$(BENCH)

asan: | $(SAN)
	$(CXX) $(CXXSTD) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(WARN) \
		$(CPPFLAGS) $(CORE_SRC) $(TEST_SRC) -o $(SAN)/mmf_tests_asan $(LDLIBS)
	ASAN_OPTIONS=detect_leaks=1 ./$(SAN)/mmf_tests_asan

tsan: | $(SAN)
	$(CXX) $(CXXSTD) -O2 -g -fsanitize=thread -fno-omit-frame-pointer $(WARN) \
		$(CPPFLAGS) $(CORE_SRC) $(TEST_SRC) -o $(SAN)/mmf_tests_tsan $(LDLIBS)
	TSAN_OPTIONS=halt_on_error=0 ./$(SAN)/mmf_tests_tsan

$(SAN):
	@mkdir -p $(SAN)

clean:
	rm -rf $(BUILD)

help:
	@sed -n '2,12p' Makefile
