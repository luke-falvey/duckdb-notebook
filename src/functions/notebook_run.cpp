#include "notebook/functions.hpp"

#include "duckdb/common/chrono.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/database.hpp"
#include "notebook/script.hpp"

#include <map>

// notebook_run(path, params := {...}) runs a notebook headlessly, in a connection of its
// own. Cells run in file order, except that a markdown cell waits for the cells its
// `{{ }}` expressions read (notebook::RunOrder). It is the seed of the runner the server
// will use.
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
	vector<int> order;
	//! Values from params := {...}, by variable name.
	std::map<string, Value> params;
};

struct RunState : public GlobalTableFunctionState {
	unique_ptr<Connection> session;
	//! Position in RunBindData::order.
	idx_t next = 0;
};

void SetVariable(Connection &session, const string &name, Value value) {
	ClientConfig::GetConfig(*session.context).SetUserVariable(name, std::move(value));
}

//! Evaluates a markdown cell's `{{ }}` expressions and returns its content with each one
//! replaced by its value. Sets `error` and returns an empty string if one fails.
string RenderMarkdown(Connection &session, const notebook::Cell &cell, string &error) {
	string rendered;
	size_t copied = 0;
	for (auto &expression : cell.expressions) {
		auto result = session.Query("SELECT (" + expression.sql + ")");
		if (result->HasError()) {
			error = "{{ " + expression.sql + " }}: " + result->GetError();
			return string();
		}
		auto chunk = result->Fetch();
		auto value = chunk && chunk->size() > 0 ? chunk->GetValue(0, 0) : Value();
		rendered += cell.content.substr(copied, expression.begin - copied);
		rendered += value.IsNull() ? "NULL" : value.ToString();
		copied = expression.end;
	}
	return rendered + cell.content.substr(copied);
}

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
	// Markdown cells: the content with each {{ expr }} replaced by its value.
	column("output", LogicalType::VARCHAR);
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
	auto state = make_uniq<RunState>();
	// A separate connection gives the notebook its own temp views and variables,
	// while sharing the caller's attached databases.
	state->session = make_uniq<Connection>(DatabaseInstance::GetDatabase(context));
	auto &bind_data = input.bind_data->Cast<RunBindData>();
	for (auto &name : notebook::UndefinedVariables(bind_data.notebook)) {
		auto param = bind_data.params.find(name);
		if (param != bind_data.params.end()) {
			SetVariable(*state->session, name, param->second);
			continue;
		}
		auto result = state->session->Query("SET VARIABLE " + notebook::QuoteIdentifier(name) + " = NULL");
		if (result->HasError()) {
			result->ThrowError();
		}
	}
	return std::move(state);
}

void Scan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<RunBindData>();
	auto &cells = bind_data.notebook.cells;
	auto &state = data.global_state->Cast<RunState>();
	if (state.next >= bind_data.order.size()) {
		// Release the session as soon as the last cell has been reported.
		state.session.reset();
		return;
	}
	// One cell per call, so a caller reading the result incrementally sees progress.
	auto &cell = cells[NumericCast<idx_t>(bind_data.order[state.next++])];
	auto sql = notebook::CellSql(cell);
	auto param = cell.kind == notebook::CellKind::INPUT ? bind_data.params.find(cell.name) : bind_data.params.end();
	bool is_markdown = cell.kind == notebook::CellKind::MARKDOWN;
	bool runs = !sql.empty() || (is_markdown && !cell.expressions.empty());

	Value status("skipped"), error(LogicalType::VARCHAR), row_count(LogicalType::BIGINT),
	    markdown(LogicalType::VARCHAR);
	auto start = steady_clock::now();
	if (param != bind_data.params.end()) {
		// A parameter replaces the input's default.
		SetVariable(*state.session, cell.name, param->second);
		status = Value("ok");
	} else if (is_markdown) {
		string message;
		auto rendered = RenderMarkdown(*state.session, cell, message);
		if (!message.empty()) {
			error = Value(message);
		} else {
			markdown = Value(rendered);
		}
		if (runs) {
			status = Value(message.empty() ? "ok" : "error");
		}
	} else if (!sql.empty()) {
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
	output.data[6].Append(runs ? Value::DOUBLE(elapsed) : Value(LogicalType::DOUBLE));
	output.data[7].Append(markdown);
}

} // namespace

void RegisterNotebookRun(ExtensionLoader &loader) {
	TableFunction function("notebook_run", {LogicalType::VARCHAR}, Scan, Bind, Init);
	// params := {name: value, ...} overrides input defaults and sets selection variables.
	function.GetSignature().WithTypedKwargs("options",
	                                        [](TypedKwargs &options) { options.Add("params", LogicalType::ANY); });
	loader.RegisterFunction(function);
}

} // namespace duckdb
