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


def still(url):
    """The page with the handwriting animation off (render-ink.js): the checks here are about the finished
    page. test_ink checks the animation itself."""
    return url + ("&" if "?" in url else "?") + "ink=off"


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
    page.goto(still(url))
    page.wait_for_selector(".nb-cell")
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

    page.locator(".nb-input select").first.select_option(label="US")
    page.wait_for_function("document.querySelector('.nb-md').textContent.includes('US')", timeout=30000)
    check(True, "changing an input re-runs the cells that read it")

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
    page.goto(still(url))
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
    page.goto(still("file://" + html))
    page.wait_for_function("window.__NB_READY === true")
    check(page.locator("svg.nb-chart-svg").count() >= 2, "HTML export draws its charts offline")
    check(page.locator("pre.nb-source").count() >= 3, "an export shows the SQL by default")
    check(page.locator("pre.nb-source .tk-kw").count() > 5 and page.locator("pre.nb-source .tk-var").count() > 0, "exported SQL is highlighted")
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
    page.goto(still("file://" + nocode))
    page.wait_for_function("window.__NB_READY === true")
    check(page.locator("pre.nb-source").count() == 0 and page.locator("table.nb-grid").count() >= 3,
          "hide := 'code' exports results without code")
    check(page.locator(".nb-input select:disabled").count() == 1, "a static export shows inputs but freezes them")

    page.goto(still("file://" + pre))
    page.wait_for_function("window.__NB_READY === true")
    page.locator(".nb-input select").first.select_option(label="US")
    page.wait_for_function("document.querySelector('.nb-md').textContent.includes('US')")
    check(True, "a precomputed export switches results when an input changes")

    gallery = os.path.join(work, "charts.html")
    sql(db, f"FROM notebook_export('{os.path.join(ROOT, 'examples', 'charts.sql')}', file := '{gallery}')")
    page.goto(still("file://" + gallery))
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
    page.goto(still("file://" + live))
    page.wait_for_function("window.__NB_WASM === true", timeout=60000)
    page.locator(".nb-input select").first.select_option(label="APAC")
    page.wait_for_function("document.querySelector('.nb-md').textContent.includes('APAC')", timeout=30000)
    check(True, "html-wasm: DuckDB-WASM re-runs the notebook's SQL in the page, offline")
    check(not errors, f"no browser errors in the html-wasm export {errors[:3]}")


# ---- Handwriting (frontend/render-ink.js, docs/HANDWRITING.md) ----------------------------------

INK_STATE = """() => ({
    canvases: document.querySelectorAll('canvas.ink-layer').length,
    hidden: document.querySelectorAll('.ink-hide').length,
    styled: [...document.querySelectorAll('[style]')].filter(e => /opacity|visibility|dash|--ink/.test(e.getAttribute('style'))).length,
    running: Ink.jobs().filter(j => j.state !== 'parked').length,
    jobs: Ink.stats.jobs })"""


def html_of(page, selector):
    """The page's markup, without the clip-path ids Sketch.fill picks at random."""
    return re.sub(r"skc[a-z0-9]+", "skc", page.locator(selector).evaluate("e => e.outerHTML"))


def frames(page, n=2):
    page.evaluate(f"() => new Promise(r => {{ let k = {n}; const f = () => --k ? requestAnimationFrame(f) : r(); requestAnimationFrame(f); }})")


def settle(page):
    """Outlines follow their element's size a little later (Sketch.outline); let them."""
    page.wait_for_timeout(400)
    frames(page, 2)


def opened(page, url):
    page.goto(url)
    page.wait_for_function("window.__NB_READY === true && document.fonts.status === 'loaded'")
    frames(page, 3)


