"""Regenerates the site's exports and screenshots from the extension built in this repository.

    python3 site/tools/assets.py            # everything
    python3 site/tools/assets.py exports    # only the live exports and the Markdown sample
    python3 site/tools/assets.py shots      # only the screenshots

Needs the release build (`GEN=ninja make release`), Python Playwright with Chromium, and Pillow. It writes:

- site/content/examples/*.html: HTML exports of the examples, published with the site;
- site/outputs/revenue_export.md: the Markdown export, shown in the exporting guide;
- site/content/images/*.webp: screenshots of the editor, at device scale factor 2;
- site/content/images/cells/charts-<cell>.webp: each chart of examples/charts.sql, from its export.

It starts one notebook server in a DuckDB shell it owns and stops it (by PID) before it exits.
Run site/tools/outputs.py as well when the extension's output may have changed.
"""
import functools
import http.server
import os
import shutil
import subprocess
import sys
import threading

from outputs import ROOT, SITE, scratch

DUCKDB = os.path.join(ROOT, "build", "release", "duckdb")
CONTENT = os.path.join(SITE, "content")
IMAGES = os.path.join(CONTENT, "images")
EXAMPLES = os.path.join(CONTENT, "examples")


def sql(work, *statements):
    args = [DUCKDB, "-no-agent", "-init", "/dev/null", "-noheader", "-list"]
    for statement in statements:
        args += ["-c", statement]
    result = subprocess.run(args, capture_output=True, text=True, cwd=work)
    if result.returncode != 0 or result.stderr.strip():
        raise SystemExit("query failed: %s\n%s%s" % (statements, result.stdout, result.stderr))
    return result.stdout


def exports(work):
    os.makedirs(EXAMPLES, exist_ok=True)
    jobs = [
        ("examples/revenue.sql", "revenue.html", ", interactive := 'precomputed'"),
        ("examples/charts.sql", "charts.html", ""),
        ("hello.sql", "hello.html", ""),
    ]
    for source, name, extra in jobs:
        print(sql(work, "SELECT path, bytes, note FROM notebook_export('%s', file := '%s'%s)" % (source, name, extra)).strip())
        shutil.copy(os.path.join(work, name), os.path.join(EXAMPLES, name))
    sql(work, "FROM notebook_export('examples/revenue.sql', format := 'md', file := 'revenue.md')")
    shutil.copy(os.path.join(work, "revenue.md"), os.path.join(SITE, "outputs", "revenue_export.md"))


class Server:
    """notebook_start in a DuckDB shell kept alive on a pipe; stop() ends that shell by PID."""

    def __init__(self, work):
        self.shell = subprocess.Popen([DUCKDB, "-no-agent", "-init", "/dev/null", "-noheader", "-list"],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      text=True, cwd=work, bufsize=1)
        self.shell.stdin.write("SELECT url FROM notebook_start(open := false);\n")
        self.shell.stdin.flush()
        self.base = self.shell.stdout.readline().strip()
        if not self.base.startswith("http://127.0.0.1:"):
            self.stop()
            raise SystemExit("notebook_start did not return a URL: " + self.base)

    def url(self, notebook):
        return self.base + "&nb=" + notebook + "&ink=off"

    def stop(self):
        self.shell.kill()
        self.shell.wait()


def ready(page):
    page.wait_for_selector(".nb-cell")
    page.evaluate("document.fonts.ready")


def run_all(page):
    page.click("#run-all")
    page.wait_for_function("document.getElementById('status').textContent.startsWith('Ran')", timeout=60000)
    page.wait_for_timeout(400)


def menu_item(page, button):
    page.click("#gear")
    page.click(button)
    page.keyboard.press("Escape")
    page.wait_for_timeout(300)


def cell(page, idx):
    return page.locator(".nb-cell[data-idx='%d']" % idx)


