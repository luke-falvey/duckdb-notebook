# Tests for src/core, which has no DuckDB dependency: no submodules or DuckDB build
# needed, and it finishes in seconds. The main Makefile includes this file.
CXX ?= c++

.PHONY: core_test
core_test:
	@mkdir -p build/core_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Isrc/include test/core/core_test.cpp src/core/*.cpp -o build/core_test/core_test
	@./build/core_test/core_test
