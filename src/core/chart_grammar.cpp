// The layers of a grammar chart: marks, channels, ordering and downsampling.

#include "chart_compiler.hpp"

#include "notebook/script.hpp"
#include "notebook/sql_lexer.hpp"
#include "notebook/text.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>

namespace notebook {

std::string Compiler::Numeric(const std::string &expr, const std::string &cls) {
	return cls == "temporal" ? "epoch(" + expr + ")" : "(" + expr + ")::DOUBLE";
}

Json Compiler::GrammarLayer(const Yaml &layer_spec, const Yaml &top) {
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
		sql = "WITH __base AS (" + inner + "), __range AS (SELECT min(" + Numeric("x", x.type) + ") AS lo, max(" +
		      Numeric("x", x.type) + ") AS hi FROM __base), __px AS (SELECT __base.*, " + px +
		      " AS __p FROM __base, __range)\nSELECT * EXCLUDE (__p) FROM __px QUALIFY row_number() OVER (PARTITION "
		      "BY " +
		      series + ", __p ORDER BY x) = 1 OR row_number() OVER (PARTITION BY " + series +
		      ", __p ORDER BY x DESC) = 1 OR row_number() OVER (PARTITION BY " + series +
		      ", __p ORDER BY y) = 1 OR row_number() OVER (PARTITION BY " + series +
		      ", __p ORDER BY y DESC) = 1\nORDER BY x, " + Positions(items.size()) + "\nLIMIT " + limit;
		flags.Set("m4", Json::Bool(true));
	} else if (!has_agg && (mark == "hexbin" || ((draw == "point" || draw == "rect" || draw == "tick") && large)) &&
	           x.type == "quantitative" && quantitative_y) {
		// Bin to a grid of 4-pixel cells and draw one mark per cell, sized by its row count.
		int nx = std::max(1, context.width / 4), ny = std::max(1, context.height / 4);
		ctes.push_back("__grid AS (SELECT min(" + x.expr + ")::DOUBLE AS x0, max(" + x.expr + ")::DOUBLE AS x1, min(" +
		               y.expr + ")::DOUBLE AS y0, max(" + y.expr + ")::DOUBLE AS y1 FROM __src)");
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

} // namespace notebook
