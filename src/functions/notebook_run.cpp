#include "notebook/functions.hpp"

#include "duckdb/common/chrono.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "notebook/script.hpp"

// notebook_run(path) runs a notebook headlessly: every cell in file order, in a
// connection of its own. It is the seed of the runner the server will use.
//
// The session connection is created here and destroyed when the scan finishes. Never
// store a Connection (or anything else that owns the DatabaseInstance) in state that
// outlives a query: it would keep the database open after the user closes it, so the
// file stays locked and is not checkpointed. The server must hold a weak_ptr to the
// DatabaseInstance and make connections on demand.
namespace duckdb {

namespace {

struct RunBindData : public TableFunctionData {
	string path;
	notebook::Notebook notebook;
};

struct RunState : public GlobalTableFunctionState {
	unique_ptr<Connection> session;
	idx_t next = 0;
};

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                              vector<Identifier> &names) {
	auto result = make_uniq<RunBindData>();
	result->path = input.inputs[0].GetValue<string>();
	result->notebook = ReadNotebook(context, result->path);
	ThrowIfInvalid(result->notebook, result->path);

	auto column = [&](const char *name, LogicalType type) {
		names.emplace_back(name);
		return_types.push_back(std::move(type));
	};
	column("idx", LogicalType::INTEGER);
	column("kind", LogicalType::VARCHAR);
	column("name", LogicalType::VARCHAR);
	// 'ok', 'error', or 'skipped' for cells with nothing to execute (markdown, chart, metadata).
	column("status", LogicalType::VARCHAR);
	column("error", LogicalType::VARCHAR);
	// Rows a query cell returns. NULL for other cells.
	column("row_count", LogicalType::BIGINT);
	column("elapsed_ms", LogicalType::DOUBLE);
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
	auto state = make_uniq<RunState>();
	// A separate connection gives the notebook its own temp views and variables,
	// while sharing the caller's attached databases.
	state->session = make_uniq<Connection>(DatabaseInstance::GetDatabase(context));
	auto &notebook = input.bind_data->Cast<RunBindData>().notebook;
	for (auto &name : notebook::UndefinedVariables(notebook)) {
		auto result = state->session->Query("SET VARIABLE " + notebook::QuoteIdentifier(name) + " = NULL");
		if (result->HasError()) {
			result->ThrowError();
		}
	}
	return std::move(state);
}

void Scan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &cells = data.bind_data->Cast<RunBindData>().notebook.cells;
	auto &state = data.global_state->Cast<RunState>();
	if (state.next >= cells.size()) {
		// Release the session as soon as the last cell has been reported.
		state.session.reset();
		return;
	}
	// One cell per call, so a caller reading the result incrementally sees progress.
	auto &cell = cells[state.next++];
	auto sql = notebook::CellSql(cell);

	Value status("skipped"), error(LogicalType::VARCHAR), row_count(LogicalType::BIGINT);
	auto start = steady_clock::now();
	if (!sql.empty()) {
		// A cell's script can hold several statements; Query runs them all and returns the last result.
		auto result = state.session->Query(sql);
		bool failed = result->HasError();
		string message = failed ? result->GetError() : string();
		bool is_query = cell.kind == notebook::CellKind::SQL && cell.is_query;
		if (!failed && is_query && cell.name.empty()) {
			row_count = Value::BIGINT(NumericCast<int64_t>(result->RowCount()));
		}
		// A result keeps its connection busy until it is released.
		result.reset();
		if (!failed && is_query && !cell.name.empty()) {
			// Creating the view only binds the query; reading it is what proves it runs.
			auto count = state.session->Query("SELECT count(*) FROM " + notebook::QuoteIdentifier(cell.name));
			failed = count->HasError();
			if (failed) {
				message = count->GetError();
			} else if (auto chunk = count->Fetch()) {
				row_count = chunk->GetValue(0, 0);
			}
		}
		status = Value(failed ? "error" : "ok");
		if (failed) {
			error = Value(message);
		}
	}
	auto elapsed = std::chrono::duration<double, std::milli>(steady_clock::now() - start).count();

	output.data[0].Append(Value::INTEGER(cell.index));
	output.data[1].Append(Value(notebook::CellKindName(cell.kind)));
	output.data[2].Append(cell.name.empty() ? Value(LogicalType::VARCHAR) : Value(cell.name));
	output.data[3].Append(status);
	output.data[4].Append(error);
	output.data[5].Append(row_count);
	output.data[6].Append(sql.empty() ? Value(LogicalType::DOUBLE) : Value::DOUBLE(elapsed));
}

} // namespace

void RegisterNotebookRun(ExtensionLoader &loader) {
	loader.RegisterFunction(TableFunction("notebook_run", {LogicalType::VARCHAR}, Scan, Bind, Init));
}

} // namespace duckdb
