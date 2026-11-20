#include "notebook/functions.hpp"

#include "duckdb/function/table_function.hpp"

namespace duckdb {

namespace {

struct CellsBindData : public TableFunctionData {
	notebook::Notebook notebook;
};

struct CellsState : public GlobalTableFunctionState {
	idx_t next = 0;
};

Value StringList(const std::vector<std::string> &values) {
	vector<Value> list;
	for (auto &value : values) {
		list.emplace_back(value);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(list));
}

Value StringOrNull(const std::string &value) {
	return value.empty() ? Value(LogicalType::VARCHAR) : Value(value);
}

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                              vector<Identifier> &names) {
	auto result = make_uniq<CellsBindData>();
	result->notebook = ReadNotebook(context, SourceArgument(input, 0, "notebook_cells"));

	// Keep this list in step with Scan below and with the notebook_cells row of DESIGN §4.
	auto column = [&](const char *name, LogicalType type) {
		names.emplace_back(name);
		return_types.push_back(std::move(type));
	};
	column("idx", LogicalType::INTEGER);
	column("kind", LogicalType::VARCHAR);
	column("name", LogicalType::VARCHAR);
	column("widget", LogicalType::VARCHAR);
	column("options", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
	column("body", LogicalType::VARCHAR);
	column("content", LogicalType::VARCHAR);
	column("charts", LogicalType::LIST(LogicalType::VARCHAR));
	column("is_query", LogicalType::BOOLEAN);
	column("variables", LogicalType::LIST(LogicalType::VARCHAR));
	column("defines", LogicalType::LIST(LogicalType::VARCHAR));
	column("reads", LogicalType::LIST(LogicalType::VARCHAR));
	column("depends_on", LogicalType::LIST(LogicalType::INTEGER));
	column("start_line", LogicalType::INTEGER);
	column("end_line", LogicalType::INTEGER);
	column("errors", LogicalType::LIST(LogicalType::VARCHAR));
	column("warnings", LogicalType::LIST(LogicalType::VARCHAR));
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<CellsState>();
}

void Scan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &cells = data.bind_data->Cast<CellsBindData>().notebook.cells;
	auto &state = data.global_state->Cast<CellsState>();

	idx_t count = 0;
	for (; state.next < cells.size() && count < STANDARD_VECTOR_SIZE; state.next++, count++) {
		auto &cell = cells[state.next];
		bool is_sql = cell.kind == notebook::CellKind::SQL;
		bool has_content = !is_sql && cell.kind != notebook::CellKind::INPUT;

		vector<Value> keys, values;
		for (auto &option : cell.options) {
			keys.emplace_back(option.first);
			values.emplace_back(option.second);
		}

		idx_t col = 0;
		output.data[col++].Append(Value::INTEGER(cell.index));
		output.data[col++].Append(Value(notebook::CellKindName(cell.kind)));
		output.data[col++].Append(StringOrNull(cell.name));
		output.data[col++].Append(StringOrNull(cell.widget));
		output.data[col++].Append(
		    Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(keys), std::move(values)));
		output.data[col++].Append(Value(cell.body));
		output.data[col++].Append(has_content ? Value(cell.content) : Value(LogicalType::VARCHAR));
		output.data[col++].Append(StringList(cell.charts));
		output.data[col++].Append(is_sql ? Value::BOOLEAN(cell.is_query) : Value(LogicalType::BOOLEAN));
		output.data[col++].Append(StringList(cell.variables));
		output.data[col++].Append(StringList(cell.defines));
		output.data[col++].Append(StringList(cell.reads));
		vector<Value> depends_on;
		for (auto index : cell.depends_on) {
			depends_on.push_back(Value::INTEGER(index));
		}
		output.data[col++].Append(Value::LIST(LogicalType::INTEGER, std::move(depends_on)));
		output.data[col++].Append(Value::INTEGER(cell.start_line));
		output.data[col++].Append(Value::INTEGER(cell.end_line));
		output.data[col++].Append(StringList(cell.errors));
		output.data[col++].Append(StringList(cell.warnings));
	}
}

} // namespace

void RegisterNotebookCells(ExtensionLoader &loader) {
	loader.RegisterFunction(TableFunction("notebook_cells", {LogicalType::VARCHAR}, Scan, Bind, Init));
}

} // namespace duckdb
