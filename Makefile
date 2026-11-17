PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=notebook
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# extension-ci-tools v2.0-cyanoptera builds wasm_threads extensions that DuckDB-WASM's threads bundle
# cannot load (dlopen fails with an empty error; DuckDB's own icu extension built the same way fails
# too). Two flags are missing, both set on extension-ci-tools main: -fwasm-exceptions (its
# WASM_CXX_THREADS_FLAGS names WASM_COMPILE_TIME_EH_FLAGS, which is not defined) and
# -DUSE_WASM_THREADS=ON (DuckDB's switch that links a side module with shared memory).
# Remove both lines when the release branch has the fix. See docs/DESIGN.md section 21.
WASM_CXX_THREADS_FLAGS=$(WASM_CXX_EH_FLAGS) -DWITH_WASM_THREADS=1 -DWITH_WASM_SIMD=1 -DWITH_WASM_BULK_MEMORY=1 -pthread
wasm_threads: BUILD_FLAGS += -DUSE_WASM_THREADS=ON

# `make core_test`: fast tests that need no DuckDB build.
include core.Makefile
