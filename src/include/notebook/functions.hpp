#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "notebook/format.hpp"

// The SQL surface of the extension. One Register function per SQL function, each in
// its own file under src/functions. To add a function: declare it here, implement it
// in a new file, list the file in CMakeLists.txt, call it from notebook_extension.cpp,
// and add a test under test/sql.
namespace duckdb {

//! notebook_cells(path): one row per cell.
void RegisterNotebookCells(ExtensionLoader &loader);
//! notebook_sql(path): the notebook as a plain SQL script, in a single row.
void RegisterNotebookSql(ExtensionLoader &loader);
//! notebook_run(path): runs every cell in a fresh connection; one row per cell with its outcome.
void RegisterNotebookRun(ExtensionLoader &loader);

//! Reads and parses a notebook through DuckDB's file system, so local paths and, with
//! httpfs loaded, URLs and object stores all work. Throws IOException if unreadable.
notebook::Notebook ReadNotebook(ClientContext &context, const string &path);

//! Throws InvalidInputException naming the first format error, if the notebook has any.
void ThrowIfInvalid(const notebook::Notebook &notebook, const string &path);

} // namespace duckdb
