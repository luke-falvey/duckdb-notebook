#!/bin/sh
# Fails if a notebook has format errors or a cell that fails to run.
#   sh check.sh report.sql [duckdb binary]
set -eu
duckdb=${2:-duckdb}
nb=$(printf '%s' "$1" | sed "s/'/''/g")
problems=$("$duckdb" -unsigned -init /dev/null -noheader -csv -c "LOAD notebook" -c "
  SELECT (SELECT count(*) FROM notebook_cells('$nb') WHERE len(errors) > 0)
       + (SELECT count(*) FROM notebook_run('$nb') WHERE status = 'error')")
if [ "$problems" -gt 0 ]; then
  echo "$1: $problems problem(s); see notebook_cells and notebook_run" >&2
  exit 1
fi
echo "$1: ok"
