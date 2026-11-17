#include "notebook/chart.hpp"

#include "chart_compiler.hpp"

#include "notebook/script.hpp"
#include "notebook/sql_lexer.hpp"
#include "notebook/text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

namespace notebook {

const ColumnInfo *Compiler::Column(const std::string &name) const {
	for (auto &column : context.columns) {
		if (Lower(column.name) == Lower(name)) {
			return &column;
		}
	}
	return nullptr;
}

bool Compiler::IsField(const std::string &name) const {
	return context.columns.empty() || Column(name) != nullptr || name == "*";
}

std::string Compiler::FieldClass(const std::string &field) const {
	auto column = Column(field);
	return column ? TypeClass(column->type) : std::string();
}

Channel Compiler::ParseChannel(const std::string &name, const Yaml *node) {
	Channel channel;
	channel.name = name;
	if (!node || node->type == Yaml::Type::NUL) {
		return channel;
	}
	channel.present = true;
	if (node->IsScalar()) {
		if (!node->quoted && IsField(node->scalar)) {
			channel.field = node->scalar;
		} else {
			channel.constant = true;
			channel.value = node->ToJson();
		}
	} else if (node->IsMap()) {
		if (auto value = node->Get("value")) {
			channel.constant = true;
			channel.value = value->ToJson();
		}
		channel.field = node->Str("field");
		channel.agg = Lower(node->Str("agg", node->Str("aggregate")));
		channel.timeunit = Lower(node->Str("timeunit"));
		channel.sort = node->Str("sort");
		channel.type = node->Str("type");
		if (auto bin = node->Get("bin")) {
			if (bin->IsScalar() && bin->scalar == "true") {
				channel.bin = true;
			} else if (bin->IsMap()) {
				channel.bin = true;
				channel.bin_step = std::atof(bin->Str("step", "0").c_str());
				channel.maxbins = std::atoi(bin->Str("maxbins", "0").c_str());
			}
		}
		for (auto key : {"title", "format", "scale", "scheme", "legend", "axis"}) {
			if (auto option = node->Get(key)) {
				channel.options.Set(key, option->ToJson());
			}
		}
		if (channel.field.empty() && !channel.constant && channel.agg != "count") {
			Fail("channel " + name + " needs a field");
		}
		if (!channel.field.empty() && !IsField(channel.field)) {
			Fail("channel " + name + ": no column named '" + channel.field + "' in " + from);
		}
	} else {
		Fail("channel " + name + " must be a column name or a mapping");
	}
	return channel;
}

void Compiler::Fail(const std::string &message) {
	if (error.empty()) {
		error = message;
	}
}

std::string Compiler::Agg(const std::string &agg, const std::string &expr, const std::string &channel) {
	if (agg == "count") {
		return expr.empty() || expr == "\"*\"" ? "count(*)" : "count(" + expr + ")";
	}
	if (agg == "count_distinct" || agg == "distinct") {
		return "count(DISTINCT " + expr + ")";
	}
	if (agg == "sum" || agg == "min" || agg == "max" || agg == "avg" || agg == "median") {
		return agg + "(" + expr + ")";
	}
	if (agg == "mean") {
		return "avg(" + expr + ")";
	}
	if (agg.size() == 3 && agg[0] == 'p' && std::isdigit(static_cast<unsigned char>(agg[1])) &&
	    std::isdigit(static_cast<unsigned char>(agg[2]))) {
		return "quantile_cont(" + expr + ", 0." + agg.substr(1) + ")";
	}
	Fail("channel " + channel + ": unknown agg '" + agg + "'");
	return expr;
}

std::string Compiler::Source(const Yaml &spec) {
	std::vector<std::string> predicates;
	for (auto &name : spec.Get("filter") ? spec.Get("filter")->Strings() : std::vector<std::string>()) {
		auto found = context.selections.find(name);
		if (found == context.selections.end()) {
			Fail("filter '" + name + "' is not the selection of any chart");
			continue;
		}
		auto &selection = found->second;
		auto var = "getvariable(" + QuoteString(name) + ")";
		std::string test;
		if (selection.type == "point" && !selection.fields.empty()) {
			test = "list_contains(" + var + ", " + QuoteIdentifier(selection.fields[0]) + ")";
		} else if (selection.fields.size() == 1) {
			test = QuoteIdentifier(selection.fields[0]) + " BETWEEN " + var + ".lo AND " + var + ".hi";
		} else if (selection.fields.size() >= 2) {
			test = QuoteIdentifier(selection.fields[0]) + " BETWEEN " + var + "." + selection.channels[0] + ".lo AND " +
			       var + "." + selection.channels[0] + ".hi AND " + QuoteIdentifier(selection.fields[1]) + " BETWEEN " +
			       var + "." + selection.channels[1] + ".lo AND " + var + "." + selection.channels[1] + ".hi";
		} else {
			continue;
		}
		predicates.push_back("(" + var + " IS NULL OR (" + test + "))");
	}
	std::string sql = "SELECT * FROM " + RelationSql(from);
	auto sample = spec.Str("sample");
	if (!sample.empty()) {
		sql += " USING SAMPLE " + std::to_string(std::atoll(sample.c_str())) + " ROWS";
	}
	for (size_t i = 0; i < predicates.size(); i++) {
		sql += (i ? " AND " : " WHERE ") + predicates[i];
	}
	return sql;
}

void Compiler::Transform(Channel &channel) {
	if (channel.constant || channel.field.empty() || channel.field == "*") {
		return;
	}
	std::string expr = QuoteIdentifier(channel.field);
	auto cls = FieldClass(channel.field);
	if (!channel.timeunit.empty()) {
		static const std::set<std::string> TRUNCATE = {"year", "quarter", "month",  "week",
		                                               "date", "hour",    "minute", "second"};
		auto &unit = channel.timeunit;
		if (TRUNCATE.count(unit)) {
			expr = "date_trunc(" + QuoteString(unit == "date" ? "day" : unit) + ", " + expr + ")";
			cls = "temporal";
		} else if (unit == "hours" || unit == "hourofday") {
			expr = "hour(" + expr + ")";
			cls = "ordinal";
		} else if (unit == "day" || unit == "dayofweek") {
			expr = "dayofweek(" + expr + ")";
			cls = "ordinal";
		} else if (unit == "monthofyear") {
			expr = "month(" + expr + ")";
			cls = "ordinal";
		} else if (unit == "dayofmonth") {
			expr = "day(" + expr + ")";
			cls = "ordinal";
		} else if (unit == "minutes") {
			expr = "minute(" + expr + ")";
			cls = "ordinal";
		} else {
			Fail("channel " + channel.name + ": unknown timeunit '" + unit + "'");
		}
	}
	if (channel.bin) {
		std::string step;
		if (channel.bin_step > 0) {
			step = Json::Number(channel.bin_step).Dump();
		} else {
			int maxbins = channel.maxbins > 0 ? channel.maxbins : 30;
			auto name = "__bin_" + channel.name;
			// A "nice" step: 1, 2 or 5 times a power of ten, giving at most maxbins bins.
			auto raw = "((max(" + expr + ") - min(" + expr + "))::DOUBLE / " + std::to_string(maxbins) + ")";
			auto power = "pow(10, floor(log10(nullif(" + raw + ", 0))))";
			ctes.push_back(name + " AS (SELECT coalesce(" + power + " * CASE WHEN " + raw + " / " + power +
			               " <= 1 THEN 1 WHEN " + raw + " / " + power + " <= 2 THEN 2 WHEN " + raw + " / " + power +
			               " <= 5 THEN 5 ELSE 10 END, 1) AS step FROM __src)");
			joins.push_back(name);
			step = name + ".step";
		}
		channel.expr = "floor(" + expr + " / " + step + ") * " + step;
		channel.end_expr = channel.expr + " + " + step;
		bin_steps[channel.name] = step;
		cls = "quantitative";
	} else {
		channel.expr = expr;
	}
	if (channel.type.empty()) {
		channel.type = cls.empty() ? "nominal" : cls;
	}
}

std::string Compiler::AxisType(const Channel &channel) {
	if (!channel.present || channel.constant) {
		return std::string();
	}
	return channel.agg.empty() ? (channel.type.empty() ? "nominal" : channel.type) : "quantitative";
}

Json Compiler::Encoding(const Channel &channel, const std::string &column) {
	auto json = channel.options;
	if (channel.constant) {
		json.Set("value", channel.value);
		return json;
	}
	json.Set("column", Json::String(column));
	if (!channel.field.empty()) {
		json.Set("field", Json::String(channel.field));
	}
	json.Set("type",
	         Json::String(channel.agg.empty() ? (channel.type.empty() ? "nominal" : channel.type) : "quantitative"));
	if (!channel.agg.empty()) {
		json.Set("agg", Json::String(channel.agg));
	}
	if (!channel.timeunit.empty()) {
		json.Set("timeunit", Json::String(channel.timeunit));
	}
	if (channel.bin) {
		json.Set("bin", Json::Bool(true));
		json.Set("end", Json::String(column + "_end"));
	}
	if (!channel.sort.empty()) {
		json.Set("sort", Json::String(channel.sort));
	}
	if (!json.Get("title")) {
		std::string title = channel.field.empty() || channel.field == "*" ? "count" : channel.field;
		if (!channel.agg.empty() && !(channel.agg == "count" && (channel.field.empty() || channel.field == "*"))) {
			title = channel.agg + "(" + title + ")";
		}
		json.Set("title", Json::String(title));
	}
	return json;
}

std::string Compiler::Query(const std::vector<std::string> &items, bool group, const std::string &order,
                            const std::string &limit) {
	std::string sql = "WITH __src AS (" + source + ")";
	for (auto &cte : ctes) {
		sql += ", " + cte;
	}
	sql += "\nSELECT ";
	for (size_t i = 0; i < items.size(); i++) {
		sql += (i ? ", " : "") + items[i];
	}
	sql += "\nFROM __src";
	for (auto &join : joins) {
		sql += ", " + join;
	}
	if (group) {
		sql += "\nGROUP BY ALL";
	}
	if (!order.empty()) {
		sql += "\nORDER BY " + order;
	}
	if (!limit.empty()) {
		sql += "\nLIMIT " + limit;
	}
	return sql;
}

std::string Compiler::Limit(const Yaml &spec) {
	auto limit = spec.Str("limit");
	return std::to_string(limit.empty() ? DEFAULT_LIMIT : std::atoll(limit.c_str()));
}

std::string Compiler::Measure(const Yaml *node, const std::string &name, const std::string &default_agg,
                              Json *encoding) {
	auto channel = ParseChannel(name, node);
	if (!channel.present) {
		return "";
	}
	if (channel.agg.empty()) {
		channel.agg = default_agg;
	}
	Transform(channel);
	if (encoding) {
		*encoding = Encoding(channel, name);
	}
	if (channel.constant) {
		return channel.value.Dump();
	}
	return channel.agg.empty() || channel.agg == "none" ? channel.expr : Agg(channel.agg, channel.expr, name);
}

std::string Compiler::Field(const Yaml &spec, const std::string &key, bool required, Json *encoding) {
	auto channel = ParseChannel(key, spec.Get(key));
	if (!channel.present) {
		if (required) {
			Fail((spec.Get("type") ? spec.Str("type") : chart_type) + " chart needs " + key + ":");
		}
		return "";
	}
	Transform(channel);
	if (encoding) {
		*encoding = Encoding(channel, key);
	}
	return channel.constant ? channel.value.Dump() : channel.expr;
}

namespace {

std::string SelectionField(const Yaml &spec, const std::string &channel) {
	auto node = spec.Get(channel);
	if (!node) {
		return std::string();
	}
	return node->IsScalar() ? node->scalar : node->Str("field");
}

} // namespace

std::string TypeClass(const std::string &duckdb_type) {
	auto t = Lower(duckdb_type);
	if (t.find('[') != std::string::npos) {
		return "nominal"; // a list or array of anything
	}
	if (t.compare(0, 4, "date") == 0 || t.compare(0, 4, "time") == 0) {
		return "temporal"; // date, time, timestamp, timestamp_ns, ... with time zone
	}
	// Numbers are matched by their whole name (DECIMAL's width and scale aside), so INTERVAL is not INT.
	auto base = Trim(t.substr(0, t.find('(')));
	static const std::set<std::string> NUMERIC = {"tinyint",   "smallint", "integer", "bigint",   "hugeint", "utinyint",
	                                              "usmallint", "uinteger", "ubigint", "uhugeint", "float",   "double",
	                                              "decimal",   "real",     "int",     "numeric",  "int1",    "int2",
	                                              "int4",      "int8",     "float4",  "float8",   "varint",  "bignum"};
	return NUMERIC.count(base) ? "quantitative" : "nominal";
}

std::string RelationSql(const std::string &from) {
	auto trimmed = from;
	if (!trimmed.empty() && trimmed[0] == '(') {
		return trimmed;
	}
	std::string result;
	size_t start = 0;
	while (true) {
		auto dot = trimmed.find('.', start);
		auto part = trimmed.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
		result += (result.empty() ? "" : ".") + QuoteIdentifier(part);
		if (dot == std::string::npos) {
			break;
		}
		start = dot + 1;
	}
	return result;
}

SelectionInfo ChartSelectionInfo(const std::string &spec_text) {
	std::string error;
	return ChartSelectionInfo(ParseYaml(spec_text, error));
}

std::map<std::string, SelectionInfo> NotebookSelections(const Notebook &notebook) {
	std::map<std::string, SelectionInfo> selections;
	for (auto &cell : notebook.cells) {
		for (auto &spec : cell.charts) {
			auto selection = ChartSelectionInfo(spec);
			if (!selection.name.empty()) {
				selections[selection.name] = selection;
			}
		}
	}
	return selections;
}

SelectionInfo ChartSelectionInfo(const Yaml &spec) {
	SelectionInfo info;
	auto select = spec.Get("select");
	if (!select || !select->IsMap()) {
		return info;
	}
	info.name = select->Str("as");
	info.type = Lower(select->Str("type", "interval"));
	auto on = select->Get("on");
	info.channels = on ? on->Strings() : std::vector<std::string> {"x"};
	if (info.channels.empty()) {
		info.channels.push_back("x");
	}
	for (auto &channel : info.channels) {
		info.fields.push_back(SelectionField(spec, channel));
	}
	return info;
}

ChartPlan CompileChart(const std::string &spec_text, const std::string &from, const ChartContext &context) {
	ChartPlan plan;
	std::string error;
	auto spec = ParseYaml(spec_text, error);
	if (!error.empty()) {
		plan.error = "chart spec: " + error;
		return plan;
	}
	if (!spec.IsMap()) {
		plan.error = "chart spec must be a YAML mapping";
		return plan;
	}
	if (from.empty()) {
		plan.error = "chart needs from=<cell or table>";
		return plan;
	}
	Compiler compiler(context, from);
	compiler.source = compiler.Source(spec);
	auto render = Json::Object();
	render.Set("width", Json::Number(context.width));
	render.Set("height", Json::Number(context.height));
	if (auto title = spec.Get("title")) {
		render.Set("title", title->ToJson());
	}
	if (spec.Get("type")) {
		// Rules, bands and notes are drawn on a grammar chart's axes. KPIs and gauges have bands of their
		// own ({from, to, color}), which colour the value.
		auto type = Lower(spec.Str("type"));
		for (auto key : {"rules", "bands", "notes"}) {
			if (spec.Get(key) && !(std::string(key) == "bands" && (type == "kpi" || type == "gauge"))) {
				compiler.Fail(std::string(key) +
				              ": needs a grammar chart (mark: or layers:), not type: " + spec.Str("type"));
			}
		}
		compiler.Typed(spec, render);
		render.Set("spec", spec.ToJson());
	} else {
		render.Set("kind", Json::String("grammar"));
		auto layers = Json::Array();
		if (auto list = spec.Get("layers")) {
			if (!list->IsList()) {
				plan.error = "layers: must be a list of specs";
				return plan;
			}
			for (auto &layer : list->items) {
				layers.Push(compiler.GrammarLayer(layer, spec));
			}
		} else {
			layers.Push(compiler.GrammarLayer(spec, spec));
		}
		render.Set("layers", layers);
		if (compiler.error.empty()) {
			compiler.References(spec, render);
		}
		if (compiler.error.empty()) {
			compiler.Notes(spec, render);
		}
		if (spec.Get("facet")) {
			render.Set("facet", Json::Object()
			                        .Set("row", Json::Bool(spec.Get("facet")->Get("row") != nullptr))
			                        .Set("col", Json::Bool(spec.Get("facet")->Get("col") != nullptr)));
		}
		auto selection = ChartSelectionInfo(spec);
		if (!selection.name.empty()) {
			auto json = Json::Object();
			json.Set("as", Json::String(selection.name));
			json.Set("type", Json::String(selection.type));
			auto channels = Json::Array(), fields = Json::Array();
			for (size_t i = 0; i < selection.channels.size(); i++) {
				channels.Push(Json::String(selection.channels[i]));
				fields.Push(Json::String(selection.fields[i]));
			}
			json.Set("channels", channels);
			json.Set("fields", fields);
			render.Set("selection", json);
		}
	}
	if (!compiler.error.empty()) {
		plan.error = compiler.error;
		return plan;
	}
	plan.queries = compiler.plan.queries;
	plan.render = render;
	return plan;
}

std::string RewriteVariables(const std::string &sql) {
	std::string out;
	size_t copied = 0;
	for (auto &ref : VariableRefs(sql)) {
		out += sql.substr(copied, ref.start - copied) + "getvariable(" + QuoteString(ref.name) + ")";
		copied = ref.end;
	}
	return out + sql.substr(copied);
}

} // namespace notebook
