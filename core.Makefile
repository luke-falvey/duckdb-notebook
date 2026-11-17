# Tests for src/core, which has no DuckDB dependency: no submodules or DuckDB build
# needed, and it finishes in seconds. The main Makefile includes this file.
CXX ?= c++

.PHONY: core_test
core_test:
	@mkdir -p build/core_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Isrc/include test/core/core_test.cpp src/core/*.cpp -o build/core_test/core_test
	@./build/core_test/core_test

# The frontend is plain JavaScript compiled into the extension; check that it parses.
.PHONY: frontend_check
frontend_check:
	@for f in frontend/*.js; do node --check $$f || exit 1; done
	@echo "frontend parses"

# The runtime html-wasm exports inline: DuckDB-WASM plus a small adapter. Needs Node and npm.
.PHONY: wasm_runtime
wasm_runtime:
	sh wasm/build.sh

# End-to-end tests in Chromium: the server and UI, and every export format. Needs the release
# build and Python with Playwright (`pip install playwright && playwright install chromium`).
# Set NOTEBOOK_WASM_RUNTIME=wasm/dist after `make wasm_runtime` to include html-wasm.
.PHONY: browser_test
browser_test:
	python3 test/browser/test_browser.py
