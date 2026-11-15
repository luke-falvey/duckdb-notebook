#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "notebook/format.hpp"
#include "notebook/graph.hpp"

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
//! notebook_chart_query(spec, from): the SQL and render description a chart spec compiles to.
void RegisterNotebookChartQuery(ExtensionLoader &loader);
//! notebook_save and notebook_checkout: notebooks stored in the database.
void RegisterNotebookStorage(ExtensionLoader &loader);
//! notebook_export(path, to := ..., format := ...).
void RegisterNotebookExport(ExtensionLoader &loader);
//! notebook_start and notebook_stop: the HTTP server and browser UI.
void RegisterNotebookServer(ExtensionLoader &loader);

//! The text of a notebook source: a file path, or `db:<name>` / `db:<name>@<version>` for a notebook
//! stored in the database. Throws if it cannot be read.
string ReadNotebookText(ClientContext &context, const string &source);
//! Same, with an explicit file system and database, for code that has no ClientContext.
string ReadNotebookText(DatabaseInstance &db, const string &source);

//! Writes (creating or replacing) a file through DuckDB's file system.
void WriteTextFile(ClientContext &context, const string &path, const string &content);
void WriteTextFile(DatabaseInstance &db, const string &path, const string &content);

//! Parses and analyses notebook text with DuckDB's parser available through `context`.
notebook::Notebook AnalyzeText(ClientContext &context, const string &text);

//! Creates the `_notebook` schema, tables and view if they are missing.
void EnsureStorage(Connection &connection, const string &database);
//! The content of a stored notebook; `version` 0 means the newest. Sets `found_version`.
string StoredNotebook(Connection &connection, const string &database, const string &name, int32_t version,
                      int32_t &found_version);

//! Reads, parses and analyses a notebook (AnalyzeNotebook, with ExtractReferences) through
//! DuckDB's file system, so local paths and, with httpfs loaded, URLs and object stores all
//! work. Throws IOException if unreadable.
notebook::Notebook ReadNotebook(ClientContext &context, const string &path);

//! What SQL defines and reads, from DuckDB's parser. Does not bind, so nothing it names
//! needs to exist. Never throws: a parse error is returned in References::error.
notebook::References ExtractReferences(ClientContext &context, const string &sql);

//! Throws InvalidInputException naming the first format error, if the notebook has any.
void ThrowIfInvalid(const notebook::Notebook &notebook, const string &path);

} // namespace duckdb
