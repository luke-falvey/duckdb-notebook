#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "notebook/format.hpp"
#include "notebook/graph.hpp"
#include "notebook/source.hpp"

// The SQL surface of the extension. One Register function per SQL function, each in
// its own file under src/functions. To add a function: declare it here, implement it
// in a new file, list the file in CMakeLists.txt, call it from sketchbook_extension.cpp,
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
//! notebook_export(path, file := ..., format := ...).
void RegisterNotebookExport(ExtensionLoader &loader);
//! notebook_start and notebook_stop: the HTTP server and browser UI.
void RegisterNotebookServer(ExtensionLoader &loader);
//! notebook_peek(sql, cursor): the CTE or sub-query under a cursor position, and the query that selects from it.
void RegisterNotebookPeek(ExtensionLoader &loader);

//! Global state of a table function that emits all its rows in one call: `done` once it has.
struct OnceState : public GlobalTableFunctionState {
	bool done = false;
};
unique_ptr<GlobalTableFunctionState> OnceInit(ClientContext &context, TableFunctionInitInput &input);

//! A named parameter as a string or bool; empty or false when absent or NULL.
string NamedString(TableFunctionBindInput &input, const char *key);
bool NamedBool(TableFunctionBindInput &input, const char *key);

//! Argument `index` as a string; throws "<function>: <what> must not be NULL" when it is NULL.
string StringArgument(TableFunctionBindInput &input, idx_t index, const char *function, const char *what);
//! A notebook source argument, checked at bind: not NULL or empty, and a well-formed `db:<name>[@<version>]`
//! (notebook/source.hpp) when it names a stored notebook. Every function taking a notebook path reads it with this.
string SourceArgument(TableFunctionBindInput &input, idx_t index, const char *function);
//! The caller's current database (the one `USE` chose); empty if there is none.
string CurrentDatabase(ClientContext &context);

//! The text of a notebook source: a file path, or `db:<name>` / `db:<name>@<version>` for a notebook
//! stored in the caller's current database. Throws if it cannot be read.
string ReadNotebookText(ClientContext &context, const string &source);
//! Same, with an explicit file system and database, for code that has no ClientContext. A stored notebook is
//! read from the database's default database.
string ReadNotebookText(DatabaseInstance &db, const string &source);

//! Writes (creating or replacing) a file through DuckDB's file system, atomically: into a uniquely named file
//! beside it, synced and closed, then moved over it. Where the file system cannot move files it is written in place.
void WriteTextFile(ClientContext &context, const string &path, const string &content);
void WriteTextFile(DatabaseInstance &db, const string &path, const string &content);

//! Parses and analyses notebook text with DuckDB's parser available through `context`.
notebook::Notebook AnalyzeText(ClientContext &context, const string &text);
//! Same, through a connection of its own, for code that has no ClientContext.
notebook::Notebook AnalyzeText(DatabaseInstance &db, const string &text);

//! Where notebooks are stored in a database: `_sketchbook`. Databases written before the extension was
//! renamed have them in `_notebook`; they are read from there until the first save copies them over.
constexpr const char *STORAGE_SCHEMA = "_sketchbook";
constexpr const char *LEGACY_STORAGE_SCHEMA = "_notebook";
//! Creates the `_sketchbook` schema, tables and view if they are missing, copying a `_notebook` history.
void EnsureStorage(Connection &connection, const string &database);
//! The schema to read stored notebooks from: `_sketchbook`, or `_notebook` in a database not saved to since.
string StorageSchema(Connection &connection, const string &database);
//! Stores `content` as the next version of notebook `name`, unless it equals the newest version.
//! Refuses (unless `force`) when it equals an older version. `origin` names the text in errors.
int32_t SaveNotebookVersion(Connection &connection, const string &database, const string &name, const string &content,
                            const string &message, bool force, bool &saved, string &note, const string &origin);
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

//! A part of a query that can be looked at on its own.
struct PeekTarget {
	//! `cte` or `subquery`; empty when there is nothing to peek at.
	string kind;
	//! The CTE's name or the sub-query's alias, if it has one.
	string name;
	//! The statement that selects from the part: empty when there is nothing to peek at.
	string sql;
	//! Byte range of the part in the text: a CTE from its name, a sub-query from its parenthesis.
	idx_t start = 0;
	idx_t end = 0;
	//! Why there is nothing to peek at.
	string reason;
};

//! The innermost CTE or sub-query around byte offset `cursor` of `sql`, from DuckDB's parser, and
//! the statement that selects from it with the CTEs it needs. Nothing is found (and `reason` says
//! why) in the main query, in SQL that does not parse, in a statement that is not a plain query,
//! and for a part that calls a function with side effects. Reads the catalog for that last check,
//! so it needs a transaction. Never throws for bad SQL.
PeekTarget FindPeekTarget(ClientContext &context, const string &sql, idx_t cursor);

//! Throws InvalidInputException naming the first format error, if the notebook has any.
void ThrowIfInvalid(const notebook::Notebook &notebook, const string &path);

} // namespace duckdb
