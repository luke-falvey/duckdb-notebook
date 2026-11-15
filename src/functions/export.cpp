#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "miniz_wrapper.hpp"
#include "notebook/chart.hpp"
#include "notebook/functions.hpp"
#include "notebook/graph.hpp"
#include "notebook/script.hpp"
#include "notebook/session.hpp"
#include "notebook/sql_lexer.hpp"
#include "notebook/yaml.hpp"

#include <cstdlib>
#include <random>
#include <set>
#include <unistd.h>

// notebook_export (docs/DESIGN.md section 7). Every format starts from one run of the notebook in
// a session of its own; BuildPayload collects what that run shows, and each format presents it.
namespace duckdb {

using notebook::Json;

namespace {

string Gzip(const string &data) {
	MiniZStream stream;
	size_t size = MiniZStream::MaxCompressedLength(data.size());
	string out(size, '\0');
	stream.Compress(data.data(), data.size(), &out[0], &size);
	out.resize(size);
	return out;
}

string Base64(const string &data) {
	return Blob::ToBase64(string_t(data.data(), NumericCast<uint32_t>(data.size())));
}

string ReplaceAll(string text, const string &from, const string &to) {
	size_t pos = 0;
	while ((pos = text.find(from, pos)) != string::npos) {
		text.replace(pos, from.size(), to);
		pos += to.size();
	}
	return text;
}

//! JavaScript or JSON that sits inside a <script> element.
string ScriptSafe(const string &code) {
	return ReplaceAll(code, "</script", "<\\/script");
}

string HtmlEscape(const string &text) {
	string out;
	for (char c : text) {
		switch (c) {
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		case '&':
			out += "&amp;";
			break;
		case '"':
			out += "&quot;";
			break;
		default:
			out += c;
		}
	}
	return out;
}

string FileStem(const string &path) {
	auto name = path.substr(path.find_last_of("/\\:") + 1);
	auto dot = name.rfind('.');
	return dot == string::npos || dot == 0 ? name : name.substr(0, dot);
}

vector<Value> QueryValues(Connection &connection, const string &sql) {
	vector<Value> values;
	auto result = connection.Query(sql);
	if (result->HasError()) {
		return values;
	}
	while (auto chunk = result->Fetch()) {
		for (idx_t r = 0; r < chunk->size(); r++) {
			values.push_back(chunk->GetValue(0, r));
		}
	}
	return values;
}

Json OptionsJson(const notebook::Cell &cell) {
	auto options = Json::Object();
	for (auto &option : cell.options) {
		options.Set(option.first, Json::String(option.second));
	}
	return options;
}

Json StringArray(const std::vector<std::string> &values) {
	auto array = Json::Array();
	for (auto &value : values) {
		array.Push(Json::String(value));
	}
	return array;
}

} // namespace

string NotebookTitle(const notebook::Notebook &notebook, const string &source) {
	for (auto &cell : notebook.cells) {
		if (cell.kind == notebook::CellKind::NOTEBOOK) {
			string error;
			auto meta = notebook::ParseYaml(cell.content, error);
			auto title = meta.Str("title");
			if (!title.empty()) {
				return title;
			}
		}
	}
	for (auto &cell : notebook.cells) {
		if (cell.kind == notebook::CellKind::MARKDOWN && cell.content.compare(0, 2, "# ") == 0) {
			return cell.content.substr(2, cell.content.find('\n') - 2);
		}
	}
	return FileStem(source);
}

//! Values an input offers: its options= list, or the distinct values of its from= column.
vector<Value> InputChoices(Connection &connection, const notebook::Cell &cell, idx_t limit) {
	vector<Value> values;
	auto options = cell.Option("options");
	if (!options.empty()) {
		size_t start = 0;
		while (start <= options.size()) {
			auto comma = options.find(',', start);
			if (comma == string::npos) {
				comma = options.size();
			}
			values.emplace_back(options.substr(start, comma - start));
			start = comma + 1;
		}
		return values;
	}
	auto from = cell.Option("from");
	auto dot = from.rfind('.');
	if (dot == string::npos) {
		return values;
	}
	auto column = notebook::QuoteIdentifier(from.substr(dot + 1));
	return QueryValues(connection, "SELECT DISTINCT " + column + " FROM " + notebook::RelationSql(from.substr(0, dot)) +
	                                   " WHERE " + column + " IS NOT NULL ORDER BY 1 LIMIT " + std::to_string(limit));
}

Json CellJson(const notebook::Cell &cell) {
	auto json = Json::Object();
	json.Set("idx", Json::Number(cell.index));
	json.Set("kind", Json::String(notebook::CellKindName(cell.kind)));
	json.Set("name", cell.name.empty() ? Json() : Json::String(cell.name));
	json.Set("head", Json::String(cell.head));
	json.Set("widget", cell.widget.empty() ? Json() : Json::String(cell.widget));
	json.Set("options", OptionsJson(cell));
	json.Set("body", Json::String(cell.body));
	json.Set("content", Json::String(cell.content));
	json.Set("is_query", Json::Bool(cell.is_query));
	json.Set("defines", StringArray(cell.defines));
	json.Set("reads", StringArray(cell.reads));
	auto depends = Json::Array();
	for (auto index : cell.depends_on) {
		depends.Push(Json::Number(index));
	}
	json.Set("depends_on", depends);
	json.Set("start_line", Json::Number(cell.start_line));
	json.Set("end_line", Json::Number(cell.end_line));
	json.Set("errors", StringArray(cell.errors));
	json.Set("warnings", StringArray(cell.warnings));
	return json;
}

Json BuildPayload(DatabaseInstance &db, const string &text, const string &source, const ExportOptions &options) {
	SessionState state;
	Runner runner(db, state);
	auto notebook = AnalyzeText(*runner.Conn().context, text);
	ThrowIfInvalid(notebook, source);
	runner.InitializeVariables(notebook, options.params);
	RunnerOptions run;
	run.page_rows = options.page_rows;
	run.charts = true;
	run.params = options.params;

	vector<Json> cells(notebook.cells.size());
	for (auto index : notebook::RunOrder(notebook)) {
		auto &cell = notebook.cells[NumericCast<idx_t>(index)];
		Json data;
		auto outcome = runner.Run(notebook, cell, run, &data);
		auto json = CellJson(cell);
		json.Set("status", Json::String(outcome.status));
		json.Set("error", outcome.status == "error" ? Json::String(outcome.error) : Json());
		json.Set("row_count", outcome.row_count >= 0 ? Json::Number(static_cast<double>(outcome.row_count)) : Json());
		if (outcome.has_output) {
			json.Set("output", Json::String(outcome.output));
		}
		if (cell.kind == notebook::CellKind::CHART) {
			json.Set("chart", data);
		} else if (!data.IsNull()) {
			json.Set("table", data);
		}
		cells[NumericCast<idx_t>(index)] = std::move(json);
	}
	for (auto &cell : notebook.cells) {
		if (cell.kind != notebook::CellKind::INPUT) {
			continue;
		}
		auto &json = cells[NumericCast<idx_t>(cell.index)];
		auto value = QueryValues(runner.Conn(), "SELECT getvariable(" + notebook::QuoteString(cell.name) + ")");
		json.Set("value", value.empty() ? Json() : ValueToJson(value[0]));
		if (cell.widget == "select" || cell.widget == "multiselect") {
			auto values = Json::Array();
			for (auto &option : InputChoices(runner.Conn(), cell, 1000)) {
				values.Push(ValueToJson(option));
			}
			json.Set("choices", values);
		}
	}
	auto array = Json::Array();
	for (auto &cell : cells) {
		array.Push(std::move(cell));
	}
	return Json::Object()
	    .Set("title", Json::String(NotebookTitle(notebook, source)))
	    .Set("source", Json::String(source))
	    .Set("cells", array);
}

namespace {

//! The html-wasm extras: the data the notebook reads as a .duckdb file in a storage version
//! DuckDB-WASM can read, and the SQL the page runs to rebuild the notebook's views.
Json WasmPayload(DatabaseInstance &db, const string &text, const string &source, const ExportOptions &options) {
	SessionState state;
	Runner runner(db, state);
	auto &connection = runner.Conn();
	auto notebook = AnalyzeText(*connection.context, text);
	runner.InitializeVariables(notebook, options.params);
	RunnerOptions run;
	run.params = options.params;
	for (auto index : notebook::RunOrder(notebook)) {
		runner.Run(notebook, notebook.cells[NumericCast<idx_t>(index)], run);
	}

	// Relations the cells read that no query cell defines: tables (or views) in the database.
	std::set<string> cell_views;
	for (auto &cell : notebook.cells) {
		if (cell.kind == notebook::CellKind::SQL && cell.is_query && !cell.name.empty()) {
			cell_views.insert(cell.name);
		}
	}
	std::set<string> relations;
	for (auto &cell : notebook.cells) {
		for (auto &name : cell.reads) {
			if (!name.empty() && name[0] != '$' && !cell_views.count(name)) {
				relations.insert(name);
			}
		}
	}
	const char *tmp = std::getenv("TMPDIR");
	std::random_device random;
	auto path = string(tmp ? tmp : "/tmp") + "/notebook_export_" + std::to_string(random()) + ".duckdb";
	auto check = [&](const string &sql) {
		auto result = connection.Query(sql);
		if (result->HasError()) {
			throw InvalidInputException("html-wasm export: %s", result->GetError());
		}
	};
	check("ATTACH " + notebook::QuoteString(path) + " AS __notebook_export (STORAGE_VERSION 'v1.5.0')");
	auto exported = Json::Array();
	for (auto &name : relations) {
		auto probe = connection.Query("SELECT * FROM " + notebook::RelationSql(name) + " LIMIT 0");
		if (probe->HasError()) {
			continue;
		}
		probe.reset();
		auto dot = name.find('.');
		if (dot != string::npos) {
			check("CREATE SCHEMA IF NOT EXISTS __notebook_export." + notebook::QuoteIdentifier(name.substr(0, dot)));
		}
		check("CREATE TABLE __notebook_export." + notebook::RelationSql(name) + " AS SELECT * FROM " +
		      notebook::RelationSql(name));
		exported.Push(Json::String(name));
	}
	check("DETACH __notebook_export");
	auto bytes = ReadNotebookText(db, path);
	FileSystem::GetFileSystem(db).RemoveFile(path);

	auto setup = Json::Array();
	for (auto &name : notebook::UndefinedVariables(notebook)) {
		// Engines before DuckDB 2.0 cannot read `.lo` from an untyped NULL, so selections start as
		// NULLs of their eventual type.
		string type;
		for (auto &cell : notebook.cells) {
			if (cell.kind == notebook::CellKind::CHART && notebook::ChartSelectionInfo(cell.content).name == name) {
				type = VariableSqlType(connection, notebook, cell, name);
			}
		}
		setup.Push(Json::String("SET VARIABLE " + notebook::QuoteIdentifier(name) + " = NULL" +
		                        (type.empty() ? "" : "::" + type)));
	}
	for (auto index : notebook::RunOrder(notebook)) {
		auto &cell = notebook.cells[NumericCast<idx_t>(index)];
		if (cell.kind == notebook::CellKind::INPUT) {
			for (auto &statement : notebook::SplitStatements(notebook::CellSql(cell))) {
				setup.Push(Json::String(notebook::RewriteVariables(statement.text)));
			}
		}
		if (cell.kind == notebook::CellKind::SQL && cell.is_query) {
			// Query cells become temporary views again; effect cells already ran into the data file.
			for (auto &statement : notebook::SplitStatements(notebook::CellSql(cell))) {
				setup.Push(Json::String(notebook::RewriteVariables(statement.text)));
			}
		}
	}
	return Json::Object().Set("db", Json::String(Base64(Gzip(bytes)))).Set("tables", exported).Set("setup", setup);
}

//! The SQL a viewer running DuckDB-WASM needs per cell to refresh it.
void AddWasmQueries(Json &cell_json, const notebook::Cell &cell) {
	if (cell.kind == notebook::CellKind::SQL && cell.is_query) {
		auto sql = Runner::DisplaySql(cell);
		cell_json.Set("display_sql", Json::String(notebook::RewriteVariables(sql)));
	} else if (cell.kind == notebook::CellKind::MARKDOWN) {
		auto expressions = Json::Array();
		for (auto &expression : cell.expressions) {
			expressions.Push(Json::Object()
			                     .Set("sql", Json::String(notebook::RewriteVariables(expression.sql)))
			                     .Set("begin", Json::Number(static_cast<double>(expression.begin)))
			                     .Set("end", Json::Number(static_cast<double>(expression.end))));
		}
		cell_json.Set("expressions", expressions);
	}
}

string ReadRuntimeFile(DatabaseInstance &db, const string &directory, const string &name) {
	auto path = directory + "/" + name;
	auto &fs = FileSystem::GetFileSystem(db);
	if (!fs.FileExists(path)) {
		throw InvalidInputException("html-wasm export: \"%s\" not found. Build the runtime with wasm/build.sh and "
		                            "pass its directory as wasm_runtime := '...' (or set NOTEBOOK_WASM_RUNTIME)",
		                            path);
	}
	return ReadNotebookText(db, path);
}

string Html(DatabaseInstance &db, const Json &payload, const string &title, const string &wasm_runtime) {
	auto html = GetAsset("viewer.html");
	auto data = Base64(Gzip(payload.Dump()));
	string wasm;
	if (!wasm_runtime.empty()) {
		wasm = "<script type=\"application/octet-stream\" id=\"nb-wasm-module\">" +
		       Base64(Gzip(ReadRuntimeFile(db, wasm_runtime, "duckdb-eh.wasm"))) + "</script>\n" +
		       "<script type=\"application/octet-stream\" id=\"nb-wasm-worker\">" +
		       Base64(ReadRuntimeFile(db, wasm_runtime, "duckdb-browser-eh.worker.js")) + "</script>\n" + "<script>" +
		       ScriptSafe(ReadRuntimeFile(db, wasm_runtime, "runtime.js")) + "</script>\n";
	}
	html = ReplaceAll(html, "{{TITLE}}", HtmlEscape(title));
	html = ReplaceAll(html, "{{STYLE}}", GetAsset("style.css"));
	html = ReplaceAll(html, "{{DATA}}", data);
	html = ReplaceAll(html, "{{WASM}}", wasm);
	html =
	    ReplaceAll(html, "{{RENDER}}",
	               ScriptSafe(GetAsset("render-core.js") + GetAsset("render-chart.js") + GetAsset("render-typed.js")));
	html = ReplaceAll(html, "{{VIEWER}}", ScriptSafe(GetAsset("viewer.js")));
	return html;
}

string Cellish(const Json &value) {
	if (value.IsNull()) {
		return "";
	}
	string text = value.GetType() == Json::Type::STRING ? value.Str() : value.Dump();
	return ReplaceAll(ReplaceAll(text, "|", "\\|"), "\n", " ");
}

string MarkdownTable(const Json &table, idx_t max_rows) {
	auto columns = table.Get("columns");
	auto rows = table.Get("rows");
	if (!columns || !rows || columns->Size() == 0) {
		return "";
	}
	string out = "|";
	string rule = "|";
	for (auto &column : columns->Items()) {
		out += " " + Cellish(*column.Get("name")) + " |";
		rule += " --- |";
	}
	out += "\n" + rule + "\n";
	idx_t count = 0;
	for (auto &row : rows->Items()) {
		if (count++ >= max_rows) {
			break;
		}
		out += "|";
		for (auto &value : row.Items()) {
			out += " " + Cellish(value) + " |";
		}
		out += "\n";
	}
	auto total = table.Get("total");
	if (total && total->Num() > static_cast<double>(std::min<idx_t>(max_rows, rows->Size()))) {
		out += "\n" + std::to_string(static_cast<int64_t>(total->Num())) + " rows in total.\n";
	}
	return out;
}

string Markdown(const Json &payload) {
	string out;
	for (auto &cell : payload.Get("cells")->Items()) {
		auto kind = cell.Get("kind")->Str();
		auto options = cell.Get("options");
		auto hide = options && options->Get("hide") ? options->Get("hide")->Str() : string();
		bool show_code = hide != "code" && hide != "all";
		bool show_output = hide != "output" && hide != "all";
		if (kind == "markdown") {
			if (auto output = cell.Get("output")) {
				out += output->Str() + "\n\n";
			} else if (auto error = cell.Get("error")) {
				out += "> Error: " + error->Str() + "\n\n";
			}
		} else if (kind == "sql") {
			if (show_code) {
				out += "```sql\n" + cell.Get("body")->Str() + "\n```\n\n";
			}
			if (show_output) {
				if (cell.Get("error") && !cell.Get("error")->IsNull()) {
					out += "> Error: " + cell.Get("error")->Str() + "\n\n";
				} else if (auto table = cell.Get("table")) {
					out += MarkdownTable(*table, 20) + "\n";
				}
			}
		} else if (kind == "input") {
			auto label = options && options->Get("label") ? options->Get("label")->Str() : cell.Get("name")->Str();
			out += "**" + label + "**: " + Cellish(cell.Get("value") ? *cell.Get("value") : Json()) + "\n\n";
		} else if (kind == "chart" && show_output) {
			auto chart = cell.Get("chart");
			string title = "Chart";
			if (chart && chart->Get("render") && chart->Get("render")->Get("title")) {
				title = chart->Get("render")->Get("title")->Str();
			}
			out += "*" + title + "* (chart data; charts are drawn in the HTML export)\n\n";
			if (chart && chart->Get("datasets") && chart->Get("datasets")->Size() > 0) {
				out += MarkdownTable(chart->Get("datasets")->Items()[0], 20) + "\n";
			} else if (chart && chart->Get("error")) {
				out += "> Error: " + chart->Get("error")->Str() + "\n\n";
			}
		}
	}
	return out;
}

struct InputSpace {
	const notebook::Cell *cell;
	vector<Value> values;
};

} // namespace

string ExportDocument(DatabaseInstance &db, const string &text, const string &source, const ExportOptions &options,
                      string &note) {
	auto &format = options.format;
	if (format == "sql") {
		auto notebook = notebook::ParseNotebook(text);
		ThrowIfInvalid(notebook, source);
		return notebook::ExpandNotebook(notebook, source);
	}
	auto payload = BuildPayload(db, text, source, options);
	auto title = payload.Get("title")->Str();
	if (format == "md") {
		return "<!-- Exported from " + source + " by notebook_export. -->\n\n" + Markdown(payload);
	}
	if (format != "html" && format != "html-wasm") {
		throw InvalidInputException("Unknown export format '%s'; expected html, html-wasm, md, sql, data or pdf",
		                            format);
	}
	if (format == "html-wasm") {
		auto runtime = options.wasm_runtime;
		if (runtime.empty() && std::getenv("NOTEBOOK_WASM_RUNTIME")) {
			runtime = std::getenv("NOTEBOOK_WASM_RUNTIME");
		}
		if (runtime.empty()) {
			throw InvalidInputException("html-wasm export needs the DuckDB-WASM runtime: build it with wasm/build.sh "
			                            "and pass wasm_runtime := '<directory>' (or set NOTEBOOK_WASM_RUNTIME)");
		}
		auto notebook = notebook::ParseNotebook(text);
		auto cells = Json::Array();
		for (auto &cell_json : payload.Get("cells")->Items()) {
			auto copy = cell_json;
			AddWasmQueries(copy, notebook.cells[static_cast<idx_t>(cell_json.Get("idx")->Num())]);
			cells.Push(std::move(copy));
		}
		payload.Set("cells", cells);
		payload.Set("wasm", WasmPayload(db, text, source, options));
		note = "DuckDB-WASM runs the notebook's queries in the page";
		return Html(db, payload, title, runtime);
	}
	if (options.interactive == "precomputed") {
		// Run once per combination of input values, within the budget; the page swaps between them.
		SessionState state;
		Runner runner(db, state);
		auto notebook = AnalyzeText(*runner.Conn().context, text);
		// Choices may come from cells (select from=cell.column), which must run first.
		runner.InitializeVariables(notebook, options.params);
		RunnerOptions run_all;
		run_all.params = options.params;
		for (auto index : notebook::RunOrder(notebook)) {
			runner.Run(notebook, notebook.cells[NumericCast<idx_t>(index)], run_all);
		}
		vector<InputSpace> spaces;
		vector<string> frozen;
		idx_t combinations = 1;
		for (auto &cell : notebook.cells) {
			if (cell.kind != notebook::CellKind::INPUT || options.params.count(cell.name)) {
				continue;
			}
			vector<Value> values;
			if (cell.widget == "select") {
				values = InputChoices(runner.Conn(), cell, 50);
			} else if (cell.widget == "checkbox") {
				values = {Value::BOOLEAN(false), Value::BOOLEAN(true)};
			} else if ((cell.widget == "slider" || cell.widget == "number") && !cell.Option("step").empty()) {
				double lo = std::atof(cell.Option("min", "0").c_str()), hi = std::atof(cell.Option("max", "0").c_str());
				double step = std::atof(cell.Option("step").c_str());
				if (step > 0 && hi >= lo && (hi - lo) / step <= 50) {
					for (double v = lo; v <= hi + step / 1000; v += step) {
						values.push_back(Value::DOUBLE(v));
					}
				}
			}
			if (values.empty() || combinations * values.size() > options.budget) {
				frozen.push_back(cell.name);
				continue;
			}
			combinations *= values.size();
			spaces.push_back({&cell, std::move(values)});
		}
		auto inputs = Json::Array();
		for (auto &space : spaces) {
			auto values = Json::Array();
			for (auto &value : space.values) {
				values.Push(ValueToJson(value));
			}
			inputs.Push(Json::Object().Set("name", Json::String(space.cell->name)).Set("values", values));
		}
		auto states = Json::Object();
		vector<idx_t> position(spaces.size(), 0);
		for (idx_t n = 0; n < combinations && !spaces.empty(); n++) {
			ExportOptions run = options;
			string key;
			for (idx_t i = 0; i < spaces.size(); i++) {
				run.params[spaces[i].cell->name] = spaces[i].values[position[i]];
				key += (i ? "," : "") + std::to_string(position[i]);
			}
			auto state_payload = BuildPayload(db, text, source, run);
			states.Set(key, *state_payload.Get("cells"));
			for (idx_t i = spaces.size(); i-- > 0;) {
				if (++position[i] < spaces[i].values.size()) {
					break;
				}
				position[i] = 0;
			}
		}
		payload.Set("precomputed", Json::Object().Set("inputs", inputs).Set("states", states));
		note = std::to_string(combinations) + " precomputed state(s)";
		if (!frozen.empty()) {
			note += "; frozen at their current value:";
			for (auto &name : frozen) {
				note += " " + name;
			}
		}
		auto frozen_json = Json::Array();
		for (auto &name : frozen) {
			frozen_json.Push(Json::String(name));
		}
		payload.Set("frozen", frozen_json);
	}
	return Html(db, payload, title, string());
}

namespace {

struct ExportBindData : public TableFunctionData {
	string source, to;
	ExportOptions options;
};

struct ExportState : public GlobalTableFunctionState {
	bool done = false;
};

string FindBrowser(const string &requested) {
	if (!requested.empty()) {
		return requested;
	}
	if (auto env = std::getenv("NOTEBOOK_CHROME")) {
		return env;
	}
	const char *names[] = {"chromium",       "chromium-browser", "google-chrome",        "google-chrome-stable",
	                       "microsoft-edge", "chrome",           "chrome-headless-shell"};
	const char *path = std::getenv("PATH");
	string dirs = path ? path : "/usr/bin:/usr/local/bin";
	size_t start = 0;
	while (start <= dirs.size()) {
		auto colon = dirs.find(':', start);
		auto dir = dirs.substr(start, colon == string::npos ? string::npos : colon - start);
		for (auto name : names) {
			auto candidate = dir + "/" + name;
			if (FILE *f = fopen(candidate.c_str(), "r")) {
				fclose(f);
				return candidate;
			}
		}
		if (colon == string::npos) {
			break;
		}
		start = colon + 1;
	}
	return string();
}

string ShellQuote(const string &text) {
	return "'" + ReplaceAll(text, "'", "'\\''") + "'";
}

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                              vector<Identifier> &names) {
	auto result = make_uniq<ExportBindData>();
	result->source = input.inputs[0].GetValue<string>();
	for (auto &entry : input.named_parameters) {
		auto key = entry.first.GetIdentifierName();
		auto &value = entry.second;
		if (value.IsNull()) {
			continue;
		}
		if (key == "file") {
			result->to = value.GetValue<string>();
		} else if (key == "format") {
			result->options.format = value.GetValue<string>();
		} else if (key == "interactive") {
			result->options.interactive = value.GetValue<string>();
		} else if (key == "budget") {
			result->options.budget = value.GetValue<idx_t>();
		} else if (key == "page_rows") {
			result->options.page_rows = value.GetValue<idx_t>();
		} else if (key == "wasm_runtime") {
			result->options.wasm_runtime = value.GetValue<string>();
		} else if (key == "chrome") {
			result->options.chrome = value.GetValue<string>();
		} else if (key == "data_format") {
			result->options.data_format = value.GetValue<string>();
		} else if (key == "params") {
			if (value.type().id() != LogicalTypeId::STRUCT) {
				throw InvalidInputException("notebook_export: params must be a struct, such as {region: 'US'}");
			}
			auto &children = StructValue::GetChildren(value);
			for (idx_t i = 0; i < children.size(); i++) {
				result->options.params[StructType::GetChildName(value.type(), i).GetIdentifierName()] = children[i];
			}
		}
	}
	auto &format = result->options.format;
	if (result->to.empty()) {
		auto stem = result->source.compare(0, 3, "db:") == 0 ? result->source.substr(3) : result->source;
		auto dot = stem.rfind('.');
		auto slash = stem.find_last_of("/\\");
		if (dot != string::npos && (slash == string::npos || dot > slash)) {
			stem = stem.substr(0, dot);
		}
		auto extension = format == "md" ? ".md" : format == "sql" ? ".expanded.sql" : format == "pdf" ? ".pdf" : "";
		result->to = format == "data" ? stem + "_data" : stem + (extension[0] ? extension : ".html");
	}
	names.emplace_back("format");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("path");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("bytes");
	return_types.push_back(LogicalType::BIGINT);
	names.emplace_back("note");
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<ExportState>();
}

void Scan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<ExportState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &bind = data.bind_data->Cast<ExportBindData>();
	auto &db = DatabaseInstance::GetDatabase(context);
	auto text = ReadNotebookText(context, bind.source);
	auto &fs = FileSystem::GetFileSystem(context);
	string note;
	int64_t bytes = 0;
	auto format = bind.options.format;

