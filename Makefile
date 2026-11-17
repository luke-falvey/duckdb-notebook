PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=notebook
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# extension-ci-tools v2.0-cyanoptera builds wasm_threads without -fwasm-exceptions: its
# WASM_CXX_THREADS_FLAGS names WASM_COMPILE_TIME_EH_FLAGS, which is not defined. DuckDB-WASM's threads
# bundle uses Wasm exceptions, so such an extension fails to load (dlopen, with an empty error).
# Fixed on extension-ci-tools main; remove this when the release branch has the fix.
WASM_CXX_THREADS_FLAGS=$(WASM_CXX_EH_FLAGS) -DWITH_WASM_THREADS=1 -DWITH_WASM_SIMD=1 -DWITH_WASM_BULK_MEMORY=1 -pthread

# `make core_test`: fast tests that need no DuckDB build.
include core.Makefile
