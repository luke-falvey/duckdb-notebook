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
    check(page.locator(".nb-cell").count() == 11, "one editor cell per notebook cell")
    check(page.locator(".nb-badge.fresh").count() == 0, "opening a notebook does not run it")

    check(not page.locator("#menu").is_visible(), "options live behind the gear, closed by default")
    page.click("#gear")
    check(page.locator("#menu").is_visible() and page.locator("#view-graph").is_visible(), "the gear opens the options menu")
    page.keyboard.press("Escape")
    check(not page.locator("#menu").is_visible(), "Escape closes the options menu")

    page.click("#run-all")
    page.wait_for_function("document.getElementById('status').textContent.startsWith('Ran')", timeout=30000)
    check(page.locator(".nb-badge.failed").count() == 0, "Run all: no cell fails")
    check(page.locator("svg.nb-chart-svg").count() >= 2, "Run all draws the charts")
    check("EU" in page.locator(".nb-md").first.text_content(), "markdown shows {{ $region }}")

    page.locator(".nb-input select").first.select_option(label="US")
    page.wait_for_function("document.querySelector('.nb-md').textContent.includes('US')", timeout=30000)
    check(True, "changing an input re-runs the cells that read it")

    before = page.locator(".nb-cell[data-idx='10'] table.nb-grid tbody tr").first.text_content()
    overlay = page.locator(".nb-cell[data-idx='7'] .nb-brush-overlay")
    overlay.scroll_into_view_if_needed()
    box = overlay.bounding_box()
    page.mouse.move(box["x"] + box["width"] * 0.2, box["y"] + 20)
    page.mouse.down()
    page.mouse.move(box["x"] + box["width"] * 0.3, box["y"] + 40, steps=5)
    page.mouse.up()
    page.wait_for_function(
        "(b) => { const r = document.querySelector(\".nb-cell[data-idx='10'] table.nb-grid tbody tr\"); "
        "return r && r.textContent !== b; }", arg=before, timeout=30000)
    months = page.locator(".nb-cell[data-idx='10'] table.nb-grid tbody tr td:nth-child(3)").all_text_contents()
    check(len(months) > 0 and all(m[5:7] in ("03", "04") for m in months),
          f"brushing the trend chart filters top_orders ({sorted(set(m[:7] for m in months))})")

    # A query's charts are tabs over its result, with the table last.
    monthly = page.locator(".nb-cell[data-idx='7']")
    tabs = monthly.locator(".nb-tab")
    check(tabs.count() == 4 and "on" in tabs.first.get_attribute("class") and "Monthly revenue" in tabs.first.text_content(),
          "a query's charts are tabs, the first one open")
    check(monthly.locator(".nb-chart-title").count() == 0, "a chart in a tab is titled by its tab")
    monthly.locator(".nb-tab", has_text="table").click()
    check(monthly.locator("table.nb-grid").count() == 1, "the table tab shows the query's rows")
    monthly.locator(".nb-tab", has_text="Category mix").click()
    monthly.locator(".nb-stub", has_text="chart spec").click()
    spec = monthly.locator("textarea.nb-spec")
    spec.fill(spec.input_value().replace("Category mix", "Revenue by category"))
    spec.press("Shift+Enter")
    page.wait_for_function("[...document.querySelectorAll(\".nb-cell[data-idx='7'] .nb-tab\")].some(t => t.textContent === 'Revenue by category')",
                           timeout=15000)
    check(page.locator("#status").text_content().startswith("Ran 1 cell "), "editing a chart spec draws that chart again")

    # Page size: the picker under a table writes rows=N to the cell, so it is saved with the notebook.
    filtered = page.locator(".nb-cell[data-idx='6']")
    check(filtered.locator("table.nb-grid tbody tr").count() == 25, "tables show 25 rows per page by default")
    check(filtered.locator("table.nb-grid thead tr").count() == 1 and filtered.locator(".nb-coltype, svg.nb-spark").count() == 0,
          "table headers show column names only, no types or profile")
    filtered.locator("select.nb-page-size").select_option("10")
    page.wait_for_function("document.querySelectorAll(\".nb-cell[data-idx='6'] table.nb-grid tbody tr\").length === 10", timeout=15000)
    check("rows=10" in filtered.locator("input.nb-head").input_value(), "choosing 10 per page writes rows=10 to the cell")
    filtered.locator("button.nb-page", has_text="›").click()
    page.wait_for_function("(document.querySelector(\".nb-cell[data-idx='6'] .nb-page-range\") || {}).textContent === '11–20'", timeout=15000)
    check(True, "paging moves through the result 10 rows at a time")

    # Hide code / output: toggles in the cell header write hide= to the cell.
    regions = page.locator(".nb-cell[data-idx='3']")
    regions.locator("button.nb-hide-code").click()
    check("hide=code" in regions.locator("input.nb-head").input_value(), "the code toggle writes hide=code")
    check(not regions.locator("textarea.nb-code").is_visible() and regions.locator(".nb-stub").first.is_visible(),
          "hidden code collapses to a line that can show it")
    regions.locator("button.nb-hide-output").click()
    check("hide=all" in regions.locator("input.nb-head").input_value(), "hiding code and output writes hide=all")
    regions.locator("button.nb-hide-output").click()
    regions.locator("button.nb-hide-code").click()
    check("hide=" not in regions.locator("input.nb-head").input_value(), "showing both again removes hide=")

    page.locator(".nb-add").last.locator("button", has_text="+ SQL").click()
    editor = page.locator(".nb-cell").last.locator("textarea")
    editor.fill("SELECT 41 + 1 AS answer;")
    editor.press("Shift+Enter")
    page.wait_for_function("[...document.querySelectorAll('.nb-cell')].pop().querySelector('table.nb-grid')",
                           timeout=30000)
    check("42" in page.locator(".nb-cell").last.locator("table.nb-grid").text_content(),
          "a new cell runs with Shift+Enter")
    answer = page.locator(".nb-cell").last
    answer.locator("button.nb-add-chart").click()
    page.wait_for_function("[...document.querySelectorAll('.nb-cell')].pop().querySelectorAll('.nb-tab').length === 3",
                           timeout=15000)
    page.wait_for_function("[...document.querySelectorAll('.nb-cell')].pop().querySelector('.nb-tab-body').children.length > 0",
                           timeout=15000)
    check(answer.locator(".nb-tab-body .nb-error").count() == 0, "the chart picker adds a chart to the cell and draws it")
    page.keyboard.press("Control+s")
    page.wait_for_function("document.getElementById('status').textContent.startsWith('Saved')", timeout=10000)
    saved = open(notebook).read()
    check("SELECT 41 + 1 AS answer;" in saved, "Ctrl+S writes the cell to the file")
    check("-- %% filtered rows=10" in saved, "the chosen page size is saved in the file")
    check("SELECT 41 + 1 AS answer;\n\n/* chart\ntype: kpi" in saved, "an added chart is saved as a /* chart */ comment in its cell")
    check("title: Revenue by category" in saved and "title: Category mix" not in saved, "an edited chart spec is saved in its cell")

    with open(notebook, "a") as f:
        f.write("\n-- %% from_disk\nSELECT 'edited elsewhere' AS note;\n")
    page.wait_for_function("document.querySelectorAll('.nb-cell').length === 13", timeout=15000)
    check(True, "an edit made to the file by another program appears in the editor")

    page.click("#gear")
    page.click("#view-graph")
    check(page.locator(".nb-graph path.edge").count() >= 8, "the graph view draws the dependencies")
    page.click("#view-reading")
    check(page.locator("textarea.nb-code:visible").count() == 0, "reading view hides the code")
    page.click("#view-reading")
    page.keyboard.press("Escape")

    token = re.search(r"token=([0-9a-f]+)", url).group(1)
    base = url.split("/?")[0]
    response = page.request.get(f"{base}/api/export?nb={notebook}&format=html&token={token}")
    check(response.ok and b"nb-data" in response.body(), "the editor's Export produces an HTML file")
    response = page.request.get(f"{base}/api/export?nb={notebook}&format=md&hide=code&token={token}")
    check(response.ok and b"```sql" not in response.body(), "the editor's Export can leave the code out")
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
    check(page.locator("svg.nb-chart-svg").count() >= 2, "HTML export draws its charts offline")
    check(page.locator("pre.nb-source").count() >= 3, "an export shows the SQL by default")
    monthly = page.locator(".nb-cell[data-idx='7']")
    check(monthly.locator(".nb-tab").count() == 3 and "/* chart" not in monthly.locator("pre.nb-source").text_content(),
          "an export shows a query's charts as tabs and its SQL without them")
    monthly.locator(".nb-tab", has_text="table").click()
    check(page.locator(".nb-cell[data-idx='7'] table.nb-grid").count() == 1, "an export's table tab shows the rows")
    check(page.evaluate("document.fonts.check('20px Excalifont')"), "an export carries its handwriting font")
    first = page.locator(".nb-cell[data-kind='sql'] table.nb-grid").nth(1)
    check(first.locator("tbody tr").count() == 25, "exported tables page 25 rows at a time")

    nocode = os.path.join(work, "revenue_nocode.html")
    sql(db, f"FROM notebook_export('{notebook}', file := '{nocode}', hide := 'code')")
    page.goto("file://" + nocode)
    page.wait_for_function("window.__NB_READY === true")
    check(page.locator("pre.nb-source").count() == 0 and page.locator("table.nb-grid").count() >= 3,
          "hide := 'code' exports results without code")
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
