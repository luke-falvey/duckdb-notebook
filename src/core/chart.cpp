#include "notebook/chart.hpp"

#include "notebook/script.hpp"
#include "notebook/sql_lexer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

namespace notebook {

namespace {

const int DEFAULT_LIMIT = 10000;
//! Above this many rows, point-like marks are binned to a pixel grid instead of drawn one by one.
const int64_t POINT_THRESHOLD = 5000;

std::string Lower(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

std::string SqlString(const std::string &s) {
	std::string out = "'";
	for (char c : s) {
		out += c;
		if (c == '\'') {
			out += '\'';
		}
	}
	return out + "'";
}

//! One encoding channel after parsing: a field with transforms, or a constant value.
struct Channel {
	std::string name;
	bool present = false;
	bool constant = false;
	Json value;
	std::string field;
	std::string agg;
	std::string timeunit;
	bool bin = false;
	double bin_step = 0;
	int maxbins = 0;
	std::string sort;
	std::string type;
	Json options = Json::Object();
	//! Filled during compilation.
	std::string expr;
	std::string end_expr;
};

class Compiler {
public:
	Compiler(const ChartContext &context, const std::string &from) : context(context), from(from) {
	}

	const ChartContext &context;
	std::string from;
	std::string error;
	ChartPlan plan;

	const ColumnInfo *Column(const std::string &name) const {
		for (auto &column : context.columns) {
			if (Lower(column.name) == Lower(name)) {
				return &column;
			}
		}
		return nullptr;
	}

	bool IsField(const std::string &name) const {
		return context.columns.empty() || Column(name) != nullptr || name == "*";
	}

	std::string FieldClass(const std::string &field) const {
		auto column = Column(field);
		return column ? TypeClass(column->type) : std::string();
	}

	Channel ParseChannel(const std::string &name, const Yaml *node) {
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

	void Fail(const std::string &message) {
		if (error.empty()) {
			error = message;
		}
	}

	std::string Agg(const std::string &agg, const std::string &expr, const std::string &channel) {
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

	//! The SQL CTEs gathered while compiling the current query.
	std::vector<std::string> ctes;
	std::vector<std::string> joins;

	//! Filters from `filter:` applied to `from`.
	std::string Source(const Yaml &spec) {
		std::vector<std::string> predicates;
		for (auto &name : spec.Get("filter") ? spec.Get("filter")->Strings() : std::vector<std::string>()) {
			auto found = context.selections.find(name);
			if (found == context.selections.end()) {
				Fail("filter '" + name + "' is not the selection of any chart");
				continue;
			}
			auto &selection = found->second;
			auto var = "getvariable(" + SqlString(name) + ")";
			std::string test;
			if (selection.type == "point" && !selection.fields.empty()) {
				test = "list_contains(" + var + ", " + QuoteIdentifier(selection.fields[0]) + ")";
			} else if (selection.fields.size() == 1) {
				test = QuoteIdentifier(selection.fields[0]) + " BETWEEN " + var + ".lo AND " + var + ".hi";
			} else if (selection.fields.size() >= 2) {
				test = QuoteIdentifier(selection.fields[0]) + " BETWEEN " + var + "." + selection.channels[0] +
				       ".lo AND " + var + "." + selection.channels[0] + ".hi AND " +
				       QuoteIdentifier(selection.fields[1]) + " BETWEEN " + var + "." + selection.channels[1] +
				       ".lo AND " + var + "." + selection.channels[1] + ".hi";
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

	//! Field expression with timeunit and bin applied; sets channel.expr and end_expr.
	void Transform(Channel &channel) {
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
				expr = "date_trunc(" + SqlString(unit == "date" ? "day" : unit) + ", " + expr + ")";
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

	std::map<std::string, std::string> bin_steps;

	//! What rules and bands need to know about a layer once it is compiled: the query of its marks
	//! before any downsampling, and the type of each axis it draws on.
	struct LayerInfo {
		std::string mark;
		std::string base;
		std::string x_type;
		std::string y_type;
	};
	std::vector<LayerInfo> layer_info;

	static std::string AxisType(const Channel &channel) {
		if (!channel.present || channel.constant) {
			return std::string();
		}
		return channel.agg.empty() ? (channel.type.empty() ? "nominal" : channel.type) : "quantitative";
	}

	Json Encoding(const Channel &channel, const std::string &column) {
		auto json = channel.options;
		if (channel.constant) {
			json.Set("value", channel.value);
			return json;
		}
		json.Set("column", Json::String(column));
		if (!channel.field.empty()) {
			json.Set("field", Json::String(channel.field));
		}
		json.Set("type", Json::String(channel.agg.empty() ? (channel.type.empty() ? "nominal" : channel.type)
		                                                  : "quantitative"));
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

	std::string Query(const std::vector<std::string> &items, bool group, const std::string &order,
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

	std::string source;

	std::string Limit(const Yaml &spec) {
		auto limit = spec.Str("limit");
		return std::to_string(limit.empty() ? DEFAULT_LIMIT : std::atoll(limit.c_str()));
	}

	//! Numeric form of an expression, for pixel binning.
	static std::string Numeric(const std::string &expr, const std::string &cls) {
		return cls == "temporal" ? "epoch(" + expr + ")" : "(" + expr + ")::DOUBLE";
	}

	Json GrammarLayer(const Yaml &layer_spec, const Yaml &top) {
		ctes.clear();
		joins.clear();
		bin_steps.clear();
		auto mark = Lower(layer_spec.Str("mark", top.Str("mark")));
		if (mark.empty()) {
			Fail("chart needs mark: (line, bar, point, ...) or type: (kpi, table, map, ...)");
			return Json();
		}
		static const std::set<std::string> MARKS = {"line",      "area",    "bar",    "point",  "rect",
		                                            "rule",      "tick",    "text",   "arc",    "boxplot",
		                                            "histogram", "density", "hexbin", "circle", "square"};
		if (!MARKS.count(mark)) {
			Fail("unknown mark '" + mark + "'");
			return Json();
		}
		static const char *const CHANNELS[] = {"x",       "y",     "x2",   "y2",    "color", "size",
		                                       "opacity", "shape", "text", "order", "theta"};
		std::vector<Channel> channels;
		for (auto name : CHANNELS) {
			channels.push_back(ParseChannel(name, layer_spec.Get(name)));
		}
		auto channel = [&](const std::string &name) -> Channel & {
			for (auto &c : channels) {
				if (c.name == name) {
					return c;
				}
			}
			return channels[0];
		};
		std::vector<Channel> tooltips;
		if (auto tooltip = layer_spec.Get("tooltip")) {
			if (tooltip->IsList()) {
				for (size_t i = 0; i < tooltip->items.size(); i++) {
					tooltips.push_back(ParseChannel("tooltip_" + std::to_string(i), &tooltip->items[i]));
				}
			} else {
				tooltips.push_back(ParseChannel("tooltip_0", tooltip));
			}
		}
		std::vector<Channel> facets;
		if (auto facet = top.Get("facet")) {
			facets.push_back(ParseChannel("facet_row", facet->Get("row")));
			facets.push_back(ParseChannel("facet_col", facet->Get("col")));
		}

		// Marks that are shorthands for a binned or aggregated bar or area.
		std::string draw = mark;
		auto &x = channel("x");
		auto &y = channel("y");
		if (mark == "histogram" || mark == "density") {
			draw = mark == "histogram" ? "bar" : "area";
			if (!x.bin) {
				x.bin = true;
				x.maxbins = x.maxbins ? x.maxbins : (mark == "histogram" ? 30 : 50);
			}
			if (!y.present) {
				y.present = true;
				y.agg = mark == "histogram" ? "count" : "density";
			}
		}
		if (mark == "circle" || mark == "square") {
			draw = "point";
		}

		std::vector<Channel *> all;
		for (auto &c : channels) {
			all.push_back(&c);
		}
		for (auto &c : tooltips) {
			all.push_back(&c);
		}
		for (auto &c : facets) {
			all.push_back(&c);
		}
		for (auto c : all) {
			if (c->present) {
				Transform(*c);
			}
		}
		if (!error.empty()) {
			return Json();
		}

		auto encoding = Json::Object();
		std::vector<std::string> items;
		bool group = false;
		Json flags = Json::Object();

		if (mark == "boxplot") {
			if (!y.present || y.field.empty()) {
				Fail("boxplot needs y: <numeric field>");
				return Json();
			}
			if (x.present && !x.constant) {
				items.push_back(x.expr + " AS x");
				encoding.Set("x", Encoding(x, "x"));
			}
			auto field = QuoteIdentifier(y.field);
			const char *stats[][2] = {{"min", "min(%)"},
			                          {"q1", "quantile_cont(%, 0.25)"},
			                          {"median", "median(%)"},
			                          {"q3", "quantile_cont(%, 0.75)"},
			                          {"max", "max(%)"}};
			for (auto &stat : stats) {
				std::string expr = stat[1];
				expr.replace(expr.find('%'), 1, field);
				items.push_back(expr + " AS y_" + std::string(stat[0]));
			}
			auto enc = Encoding(y, "y_median");
			enc.Set("type", Json::String("quantitative"));
			encoding.Set("y", enc);
			auto layer = Json::Object();
			layer.Set("mark", Json::String("boxplot"));
			layer.Set("data", Json::Number(static_cast<double>(plan.queries.size())));
			layer.Set("encoding", encoding);
			plan.queries.push_back(Query(items, true, x.present && !x.constant ? "x" : "", Limit(top)));
			layer_info.push_back({mark, std::string(), AxisType(x), "quantitative"});
			return layer;
		}

		bool has_agg = false;
		for (auto c : all) {
			if (c->present && !c->constant && !c->agg.empty()) {
				has_agg = true;
			}
		}
		for (auto c : all) {
			if (!c->present) {
				continue;
			}
			auto column = c->name;
			if (!c->constant) {
				std::string expr = c->expr;
				if (c->agg == "density") {
					auto step = bin_steps.count("x") ? bin_steps["x"] : "1";
					expr = "count(*) / (sum(count(*)) OVER () * any_value(" + step + "))";
				} else if (!c->agg.empty()) {
					expr = Agg(c->agg, c->expr, c->name);
				}
				items.push_back(expr + " AS " + QuoteIdentifier(column));
				if (c->bin && c->agg.empty()) {
					items.push_back(c->end_expr + " AS " + QuoteIdentifier(column + "_end"));
				}
			}
			if (c->name.compare(0, 8, "tooltip_") == 0) {
				continue;
			}
			if (c->name.compare(0, 6, "facet_") == 0) {
				continue;
			}
			encoding.Set(c->name, Encoding(*c, column));
		}
		auto tooltip_json = Json::Array();
		for (auto &c : tooltips) {
			if (c.present && !c.constant) {
				tooltip_json.Push(Encoding(c, c.name));
			}
		}
		group = has_agg;

		// Downsampling for marks that would otherwise ship every row.
		bool continuous_x =
		    x.present && !x.constant && x.agg.empty() && !x.bin && (x.type == "temporal" || x.type == "quantitative");
		bool quantitative_y = y.present && !y.constant && y.agg.empty() && y.type == "quantitative";
		auto limit = Limit(top);
		std::string order;
		auto order_for = [&](const Channel &c) -> std::string {
			if (c.sort.empty()) {
				return "";
			}
			if (c.sort == "asc" || c.sort == "ascending") {
				return QuoteIdentifier(c.name);
			}
			if (c.sort == "desc" || c.sort == "descending") {
				return QuoteIdentifier(c.name) + " DESC";
			}
			bool desc = c.sort[0] == '-';
			auto other = desc ? c.sort.substr(1) : c.sort;
			return QuoteIdentifier(other) + (desc ? " DESC" : "");
		};
		for (auto c : {&x, &y, &channel("color")}) {
			auto o = order_for(*c);
			if (!o.empty()) {
				order += (order.empty() ? "" : ", ") + o;
			}
		}
		if (order.empty() && x.present && !x.constant && x.agg.empty()) {
			order = "x";
			if (channel("color").present && !channel("color").constant && channel("color").agg.empty()) {
				order += ", color";
			}
		}

		// The marks before downsampling: what an aggregate rule or band (`y: avg`) is taken over.
		layer_info.push_back({mark, Query(items, group, "", ""), AxisType(x), AxisType(y)});

		std::string sql;
		bool large = context.row_count > POINT_THRESHOLD;
		bool lines = draw == "line" || draw == "area";
		if (!has_agg && lines && continuous_x && quantitative_y && context.row_count > 4 * context.width) {
			// M4: per series and horizontal pixel, keep the rows with the first and last x and the
			// lowest and highest y. The line drawn from them is pixel-identical to the full one.
			auto series = channel("color").present && !channel("color").constant ? "color" : "NULL";
			auto px = "floor((" + Numeric("x", x.type) + " - __range.lo) / nullif(__range.hi - __range.lo, 0) * " +
			          std::to_string(context.width) + ")";
			auto inner = Query(items, false, "", "");
			sql =
			    "WITH __base AS (" + inner + "), __range AS (SELECT min(" + Numeric("x", x.type) + ") AS lo, max(" +
			    Numeric("x", x.type) + ") AS hi FROM __base), __px AS (SELECT __base.*, " + px +
			    " AS __p FROM __base, __range)\nSELECT * EXCLUDE (__p) FROM __px QUALIFY row_number() OVER (PARTITION "
			    "BY " +
			    series + ", __p ORDER BY x) = 1 OR row_number() OVER (PARTITION BY " + series +
			    ", __p ORDER BY x DESC) = 1 OR row_number() OVER (PARTITION BY " + series +
			    ", __p ORDER BY y) = 1 OR row_number() OVER (PARTITION BY " + series +
			    ", __p ORDER BY y DESC) = 1\nORDER BY x\nLIMIT " + limit;
			flags.Set("m4", Json::Bool(true));
		} else if (!has_agg && (mark == "hexbin" || ((draw == "point" || draw == "rect" || draw == "tick") && large)) &&
		           x.type == "quantitative" && quantitative_y) {
			// Bin to a grid of 4-pixel cells and draw one mark per cell, sized by its row count.
			int nx = std::max(1, context.width / 4), ny = std::max(1, context.height / 4);
			ctes.push_back("__grid AS (SELECT min(" + x.expr + ")::DOUBLE AS x0, max(" + x.expr +
			               ")::DOUBLE AS x1, min(" + y.expr + ")::DOUBLE AS y0, max(" + y.expr +
			               ")::DOUBLE AS y1 FROM __src)");
			joins.push_back("__grid");
			auto cell = [](const std::string &e, const std::string &lo, const std::string &hi, int n) {
				auto width = "(nullif(" + hi + " - " + lo + ", 0) / " + std::to_string(n) + ")";
				return "coalesce(" + lo + " + (least(floor((" + e + " - " + lo + ") / " + width + "), " +
				       std::to_string(n - 1) + ") + 0.5) * " + width + ", " + lo + ")";
			};
			std::vector<std::string> grid_items = {cell(x.expr, "x0", "x1", nx) + " AS x",
			                                       cell(y.expr, "y0", "y1", ny) + " AS y", "count(*) AS count"};
			if (channel("color").present && !channel("color").constant) {
				grid_items.push_back(channel("color").expr + " AS color");
			}
			sql = Query(grid_items, true, "", limit);
			flags.Set("binned", Json::Bool(true));
		} else {
			sql = Query(items, group, order, limit);
		}

		auto layer = Json::Object();
		layer.Set("mark", Json::String(draw));
		if (draw != mark) {
			layer.Set("from_mark", Json::String(mark));
		}
		layer.Set("data", Json::Number(static_cast<double>(plan.queries.size())));
		layer.Set("encoding", encoding);
		if (tooltip_json.Size()) {
			layer.Set("tooltip", tooltip_json);
		}
		for (auto &entry : flags.Entries()) {
			layer.Set(entry.first, entry.second);
		}
		for (auto key : {"opacity", "strokeWidth", "interpolate", "point", "stack", "cornerRadius"}) {
			if (auto option = layer_spec.Get(key)) {
				if (!layer_spec.Get(key)->IsMap() || std::string(key) != "opacity") {
					layer.Set(std::string("mark_") + key, option->ToJson());
				}
			}
		}
		plan.queries.push_back(sql);
		return layer;
	}

	// ---- Rules and bands ------------------------------------------------------------------------

	//! Scalar subqueries gathered for the aggregates of rules and bands; one row, one column each.
	std::vector<std::string> reference_items;

	static bool IsNumber(const std::string &text) {
		if (text.empty()) {
			return false;
		}
		char *end = nullptr;
		std::strtod(text.c_str(), &end);
		return end && *end == '\0';
	}

	static bool IsReferenceAgg(const std::string &name) {
		static const std::set<std::string> AGGS = {"avg", "mean", "median", "min", "max"};
		return AGGS.count(name) ||
		       (name.size() == 3 && name[0] == 'p' && std::isdigit(static_cast<unsigned char>(name[1])) &&
		        std::isdigit(static_cast<unsigned char>(name[2])));
	}

	//! One end of a reference on `axis`: a fixed value, an aggregate of what the chart draws on that
	//! axis (`avg`, `median`, `p90`, `min`, `max`), or an aggregate of a column (`{field, agg}`).
	Json ReferenceValue(const Yaml &node, const std::string &axis, const std::string &what) {
		auto json = Json::Object();
		const LayerInfo *layer = nullptr;
		size_t index = 0;
		for (size_t i = 0; i < layer_info.size(); i++) {
			auto &type = axis == "x" ? layer_info[i].x_type : layer_info[i].y_type;
			if (!type.empty()) {
				layer = &layer_info[i];
				index = i;
				break;
			}
		}
		if (!layer) {
			Fail(what + ": the chart has no " + axis + " axis");
			return json;
		}
		auto &type = axis == "x" ? layer->x_type : layer->y_type;
		bool continuous = type == "quantitative" || type == "temporal";
		auto column = "r" + std::to_string(reference_items.size());
		if (node.IsMap()) {
			auto field = node.Str("field");
			auto agg = Lower(node.Str("agg"));
			if (field.empty() || agg.empty()) {
				Fail(what + ": " + axis + " as a mapping needs field: and agg:");
				return json;
			}
			if (!IsField(field)) {
				Fail(what + ": no column named '" + field + "' in " + from);
				return json;
			}
			if (!continuous) {
				Fail(what + ": an aggregate needs a numeric or date " + axis + " axis");
				return json;
			}
			reference_items.push_back("(SELECT " + Agg(agg, QuoteIdentifier(field), what) + " FROM (" + source +
			                          ")) AS " + column);
			json.Set("column", Json::String(column));
			json.Set("agg", Json::String(agg + "(" + field + ")"));
			return json;
		}
		if (!node.IsScalar()) {
			Fail(what + ": " + axis + " must be a value, an aggregate such as avg, or {field, agg}");
			return json;
		}
		auto name = Lower(node.scalar);
		if (!node.quoted && IsReferenceAgg(name)) {
			if (!continuous) {
				Fail(what + ": " + name + " needs a numeric or date " + axis + " axis");
				return json;
			}
			if (type == "temporal" && (name == "avg" || name == "mean")) {
				Fail(what + ": " + name + " is not defined for dates; use median, min or max");
				return json;
			}
			if (layer->base.empty()) {
				Fail(what + ": " + name + " is not available on a " + layer->mark + " chart; give a value");
				return json;
			}
			auto marks = "__marks" + std::to_string(index);
			reference_items.push_back("(SELECT " + Agg(name, QuoteIdentifier(axis), what) + " FROM " + marks + ") AS " +
			                          column);
			reference_layers.insert(index);
			json.Set("column", Json::String(column));
			json.Set("agg", Json::String(name));
			return json;
		}
		if (type == "quantitative" && !IsNumber(node.scalar)) {
			Fail(what + ": " + axis + " must be a number or one of avg, median, min, max, p05..p99, not '" +
			     node.scalar + "'");
			return json;
		}
		json.Set("value",
		         type == "quantitative" ? Json::Number(std::atof(node.scalar.c_str())) : Json::String(node.scalar));
		return json;
	}

	std::set<size_t> reference_layers;

	//! `rules:` (lines at a value) and `bands:` (shading between two values) of a grammar chart.
	void References(const Yaml &spec, Json &render) {
		for (auto key : {"rules", "bands"}) {
			auto list = spec.Get(key);
			if (!list) {
				continue;
			}
			std::string what = key;
			bool band = what == "bands";
			auto example = band ? "{y: [100, 200], label: Target}" : "{y: avg, label: Average}";
			if (!list->IsList()) {
				Fail(what + ": must be a list of mappings such as " + example);
				return;
			}
			for (auto &info : layer_info) {
				if (info.mark == "arc") {
					Fail(what + ": an arc chart has no axes");
					return;
				}
			}
			auto out = Json::Array();
			for (auto &item : list->items) {
				if (!item.IsMap()) {
					Fail(what + ": each item is a mapping such as " + example);
					return;
				}
				for (auto &entry : item.entries) {
					if (entry.first != "x" && entry.first != "y" && entry.first != "label" && entry.first != "color") {
						Fail(what + ": unknown key '" + entry.first + "' (x, y, label, color)");
						return;
					}
				}
				auto x = item.Get("x"), y = item.Get("y");
				if ((x != nullptr) == (y != nullptr)) {
					Fail(what + ": each item needs x: or y:, not both");
					return;
				}
				std::string axis = x ? "x" : "y";
				auto &node = x ? *x : *y;
				auto json = Json::Object();
				json.Set("axis", Json::String(axis));
				if (band) {
					if (!node.IsList() || node.items.size() != 2) {
						Fail(what + ": " + axis + " must be [from, to]");
						return;
					}
					json.Set("from", ReferenceValue(node.items[0], axis, what));
					json.Set("to", ReferenceValue(node.items[1], axis, what));
				} else {
					auto value = ReferenceValue(node, axis, what);
					for (auto &entry : value.Entries()) {
						json.Set(entry.first, entry.second);
					}
				}
				for (auto option : {"label", "color"}) {
					if (auto found = item.Get(option)) {
						if (!found->IsScalar()) {
							Fail(what + ": " + option + " must be text");
							return;
						}
						json.Set(option, Json::String(found->scalar));
					}
				}
				out.Push(json);
			}
			render.Set(key, out);
		}
		if (!error.empty() || reference_items.empty()) {
			return;
		}
		// One query, one row: every aggregate is a scalar subquery over its layer's marks or the source.
		std::string sql;
		for (auto index : reference_layers) {
			sql += (sql.empty() ? "WITH " : ", ") + std::string("__marks") + std::to_string(index) + " AS (" +
			       layer_info[index].base + ")";
		}
		sql += (sql.empty() ? "SELECT " : "\nSELECT ");
		for (size_t i = 0; i < reference_items.size(); i++) {
			sql += (i ? ", " : "") + reference_items[i];
		}
		render.Set("reference_data", Json::Number(static_cast<double>(plan.queries.size())));
		plan.queries.push_back(sql);
	}

	//! A measure such as `value: {field: revenue, agg: sum}`; the default agg applies to bare fields.
	std::string Measure(const Yaml *node, const std::string &name, const std::string &default_agg, Json *encoding) {
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

	std::string Field(const Yaml &spec, const std::string &key, bool required, Json *encoding = nullptr) {
		auto channel = ParseChannel(key, spec.Get(key));
		if (!channel.present) {
			if (required) {
				Fail(spec.Str("type") + " chart needs " + key + ":");
			}
			return "";
		}
		Transform(channel);
		if (encoding) {
			*encoding = Encoding(channel, key);
		}
		return channel.constant ? channel.value.Dump() : channel.expr;
	}

	void Typed(const Yaml &spec, Json &render) {
		auto type = Lower(spec.Str("type"));
		render.Set("kind", Json::String(type));
		auto limit = Limit(spec);
		auto encodings = Json::Object();
		auto add = [&](const std::vector<std::string> &items, bool group, const std::string &order,
		               const std::string &lim) {
			if (error.empty()) {
				plan.queries.push_back(Query(items, group, order, lim));
			}
			ctes.clear();
			joins.clear();
		};
		if (type == "kpi" || type == "gauge") {
			Json value_enc;
			auto value = Measure(spec.Get("value"), "value", "sum", &value_enc);
			if (value.empty()) {
				Fail(type + " chart needs value:");
				return;
			}
			encodings.Set("value", value_enc);
			std::vector<std::string> items = {value + " AS value"};
			if (spec.Get("compare")) {
				Json compare_enc;
				items.push_back(Measure(spec.Get("compare"), "compare", "sum", &compare_enc) + " AS compare");
				encodings.Set("compare", compare_enc);
			}
			add(items, false, "", "");
			if (auto spark = spec.Get("sparkline")) {
				Json x_enc, y_enc;
				auto sx = Field(*spark, "x", true, &x_enc);
				auto sy = Measure(spark->Get("y"), "y", "sum", &y_enc);
				add({sx + " AS x", sy + " AS y"}, true, "x", "");
				encodings.Set("sparkline", Json::Object().Set("x", x_enc).Set("y", y_enc));
			}
		} else if (type == "table") {
			std::vector<std::string> items;
			for (auto &column : spec.Get("columns") ? spec.Get("columns")->Strings() : std::vector<std::string>()) {
				if (!IsField(column)) {
					Fail("table column '" + column + "' is not in " + from);
				}
				items.push_back(QuoteIdentifier(column));
			}
			if (items.empty()) {
				items.push_back("*");
			}
			add(items, false, "", spec.Get("limit") ? limit : "1000");
		} else if (type == "pivot") {
			auto rows = spec.Get("rows") ? spec.Get("rows")->Strings() : std::vector<std::string>();
			auto column = spec.Str("columns");
			Json value_enc;
			auto value = Measure(spec.Get("values"), "values", "", &value_enc);
			auto values_node = spec.Get("values");
			auto agg = values_node && values_node->IsMap() ? Lower(values_node->Str("agg", "sum")) : "sum";
			if (rows.empty() || column.empty() || !values_node) {
				Fail("pivot chart needs rows:, columns: and values:");
				return;
			}
			auto field = values_node->IsMap() ? values_node->Str("field") : values_node->scalar;
			std::string select;
			for (auto &row : rows) {
				select += QuoteIdentifier(row) + ", ";
			}
			std::string group;
			for (size_t i = 0; i < rows.size(); i++) {
				group += (i ? ", " : "") + QuoteIdentifier(rows[i]);
			}
			auto pivoted = Agg(agg, "__v", "values");
			auto sql = "WITH __src AS (" + source + ")\nPIVOT (SELECT " + select + QuoteIdentifier(column) + ", " +
			           (field.empty() ? "1" : QuoteIdentifier(field)) + " AS __v FROM __src) ON " +
			           QuoteIdentifier(column) + " USING " + pivoted + " GROUP BY " + group + "\nORDER BY " + group;
			if (error.empty()) {
				plan.queries.push_back(sql);
			}
			render.Set("rows", spec.Get("rows")->ToJson());
		} else if (type == "map") {
			std::vector<std::string> items;
			auto color = Measure(spec.Get("color"), "color", "", nullptr);
			Json color_enc, size_enc;
			if (spec.Get("color")) {
				Measure(spec.Get("color"), "color", "", &color_enc);
				encodings.Set("color", color_enc);
			}
			auto size = Measure(spec.Get("size"), "size", "", &size_enc);
			if (spec.Get("size")) {
				encodings.Set("size", size_enc);
			}
			auto layer = Lower(spec.Str("layer", "point"));
			if (spec.Get("geometry")) {
				items.push_back("ST_AsGeoJSON(" + Field(spec, "geometry", true) + ") AS geojson");
				if (!color.empty()) {
					items.push_back(color + " AS color");
				}
				add(items, false, "", limit);
			} else {
				auto lat = Field(spec, "lat", true), lon = Field(spec, "lon", true);
				if (layer == "heat" || layer == "hex" || (context.row_count > 20000 && layer == "point")) {
					// Aggregate to a grid of about 1/200 of the extent.
					ctes.push_back("__ext AS (SELECT min(" + lat + ") AS a0, max(" + lat + ") AS a1, min(" + lon +
					               ") AS o0, max(" + lon + ") AS o1 FROM __src)");
					joins.push_back("__ext");
					auto snap = [](const std::string &e, const std::string &lo, const std::string &hi) {
						auto w = "(nullif(" + hi + " - " + lo + ", 0) / 200)";
						return "coalesce(" + lo + " + (floor((" + e + " - " + lo + ") / " + w + ") + 0.5) * " + w +
						       ", " + lo + ")";
					};
					items = {snap(lat, "a0", "a1") + " AS lat", snap(lon, "o0", "o1") + " AS lon",
					         (color.empty() ? std::string("count(*)") : color) + " AS color", "count(*) AS count"};
					if (!color.empty() && spec.Get("color")->IsMap() && spec.Get("color")->Get("agg")) {
						items[2] = color + " AS color";
					} else if (!color.empty()) {
						items[2] = "avg(" + color + ") AS color";
					}
					add(items, true, "", limit);
					render.Set("binned", Json::Bool(true));
				} else {
					items = {lat + " AS lat", lon + " AS lon"};
					if (layer == "arc") {
						items.push_back(Field(spec, "lat2", true) + " AS lat2");
						items.push_back(Field(spec, "lon2", true) + " AS lon2");
					}
					if (!color.empty()) {
						items.push_back(color + " AS color");
					}
					if (!size.empty()) {
						items.push_back(size + " AS size");
					}
					add(items, false, "", limit);
				}
			}
			render.Set("layer", Json::String(layer));
		} else if (type == "sankey" || type == "chord" || type == "network") {
			auto source_field = Field(spec, "source", true);
			auto target_field = Field(spec, "target", true);
			auto weight = Measure(spec.Get(type == "network" ? "weight" : "value"), "value", "sum", nullptr);
			if (weight.empty()) {
				if (type != "network") {
					Fail(type + " chart needs value:");
				}
				weight = "count(*)";
			}
			add({"(" + source_field + ")::VARCHAR AS source", "(" + target_field + ")::VARCHAR AS target",
			     weight + " AS value"},
			    true, "value DESC", limit);
		} else if (type == "treemap" || type == "sunburst" || type == "icicle") {
			auto path = spec.Get("path") ? spec.Get("path")->Strings() : std::vector<std::string>();
			if (path.empty()) {
				Fail(type + " chart needs path: [column, ...]");
				return;
			}
			std::vector<std::string> items;
			for (size_t i = 0; i < path.size(); i++) {
				if (!IsField(path[i])) {
					Fail("path column '" + path[i] + "' is not in " + from);
				}
				items.push_back("(" + QuoteIdentifier(path[i]) + ")::VARCHAR AS path_" + std::to_string(i));
			}
			auto value = Measure(spec.Get("value"), "value", "sum", nullptr);
			items.push_back((value.empty() ? std::string("count(*)") : value) + " AS value");
			add(items, true, "value DESC", limit);
			render.Set("depth", Json::Number(static_cast<double>(path.size())));
		} else if (type == "funnel") {
			auto stage = Field(spec, "stage", true);
			auto value = Measure(spec.Get("value"), "value", "sum", nullptr);
			if (value.empty()) {
				Fail("funnel chart needs value:");
				return;
			}
			add({"(" + stage + ")::VARCHAR AS stage", value + " AS value"}, true, "value DESC", limit);
		} else if (type == "candlestick") {
			Json x_enc;
			std::vector<std::string> items = {Field(spec, "x", true, &x_enc) + " AS x"};
			for (auto key : {"open", "high", "low", "close"}) {
				items.push_back(Field(spec, key, true) + " AS " + key);
			}
			encodings.Set("x", x_enc);
			add(items, false, "x", limit);
		} else if (type == "calendar") {
			auto date = Field(spec, "date", true);
			auto value = Measure(spec.Get("value"), "value", "sum", nullptr);
			if (value.empty()) {
				Fail("calendar chart needs value:");
				return;
			}
			add({"CAST(" + date + " AS DATE) AS date", value + " AS value"}, true, "date", limit);
		} else if (type == "gantt") {
			std::vector<std::string> items = {"(" + Field(spec, "task", true) + ")::VARCHAR AS task",
			                                  Field(spec, "start", true) + " AS start",
			                                  Field(spec, "end", true) + " AS \"end\""};
			if (spec.Get("color")) {
				items.push_back(Field(spec, "color", false) + " AS color");
			}
			add(items, false, "start", spec.Get("limit") ? limit : "500");
		} else if (type == "vega-lite" || type == "echarts") {
			add({"*"}, false, "", spec.Get("limit") ? limit : "5000");
			if (!spec.Get("spec")) {
				Fail(type + " chart needs spec:");
			}
		} else {
			Fail("unknown chart type '" + type + "'");
			return;
		}
		render.Set("encoding", encodings);
	}
};

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
	if (t.compare(0, 4, "date") == 0 || t.compare(0, 9, "timestamp") == 0 || t.compare(0, 4, "time") == 0) {
		return "temporal";
	}
	static const char *const NUMERIC[] = {"tinyint",   "smallint", "integer", "bigint",   "hugeint", "utinyint",
	                                      "usmallint", "uinteger", "ubigint", "uhugeint", "float",   "double",
	                                      "decimal",   "real",     "int",     "numeric"};
	for (auto n : NUMERIC) {
		if (t.compare(0, std::char_traits<char>::length(n), n) == 0) {
			return "quantitative";
		}
	}
	return "nominal";
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
	SelectionInfo info;
	std::string error;
	auto spec = ParseYaml(spec_text, error);
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
		if (spec.Get("facet")) {
			render.Set("facet", Json::Object()
			                        .Set("row", Json::Bool(spec.Get("facet")->Get("row") != nullptr))
			                        .Set("col", Json::Bool(spec.Get("facet")->Get("col") != nullptr)));
		}
		auto selection = ChartSelectionInfo(spec_text);
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
	SqlScanner scan(sql);
	size_t copied = 0;
	while (!scan.Done()) {
		auto state = scan.CurrentState();
		size_t at = scan.Position();
		scan.Advance();
		if (state != SqlScanner::State::CODE || sql[at] != '$' ||
		    scan.CurrentState() == SqlScanner::State::DOLLAR_STRING) {
			continue;
		}
		size_t p = at + 1;
		std::string name;
		size_t end = p;
		if (p < sql.size() && sql[p] == '"') {
			end = p + 1;
			while (end < sql.size() && sql[end] != '"') {
				name += sql[end++];
			}
			end++;
		} else {
			while (end < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[end])) || sql[end] == '_')) {
				end++;
			}
			name = sql.substr(p, end - p);
			if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) {
				continue;
			}
		}
		out += sql.substr(copied, at - copied) + "getvariable(" + SqlString(name) + ")";
		copied = end;
		while (scan.Position() < end && !scan.Done()) {
			scan.Advance();
		}
	}
	return out + sql.substr(copied);
}

} // namespace notebook
