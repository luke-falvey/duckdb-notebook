#pragma once

#include <string>
#include <utility>
#include <vector>

namespace notebook {

//! The notebook format is specified in docs/FORMAT.md. Keep the two in step.
enum class CellKind { NOTEBOOK, MARKDOWN, INPUT, CHART, SQL };

const char *CellKindName(CellKind kind);

//! A `{{ expr }}` in a markdown cell.
struct Expression {
	//! Byte range of the whole `{{ ... }}`, braces included, within the cell's content.
	size_t begin = 0;
	size_t end = 0;
	//! The DuckDB scalar expression between the braces, trimmed.
	std::string sql;
};

struct Cell {
	//! Position in the file, starting at 0.
	int index = 0;
	CellKind kind = CellKind::SQL;
	//! Cell name; empty for unnamed cells. For input cells, the variable name.
	std::string name;
	//! Input cells only: the widget type (`select`, `slider`, ...).
	std::string widget;
	//! `key=value` options from the marker line, in file order.
	std::vector<std::pair<std::string, std::string>> options;
	//! Everything between this cell's marker line and the next, trimmed.
	std::string body;
	//! For notebook, markdown and chart cells: the text inside the block comment.
	std::string content;
	//! SQL cells: true if the last statement returns rows, making the cell a view.
	bool is_query = false;
	//! Variables the cell reads as `$name`.
	std::vector<std::string> variables;
	//! Markdown cells: the `{{ expr }}` expressions, in order.
	std::vector<Expression> expressions;
	//! Filled by AnalyzeNotebook (graph.hpp), empty until then. Names are lower-case relation
	//! names (`monthly`, `wh.orders`) or variables written with a leading `$` (`$region`).
	std::vector<std::string> defines;
	std::vector<std::string> reads;
	//! Indexes of the cells that define something this cell reads, ascending.
	std::vector<int> depends_on;
	//! 1-based line of the `-- %%` marker and of the cell's last line.
	int start_line = 0;
	int end_line = 0;
	//! Problems found in this cell. A notebook with any errors should not be run.
	std::vector<std::string> errors;
	//! Things worth knowing that do not stop the notebook from running.
	std::vector<std::string> warnings;

	//! Value of an option, or `fallback` if absent.
	std::string Option(const std::string &key, const std::string &fallback = std::string()) const;
};

struct Notebook {
	std::vector<Cell> cells;

	bool HasErrors() const;
};

//! Parses notebook text. Never throws: problems are reported on the cells.
Notebook ParseNotebook(const std::string &text);

//! The `{{ expr }}` expressions in markdown text. The closing `}}` is looked for in SQL
//! code only, so `{{ '}}' }}` and `{{ {'a': {'b': 1}} }}` are single expressions. Sets
//! `error` for an unterminated or empty expression and returns those found before it.
std::vector<Expression> FindExpressions(const std::string &markdown, std::string &error);

} // namespace notebook
