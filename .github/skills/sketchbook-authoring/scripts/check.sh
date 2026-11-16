#!/bin/sh
# Checks a sketchbook the way a reader would meet it: parse and graph problems first, then a headless
# run of every cell, charts included. Exits 1 if any cell has an error or fails to run.
#
#   .github/skills/sketchbook-authoring/scripts/check.sh notebooks/sales.sql [path/to/duckdb]
set -eu
if [ $# -lt 1 ]; then
	echo "usage: $0 <notebook.sql> [duckdb binary, default ./build/release/duckdb]" >&2
	exit 2
fi
notebook=$1
duckdb=${2:-./build/release/duckdb}
quoted=$(printf '%s' "$notebook" | sed "s/'/''/g")

# -init /dev/null: a personal ~/.duckdbrc can load extensions this build lacks and stop the shell.
run() { "$duckdb" -init /dev/null -no-agent -csv -c "$1"; }

echo "== problems found without running (errors stop the notebook, warnings do not)"
run "SELECT idx, kind, coalesce(name, '') AS name, errors, warnings
     FROM notebook_cells('$quoted') WHERE len(errors) + len(warnings) > 0 ORDER BY idx"

echo "== cells that fail when run"
run "SELECT idx, kind, coalesce(name, '') AS name, error
     FROM notebook_run('$quoted') WHERE status = 'error' ORDER BY idx"

failures=$("$duckdb" -init /dev/null -no-agent -csv -noheader -c "
  SELECT (SELECT count(*) FROM notebook_cells('$quoted') WHERE len(errors) > 0)
       + (SELECT count(*) FROM notebook_run('$quoted') WHERE status = 'error')")
if [ "$failures" -gt 0 ]; then
	echo "== $failures problem(s): fix them before sharing"
	exit 1
fi
echo "== ok: every cell parses and runs"
