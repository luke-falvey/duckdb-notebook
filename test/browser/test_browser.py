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
    page.wait_for_function("window.Draw && Draw.running() > 0", timeout=10000)
    check(True, "opening a notebook draws it in")
    page.mouse.click(5, 300)
    check(page.evaluate("Draw.running()") == 0, "a click finishes the drawing at once")
    check(page.locator("#title").text_content() == "Revenue review", "the editor shows the notebook's title")
    check(page.locator(".nb-cell").count() == 11, "one editor cell per notebook cell")
    check(page.locator(".nb-badge.fresh").count() == 0, "opening a notebook does not run it")

    check(not page.locator("#menu").is_visible(), "options live behind the gear, closed by default")

    # SQL highlighting: a coloured copy behind each SQL text area, with DuckDB's own keywords.
    filtered_code = page.locator(".nb-cell[data-idx='6']")
    hl, ta = filtered_code.locator(".nb-code-hl"), filtered_code.locator("textarea")
    check(hl.locator(".tk-kw").first.text_content() == "SELECT" and hl.locator(".tk-var").first.text_content() == "$region"
          and hl.locator(".tk-name").first.text_content() == "orders", "SQL is highlighted: keywords, $variables and cell names")
    check(hl.evaluate("e => e.textContent") == ta.input_value() + "\n" and abs(hl.bounding_box()["height"] - ta.bounding_box()["height"]) < 1
          and ta.evaluate("e => getComputedStyle(e).color") == "rgba(0, 0, 0, 0)", "the highlighted copy matches the text area exactly, behind transparent text")
    tokens = page.evaluate("""() => NB.tokenizeSql("SELECT $$a -- b$$ /* x /* y */ z */ 'it''s' $v collate day sum(n) t.select AS orders FROM orders",
      {names: {orders: true}}).filter(t => t.type !== 'ws').map(t => t.type)""")
    check(tokens == ["kw", "str", "comment", "str", "var", "kw", "ident", "fn", "op", "ident", "op", "ident", "op", "ident",
                     "kw", "ident", "kw", "name"],
          f"the tokenizer follows DuckDB's lexer and keyword list ({tokens})")
    page.click("#gear")
    check(page.locator("#menu").is_visible() and page.locator("#view-graph").is_visible(), "the gear opens the options menu")
    page.keyboard.press("Escape")
    check(not page.locator("#menu").is_visible(), "Escape closes the options menu")

    page.click("#run-all")
    page.wait_for_function("document.getElementById('status').textContent.startsWith('Ran')", timeout=30000)
    check(page.locator(".nb-badge.failed").count() == 0, "Run all: no cell fails")
    check(page.locator("svg.nb-chart-svg").count() >= 2, "Run all draws the charts")
    check("EU" in page.locator(".nb-md").first.text_content(), "markdown shows {{ $region }}")
    foot = page.locator(".nb-cell[data-idx='6'] .nb-foot")
    check(foot.count() == 1 and "rows" in foot.text_content() and "defines filtered" in foot.text_content() and foot.locator(".nb-time").count() == 1
          and "rows" not in page.locator(".nb-cell[data-idx='6'] .nb-meta").text_content(),
          "rows, what a cell defines and its run time sit together in the bottom right of its box")
    kinds = page.locator(".nb-kind").all()
    check([k.text_content() for k in kinds[:8]] == ["☰", "¶", "✎", "▦", "◉", "◉", "▦", "▦"] and "Query" in kinds[3].get_attribute("title"),
          "cell kinds are symbols, named in their tooltips")
    # Margins: kind and name to the left of a cell, state and buttons to the right, frame as a bracket.
    cell6 = page.locator(".nb-cell[data-idx='6']")
    cb, mb, hb = cell6.bounding_box(), cell6.locator(".nb-margin").bounding_box(), cell6.locator(".nb-cell-head").bounding_box()
    check(mb["x"] + mb["width"] <= cb["x"] and hb["x"] >= cb["x"] + cb["width"] and cell6.locator(".nb-name").text_content() == "filtered",
          "a cell's kind and name sit in its left margin, its state and buttons in its right")
    check(cell6.locator("svg.sk-outline").evaluate("e => getComputedStyle(e).clipPath") != "none", "a cell's frame is drawn as a bracket on its left")
    # Buttons: a horizontal strip on the top edge, shown on hover, reachable however short the cell.
    tools = page.locator(".nb-cell[data-idx='3'] .nb-tools")
    check(tools.evaluate("e => getComputedStyle(e).opacity") == "0", "a cell's buttons are hidden until the pointer is on it")
    page.locator(".nb-cell[data-idx='3']").hover()
    page.wait_for_timeout(300)
    rb, tb3 = page.locator(".nb-cell[data-idx='3']").bounding_box(), tools.bounding_box()
    dot = page.locator(".nb-cell[data-idx='3'] .nb-badge i").bounding_box()
    check(tools.evaluate("e => getComputedStyle(e).opacity") == "1" and tb3["height"] < 40 and tb3["width"] > 150
          and tb3["x"] + tb3["width"] <= rb["x"] + rb["width"]
          and abs((tb3["y"] + tb3["height"] / 2) - (dot["y"] + dot["height"] / 2)) < 2,
          "hovering shows them as one row at the top right, in line with the state dot")
    cell6.locator(".nb-name").click()
    head_input = cell6.locator("input.nb-head")
    check(head_input.is_visible() and head_input.evaluate("e => e === document.activeElement"), "clicking a cell's name opens its -- %% line")
    page.keyboard.press("Escape")
    page.wait_for_function("!document.querySelector(\".nb-cell[data-idx='6'] input.nb-head\").offsetParent", timeout=5000)
    check(head_input.input_value() == "filtered", "Escape closes it unchanged")
    trend = page.locator(".nb-cell[data-idx='7'] .nb-brush-overlay")
    trend.scroll_into_view_if_needed()
    tb = trend.bounding_box()
    page.mouse.move(tb["x"] + tb["width"] * 0.5, tb["y"] + tb["height"] * 0.5)
    page.mouse.move(tb["x"] + tb["width"] * 0.5 + 3, tb["y"] + tb["height"] * 0.5 + 3)
    page.wait_for_selector(".nb-tip", state="visible", timeout=5000)
    check("revenue" in page.locator(".nb-tip").text_content() and page.locator(".nb-cell[data-idx='7'] .nb-hover-dot[visibility='visible']").count() == 1,
          "hovering a line chart shows the nearest point's values, through the brush overlay")
    monthly_cell = page.locator(".nb-cell[data-idx='7']")
    monthly_cell.locator(".nb-tab", has_text="Category mix").click()
    bar = monthly_cell.locator("svg.nb-chart-svg [data-tip]").first
    bb, plot = bar.bounding_box(), monthly_cell.locator("svg.nb-chart-svg").bounding_box()
    # Past the end of the bar, in its row: the bar still answers.
    page.mouse.move(plot["x"] + plot["width"] - 30, bb["y"] + bb["height"] / 2)
    page.mouse.move(plot["x"] + plot["width"] - 28, bb["y"] + bb["height"] / 2 + 1)
    page.wait_for_function("(document.querySelector('.nb-tip') || {}).style && document.querySelector('.nb-tip').style.display === 'block' && document.querySelector('.nb-tip').textContent.includes('category')", timeout=5000)
    check(monthly_cell.locator(".nb-hover-band[visibility='visible']").count() == 1, "a bar answers hover anywhere in its row, highlighted")
    monthly_cell.locator(".nb-tab").first.click()
    heat = page.locator(".nb-cell[data-idx='9'] [data-tip]").nth(5)
    heat.scroll_into_view_if_needed()
    heat.hover(force=True)
    page.wait_for_function("(document.querySelector('.nb-tip') || {}).textContent && document.querySelector('.nb-tip').textContent.includes('amount')", timeout=5000)
    check(True, "hovering a mark shows its values")
    page.mouse.move(5, 5)
    check(not page.locator(".nb-tip").is_visible(), "the values go away when the pointer leaves")
    add_bar = page.locator(".nb-add").nth(3)
    check(add_bar.evaluate("e => getComputedStyle(e).opacity") == "0", "the add-cell buttons between cells are hidden")
    add_bar.hover()
    page.wait_for_timeout(300)
    check(add_bar.evaluate("e => getComputedStyle(e).opacity") == "1", "hovering between cells shows the add-cell buttons")
    regions = page.locator(".nb-cell[data-idx='3']")
    check(regions.locator(".nb-switcher").count() == 0 and regions.locator("button.nb-pick-chart").count() == 1,
          "a table's chart picker is hidden behind '+ chart'")
    regions.locator("button.nb-pick-chart").click()
    check(regions.locator(".nb-switcher").count() == 1, "'+ chart' opens the chart picker")
    regions.locator("button.nb-switcher-close").click()
    check(regions.locator(".nb-switcher").count() == 0, "the chart picker closes again")

    page.wait_for_function("Draw.running() === 0", timeout=10000)
    page.locator(".nb-input select").first.select_option(label="US")
    page.wait_for_function("document.querySelector('.nb-md').textContent.includes('US')", timeout=30000)
    check(True, "changing an input re-runs the cells that read it")
    check(page.evaluate("Draw.running()") == 0, "an input change redraws without the drawing animation")

    before = page.locator(".nb-cell[data-idx='10'] table.nb-grid tbody tr").first.text_content()
    overlay = page.locator(".nb-cell[data-idx='7'] .nb-brush-overlay")
    overlay.scroll_into_view_if_needed()
    box = overlay.bounding_box()
    scrolled = page.evaluate("document.getElementById('main').scrollTop")
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
    check(abs(page.evaluate("document.getElementById('main').scrollTop") - scrolled) < 5 and scrolled > 100,
          f"brushing keeps the page where it was (scrollTop {scrolled})")

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
    regions.hover()
    regions.locator("button.nb-hide-code").click()
    check("hide=code" in regions.locator("input.nb-head").input_value(), "the code toggle writes hide=code")
    page.wait_for_timeout(700)
    check(regions.locator(".nb-tools").evaluate("e => getComputedStyle(e).opacity") == "1",
          "after a click the buttons stay, though the cell redrew and got shorter")
    check(not regions.locator("textarea.nb-code").is_visible() and regions.locator(".nb-stub").first.is_visible(),
          "hidden code collapses to a line that can show it")
    regions.hover()
    regions.locator("button.nb-hide-output").click()
    check("hide=all" in regions.locator("input.nb-head").input_value(), "hiding code and output writes hide=all")
    regions.hover()
    regions.locator("button.nb-hide-output").click()
    regions.hover()
    regions.locator("button.nb-hide-code").click()
    check("hide=" not in regions.locator("input.nb-head").input_value(), "showing both again removes hide=")

    page.locator(".nb-add").last.locator("button", has_text="+ SQL").click()
    editor = page.locator(".nb-cell").last.locator("textarea")
    editor.fill("SELECT 41 + 1 AS answer;")
    check(page.locator(".nb-cell").last.locator(".nb-code-hl .tk-num").first.text_content() == "41", "highlighting follows typing")
    editor.press("Shift+Enter")
    page.wait_for_function("[...document.querySelectorAll('.nb-cell')].pop().querySelector('table.nb-grid')",
                           timeout=30000)
    check("42" in page.locator(".nb-cell").last.locator("table.nb-grid").text_content(),
          "a new cell runs with Shift+Enter")
    answer = page.locator(".nb-cell").last
    answer.locator("button.nb-pick-chart").click()
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

    # Timing is a notebook setting: the gear's switch writes timing: false into the [notebook] cell.
    page.click("#run-all")
    page.wait_for_function("document.getElementById('status').textContent.startsWith('Ran')", timeout=30000)
    check(page.locator(".nb-time").count() > 5, "run times show after Run all")
    page.click("#gear")
    page.uncheck("#show-timing")
    check(page.locator(".nb-time").count() == 0, "switching timing off hides run times")
    page.keyboard.press("Escape")
    page.keyboard.press("Control+s")
    page.wait_for_function("document.getElementById('status').textContent.startsWith('Saved')", timeout=10000)
    check("timing: false\n*/" in open(notebook).read(), "the timing switch is saved in the [notebook] cell")
    page.click("#gear")
    page.check("#show-timing")
    check(page.locator(".nb-time").count() > 0, "switching timing on shows run times again")
    page.keyboard.press("Escape")

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

    # The drawing is a notebook setting: the gear's switch writes animate: false into the [notebook] cell.
    page.click("#gear")
    page.uncheck("#show-drawing")
    page.keyboard.press("Escape")
    page.keyboard.press("Control+s")
    page.wait_for_function("document.getElementById('status').textContent.startsWith('Saved')", timeout=10000)
    check("animate: false\n" in open(notebook).read(), "the drawing switch is saved in the [notebook] cell")
    page.reload()
    page.wait_for_selector(".nb-cell")
    page.wait_for_timeout(800)
    check(page.evaluate("Draw.running()") == 0, "with animate: false the notebook opens without drawing in")

    shell.stdin.write("SELECT stopped FROM notebook_stop();\n.quit\n")
    shell.stdin.flush()
    output = shell.communicate(timeout=30)[0]
    check("true" in output, "notebook_stop stops the server")
    check(not os.path.exists(db + ".wal"), "the database closes and checkpoints when the shell exits")
    check(sql(db, "SELECT count(*) FROM orders").strip() == "50000",
          "another process can open the database afterwards")


