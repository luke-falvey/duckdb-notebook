"""MkDocs hooks for the documentation site.

- Pages include files from anywhere in the repository (`--8<-- "examples/revenue.sql"`), so the
  notebooks shown in the docs are the files that test/sql/docs_notebooks.test checks.
- reference/format.md is docs/FORMAT.md, rendered here: the specification is written once.
- `<!-- cell NAME -->` in a page becomes that cell of examples/charts.sql (or `<!-- cell NAME in
  PATH -->`), with its screenshot from content/images/cells/<notebook>-<cell>.webp if there is one, so chart examples are the tested ones.
- The fonts (from frontend/) and the handwriting captures (from docs/handwriting/) are copied into
  the built site, so they are not duplicated in the repository.
"""
import os
import re
import shutil

SITE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SITE)
BLOB = "https://github.com/luke-falvey/duckdb-notebook/blob/main/"
FONTS = ["Excalifont.woff2", "CascadiaCode.woff2"]

# Generated pages: the page in content/ is a placeholder, its text comes from this file.
RENDERED = {"reference/format.md": "docs/FORMAT.md"}


def on_config(config):
    snippets = config["mdx_configs"].setdefault("pymdownx.snippets", {})
    snippets["base_path"] = [ROOT]
    return config


def repository_links(markdown, source):
    """Rewrites relative links in a repository document to GitHub, keeping anchors."""
    folder = os.path.dirname(source)

    def rewrite(match):
        text, target = match.group(1), match.group(2)
        if re.match(r"^[a-z]+:|^#", target):
            return match.group(0)
        path, _, anchor = target.partition("#")
        resolved = os.path.normpath(os.path.join(folder, path)).replace(os.sep, "/")
        return "[%s](%s%s%s)" % (text, BLOB, resolved, "#" + anchor if anchor else "")

    return re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", rewrite, markdown)


def notebook_cell(path, name):
    """The text of the cell called `name` in a notebook: from its -- %% line to the next one."""
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        lines = f.read().split("\n")
    start = None
    for i, line in enumerate(lines):
        if line.startswith("-- %%"):
            if start is not None:
                return "\n".join(lines[start:i]).rstrip()
            words = line[5:].split()
            if words and words[0] in ("chart", "input"):
                words = words[1:]
            if words and words[0] == name:
                start = i
    if start is None:
        raise ValueError("no cell %r in %s" % (name, path))
    return "\n".join(lines[start:]).rstrip()


def on_page_markdown(markdown, page, config, files):
    depth = page.file.src_uri.count("/")

    def cell(match):
        name, path = match.group(1), match.group(2) or "examples/charts.sql"
        text = notebook_cell(path, name)
        stem = os.path.splitext(os.path.basename(path))[0]
        image = "images/cells/%s-%s.webp" % (stem, name)
        out = ""
        if os.path.exists(os.path.join(SITE, "content", image)):
            out += '<figure class="shot cell" markdown>\n![The %s chart](%s%s)\n</figure>\n\n' % (
                name, "../" * depth, image)
        return out + "```sql\n" + text + "\n```\n"

    return re.sub(r"<!-- cell (\w+)(?: in (\S+))? -->", cell, markdown)


def on_page_read_source(page, config):
    source = RENDERED.get(page.file.src_uri)
    if not source:
        return None
    with open(os.path.join(ROOT, source), encoding="utf-8") as f:
        text = f.read()
    note = (
        '!!! note "This page is [`%s`](%s%s)"\n'
        "    The format specification lives in the repository and is rendered here unchanged. "
        "The [guides](../guides/writing.md) explain it by task.\n\n" % (source, BLOB, source)
    )
    # Put the note under the page's first heading.
    first, _, rest = repository_links(text, source).partition("\n")
    return first + "\n\n" + note + rest


def on_post_build(config):
    fonts = os.path.join(config["site_dir"], "assets", "fonts")
    os.makedirs(fonts, exist_ok=True)
    for name in FONTS:
        shutil.copyfile(os.path.join(ROOT, "frontend", name), os.path.join(fonts, name))
    shutil.copyfile(os.path.join(ROOT, "third_party", "fonts", "LICENSE.md"), os.path.join(fonts, "LICENSE.txt"))
    # The handwriting captures live with the document that explains them.
    gifs = os.path.join(config["site_dir"], "images", "handwriting")
    os.makedirs(gifs, exist_ok=True)
    for name in ("revenue.gif", "charts.gif", "watch-open.gif", "watch-chart.gif"):
        shutil.copyfile(os.path.join(ROOT, "docs", "handwriting", name), os.path.join(gifs, name))
