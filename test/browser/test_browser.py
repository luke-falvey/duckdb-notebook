"""End-to-end tests in a real browser: the notebook server and UI, and every export format.

Needs the release build, Python with Playwright, and Chromium (`playwright install chromium`).
Run with `make browser_test`. Set NOTEBOOK_WASM_RUNTIME to a built runtime (sh wasm/build.sh)
to include the html-wasm export.
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DUCKDB = os.path.join(ROOT, "build", "release", "duckdb")
failures = []


def check(condition, message):
    print(("ok   " if condition else "FAIL ") + message)
    if not condition:
        failures.append(message)


def sql(db, *statements):
    """Runs statements in a fresh DuckDB shell and returns its output."""
    args = [DUCKDB, "-init", "/dev/null", "-noheader", "-list"]
    for statement in statements:
        args += ["-c", statement]
    result = subprocess.run(args + [db], capture_output=True, text=True, cwd=ROOT, env=os.environ)
    if result.returncode != 0:
        raise RuntimeError(result.stderr or result.stdout)
    return result.stdout


def watch(page):
    errors = []
    page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
    page.on("pageerror", lambda e: errors.append(str(e)))
    return errors


def test_server(browser, work):
    notebook = os.path.join(work, "revenue.sql")
    shutil.copy(os.path.join(ROOT, "examples", "revenue.sql"), notebook)
    db = os.path.join(work, "server.duckdb")
    shell = subprocess.Popen([DUCKDB, "-init", "/dev/null", "-noheader", "-list", db], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=work, bufsize=1)
    shell.stdin.write(f"SELECT url FROM notebook_start('{notebook}', open := false);\n")
    shell.stdin.flush()
    url = shell.stdout.readline().strip()
    check(url.startswith("http://127.0.0.1:"), f"notebook_start returns a URL ({url[:40]}...)")

    page = browser.new_page(viewport={"width": 1400, "height": 1000})
    errors = watch(page)
    page.goto(url)
    page.wait_for_selector(".nb-cell")
    check(page.locator("#title").text_content() == "Revenue review", "the editor shows the notebook's title")
    check(page.locator(".nb-cell").count() == 13, "one editor cell per notebook cell")
    check(page.locator(".nb-badge.fresh").count() == 0, "opening a notebook does not run it")

    page.click("#run-all")
    page.wait_for_function("document.getElementById('status').textContent.startsWith('Ran')", timeout=30000)
    check(page.locator(".nb-badge.failed").count() == 0, "Run all: no cell fails")
    check(page.locator("svg.nb-chart-svg").count() >= 3, "Run all draws the charts")
    check("EU" in page.locator(".nb-md").first.text_content(), "markdown shows {{ $region }}")

    page.locator(".nb-input select").first.select_option(label="US")
    page.wait_for_function("document.querySelector('.nb-md').textContent.includes('US')", timeout=30000)
    check(True, "changing an input re-runs the cells that read it")

    before = page.locator(".nb-cell[data-idx='12'] table.nb-grid tbody tr").first.text_content()
    overlay = page.locator(".nb-cell[data-idx='9'] .nb-brush-overlay")
    overlay.scroll_into_view_if_needed()
    box = overlay.bounding_box()
    page.mouse.move(box["x"] + box["width"] * 0.2, box["y"] + 20)
    page.mouse.down()
    page.mouse.move(box["x"] + box["width"] * 0.3, box["y"] + 40, steps=5)
    page.mouse.up()
    page.wait_for_function(
        "(b) => { const r = document.querySelector(\".nb-cell[data-idx='12'] table.nb-grid tbody tr\"); "
        "return r && r.textContent !== b; }", arg=before, timeout=30000)
    months = page.locator(".nb-cell[data-idx='12'] table.nb-grid tbody tr td:nth-child(3)").all_text_contents()
    check(len(months) > 0 and all(m[5:7] in ("03", "04") for m in months),
          f"brushing the trend chart filters top_orders ({sorted(set(m[:7] for m in months))})")

    page.locator(".nb-add").last.locator("button", has_text="+ SQL").click()
    editor = page.locator(".nb-cell").last.locator("textarea")
    editor.fill("SELECT 41 + 1 AS answer;")
    editor.press("Shift+Enter")
    page.wait_for_function("[...document.querySelectorAll('.nb-cell')].pop().querySelector('table.nb-grid')",
                           timeout=30000)
    check("42" in page.locator(".nb-cell").last.locator("table.nb-grid").text_content(),
          "a new cell runs with Shift+Enter")
    page.keyboard.press("Control+s")
    page.wait_for_function("document.getElementById('status').textContent.startsWith('Saved')", timeout=10000)
    check("SELECT 41 + 1 AS answer;" in open(notebook).read(), "Ctrl+S writes the cell to the file")

    with open(notebook, "a") as f:
        f.write("\n-- %% from_disk\nSELECT 'edited elsewhere' AS note;\n")
    page.wait_for_function("document.querySelectorAll('.nb-cell').length === 15", timeout=15000)
    check(True, "an edit made to the file by another program appears in the editor")

    page.click("#view-graph")
    check(page.locator(".nb-graph path.edge").count() >= 8, "the graph view draws the dependencies")
    page.click("#view-reading")
    check(page.locator("textarea.nb-code:visible").count() == 0, "reading view hides the code")
    page.click("#view-reading")

    token = re.search(r"token=([0-9a-f]+)", url).group(1)
    base = url.split("/?")[0]
    response = page.request.get(f"{base}/api/export?nb={notebook}&format=html&token={token}")
    check(response.ok and b"nb-data" in response.body(), "the editor's Export produces an HTML file")
    other = browser.new_page()
    check(other.request.get(f"{base}/api/catalog?nb={notebook}").status == 403,
          "the API refuses requests without the token")
    check(not errors, f"no browser errors in the editor {errors[:3]}")

    shell.stdin.write("SELECT stopped FROM notebook_stop();\n.quit\n")
    shell.stdin.flush()
    output = shell.communicate(timeout=30)[0]
    check("true" in output, "notebook_stop stops the server")
    check(not os.path.exists(db + ".wal"), "the database closes and checkpoints when the shell exits")
    check(sql(db, "SELECT count(*) FROM orders").strip() == "50000",
          "another process can open the database afterwards")


def test_exports(browser, work, chromium):
    notebook = os.path.join(ROOT, "examples", "revenue.sql")
    db = os.path.join(work, "exports.duckdb")
    html = os.path.join(work, "revenue.html")
    pre = os.path.join(work, "revenue_pre.html")
    pdf = os.path.join(work, "revenue.pdf")
    sql(db, f"FROM notebook_export('{notebook}', file := '{html}')",
        f"FROM notebook_export('{notebook}', file := '{pre}', interactive := 'precomputed')",
        f"FROM notebook_export('{notebook}', file := '{pdf}', format := 'pdf', chrome := '{chromium}')")

    page = browser.new_page()
    errors = watch(page)
    page.route("**/*", lambda r: r.abort() if not r.request.url.startswith("file:") else r.continue_())
    page.goto("file://" + html)
    page.wait_for_function("window.__NB_READY === true")
    check(page.locator("svg.nb-chart-svg").count() >= 3, "HTML export draws its charts offline")
    check(page.locator(".nb-input select:disabled").count() == 1, "a static export shows inputs but freezes them")

    page.goto("file://" + pre)
    page.wait_for_function("window.__NB_READY === true")
    page.locator(".nb-input select").first.select_option(label="US")
    page.wait_for_function("document.querySelector('.nb-md').textContent.includes('US')")
    check(True, "a precomputed export switches results when an input changes")

    gallery = os.path.join(work, "charts.html")
    sql(db, f"FROM notebook_export('{os.path.join(ROOT, 'examples', 'charts.sql')}', file := '{gallery}')")
    page.goto("file://" + gallery)
    page.wait_for_function("window.__NB_READY === true")
    check(page.locator("svg.nb-chart-svg").count() >= 24 and page.locator(".nb-error").count() == 0,
          "every chart type in examples/charts.sql draws without errors")
    check(not errors, f"no browser errors in the exports {errors[:3]}")

    check(open(pdf, "rb").read(5) == b"%PDF-", "PDF export prints through headless Chromium")

    runtime = os.environ.get("NOTEBOOK_WASM_RUNTIME")
    if not runtime:
        print("skip html-wasm: set NOTEBOOK_WASM_RUNTIME to a runtime built by wasm/build.sh")
        return
    live = os.path.join(work, "revenue_wasm.html")
    sql(db, f"FROM notebook_export('{notebook}', file := '{live}', format := 'html-wasm', "
            f"wasm_runtime := '{runtime}')")
    page = browser.new_page()
    errors = watch(page)
    page.route("**/*", lambda r: r.abort() if not r.request.url.startswith(("file:", "blob:")) else r.continue_())
    page.goto("file://" + live)
    page.wait_for_function("window.__NB_WASM === true", timeout=60000)
    page.locator(".nb-input select").first.select_option(label="APAC")
    page.wait_for_function("document.querySelector('.nb-md').textContent.includes('APAC')", timeout=30000)
    check(True, "html-wasm: DuckDB-WASM re-runs the notebook's SQL in the page, offline")
    check(not errors, f"no browser errors in the html-wasm export {errors[:3]}")


def main():
    work = tempfile.mkdtemp(prefix="notebook_browser_test_")
    with sync_playwright() as p:
        browser = p.chromium.launch()
        try:
            test_server(browser, work)
            # The headless shell prints PDFs too and needs fewer system libraries than full Chrome.
            shells = glob.glob(os.path.expanduser(
                "~/.cache/ms-playwright/chromium_headless_shell-*/chrome-headless-shell-linux64/chrome-headless-shell"))
            test_exports(browser, work, shells[0] if shells else p.chromium.executable_path)
        finally:
            browser.close()
    shutil.rmtree(work, ignore_errors=True)
    print(f"{len(failures)} failure(s)" if failures else "browser tests passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
