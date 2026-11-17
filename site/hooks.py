"""MkDocs hooks for the documentation site.

- Pages include files from anywhere in the repository (`--8<-- "examples/revenue.sql"`), so the
  notebooks shown in the docs are the files that test/sql/docs_notebooks.test checks.
- reference/format.md is docs/FORMAT.md, rendered here: the specification is written once.
- The fonts are copied from frontend/, where the extension gets them, into the built site.
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
