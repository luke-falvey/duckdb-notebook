// Rules, bands and notes of a grammar chart.

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

bool Compiler::IsNumber(const std::string &text) {
	if (text.empty()) {
		return false;
	}
	char *end = nullptr;
	std::strtod(text.c_str(), &end);
	return end && *end == '\0';
}

bool Compiler::IsReferenceAgg(const std::string &name) {
	static const std::set<std::string> AGGS = {"avg", "mean", "median", "min", "max"};
	return AGGS.count(name) ||
	       (name.size() == 3 && name[0] == 'p' && std::isdigit(static_cast<unsigned char>(name[1])) &&
	        std::isdigit(static_cast<unsigned char>(name[2])));
}

Json Compiler::ReferenceValue(const Yaml &node, const std::string &axis, const std::string &what) {
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
		reference_items.push_back("(SELECT " + Agg(agg, QuoteIdentifier(field), what) + " FROM (" + source + ")) AS " +
		                          column);
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
		Fail(what + ": " + axis + " must be a number or one of avg, median, min, max, p05..p99, not '" + node.scalar +
		     "'");
		return json;
	}
	json.Set("value",
	         type == "quantitative" ? Json::Number(std::atof(node.scalar.c_str())) : Json::String(node.scalar));
	return json;
}

void Compiler::References(const Yaml &spec, Json &render) {
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

std::string Compiler::AxisTypeOf(const std::string &axis) const {
	for (auto &info : layer_info) {
		auto &type = axis == "x" ? info.x_type : info.y_type;
		if (!type.empty()) {
			return type;
		}
	}
	return std::string();
}

Json Compiler::NoteValue(const Yaml &node, const std::string &axis) {
	auto type = AxisTypeOf(axis);
	if (type.empty()) {
		Fail("notes: the chart has no " + axis + " axis");
		return Json();
	}
	if (!node.IsScalar()) {
		Fail("notes: " + axis + " must be a value or [from, to]");
		return Json();
	}
	if (type == "quantitative") {
		if (!IsNumber(node.scalar)) {
			Fail("notes: " + axis + " must be a number, not '" + node.scalar + "'");
			return Json();
		}
		return Json::Number(std::atof(node.scalar.c_str()));
	}
	return Json::String(node.scalar);
}

void Compiler::Notes(const Yaml &spec, Json &render) {
	auto list = spec.Get("notes");
	if (!list) {
		return;
	}
	const std::string example = "{x: 2026-06-01, y: 1200, text: Launch}";
	if (!list->IsList()) {
		Fail("notes: must be a list of mappings such as " + example);
		return;
	}
	for (auto &info : layer_info) {
		if (info.mark == "arc") {
			Fail("notes: an arc chart has no axes");
			return;
		}
	}
	static const std::set<std::string> KEYS = {"x", "y", "text", "circle", "side", "dx", "dy", "color"};
	static const std::set<std::string> SIDES = {"above", "below", "left", "right"};
	auto out = Json::Array();
	for (auto &item : list->items) {
		if (!item.IsMap()) {
			Fail("notes: each item is a mapping such as " + example);
			return;
		}
		for (auto &entry : item.entries) {
			if (!KEYS.count(entry.first)) {
				Fail("notes: unknown key '" + entry.first + "' (x, y, text, circle, side, dx, dy, color)");
				return;
			}
		}
		auto json = Json::Object();
		bool range = false;
		size_t positions = 0;
		for (auto axis : {"x", "y"}) {
			auto node = item.Get(axis);
			if (!node) {
				continue;
			}
			positions++;
			if (node->IsList()) {
				if (node->items.size() != 2 || range) {
					Fail(range ? "notes: only one of x and y can be a range"
					           : std::string("notes: ") + axis + " as a range must be [from, to]");
					return;
				}
				range = true;
				json.Set(axis,
				         Json::Array().Push(NoteValue(node->items[0], axis)).Push(NoteValue(node->items[1], axis)));
			} else {
				json.Set(axis, NoteValue(*node, axis));
			}
		}
		if (!positions) {
			Fail("notes: each note needs x: or y: (or both), the data position it points at");
			return;
		}
		auto circle = item.Str("circle");
		if (!circle.empty() && circle != "true" && circle != "false") {
			Fail("notes: circle must be true or false");
			return;
		}
		if (circle == "true" && range) {
			Fail("notes: circle needs a point, not a range");
			return;
		}
		auto text = item.Get("text");
		if (text && !text->IsScalar()) {
			Fail("notes: text must be a short piece of text");
			return;
		}
		if ((!text || text->scalar.empty()) && circle != "true" && !range) {
			Fail("notes: a note needs text: (or circle: true)");
			return;
		}
		if (text && !text->scalar.empty()) {
			json.Set("text", Json::String(text->scalar));
		}
		if (circle == "true") {
			json.Set("circle", Json::Bool(true));
		}
		auto side = Lower(item.Str("side"));
		if (item.Get("side")) {
			if (!SIDES.count(side)) {
				Fail("notes: side must be above, below, left or right, not '" + item.Str("side") + "'");
				return;
			}
			json.Set("side", Json::String(side));
		}
		for (auto key : {"dx", "dy"}) {
			if (auto offset = item.Get(key)) {
				if (!offset->IsScalar() || !IsNumber(offset->scalar)) {
					Fail(std::string("notes: ") + key + " must be a number of pixels");
					return;
				}
				json.Set(key, Json::Number(std::atof(offset->scalar.c_str())));
			}
		}
		if (auto color = item.Get("color")) {
			json.Set("color", Json::String(color->scalar));
		}
		if (!error.empty()) {
			return;
		}
		out.Push(json);
	}
	render.Set("notes", out);
}

} // namespace notebook
