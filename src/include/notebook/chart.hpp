#pragma once

#include "notebook/format.hpp"
#include "notebook/json.hpp"
#include "notebook/yaml.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The chart compiler (docs/DESIGN.md section 6, docs/FORMAT.md section 5). A chart spec and its
// `from` relation become SQL queries that do the aggregation, binning and downsampling in DuckDB,
// plus a render description that tells the browser how to draw their results. Core needs no
// database for this: the caller supplies what it knows about the relation in a ChartContext.
namespace notebook {

struct ColumnInfo {
	std::string name;
	//! DuckDB type name, such as `TIMESTAMP` or `DECIMAL(18,2)`.
	std::string type;
};

//! What a chart's `select:` writes, so that other charts can `filter:` on it.
struct SelectionInfo {
	//! The variable name (`as`). Empty if the chart has no selection.
	std::string name;
	//! `interval` or `point`.
	std::string type;
	//! The channels selected on (`x`, `y`) and the fields behind them, pairwise.
	std::vector<std::string> channels;
	std::vector<std::string> fields;
};

struct ChartContext {
	//! Columns of `from`. When empty, every scalar channel value is taken as a field.
	std::vector<ColumnInfo> columns;
	//! Rows in `from`, or -1 if unknown. Decides whether to downsample.
	int64_t row_count = -1;
	//! Plot size in pixels, which sets the downsampling resolution.
	int width = 640;
	int height = 320;
	//! Selections defined anywhere in the notebook, by variable name.
	std::map<std::string, SelectionInfo> selections;
};

struct ChartPlan {
	//! One query per dataset; the render description refers to them by index.
	std::vector<std::string> queries;
	Json render;
	//! Set if the spec cannot be compiled; queries are then empty.
	std::string error;
};

//! Classifies a DuckDB type: `temporal`, `quantitative` or `nominal`.
std::string TypeClass(const std::string &duckdb_type);

//! SQL for a `from` value: a quoted, possibly qualified, name.
std::string RelationSql(const std::string &from);

//! Compiles a chart cell's spec (YAML text) over `from`.
ChartPlan CompileChart(const std::string &spec, const std::string &from, const ChartContext &context);

//! The selection a spec defines, if any.
SelectionInfo ChartSelectionInfo(const std::string &spec);
SelectionInfo ChartSelectionInfo(const Yaml &spec);

//! Every selection the notebook's charts define, by name. Where two charts define one name, the later wins.
std::map<std::string, SelectionInfo> NotebookSelections(const Notebook &notebook);

//! Rewrites `$name` to `getvariable('name')` in SQL code (not strings or comments), for engines
//! older than DuckDB 2.0 such as the DuckDB-WASM used by the html-wasm export.
std::string RewriteVariables(const std::string &sql);

} // namespace notebook