def pixel_diff(page, a, b):
    """Compares two PNG screenshots in the browser: pixels differing by more than 48 in a channel."""
    import base64
    return page.evaluate("""async ([a, b]) => {
      const load = async s => createImageBitmap(await (await fetch('data:image/png;base64,' + s)).blob());
      const [x, y] = [await load(a), await load(b)];
      const px = img => { const c = new OffscreenCanvas(img.width, img.height), g = c.getContext('2d'); g.drawImage(img, 0, 0); return g.getImageData(0, 0, img.width, img.height).data; };
      const p = px(x), q = px(y);
      let differ = 0, ink = 0;
      for (let i = 0; i < p.length; i += 4) {
        const d = Math.max(Math.abs(p[i] - q[i]), Math.abs(p[i + 1] - q[i + 1]), Math.abs(p[i + 2] - q[i + 2]));
        if (d > 48) differ++;
        if (Math.min(q[i], q[i + 1], q[i + 2]) < 160) ink++;
      }
      return { differ, ink, total: p.length / 4 };
    }""", [base64.b64encode(a).decode(), base64.b64encode(b).decode()])


def test_ink(browser, work, chromium):
    revenue = os.path.join(ROOT, "examples", "revenue.sql")
    charts = os.path.join(ROOT, "examples", "charts.sql")
    db = os.path.join(work, "ink.duckdb")
    html, gallery = os.path.join(work, "ink_revenue.html"), os.path.join(work, "ink_charts.html")
    sql(db, f"FROM notebook_export('{revenue}', file := '{html}')", f"FROM notebook_export('{charts}', file := '{gallery}')")
    url = "file://" + html
    viewport = {"width": 1300, "height": 950}

    # The reference: the same page with the animation off.
    off = browser.new_page(viewport=viewport)
    opened(off, still(url))
    settle(off)
    reference_html, reference_text = html_of(off, "#nb-root"), off.locator("#nb-root").inner_text()
    reference_shot = off.screenshot()
    check(off.evaluate(INK_STATE)["jobs"] == 0, "ink=off: the page is drawn with no handwriting")

    # 4. Mid-flight, on a stopped clock: text is all in the DOM, a canvas writes it, shapes wait.
    page = browser.new_page(viewport=viewport)
    errors = watch(page)
    page.add_init_script("window.__INK_PAUSE = true")
    opened(page, url)
    page.evaluate("Ink.advance(450)")
    frames(page, 2)
    state = page.evaluate(INK_STATE)
    hidden_paths = page.locator("svg path[style*='visibility: hidden']").count()
    check(page.locator("#nb-root").inner_text() == reference_text and state["canvases"] > 0 and state["hidden"] > 0 and hidden_paths > 0,
          f"part-way through, the text is complete in the DOM while a canvas writes it and paths wait ({state}, {hidden_paths} paths hidden)")

    # 1. No trace: finished, the page is exactly the page drawn without the animation.
    page.evaluate("Ink.advance(60000); Ink.finishAll()")
    settle(page)
    state = page.evaluate(INK_STATE)
    check(state["canvases"] == 0 and state["hidden"] == 0 and state["styled"] == 0 and html_of(page, "#nb-root") == reference_html,
          f"no trace: once written, the page's DOM is identical to a page drawn with the animation off ({state})")
    check(not errors, f"no browser errors while writing {errors[:3]}")

       # 3. Glyph alignment: the writer's last frame is the real text, in both fonts and at several sizes.
    page = browser.new_page(viewport=viewport)
    page.add_init_script("window.__INK_PAUSE = true")
    opened(page, url)
    page.evaluate("Ink.advance(60000)")
    frames(page, 2)
    page.evaluate("""() => {
      const box = document.createElement('div');
      box.id = 'ink-samples';
      box.style.cssText = 'position:relative;padding:8px;background:var(--nb-bg)';
      [['var(--nb-hand)', 14], ['var(--nb-hand)', 20], ['var(--nb-hand)', 42], ['var(--nb-mono)', 13.5], ['var(--nb-mono)', 22]].forEach(([f, size]) => {
        const p = document.createElement('div');
        p.style.cssText = `font-family:${f};font-size:${size}px;line-height:1.4;white-space:nowrap`;
        p.textContent = f.includes('mono') ? "SELECT sum(amount) AS revenue, 'EU' FROM t WHERE i >= 3; -- 0123" : 'Revenue review: The quick brown fox, 4,490,867.00 (APAC & EU)?';
        box.appendChild(p);
      });
      document.getElementById('nb-root').prepend(box);
      window.scrollTo(0, 0);
      Ink.present(box, 'test/samples', 'samples');
    }""")
    frames(page, 2)
    job = page.evaluate("(() => { const j = Ink.jobs().find(j => j.key === 'test/samples'); return { start: j.start, end: j.end, now: Ink.now() }; })()")
    page.evaluate(f"Ink.advance({job['start'] + job['end'] - job['now'] - 0.5})")
    frames(page, 2)
    samples = page.locator("#ink-samples")
    last_frame = samples.screenshot()
    writing = page.evaluate("Ink.jobs().some(j => j.key === 'test/samples')")
    page.evaluate("Ink.advance(5)")
    frames(page, 2)
    real = samples.screenshot()
    diff = pixel_diff(page, last_frame, real)
    check(writing and diff["ink"] > 3000 and diff["differ"] < diff["ink"] * 0.01,
          f"a glyph's last frame matches the real text it hands over to ({diff['differ']} of {diff['ink']} inked pixels differ)")

 # 2. Same pixels, and 7. the budget, on a running clock.
    page = browser.new_page(viewport=viewport)
    page.goto(url)
    page.wait_for_function("Ink.plans.length > 0")
    page.wait_for_function("Ink.jobs().every(j => j.state === 'parked')", timeout=10000)
    took = page.evaluate("performance.now() - Math.min(...Ink.plans.map(p => p.at))")
    shot = page.screenshot()
    diff = pixel_diff(page, shot, reference_shot)
    check(diff["differ"] == 0, f"same pixels: the first screen, written, looks exactly as it does without the animation ({diff['differ']} pixels differ)")
    check(took < 2500 * 1.25, f"revenue.sql: the first screen is written within the budget ({took:.0f} ms)")

    page = browser.new_page(viewport=viewport)
    page.goto("file://" + gallery)
    page.wait_for_function("Ink.plans.length > 0")
    page.wait_for_function("Ink.jobs().every(j => j.state === 'parked')", timeout=10000)
    took = page.evaluate("performance.now() - Math.min(...Ink.plans.map(p => p.at))")
    for step in range(12):
        page.mouse.wheel(0, 800)
        page.wait_for_timeout(1500)
    smooth = page.evaluate("""() => {
      const f = Ink.stats.frames.filter(x => x[2] > 0), gaps = [];
      for (let i = 1; i < f.length; i++) { const g = f[i][0] - f[i - 1][0]; if (g < 300) gaps.push(g); }
      gaps.sort((a, b) => a - b);
      const work = f.map(x => x[1]).sort((a, b) => a - b), q = (a, k) => a[Math.min(a.length - 1, Math.floor(a.length * k))];
      return { frames: f.length, gap50: q(gaps, .5), gap95: q(gaps, .95), work50: q(work, .5), work95: q(work, .95), workMax: q(work, 1) };
    }""")
    print(f"     charts.sql: first screen in {took:.0f} ms; frames while writing: " + ", ".join(f"{k} {v:.1f}" for k, v in smooth.items()))
    check(took < 2500 * 1.25 and smooth["gap95"] < 34, f"charts.sql: the first screen is written within the budget, at a smooth frame rate ({took:.0f} ms)")

    # A reader's own change (a tab) is drawn at once, not written out at length.
    page = browser.new_page(viewport=viewport)
    page.goto(url)
    page.wait_for_function("Ink.plans.length > 0")
    page.wait_for_function("Ink.jobs().every(j => j.state === 'parked')", timeout=10000)
    tab = page.locator(".nb-cell[data-idx='7'] .nb-tab", has_text="table")
    tab.scroll_into_view_if_needed()
    page.wait_for_function("Ink.jobs().every(j => j.state === 'parked')", timeout=10000)
    tab.click()
    quick = page.evaluate("Ink.plans[Ink.plans.length - 1]")
    page.wait_for_timeout(250)
    check(quick["dur"] <= 200 and page.evaluate(INK_STATE)["running"] == 0, f"switching a tab draws the new tab within 200 ms ({quick['key']}, {quick['dur']} ms)")

    # 8. Switches: each gives the complete page and no jobs.
    for name, setup in (("reduced motion", {"reduced_motion": "reduce"}), ("print media", None), ("the gear menu's setting", "localStorage.setItem('nb.ink', 'off')")):
        page = browser.new_page(viewport=viewport, **(setup if isinstance(setup, dict) else {}))
        if name == "print media":
            page.emulate_media(media="print")
        elif isinstance(setup, str):
            page.add_init_script(setup)
        opened(page, url)
        settle(page)
        state = page.evaluate(INK_STATE)
        # Print media lays the page out differently: compare only what does not depend on layout.
        same = html_of(page, "#nb-root") == reference_html or name == "print media"
        check(state["jobs"] == 0 and state["canvases"] == 0 and same, f"{name} switches the handwriting off ({state})")
    page = browser.new_page(viewport=viewport)
    page.add_init_script("window.__INK_PAUSE = true")
    opened(page, url)
    page.evaluate("Ink.advance(300)")
    frames(page, 2)
    page.evaluate("window.dispatchEvent(new Event('beforeprint'))")
    settle(page)
    state = page.evaluate(INK_STATE)
    check(state["canvases"] == 0 and state["hidden"] == 0 and state["running"] == 0 and html_of(page, "#nb-root") == reference_html,
          f"printing part-way through finishes the page at once ({state})")
    page.evaluate("Ink.advance(300)")
    page.keyboard.press("Escape")
    check(page.evaluate(INK_STATE)["running"] == 0, "a key press finishes everything on screen")

    # 9. PDF: the page handed to Chrome is marked not to animate.
    wrapper = os.path.join(work, "chrome.sh")
    with open(wrapper, "w") as f:
        f.write(f"#!/bin/sh\nfor a in \"$@\"; do case $a in file://*) cp \"${{a#file://}}\" '{work}/printed.html';; esac; done\nexec '{chromium}' \"$@\"\n")
    os.chmod(wrapper, 0o755)
    pdf = os.path.join(work, "ink.pdf")
    sql(db, f"FROM notebook_export('{revenue}', file := '{pdf}', format := 'pdf', chrome := '{wrapper}')")
    printed = open(os.path.join(work, "printed.html")).read()
    check(open(pdf, "rb").read(5) == b"%PDF-" and '<html lang="en" data-ink="off">' in printed,
          "PDF export prints a page marked not to animate")
    page = browser.new_page(viewport=viewport)
    page.goto("file://" + os.path.join(work, "printed.html"))
    page.wait_for_function("window.__NB_READY === true")
    frames(page, 3)
    check(page.evaluate(INK_STATE)["jobs"] == 0, "the page Chrome prints draws without handwriting")


