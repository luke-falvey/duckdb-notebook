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
	//! Rows of each query cell's result to keep in CellResult::table, 0 for none.
	idx_t page_rows = 0;
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

//! A Value as JSON: numbers stay numbers (unless too large for a double), temporal values are
//! ISO-like strings, lists and structs nest.
notebook::Json ValueToJson(const Value &value);

} // namespace duckdb
