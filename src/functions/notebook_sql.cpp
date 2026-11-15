#include "notebook/functions.hpp"

#include "duckdb/function/table_function.hpp"
#include "notebook/script.hpp"

namespace duckdb {

namespace {

struct SqlBindData : public TableFunctionData {
	string script;
};

struct SqlState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                              vector<Identifier> &names) {
	auto path = input.inputs[0].GetValue<string>();
	auto notebook = ReadNotebook(context, path);
	ThrowIfInvalid(notebook, path);

	auto result = make_uniq<SqlBindData>();
	result->script = notebook::ExpandNotebook(notebook, path);
	names.emplace_back("sql");
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<SqlState>();
}

void Scan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<SqlState>();
	if (state.done) {
		return;
	}
	output.data[0].Append(Value(data.bind_data->Cast<SqlBindData>().script));
	state.done = true;
}

} // namespace

void RegisterNotebookSql(ExtensionLoader &loader) {
	loader.RegisterFunction(TableFunction("notebook_sql", {LogicalType::VARCHAR}, Scan, Bind, Init));
}

} // namespace duckdb
