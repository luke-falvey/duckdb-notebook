#include "notebook/functions.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "notebook/chart.hpp"
#include "notebook/session.hpp"

// notebook_chart_query(spec, from): the SQL a chart spec compiles to, and its render description,
// for inspection. The columns of `from` are looked up on a separate connection, so a temporary view
// of the caller's own session cannot be described; the spec is then compiled without column types.
namespace duckdb {

namespace {

struct ChartBindData : public TableFunctionData {
	notebook::ChartPlan plan;
};

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                              vector<Identifier> &names) {
	auto spec = StringArgument(input, 0, "notebook_chart_query", "the chart spec");
	auto from = StringArgument(input, 1, "notebook_chart_query", "the relation to chart (from)");
	notebook::ChartContext chart;
	for (auto &entry : input.named_parameters) {
		auto key = entry.first.GetIdentifierName();
		if (key == "width") {
			chart.width = entry.second.GetValue<int32_t>();
		} else if (key == "height") {
			chart.height = entry.second.GetValue<int32_t>();
		} else if (key == "row_count") {
			chart.row_count = entry.second.GetValue<int64_t>();
		}
	}
	{
		Connection connection(DatabaseInstance::GetDatabase(context));
		string error; // without columns, every scalar channel value is taken as a field
		DescribeColumns(connection, notebook::RelationSql(from), chart.columns, error);
	}
	auto result = make_uniq<ChartBindData>();
	result->plan = notebook::CompileChart(spec, from, chart);
	if (!result->plan.error.empty()) {
		throw InvalidInputException("notebook_chart_query: %s", result->plan.error);
	}
	names.emplace_back("queries");
	return_types.push_back(LogicalType::LIST(LogicalType::VARCHAR));
	names.emplace_back("render");
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(result);
}

void Scan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<OnceState>();
	if (state.done) {
		return;
	}
	auto &plan = data.bind_data->Cast<ChartBindData>().plan;
	vector<Value> queries;
	for (auto &sql : plan.queries) {
		queries.emplace_back(sql);
	}
	output.data[0].Append(Value::LIST(LogicalType::VARCHAR, std::move(queries)));
	output.data[1].Append(Value(plan.render.Dump()));
	state.done = true;
}

} // namespace

void RegisterNotebookChartQuery(ExtensionLoader &loader) {
	TableFunction function("notebook_chart_query", {LogicalType::VARCHAR, LogicalType::VARCHAR}, Scan, Bind, OnceInit);
	function.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("width", LogicalType::INTEGER)
		    .Add("height", LogicalType::INTEGER)
		    .Add("row_count", LogicalType::BIGINT);
	});
	loader.RegisterFunction(function);
}

} // namespace duckdb
