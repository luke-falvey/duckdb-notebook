#include "notebook/functions.hpp"

#include "duckdb/function/table_function.hpp"
#include "notebook/script.hpp"

namespace duckdb {

namespace {

struct SqlBindData : public TableFunctionData {
	string script;
};

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                              vector<Identifier> &names) {
	auto path = SourceArgument(input, 0, "notebook_sql");
	auto notebook = ReadNotebook(context, path);
	ThrowIfInvalid(notebook, path);

	auto result = make_uniq<SqlBindData>();
	result->script = notebook::ExpandNotebook(notebook, path);
	names.emplace_back("sql");
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(result);
}

void Scan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<OnceState>();
	if (state.done) {
		return;
	}
	output.data[0].Append(Value(data.bind_data->Cast<SqlBindData>().script));
	state.done = true;
}

} // namespace

void RegisterNotebookSql(ExtensionLoader &loader) {
	loader.RegisterFunction(TableFunction("notebook_sql", {LogicalType::VARCHAR}, Scan, Bind, OnceInit));
}

} // namespace duckdb