def shoot_editor(browser, server):
    page = browser.new_page(viewport={"width": 1360, "height": 860}, device_scale_factor=2)

    # The hello notebook, run.
    page.goto(server.url("hello.sql"))
    ready(page)
    run_all(page)
    page.screenshot(path=os.path.join(IMAGES, "hello-editor.png"))

    # The revenue example: the editor, its tabs, a brush, the menu, the graph and the reading view.
    page.goto(server.url("examples/revenue.sql"))
    ready(page)
    run_all(page)
    page.screenshot(path=os.path.join(IMAGES, "editor-top.png"))
    # The inputs and the queries they drive.
    page.evaluate("y => document.getElementById('main').scrollBy(0, y)", cell(page, 4).bounding_box()["y"] - 80)
    page.wait_for_timeout(300)
    page.screenshot(path=os.path.join(IMAGES, "editor-revenue.png"))

    monthly = cell(page, 7)
    monthly.scroll_into_view_if_needed()
    page.wait_for_timeout(200)
    monthly.screenshot(path=os.path.join(IMAGES, "revenue-tabs.png"))

    page.click("#gear")
    page.wait_for_timeout(200)
    box = page.locator("#menu").bounding_box()
    page.screenshot(path=os.path.join(IMAGES, "menu.png"),
                    clip={"x": box["x"] - 24, "y": 0, "width": box["width"] + 48, "height": box["y"] + box["height"] + 24})
    page.keyboard.press("Escape")

    # A cell's margins and buttons: hover the filtered cell.
    filtered = cell(page, 6)
    filtered.scroll_into_view_if_needed()
    filtered.hover()
    page.wait_for_timeout(400)
    fb = filtered.bounding_box()
    page.screenshot(path=os.path.join(IMAGES, "cell-anatomy.png"),
                    clip={"x": max(0, fb["x"] - 165), "y": fb["y"] - 14, "width": fb["width"] + 235, "height": fb["height"] + 28})

    # Brush March to May on the trend chart; top_orders runs again.
    overlay = monthly.locator(".nb-brush-overlay")
    overlay.scroll_into_view_if_needed()
    ob = overlay.bounding_box()
    y = ob["y"] + ob["height"] * 0.5
    page.mouse.move(ob["x"] + ob["width"] * 0.17, y)
    page.mouse.down()
    page.mouse.move(ob["x"] + ob["width"] * 0.30, y, steps=6)
    page.mouse.move(ob["x"] + ob["width"] * 0.40, y, steps=6)
    page.mouse.up()
    page.wait_for_function("document.getElementById('status').textContent.startsWith('Ran')", timeout=60000)
    page.wait_for_timeout(600)
    monthly.locator(".nb-tab-body").screenshot(path=os.path.join(IMAGES, "brush.png"))
    top = cell(page, 10)
    top.scroll_into_view_if_needed()
    page.wait_for_timeout(300)
    top.locator(".nb-output").screenshot(path=os.path.join(IMAGES, "brush-top-orders.png"))

    # The dependency graph.
    page.goto(server.url("examples/revenue.sql"))
    ready(page)
    menu_item(page, "#view-graph")
    graph = page.locator("#graph")
    graph.screenshot(path=os.path.join(IMAGES, "graph.png"))

    # The sketchbook template, laid out for reading: the page a reader gets.
    page.goto(server.url("template.sql"))
    ready(page)
    run_all(page)
    menu_item(page, "#view-reading")
    page.evaluate("document.getElementById('main').scrollTo(0, 0)")
    page.wait_for_timeout(300)
    page.screenshot(path=os.path.join(IMAGES, "template-reading.png"))

    page.set_viewport_size({"width": 1360, "height": 1000})
    page.goto(server.url("inputs.sql"))
    ready(page)
    run_all(page)
    menu_item(page, "#view-reading")
    page.screenshot(path=os.path.join(IMAGES, "inputs-reading.png"))

    # Dark mode, for the reading guide.
    page.set_viewport_size({"width": 1360, "height": 860})
    page.goto(server.url("examples/revenue.sql"))
    ready(page)
    run_all(page)
    menu_item(page, "#theme")
    page.screenshot(path=os.path.join(IMAGES, "editor-dark.png"))
    menu_item(page, "#theme")
    page.close()


def shoot_reopen(browser, work):
    """A fresh server opening hello.sql after it ran: the kept results show without running anything,
    and a changed input marks what reads it stale."""
    server = Server(work)
    try:
        page = browser.new_page(viewport={"width": 1360, "height": 860}, device_scale_factor=2)
        page.goto(server.url("hello.sql"))
        ready(page)
        page.wait_for_timeout(400)
        page.select_option(".nb-cell[data-idx='1'] select", label="Oslo")
        page.wait_for_timeout(600)
        page.screenshot(path=os.path.join(IMAGES, "cache-stale.png"))
        page.close()
    finally:
        server.stop()


def shoot_charts(browser):
    """Each chart cell of examples/charts.sql, from its HTML export."""
    os.makedirs(os.path.join(IMAGES, "cells"), exist_ok=True)
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=EXAMPLES)
    handler.log_message = lambda *a: None
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        cells = sql(ROOT, "SELECT idx || ' ' || name FROM notebook_cells('examples/charts.sql') WHERE kind = 'chart'")
        page = browser.new_page(viewport={"width": 1300, "height": 900}, device_scale_factor=2)
        page.goto("http://127.0.0.1:%d/charts.html?ink=off" % httpd.server_address[1])
        page.wait_for_function("window.__NB_READY === true", timeout=30000)
        page.evaluate("document.fonts.ready")
        page.wait_for_timeout(500)
        for line in cells.strip().splitlines():
            idx, name = line.split()
            element = page.locator(".nb-cell[data-idx='%s']" % idx)
            element.scroll_into_view_if_needed()
            page.wait_for_timeout(100)
            element.screenshot(path=os.path.join(IMAGES, "cells", "charts-%s.png" % name))
        page.close()
    finally:
        httpd.shutdown()


def to_webp():
    """Screenshots of paper compress badly as PNG; WebP is a tenth of the size and looks the same."""
    from PIL import Image
    for folder, _, files in os.walk(IMAGES):
        for name in files:
            if name.endswith(".png"):
                path = os.path.join(folder, name)
                Image.open(path).save(path[:-4] + ".webp", quality=85, method=6)
                os.remove(path)


def main():
    what = sys.argv[1:] or ["exports", "shots"]
    work = scratch()
    try:
        if "exports" in what:
            exports(work)
        if "shots" in what:
            from playwright.sync_api import sync_playwright
            os.makedirs(IMAGES, exist_ok=True)
            server = Server(work)
            try:
                with sync_playwright() as p:
                    browser = p.chromium.launch()
                    shoot_editor(browser, server)
                    server.stop()
                    shoot_reopen(browser, work)
                    shoot_charts(browser)
                    browser.close()
            finally:
                server.stop()
            to_webp()
    finally:
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