	if (format == "data") {
		// One file per named query cell, written from the run's own session.
		if (!fs.DirectoryExists(bind.to)) {
			fs.CreateDirectory(bind.to);
		}
		SessionState session;
		Runner runner(db, session);
		auto notebook = AnalyzeText(*runner.Conn().context, text);
		ThrowIfInvalid(notebook, bind.source);
		runner.InitializeVariables(notebook, bind.options.params);
		RunnerOptions run;
		run.params = bind.options.params;
		for (auto index : notebook::RunOrder(notebook)) {
			runner.Run(notebook, notebook.cells[NumericCast<idx_t>(index)], run);
		}
		idx_t files = 0;
		auto extension = bind.options.data_format == "csv" ? ".csv" : ".parquet";
		for (auto &cell : notebook.cells) {
			if (cell.kind != notebook::CellKind::SQL || !cell.is_query || cell.name.empty()) {
				continue;
			}
			auto path = fs.JoinPath(bind.to, cell.name + extension);
			auto result = runner.Conn().Query("COPY (SELECT * FROM " + notebook::QuoteIdentifier(cell.name) + ") TO " +
			                                  notebook::QuoteString(path) + " (FORMAT " +
			                                  (bind.options.data_format == "csv" ? "csv" : "parquet") + ")");
			if (result->HasError()) {
				throw InvalidInputException("notebook_export: cell '%s': %s", cell.name, result->GetError());
			}
			files++;
		}
		note = std::to_string(files) + " file(s)";
	} else if (format == "pdf") {
		auto browser = FindBrowser(bind.options.chrome);
		if (browser.empty()) {
			throw InvalidInputException("notebook_export: PDF needs Chrome, Chromium or Edge. Pass chrome := "
			                            "'/path/to/browser' or set NOTEBOOK_CHROME, or export html and print it");
		}
		ExportOptions html_options = bind.options;
		html_options.format = "html";
		auto html = ExportDocument(db, text, bind.source, html_options, note);
		auto html_path = bind.to + ".html";
		WriteTextFile(context, html_path, html);
		string absolute = html_path;
		if (!absolute.empty() && absolute[0] != '/') {
			char cwd[4096];
			if (getcwd(cwd, sizeof(cwd))) {
				absolute = string(cwd) + "/" + absolute;
			}
		}
		auto command = ShellQuote(browser) +
		               " --headless --disable-gpu --no-sandbox --no-pdf-header-footer --virtual-time-budget=10000 "
		               "--print-to-pdf=" +
		               ShellQuote(bind.to) + " " + ShellQuote("file://" + absolute) + " >/dev/null 2>&1";
		auto status = std::system(command.c_str());
		fs.RemoveFile(html_path);
		if (status != 0 || !fs.FileExists(bind.to)) {
			throw InvalidInputException("notebook_export: %s failed to print the PDF (exit status %d)", browser,
			                            status);
		}
		note = "printed with " + browser;
	} else {
		auto document = ExportDocument(db, text, bind.source, bind.options, note);
		WriteTextFile(context, bind.to, document);
		bytes = static_cast<int64_t>(document.size());
	}
	if (bytes == 0 && fs.FileExists(bind.to)) {
		auto handle = fs.OpenFile(bind.to, FileFlags::FILE_FLAGS_READ);
		bytes = handle->GetFileSize();
	}
	output.data[0].Append(Value(format));
	output.data[1].Append(Value(bind.to));
	output.data[2].Append(Value::BIGINT(bytes));
	output.data[3].Append(note.empty() ? Value(LogicalType::VARCHAR) : Value(note));
}

} // namespace

void RegisterNotebookExport(ExtensionLoader &loader) {
	TableFunction function("notebook_export", {LogicalType::VARCHAR}, Scan, Bind, Init);
	function.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("file", LogicalType::VARCHAR)
		    .Add("format", LogicalType::VARCHAR)
		    .Add("params", LogicalType::ANY)
		    .Add("interactive", LogicalType::VARCHAR)
		    .Add("budget", LogicalType::UBIGINT)
		    .Add("page_rows", LogicalType::UBIGINT)
		    .Add("wasm_runtime", LogicalType::VARCHAR)
		    .Add("chrome", LogicalType::VARCHAR)
		    .Add("data_format", LogicalType::VARCHAR);
	});
	loader.RegisterFunction(function);
}

} // namespace duckdb
