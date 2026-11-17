#pragma once

#include "duckdb.hpp"
#include "notebook/chart.hpp"
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
	//! Query cells with charts, when charts are requested: one Runner::Chart result per chart.
	notebook::Json charts;
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

	//! Before the first cell of a run: ApplyMetadata, then InitializeVariables.
	void Prepare(const notebook::Notebook &notebook, const string &source, const std::map<string, Value> &params);

	//! Runs every cell in run order (notebook::RunOrder), keeping no outcomes.
	void RunAll(const notebook::Notebook &notebook, const RunnerOptions &options);

	//! Runs one cell. Fills `data` with the result page (query cells) or chart data (chart cells)
	//! as requested in the options.
	CellOutcome Run(const notebook::Notebook &notebook, const notebook::Cell &cell, const RunnerOptions &options,
	                notebook::Json *data = nullptr);

	//! `{columns: [{name, type}], rows: [[...]], truncated}` for a query, at most `limit` rows.
	notebook::Json Page(const string &sql, idx_t limit, string &error);

	//! `{render, datasets: [...], queries: [...]}` for one of a cell's charts, or `{error}`.
	notebook::Json Chart(const notebook::Notebook &notebook, const notebook::Cell &cell, const string &spec);

	//! Every chart of a cell, in order. Sets `error` to the first chart's error, if any.
	notebook::Json Charts(const notebook::Notebook &notebook, const notebook::Cell &cell, string &error);

	//! Peek under the pen: the first `rows` rows of the CTE or sub-query around byte offset `cursor`
	//! of `sql`, as `{kind, name, start, end, columns, rows, more, total}` (`total` only when counting
	//! was cheap), or `{reason}` when there is nothing to show. Read-only: it runs one SELECT in a
	//! read-only transaction with a time limit, and creates, replaces and records nothing.
	notebook::Json Peek(const string &sql, idx_t cursor, idx_t rows, idx_t timeout_ms);

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

//! The values a select input offers: its options= list or the distinct values of its from= column.
vector<Value> InputChoices(Connection &connection, const notebook::Cell &cell, idx_t limit);

//! The words SQL highlighting colours, from this DuckDB: `{keywords, types}`. Keywords are the
//! reserved, column-name and type/function ones; unreserved keywords are also common column names.
notebook::Json SqlWords(Connection &connection);

//! A cell's parse and graph results as JSON, as the browser sees them.
notebook::Json CellJson(const notebook::Cell &cell);

//! An embedded frontend file (frontend/ in the repository), compiled into the extension.
const string &GetAsset(const string &name);

//! Runs one cell as the editor does and builds the result the editor is sent: status, error, row
//! count, timing, rendered markdown, result page, charts, and an input's value and choices.
CellOutcome RunForEditor(Runner &runner, const notebook::Notebook &notebook, const notebook::Cell &cell,
                         const RunnerOptions &options, notebook::Json &result);

//! The columns of `relation` (SQL text, as RelationSql gives) with their DuckDB types. On failure returns
//! false and sets `error`; `columns` is left empty.
bool DescribeColumns(Connection &connection, const string &relation, std::vector<notebook::ColumnInfo> &columns,
                     string &error);

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