def test_metadata_error(browser, work):
    # A notebook whose [notebook] cell needs an extension this DuckDB lacks says so in full.
    notebook = os.path.join(work, "needs_ext.sql")
    with open(notebook, "w") as f:
        f.write("-- %% [notebook]\n/*\ntitle: Needs an extension\nextensions: [no_such_extension_xyz]\n*/\n\n-- %% one\nSELECT 1 AS x;\n")
    shell = subprocess.Popen([DUCKDB, "-init", "/dev/null", "-noheader", "-list"], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=work, bufsize=1)
    shell.stdin.write(f"SELECT url FROM notebook_start('{notebook}', open := false);\n")
    shell.stdin.flush()
    url = shell.stdout.readline().strip()
    page = browser.new_page()
    page.goto(url)
    page.wait_for_selector(".nb-cell")
    page.click("#run-all")
    page.wait_for_selector(".nb-problem", timeout=30000)
    text = page.locator(".nb-problem .nb-error").text_content()
    check("no_such_extension_xyz" in text and "LOAD no_such_extension_xyz works" in text and page.locator(".nb-problem").is_visible(),
          "a run that fails before any cell shows the whole message, naming the missing extension")
    shell.stdin.write("SELECT stopped FROM notebook_stop();\n.quit\n")
    shell.stdin.flush()
    shell.communicate(timeout=30)


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
    page.wait_for_function("Draw.running() > 0", timeout=10000)
    check(True, "an export draws itself in when it opens")
    page.wait_for_function("Draw.running() === 0", timeout=10000)
    check(page.locator("svg.nb-chart-svg").count() >= 2, "HTML export draws its charts offline")
    check(page.locator("pre.nb-source").count() >= 3, "an export shows the SQL by default")
    check(page.locator("pre.nb-source .tk-kw").count() > 5 and page.locator("pre.nb-source .tk-var").count() > 0, "exported SQL is highlighted")
    monthly = page.locator(".nb-cell[data-idx='7']")
    check(monthly.locator(".nb-tab").count() == 3 and "/* chart" not in monthly.locator("pre.nb-source").text_content(),
          "an export shows a query's charts as tabs and its SQL without them")
    monthly.locator(".nb-tab", has_text="table").click()
    check(page.locator(".nb-cell[data-idx='7'] table.nb-grid").count() == 1, "an export's table tab shows the rows")
    check(page.evaluate("document.fonts.check('20px Excalifont')"), "an export carries its handwriting font")
    calm = browser.new_page(reduced_motion="reduce")
    calm.goto("file://" + html)
    calm.wait_for_function("window.__NB_READY === true")
    calm.wait_for_timeout(800)
    check(calm.evaluate("Draw.running()") == 0, "the drawing is skipped for prefers-reduced-motion")
    calm.close()
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
    legend = page.locator(".nb-cell[data-idx='6'] .nb-legend-item").evaluate_all(
        "els => els.map(e => e.textContent + '=' + getComputedStyle(e.querySelector('i')).borderColor)")
    check(legend == ["North=rgb(25, 113, 194)", "South=rgb(47, 158, 68)", "East=rgb(232, 89, 12)", "West=rgb(156, 54, 181)"],
          f"a color scale's domain and range fix each category's order and colour ({legend})")
    kpi_colour = page.locator(".nb-kpi-banded .nb-kpi-value").first.evaluate("e => getComputedStyle(e).color")
    check(kpi_colour == "rgb(47, 158, 68)", f"a KPI's bands colour its value by the band it falls in ({kpi_colour})")
    widths = page.locator(".nb-cell[data-idx='15'] svg.nb-chart-svg [data-tip] > .sk-hit").evaluate_all("els => els.map(e => e.getBoundingClientRect().width)")
    check(len(widths) == 48 and min(widths) > 0.9 * max(widths),
          f"bars on a date axis fill their slot, the first and last as wide as the rest ({min(widths or [0]):.1f}-{max(widths or [0]):.1f}px)")
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
            test_metadata_error(browser, work)
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
