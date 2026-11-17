#include "notebook/functions.hpp"

#include "duckdb/common/chrono.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/database.hpp"
#include "notebook/result_cache.hpp"
#include "notebook/script.hpp"
#include "notebook/session.hpp"

#include <map>

// notebook_run(path, params := {...}) runs a notebook headlessly, in a connection of its
// own. Cells run in file order, except that a markdown cell waits for the cells its
// `{{ }}` expressions read (notebook::RunOrder). Cells run through Runner (session.cpp), as in
// the server and the exports.
//
// A run never reads the result cache: it exists to compute from the data as it is now. With
// cache := true it writes it, so the next person to open the notebook sees these results at once.
//
// The Runner's connection is created here and destroyed when the scan finishes. Never
// store a Connection (or anything else that owns the DatabaseInstance) in state that
// outlives a query: it would keep the database open after the user closes it, so the
// file stays locked and is not checkpointed.
namespace duckdb {

namespace {

struct RunBindData : public TableFunctionData {
	string path;
	notebook::Notebook notebook;
	vector<int> order;
	//! Values from params := {...}, by variable name.
	std::map<string, Value> params;
	//! cache := true: keep each cell's result as the editor would (FORMAT §8).
	bool cache = false;
};

struct RunState : public GlobalTableFunctionState {
	SessionState session;
	unique_ptr<Runner> runner;
	//! Position in RunBindData::order.
	idx_t next = 0;
	//! With cache := true: the notebook's cache and what this run adds to it.
	ResultCache cache;
	vector<RanCell> ran;
};

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                              vector<Identifier> &names) {
	auto result = make_uniq<RunBindData>();
	result->path = input.inputs[0].GetValue<string>();
	result->notebook = ReadNotebook(context, result->path);
	ThrowIfInvalid(result->notebook, result->path);
	result->order = notebook::RunOrder(result->notebook);

	auto params = input.named_parameters.find("params");
	if (params != input.named_parameters.end() && !params->second.IsNull()) {
		auto &value = params->second;
		if (value.type().id() != LogicalTypeId::STRUCT) {
			throw InvalidInputException("notebook_run: params must be a struct, such as {region: 'US'}, not %s",
			                            value.type().ToString());
		}
		auto &children = StructValue::GetChildren(value);
		for (idx_t i = 0; i < children.size(); i++) {
			auto name = StructType::GetChildName(value.type(), i).GetIdentifierName();
			auto problem = notebook::ParameterError(result->notebook, name);
			if (!problem.empty()) {
				throw InvalidInputException("notebook_run: %s", problem);
			}
			result->params[name] = children[i];
		}
	}

	result->cache = NamedBool(input, "cache");

	auto column = [&](const char *name, LogicalType type) {
		names.emplace_back(name);
		return_types.push_back(std::move(type));
	};
	column("idx", LogicalType::INTEGER);
	column("kind", LogicalType::VARCHAR);
	column("name", LogicalType::VARCHAR);
	// 'ok', 'error', or 'skipped' for cells with nothing to execute (metadata, markdown without {{ }}).
	column("status", LogicalType::VARCHAR);
	column("error", LogicalType::VARCHAR);
	// Rows a query cell returns, or rows of a chart's (first) data query. NULL for other cells.
	column("row_count", LogicalType::BIGINT);
	column("elapsed_ms", LogicalType::DOUBLE);
	// Markdown cells: the content with each {{ expr }} replaced by its value.
	column("output", LogicalType::VARCHAR);
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
	auto state = make_uniq<RunState>();
	// A separate connection gives the notebook its own temp views and variables, while sharing
	// the caller's attached databases.
	state->runner = make_uniq<Runner>(DatabaseInstance::GetDatabase(context), state->session);
	auto &bind_data = input.bind_data->Cast<RunBindData>();
	state->runner->Prepare(bind_data.notebook, bind_data.path, bind_data.params);
	if (bind_data.cache) {
		state->cache.Load(DatabaseInstance::GetDatabase(context), bind_data.path, bind_data.notebook);
	}
	return std::move(state);
}

void Scan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<RunBindData>();
	auto &cells = bind_data.notebook.cells;
	auto &state = data.global_state->Cast<RunState>();
	if (state.next >= bind_data.order.size()) {
		if (state.runner && bind_data.cache) {
			state.runner->Save();
			state.cache.Record(DatabaseInstance::GetDatabase(context), bind_data.path, bind_data.notebook,
			                   state.session, state.ran);
		}
		// Release the connection as soon as the last cell has been reported.
		state.runner.reset();
		return;
	}
	// One cell per call, so a caller reading the result incrementally sees progress.
	auto &cell = cells[NumericCast<idx_t>(bind_data.order[state.next++])];
	RunnerOptions options;
	options.params = bind_data.params;
	// Chart cells compile and query their data, so a broken spec fails the run.
	options.charts = true;
	CellOutcome outcome;
	if (bind_data.cache) {
		// The result the editor would be sent: a page of rows, chart data, marked {{ }} values.
		options.page_rows = 100;
		options.mark_values = true;
		auto result = notebook::Json::Object();
		outcome = RunForEditor(*state.runner, bind_data.notebook, cell, options, result);
		outcome.output = StringUtil::Replace(StringUtil::Replace(outcome.output, VALUE_OPEN, ""), VALUE_CLOSE, "");
		RanCell done;
		done.index = cell.index;
		done.status = outcome.status;
		if (auto value = result.Get("value")) {
			done.value = *value;
		}
		done.result = result.Dump();
		state.ran.push_back(std::move(done));
	} else {
		outcome = state.runner->Run(bind_data.notebook, cell, options);
	}

	output.data[0].Append(Value::INTEGER(cell.index));
	output.data[1].Append(Value(notebook::CellKindName(cell.kind)));
	output.data[2].Append(cell.name.empty() ? Value(LogicalType::VARCHAR) : Value(cell.name));
	output.data[3].Append(Value(outcome.status));
	output.data[4].Append(outcome.status == "error" ? Value(outcome.error) : Value(LogicalType::VARCHAR));
	output.data[5].Append(outcome.row_count >= 0 ? Value::BIGINT(outcome.row_count) : Value(LogicalType::BIGINT));
	output.data[6].Append(outcome.elapsed_ms >= 0 ? Value::DOUBLE(outcome.elapsed_ms) : Value(LogicalType::DOUBLE));
	output.data[7].Append(outcome.has_output ? Value(outcome.output) : Value(LogicalType::VARCHAR));
}

} // namespace

void RegisterNotebookRun(ExtensionLoader &loader) {
	TableFunction function("notebook_run", {LogicalType::VARCHAR}, Scan, Bind, Init);
	// params := {name: value, ...} overrides input defaults and sets selection variables.
	// cache := true keeps the results for the editor to show when the notebook is next opened.
	function.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("params", LogicalType::ANY).Add("cache", LogicalType::BOOLEAN);
	});
	loader.RegisterFunction(function);
}

} // namespace duckdb