def test_ink_editor(browser, work):
    # 5 and 6 in the editor, whose render() rebuilds every cell on every change.
    notebook = os.path.join(work, "ink_editor.sql")
    shutil.copy(os.path.join(ROOT, "examples", "revenue.sql"), notebook)
    shell = subprocess.Popen([DUCKDB, "-init", "/dev/null", "-noheader", "-list"], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=work, bufsize=1)
    shell.stdin.write(f"SELECT url FROM notebook_start('{notebook}', open := false);\n")
    shell.stdin.flush()
    url = shell.stdout.readline().strip()
    try:
        off = browser.new_page(viewport={"width": 1400, "height": 1000})
        off.goto(still(url))
        off.wait_for_selector(".nb-cell")
        off.wait_for_function("document.fonts.status === 'loaded'")
        settle(off)
        reference = html_of(off, "#cells")

        page = browser.new_page(viewport={"width": 1400, "height": 1000})
        errors = watch(page)
        page.add_init_script("window.__INK_PAUSE = true")
        page.goto(url)
        page.wait_for_selector(".nb-cell")
        page.wait_for_function("document.fonts.status === 'loaded'")
        frames(page, 3)
        page.evaluate("Ink.advance(700)")
        frames(page, 2)
        before = page.evaluate(INK_STATE)
        before_hidden = page.locator("#cells [style*='visibility: hidden']").count()
        page.evaluate("App.render()")
        frames(page, 2)
        after = page.evaluate(INK_STATE)
        after_hidden = page.locator("#cells [style*='visibility: hidden']").count()
        check(before["running"] > 0 and after["jobs"] == before["jobs"] and after["running"] == before["running"] and after_hidden == before_hidden,
              f"a render part-way through continues each piece where it was ({before} -> {after}, hidden paths {before_hidden} -> {after_hidden})")
        page.evaluate("Ink.advance(10000)")
        frames(page, 3)
        page.evaluate("Ink.finishAll()")
        settle(page)
        done = page.evaluate(INK_STATE)
        check(done["canvases"] == 0 and done["styled"] == 0 and html_of(page, "#cells") == reference,
              f"no trace in the editor: written, its cells are exactly the cells drawn without the animation ({done})")
        page.evaluate("App.render()")
        frames(page, 3)
        again = page.evaluate(INK_STATE)
        check(again["jobs"] == done["jobs"] and again["canvases"] == 0, f"rendering again after everything is written writes nothing ({again})")
        # Typing never writes: code is keyed to the notebook as opened, not to its text.
        page = browser.new_page(viewport={"width": 1400, "height": 1000})
        page.goto(url)
        page.wait_for_selector(".nb-cell")
        page.wait_for_function("Ink.jobs().every(j => j.state === 'parked')", timeout=10000)
        codes = page.evaluate("Ink.plans.filter(p => p.key.endsWith('/code')).length")
        area = page.locator(".nb-cell[data-idx='3'] textarea")
        area.click()
        area.press("End")
        area.type(" -- typed", delay=20)
        page.wait_for_timeout(900)
        check(page.evaluate("Ink.plans.filter(p => p.key.endsWith('/code')).length") == codes and page.locator(".nb-cell[data-idx='3'] canvas.ink-layer").count() == 0,
              "typing in a cell writes nothing")
        page.click("#gear")
        page.select_option("#ink", "off")
        page.keyboard.press("Escape")
        check(page.evaluate("localStorage.getItem('nb.ink')") == "off" and not page.evaluate("Ink.enabled()"), "the gear menu switches the handwriting off")
        check(not errors, f"no browser errors in the written editor {errors[:3]}")
    finally:
        shell.stdin.write("SELECT stopped FROM notebook_stop();\n.quit\n")
        shell.stdin.flush()
        shell.communicate(timeout=30)


def main():
    work = tempfile.mkdtemp(prefix="notebook_browser_test_")
    with sync_playwright() as p:
        browser = p.chromium.launch()
        try:
            test_server(browser, work)
            test_metadata_error(browser, work)
            # The headless shell prints PDFs too and needs fewer system libraries than full Chrome.
            shells = glob.glob(os.path.expanduser(
                os.environ.get("PLAYWRIGHT_BROWSERS_PATH", "~/.cache/ms-playwright") + "/chromium_headless_shell-*/chrome-headless-shell-linux64/chrome-headless-shell"))
            chromium = shells[0] if shells else p.chromium.executable_path
            test_exports(browser, work, chromium)
            test_ink(browser, work, chromium)
            test_ink_editor(browser, work)
        finally:
            browser.close()
    shutil.rmtree(work, ignore_errors=True)
    print(f"{len(failures)} failure(s)" if failures else "browser tests passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
