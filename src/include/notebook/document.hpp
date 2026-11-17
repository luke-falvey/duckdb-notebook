//===----------------------------------------------------------------------===//
// What an export makes from a notebook that has run: its title and its Markdown text.
// Plain C++17, no DuckDB headers.
//===----------------------------------------------------------------------===//

#pragma once

#include "notebook/format.hpp"
#include "notebook/json.hpp"

#include <string>

namespace notebook {

//! The notebook's title: its [notebook] cell's `title:`, its first `# heading`, or the file name of `source`
//! without its directory and extension.
std::string NotebookTitle(const Notebook &notebook, const std::string &source);

//! The Markdown export of a run's payload (BuildPayload): prose, code in fences, results and chart data as
//! tables of at most 20 rows, inputs as their values. Honours the payload's and each cell's hide=.
std::string MarkdownDocument(const Json &payload);

} // namespace notebook
