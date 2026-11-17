"""Runs the queries the documentation shows and records what DuckDB printed.

Each site/outputs/<name>.sql is run with the extension's shell in a fresh in-memory database, in a
scratch folder holding copies of examples/ and site/notebooks/ (so `hello.sql` and
`examples/revenue.sql` resolve as they do for a reader), and its output is written to
site/outputs/<name>.txt. Pages include both files, so the query and its output cannot disagree.

    python3 site/tools/outputs.py [build/release/duckdb] [name ...]
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

SITE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(SITE)
OUTPUTS = os.path.join(SITE, "outputs")

# Outputs shown as plain text rather than a table.
RAW = {"sql_hello"}


def scratch():
    work = tempfile.mkdtemp(prefix="notebook-docs-")
    shutil.copytree(os.path.join(ROOT, "examples"), os.path.join(work, "examples"))
    for path in glob.glob(os.path.join(SITE, "notebooks", "*")):
        shutil.copy(path, work)
    shutil.copy(os.path.join(ROOT, ".github", "skills", "sketchbook-authoring", "assets", "template.sql"), work)
    return work


def run(duckdb, name):
    with open(os.path.join(OUTPUTS, name + ".sql"), encoding="utf-8") as f:
        sql = f.read()
    work = scratch()
    try:
        mode = [".mode list", ".headers off"] if name in RAW else [".mode duckbox", ".maxwidth 104"]
        result = subprocess.run([duckdb, "-no-agent", "-init", "/dev/null", ":memory:"], input="\n".join(mode) + "\n" + sql,
                                capture_output=True, text=True, cwd=work, timeout=600)
    finally:
        shutil.rmtree(work)
    if result.returncode != 0 or result.stderr.strip():
        raise SystemExit("%s failed:\n%s%s" % (name, result.stdout, result.stderr))
    text = result.stdout
    # Run times and sizes vary from run to run and machine to machine; keep them out of the diff.
    text = re.sub(r"(?m)^(\s*│?\s*).*\btmp/notebook-docs-\w+/", r"\1", text)
    with open(os.path.join(OUTPUTS, name + ".txt"), "w", encoding="utf-8") as f:
        f.write(text.rstrip() + "\n")
    print("wrote site/outputs/%s.txt" % name)


def main():
    args = sys.argv[1:]
    duckdb = os.path.join(ROOT, "build", "release", "duckdb")
    if args and os.path.sep in args[0]:
        duckdb = os.path.abspath(args.pop(0))
    names = args or sorted(os.path.basename(p)[:-4] for p in glob.glob(os.path.join(OUTPUTS, "*.sql")))
    for name in names:
        run(duckdb, name)


if __name__ == "__main__":
    main()
