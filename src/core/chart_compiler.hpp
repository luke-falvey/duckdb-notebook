#pragma once

// Private to the chart compiler (chart*.cpp): the Compiler class and its parsed channel. Split by
// subject: chart.cpp (channels, aggregates, filters, encodings, the public API), chart_grammar.cpp
// (layers of a grammar chart and their downsampling), chart_references.cpp (rules, bands and notes)
// and chart_typed.cpp (typed charts).

#include "notebook/chart.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace notebook {

const int DEFAULT_LIMIT = 10000;
//! Above this many rows, point-like marks are binned to a pixel grid instead of drawn one by one.
const int64_t POINT_THRESHOLD = 5000;

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
	//! A typed chart's `type:`, as written, for error messages from nested specs such as a sparkline.
	std::string chart_type;
	ChartPlan plan;

	const ColumnInfo *Column(const std::string &name) const;

	bool IsField(const std::string &name) const;

	std::string FieldClass(const std::string &field) const;

	Channel ParseChannel(const std::string &name, const Yaml *node);

	void Fail(const std::string &message);

	std::string Agg(const std::string &agg, const std::string &expr, const std::string &channel);

	//! The SQL CTEs gathered while compiling the current query.
	std::vector<std::string> ctes;
	std::vector<std::string> joins;

	//! Filters from `filter:` applied to `from`.
	std::string Source(const Yaml &spec);

	//! Field expression with timeunit and bin applied; sets channel.expr and end_expr.
	void Transform(Channel &channel);

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

	static std::string AxisType(const Channel &channel);

	Json Encoding(const Channel &channel, const std::string &column);

	std::string Query(const std::vector<std::string> &items, bool group, const std::string &order,
	                  const std::string &limit);

	std::string source;

	std::string Limit(const Yaml &spec);

	//! Numeric form of an expression, for pixel binning.
	static std::string Numeric(const std::string &expr, const std::string &cls);

	Json GrammarLayer(const Yaml &layer_spec, const Yaml &top);

	// ---- Rules and bands ------------------------------------------------------------------------

	//! Scalar subqueries gathered for the aggregates of rules and bands; one row, one column each.
	std::vector<std::string> reference_items;

	static bool IsNumber(const std::string &text);

	static bool IsReferenceAgg(const std::string &name);

	//! One end of a reference on `axis`: a fixed value, an aggregate of what the chart draws on that
	//! axis (`avg`, `median`, `p90`, `min`, `max`), or an aggregate of a column (`{field, agg}`).
	Json ReferenceValue(const Yaml &node, const std::string &axis, const std::string &what);

	std::set<size_t> reference_layers;

	//! `rules:` (lines at a value) and `bands:` (shading between two values) of a grammar chart.
	void References(const Yaml &spec, Json &render);

	// ---- Notes ----------------------------------------------------------------------------------

	//! The type of the first layer's `axis`, or empty if the chart has none.
	std::string AxisTypeOf(const std::string &axis) const;

	//! A position on `axis` written in a note: a number on a numeric axis, otherwise text (a date or
	//! a category).
	Json NoteValue(const Yaml &node, const std::string &axis);

	//! `notes:` of a grammar chart: short handwritten remarks attached to data positions. The
	//! compiler checks them and passes them on; the renderer finds room for them.
	void Notes(const Yaml &spec, Json &render);

	//! A measure such as `value: {field: revenue, agg: sum}`; the default agg applies to bare fields.
	std::string Measure(const Yaml *node, const std::string &name, const std::string &default_agg, Json *encoding);

	std::string Field(const Yaml &spec, const std::string &key, bool required, Json *encoding = nullptr);

	void Typed(const Yaml &spec, Json &render);
};

} // namespace notebook
