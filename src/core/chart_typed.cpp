// Typed charts (`type:`): KPIs, tables, maps, Sankeys and the rest.

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

// The columns a table link: reads: the {column} placeholders of a URL template, or the whole value when it is
// a column holding the URL.
static std::vector<std::string> LinkColumns(const std::string &link) {
	std::vector<std::string> result;
	if (link.find('{') == std::string::npos) {
		result.push_back(link);
		return result;
	}
	size_t at = 0;
	while ((at = link.find('{', at)) != std::string::npos) {
		auto close = link.find('}', at + 1);
		if (close == std::string::npos) {
			break;
		}
		if (close > at + 1) {
			AddUnique(result, link.substr(at + 1, close - at - 1));
		}
		at = close + 1;
	}
	return result;
}

void Compiler::Typed(const Yaml &spec, Json &render) {
	auto type = Lower(spec.Str("type"));
	chart_type = spec.Str("type");
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
		auto columns = spec.Get("columns") ? spec.Get("columns")->Strings() : std::vector<std::string>();
		for (auto &column : columns) {
			if (!IsField(column)) {
				Fail("table column '" + column + "' is not in " + from);
			}
			items.push_back(QuoteIdentifier(column));
		}
		// tip: {shown column: source or [sources]} and link: {shown column: url column or "https://...{column}..."}
		// read other columns of the row. The browser leaves those out of the table unless columns: lists them, so
		// they are selected here even when columns: does not name them.
		auto decorate = [&](const std::string &key, const std::string &example) -> bool {
			auto node = spec.Get(key);
			if (!node) {
				return true;
			}
			auto what = "table " + key + ": ";
			if (!node->IsMap() || node->entries.empty()) {
				Fail(what + "needs a mapping such as " + example);
				return false;
			}
			for (auto &entry : node->entries) {
				if (!IsField(entry.first)) {
					Fail(what + "column '" + entry.first + "' is not in " + from);
					return false;
				}
				if (!columns.empty() && std::find(columns.begin(), columns.end(), entry.first) == columns.end()) {
					Fail(what + "column '" + entry.first + "' is not one of columns:");
					return false;
				}
				std::vector<std::string> sources;
				if (key == "link") {
					if (!entry.second.IsScalar() || entry.second.scalar.empty()) {
						Fail(what + "'" + entry.first + "' needs a URL column or a URL with {column} in it");
						return false;
					}
					sources = LinkColumns(entry.second.scalar);
				} else {
					sources = entry.second.Strings();
					if (sources.empty() || sources.size() != (entry.second.IsList() ? entry.second.items.size() : 1)) {
						Fail(what + "'" + entry.first + "' needs a column name or a list of them");
						return false;
					}
				}
				for (auto &source : sources) {
					if (!IsField(source)) {
						Fail(what + "column '" + source + "' is not in " + from);
						return false;
					}
					if (!columns.empty() && std::find(columns.begin(), columns.end(), source) == columns.end()) {
						AddUnique(columns, source);
						items.push_back(QuoteIdentifier(source));
					}
				}
			}
			return true;
		};
		if (!decorate("tip", "{name: description}") || !decorate("link", "{key: \"https://example.com/{key}\"}")) {
			return;
		}
		if (items.empty()) {
			items.push_back("*");
		}
		add(items, false, "", spec.Get("limit") ? limit : "1000");
	} else if (type == "pivot") {
		auto rows = spec.Get("rows") ? spec.Get("rows")->Strings() : std::vector<std::string>();
		auto column = spec.Str("columns");
		Measure(spec.Get("values"), "values", "", nullptr); // validates the channel
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
		Json color_enc, size_enc;
		auto color = Measure(spec.Get("color"), "color", "", &color_enc);
		if (spec.Get("color")) {
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
					return "coalesce(" + lo + " + (floor((" + e + " - " + lo + ") / " + w + ") + 0.5) * " + w + ", " +
					       lo + ")";
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

} // namespace notebook
