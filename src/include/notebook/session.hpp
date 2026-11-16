#pragma once

#include "duckdb.hpp"
#include "notebook/format.hpp"
#include "notebook/json.hpp"

#include <map>

// Running cells, shared by notebook_run, notebook_export and the server.
//
// A Session holds no Connection. Its state is the variables it has set and the statements that
// rebuild its per-connection objects (temp views of query cells, temp tables, settings). A Runner
// opens a connection, replays that state, runs cells, and records the new state when it goes
// away. The extension therefore never keeps the database alive between requests (DESIGN §11).
namespace duckdb {

struct SessionState {
	//! User variables, as they were when the last runner finished.
	std::map<string, Value> variables;
	//! Per cell index: the SQL that recreates what that cell made which a new connection lacks.
	std::map<int, string> replay;
};

struct CellOutcome {
	//! `ok`, `error` or `skipped`.
	string status = "skipped";
	string error;
	//! Rows a query cell returns, or -1.
	int64_t row_count = -1;
	double elapsed_ms = -1;
	//! Markdown cells: the rendered text. `has_output` tells an empty render from none.
	string output;
	bool has_output = false;
};

struct RunnerOptions {
	//! Rows of each query cell's result to keep, 0 for none. A cell's rows= option replaces it, or
	//! with rows_at_least (exports, which page on the client) raises it.
	idx_t page_rows = 0;
	bool rows_at_least = false;
	//! Wrap each markdown {{ }} value in U+E000 ... U+E001 so a renderer can highlight it.
	bool mark_values = false;
	//! Compile and fetch chart data.
	bool charts = false;
	//! Parameters by variable name (notebook_run / export params).
	std::map<string, Value> params;
};

class Runner {
public:
	Runner(DatabaseInstance &db, SessionState &state);
	~Runner();

	Connection &Conn() {
		return *connection;
	}

	//! Applies the [notebook] cell: loads its `extensions` and attaches its `attach` databases
	//! (paths relative to the notebook file). Throws if one fails.
	void ApplyMetadata(const notebook::Notebook &notebook, const string &source);

	//! Sets variables that cells read but nothing sets to NULL, or to their param.
	void InitializeVariables(const notebook::Notebook &notebook, const std::map<string, Value> &params);

	//! Runs one cell. Fills `data` with the result page (query cells) or chart data (chart cells)
	//! as requested in the options.
	CellOutcome Run(const notebook::Notebook &notebook, const notebook::Cell &cell, const RunnerOptions &options,
	                notebook::Json *data = nullptr);

	//! `{columns: [{name, type}], rows: [[...]], truncated}` for a query, at most `limit` rows.
	notebook::Json Page(const string &sql, idx_t limit, string &error);

	//! `{render, datasets: [...]} ` for a chart cell, or `{error}`.
	notebook::Json Chart(const notebook::Notebook &notebook, const notebook::Cell &cell);

	//! The SQL whose rows a query cell displays, or empty for other cells.
	static string DisplaySql(const notebook::Cell &cell);

	//! Records the connection's variables into the state (also done by the destructor).
	void Save();

private:
	SessionState &state;
	unique_ptr<Connection> connection;
};

struct ExportOptions {
	//! html, html-wasm, md, sql, data or pdf.
	string format = "html";
	std::map<string, Value> params;
	//! `precomputed`: also run once per input value so the static page can switch between them.
	string interactive;
	//! Most notebook runs `interactive := 'precomputed'` may make.
	idx_t budget = 16;
	//! Result rows kept per query cell.
	idx_t page_rows = 100;
	//! html-wasm: directory holding the DuckDB-WASM runtime built by wasm/build.sh.
	string wasm_runtime;
	//! pdf: browser executable.
	string chrome;
	//! data: parquet or csv.
	string data_format = "parquet";
	//! What to hide in every cell, on top of each cell's own hide= option: code, output, all, or
	//! empty for nothing.
	string hide;
};

//! Runs a notebook and returns everything a reader needs to see it: cells, rendered markdown,
//! result pages, chart render descriptions and data, input values.
notebook::Json BuildPayload(DatabaseInstance &db, const string &text, const string &source,
                            const ExportOptions &options);

//! The export as a document (html, html-wasm, md, sql). `note` reports frozen inputs and the like.
string ExportDocument(DatabaseInstance &db, const string &text, const string &source, const ExportOptions &options,
                      string &note);

//! The notebook's title: its [notebook] cell's `title:`, its first `# heading`, or the file name.
string NotebookTitle(const notebook::Notebook &notebook, const string &source);

//! The values a select input offers: its options= list or the distinct values of its from= column.
vector<Value> InputChoices(Connection &connection, const notebook::Cell &cell, idx_t limit);

//! A cell's parse and graph results as JSON, as the browser sees them.
notebook::Json CellJson(const notebook::Cell &cell);

//! An embedded frontend file (frontend/ in the repository), compiled into the extension.
const string &GetAsset(const string &name);

//! The SQL type a variable set from the browser should have, so it compares like the column it
//! came from: from an input's widget or from= column, or a chart selection's fields. Empty when
//! unknown (the literal's own type is used).
string VariableSqlType(Connection &connection, const notebook::Notebook &notebook, const notebook::Cell &cell,
                       const string &name);

//! Markers around rendered {{ }} values when RunnerOptions::mark_values is set.
extern const char *const VALUE_OPEN;
extern const char *const VALUE_CLOSE;

//! The page size a cell asks for with rows=, or 0.
idx_t CellRows(const notebook::Cell &cell);

//! A Value as JSON: numbers stay numbers (unless too large for a double), temporal values are
//! ISO-like strings, lists and structs nest.
notebook::Json ValueToJson(const Value &value);

} // namespace duckdb
