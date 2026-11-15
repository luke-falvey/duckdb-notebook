#include "notebook/session.hpp"

#include "duckdb/common/chrono.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/connection.hpp"
#include "notebook/chart.hpp"
#include "notebook/script.hpp"
#include "notebook/sql_lexer.hpp"

#include <cmath>
#include "duckdb/common/string_util.hpp"

namespace duckdb {

using notebook::Json;

Json ValueToJson(const Value &value) {
	if (value.IsNull()) {
		return Json::Null();
	}
	auto &type = value.type();
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return Json::Bool(value.GetValue<bool>());
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
		return Json::Number(value.GetValue<double>());
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT: {
		// Beyond 2^53 a double loses digits, so very large integers travel as text.
		auto as_double = value.GetValue<double>();
		if (std::fabs(as_double) < 9007199254740992.0) {
			return Json::Number(as_double);
		}
		return Json::String(value.ToString());
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY: {
		auto result = Json::Array();
		auto &children =
		    type.id() == LogicalTypeId::LIST ? ListValue::GetChildren(value) : ArrayValue::GetChildren(value);
		for (auto &child : children) {
			result.Push(ValueToJson(child));
		}
		return result;
	}
	case LogicalTypeId::STRUCT: {
		auto result = Json::Object();
		auto &children = StructValue::GetChildren(value);
		for (idx_t i = 0; i < children.size(); i++) {
			result.Set(StructType::GetChildName(type, i).GetIdentifierName(), ValueToJson(children[i]));
		}
		return result;
	}
	default:
		return Json::String(value.ToString());
	}
}

namespace {

Json Columns(QueryResult &result) {
	auto columns = Json::Array();
	for (idx_t i = 0; i < result.ColumnCount(); i++) {
		columns.Push(Json::Object()
		                 .Set("name", Json::String(result.ColumnName(i).GetIdentifierName()))
		                 .Set("type", Json::String(result.GetTypes()[i].ToString())));
	}
	return columns;
}

//! Up to `limit` rows of a result as JSON; reports in `total` how many rows there were.
Json ResultToJson(QueryResult &result, idx_t limit, int64_t &total) {
	auto rows = Json::Array();
	total = 0;
	while (auto chunk = result.Fetch()) {
		for (idx_t r = 0; r < chunk->size(); r++, total++) {
			if (NumericCast<idx_t>(total) >= limit) {
				continue;
			}
			auto row = Json::Array();
			for (idx_t c = 0; c < chunk->ColumnCount(); c++) {
				row.Push(ValueToJson(chunk->GetValue(c, r)));
			}
			rows.Push(std::move(row));
		}
	}
	return Json::Object()
	    .Set("columns", Columns(result))
	    .Set("rows", std::move(rows))
	    .Set("total", Json::Number(static_cast<double>(total)))
	    .Set("truncated", Json::Bool(NumericCast<idx_t>(total) > limit));
}

string ReplaySql(const notebook::Cell &cell) {
	if (cell.kind != notebook::CellKind::SQL) {
		return string();
	}
	auto sql = notebook::CellSql(cell);
	if (cell.is_query && !cell.name.empty()) {
		// The view (or temp table) the cell defines, plus anything else per-connection before it.
		string replay;
		auto statements = notebook::SplitStatements(cell.body);
		for (size_t i = 0; i + 1 < statements.size(); i++) {
			if (notebook::IsConnectionState(statements[i].text)) {
				replay += statements[i].text + ";\n";
			}
		}
		auto wrapped = notebook::SplitStatements(sql);
		return replay + (wrapped.empty() ? string() : wrapped.back().text + ";\n");
	}
	string replay;
	for (auto &statement : notebook::SplitStatements(cell.body)) {
		if (notebook::IsConnectionState(statement.text)) {
			replay += statement.text + ";\n";
		}
	}
	return replay;
}

} // namespace

Runner::Runner(DatabaseInstance &db, SessionState &state) : state(state) {
	connection = make_uniq<Connection>(db);
	auto &config = ClientConfig::GetConfig(*connection->context);
	for (auto &variable : state.variables) {
		config.SetUserVariable(variable.first, variable.second);
	}
	for (auto &replay : state.replay) {
		// Best effort: a replayed view whose source has since been dropped fails here and the
		// cell reports the problem when it is next run.
		auto result = connection->Query(replay.second);
		(void)result;
	}
}

Runner::~Runner() {
	try {
		Save();
	} catch (...) { // NOLINT: a destructor must not throw
	}
}

void Runner::Save() {
	state.variables.clear();
	auto &config = ClientConfig::GetConfig(*connection->context);
	for (auto &variable : config.user_variables) {
		state.variables[variable.first.GetIdentifierName()] = variable.second;
	}
}

void Runner::InitializeVariables(const notebook::Notebook &notebook, const std::map<string, Value> &params) {
	auto &config = ClientConfig::GetConfig(*connection->context);
	for (auto &name : notebook::UndefinedVariables(notebook)) {
		auto param = params.find(name);
		if (param != params.end()) {
			config.SetUserVariable(name, param->second);
		} else if (state.variables.find(name) == state.variables.end()) {
			config.SetUserVariable(name, Value());
		}
	}
}

string Runner::DisplaySql(const notebook::Cell &cell) {
	if (cell.kind != notebook::CellKind::SQL || !cell.is_query) {
		return string();
	}
	if (!cell.name.empty()) {
		return "SELECT * FROM " + notebook::QuoteIdentifier(cell.name);
	}
	auto statements = notebook::SplitStatements(cell.body);
	return statements.empty() ? string() : statements.back().text;
}

Json Runner::Page(const string &sql, idx_t limit, string &error) {
	auto result = connection->Query(sql);
	if (result->HasError()) {
		error = result->GetError();
		return Json();
	}
	int64_t total;
	return ResultToJson(*result, limit, total);
}

Json Runner::Chart(const notebook::Notebook &notebook, const notebook::Cell &cell) {
	auto from = cell.Option("from");
	notebook::ChartContext context;
	context.width = std::atoi(cell.Option("width", "640").c_str());
	context.height = std::atoi(cell.Option("height", "320").c_str());
	auto relation = notebook::RelationSql(from);
	{
		auto described = connection->Query("DESCRIBE SELECT * FROM " + relation);
		if (described->HasError()) {
			return Json::Object().Set("error", Json::String(described->GetError()));
		}
		while (auto chunk = described->Fetch()) {
			for (idx_t r = 0; r < chunk->size(); r++) {
				context.columns.push_back({chunk->GetValue(0, r).ToString(), chunk->GetValue(1, r).ToString()});
			}
		}
	}
	{
		auto counted = connection->Query("SELECT count(*) FROM " + relation);
		if (!counted->HasError()) {
			if (auto chunk = counted->Fetch()) {
				context.row_count = chunk->GetValue(0, 0).GetValue<int64_t>();
			}
		}
	}
	for (auto &other : notebook.cells) {
		if (other.kind == notebook::CellKind::CHART) {
			auto selection = notebook::ChartSelectionInfo(other.content);
			if (!selection.name.empty()) {
				context.selections[selection.name] = selection;
			}
		}
	}
	auto plan = notebook::CompileChart(cell.content, from, context);
	if (!plan.error.empty()) {
		return Json::Object().Set("error", Json::String(plan.error));
	}
	auto datasets = Json::Array();
	for (auto &sql : plan.queries) {
		string error;
		auto page = Page(sql, 100000, error);
		if (!error.empty()) {
			return Json::Object().Set("error", Json::String(error)).Set("sql", Json::String(sql));
		}
		datasets.Push(std::move(page));
	}
	auto queries = Json::Array();
	for (auto &sql : plan.queries) {
		queries.Push(Json::String(sql));
	}
	return Json::Object().Set("render", plan.render).Set("datasets", datasets).Set("queries", queries);
}

CellOutcome Runner::Run(const notebook::Notebook &notebook, const notebook::Cell &cell, const RunnerOptions &options,
                        Json *data) {
	CellOutcome outcome;
	auto start = steady_clock::now();
	auto finish = [&]() {
		outcome.elapsed_ms = std::chrono::duration<double, std::milli>(steady_clock::now() - start).count();
	};
	auto &config = ClientConfig::GetConfig(*connection->context);

	if (cell.kind == notebook::CellKind::INPUT) {
		auto param = options.params.find(cell.name);
		if (param != options.params.end()) {
			// A parameter replaces the input's default.
			config.SetUserVariable(cell.name, param->second);
			outcome.status = "ok";
			finish();
			return outcome;
		}
	}
	if (cell.kind == notebook::CellKind::MARKDOWN) {
		string rendered;
		size_t copied = 0;
		for (auto &expression : cell.expressions) {
			auto result = connection->Query("SELECT (" + expression.sql + ")");
			if (result->HasError()) {
				outcome.status = "error";
				outcome.error = "{{ " + expression.sql + " }}: " + result->GetError();
				finish();
				return outcome;
			}
			auto chunk = result->Fetch();
			auto value = chunk && chunk->size() > 0 ? chunk->GetValue(0, 0) : Value();
			rendered += cell.content.substr(copied, expression.begin - copied);
			rendered += value.IsNull() ? "NULL" : value.ToString();
			copied = expression.end;
		}
		outcome.output = rendered + cell.content.substr(copied);
		outcome.has_output = true;
		if (!cell.expressions.empty()) {
			outcome.status = "ok";
			finish();
		}
		return outcome;
	}
	if (cell.kind == notebook::CellKind::CHART) {
		if (options.charts) {
			auto chart = Chart(notebook, cell);
			if (auto error = chart.Get("error")) {
				outcome.status = "error";
				outcome.error = error->Str();
			} else {
				outcome.status = "ok";
			}
			if (data) {
				*data = std::move(chart);
			}
			finish();
		}
		return outcome;
	}

	auto sql = notebook::CellSql(cell);
	if (sql.empty()) {
		return outcome;
	}
	auto result = connection->Query(sql);
	bool failed = result->HasError();
	string message = failed ? result->GetError() : string();
	bool is_query = cell.kind == notebook::CellKind::SQL && cell.is_query;
	if (!failed && is_query && cell.name.empty()) {
		int64_t total;
		auto page = ResultToJson(*result, options.page_rows, total);
		outcome.row_count = total;
		if (data && options.page_rows > 0) {
			*data = std::move(page);
		}
	}
	// A result keeps its connection busy until it is released.
	result.reset();
	if (!failed && is_query && !cell.name.empty()) {
		// Creating the view only binds the query; reading it is what proves it runs.
		auto count = connection->Query("SELECT count(*) FROM " + notebook::QuoteIdentifier(cell.name));
		failed = count->HasError();
		if (failed) {
			message = count->GetError();
		} else if (auto chunk = count->Fetch()) {
			outcome.row_count = chunk->GetValue(0, 0).GetValue<int64_t>();
		}
		count.reset();
		if (!failed && data && options.page_rows > 0) {
			string error;
			*data = Page(DisplaySql(cell), options.page_rows, error);
		}
	}
	outcome.status = failed ? "error" : "ok";
	outcome.error = message;
	if (!failed) {
		auto replay = ReplaySql(cell);
		if (replay.empty()) {
			state.replay.erase(cell.index);
		} else {
			state.replay[cell.index] = replay;
		}
	}
	finish();
	return outcome;
}

static string ColumnType(Connection &connection, const string &relation, const string &column) {
	auto described = connection.Query("DESCRIBE SELECT * FROM " + relation);
	if (described->HasError()) {
		return string();
	}
	while (auto chunk = described->Fetch()) {
		for (idx_t r = 0; r < chunk->size(); r++) {
			if (StringUtil::CIEquals(chunk->GetValue(0, r).ToString(), column)) {
				return chunk->GetValue(1, r).ToString();
			}
		}
	}
	return string();
}

string VariableSqlType(Connection &connection, const notebook::Notebook &notebook, const notebook::Cell &cell,
                       const string &name) {
	if (cell.kind == notebook::CellKind::INPUT) {
		auto &w = cell.widget;
		if (w == "text") {
			return "VARCHAR";
		}
		if (w == "number" || w == "slider") {
			return "DOUBLE";
		}
		if (w == "checkbox") {
			return "BOOLEAN";
		}
		if (w == "date" || w == "daterange") {
			return "DATE";
		}
		auto from = cell.Option("from");
		auto dot = from.rfind('.');
		if (dot != string::npos) {
			auto type = ColumnType(connection, notebook::RelationSql(from.substr(0, dot)), from.substr(dot + 1));
			return type.empty() ? type : (w == "multiselect" ? type + "[]" : type);
		}
		return string();
	}
	if (cell.kind == notebook::CellKind::CHART) {
		auto selection = notebook::ChartSelectionInfo(cell.content);
		if (selection.name != name) {
			return string();
		}
		auto relation = notebook::RelationSql(cell.Option("from"));
		vector<string> types;
		for (auto &field : selection.fields) {
			auto type = ColumnType(connection, relation, field);
			// A brush on a date axis lands between days; compare as timestamps.
			types.push_back(type == "DATE" ? "TIMESTAMP" : type);
		}
		if (selection.type == "point") {
			return types.empty() || types[0].empty() ? string() : types[0] + "[]";
		}
		if (types.size() >= 2) {
			string type = "STRUCT(";
			for (idx_t i = 0; i < types.size(); i++) {
				auto inner = types[i].empty() ? "VARCHAR" : types[i];
				type += (i ? ", " : "") + selection.channels[i] + " STRUCT(lo " + inner + ", hi " + inner + ")";
			}
			return type + ")";
		}
		if (types.empty() || types[0].empty()) {
			return string();
		}
		return "STRUCT(lo " + types[0] + ", hi " + types[0] + ")";
	}
	return string();
}

} // namespace duckdb
