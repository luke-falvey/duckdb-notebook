"""Checks notebooks and exports them to HTML; exits 1 if any cell has an error or fails to run.

    python check_notebooks.py sketchbook.duckdb_extension out/ notebooks/*.sql
"""
import os
import sys

import duckdb

extension, out, notebooks = sys.argv[1], sys.argv[2], sys.argv[3:]
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("FORCE INSTALL '%s'" % extension.replace("'", "''"))
con.execute("LOAD sketchbook")
os.makedirs(out, exist_ok=True)

failed = False
for path in notebooks:
    problems = con.execute(
        "SELECT idx, name, errors FROM notebook_cells(?) WHERE len(errors) > 0", [path]).fetchall()
    problems += con.execute(
        "SELECT idx, name, error FROM notebook_run(?) WHERE status = 'error'", [path]).fetchall()
    for idx, name, error in problems:
        print("%s: cell %s (%s): %s" % (path, idx, name or "unnamed", error))
    if problems:
        failed = True
        continue
    page = os.path.join(out, os.path.splitext(os.path.basename(path))[0] + ".html")
    con.execute("FROM notebook_export(?, file := ?, hide := 'code')", [path, page])
    print("%s: ok, exported to %s" % (path, page))
sys.exit(1 if failed else 0)
