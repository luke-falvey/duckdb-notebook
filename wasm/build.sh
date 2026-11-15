#!/bin/sh
# Builds the DuckDB-WASM runtime for html-wasm exports into wasm/dist (or the directory given as
# the first argument). Needs Node and npm, once; the extension itself does not.
#
#   sh wasm/build.sh
#   FROM notebook_export('report.sql', format := 'html-wasm', wasm_runtime := 'wasm/dist');
set -eu
cd "$(dirname "$0")"
out="${1:-dist}"
npm install --no-audit --no-fund --silent
mkdir -p "$out"
npx esbuild runtime.js --bundle --format=iife --minify --outfile="$out/runtime.js" --log-level=warning
cp node_modules/@duckdb/duckdb-wasm/dist/duckdb-eh.wasm "$out/"
cp node_modules/@duckdb/duckdb-wasm/dist/duckdb-browser-eh.worker.js "$out/"
echo "DuckDB-WASM runtime written to $(cd "$out" && pwd)"
