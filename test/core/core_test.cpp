// Tests for src/core, which has no DuckDB dependency. Build and run with `make core_test`.
// End-to-end behaviour through SQL is covered by test/sql/*.test.
#include "notebook/format.hpp"
#include "notebook/graph.hpp"
#include "notebook/script.hpp"
#include "notebook/sql_lexer.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>

static int failures = 0;

#define CHECK(condition)                                                                                               \
	do {                                                                                                               \
		if (!(condition)) {                                                                                            \
			std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition << std::endl;                    \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

using namespace notebook;

static void TestSplitStatements() {
	auto s = SplitStatements("SELECT 1; -- note; not a split\nSELECT ';' AS a, $$x;y$$ /* ; */;\n\n-- trailing\n");
	CHECK(s.size() == 2);
	CHECK(s[0].text == "SELECT 1");
	CHECK(s[1].text == "-- note; not a split\nSELECT ';' AS a, $$x;y$$ /* ; */");
	CHECK(s[1].line == 2);
	CHECK(SplitStatements("/* only /* nested */ comment */").empty());
	CHECK(SplitStatements("SELECT 'it''s; fine'").size() == 1);
}

static void TestIsQueryStatement() {
	for (auto sql : {"SELECT 1", "from t", "VALUES (1)", "(SELECT 1) UNION ALL (SELECT 2)", "SUMMARIZE t",
	                 "PIVOT t ON a USING sum(b)", "WITH a AS (SELECT 1) SELECT * FROM a",
	                 "-- c\nWITH a AS (INSERT INTO t VALUES (1) RETURNING *) SELECT * FROM a", "DESCRIBE t"}) {
		CHECK(IsQueryStatement(sql));
	}
	for (auto sql :
	     {"CREATE TABLE t AS SELECT 1", "INSERT INTO t SELECT 1", "WITH a AS (SELECT 1) INSERT INTO t FROM a",
	      "SET VARIABLE x = 1", "ATTACH 'x.db'", "COPY t TO 'x.csv'", ""}) {
		CHECK(!IsQueryStatement(sql));
	}
}

static void TestReferencedVariables() {
	auto v = ReferencedVariables(
	    "SELECT $region, '$not', $1, $\"my var\", $$ $in_string $$, $region -- $comment\n, $tag$ $x $tag$");
	CHECK(v.size() == 2);
	CHECK(v.size() == 2 && v[0] == "region" && v[1] == "my var");
	std::string name;
	CHECK(IsSetVariable("-- default\nset variable Region = 'EU'", name) && name == "Region");
	CHECK(!IsSetVariable("SET threads = 4", name));
}

static void TestParseNotebook() {
	auto nb = ParseNotebook(
	    "-- licence header\n"
	    "-- %% [markdown]\n/*\n# Title\n-- %% not a marker, we are inside a comment\n*/\n\n"
	    "-- %% input region select from=regions.name label=\"Sales region\"\nSET VARIABLE region = 'EU';\n"
	    "-- %% monthly materialize=table\nSELECT '\n-- %% not a marker either' AS s WHERE r = $region;\n"
	    "-- %% chart from=monthly\n/*\nmark: line\n*/\n"
	    "-- %%\nCREATE TABLE t AS SELECT 1;\n");
	CHECK(nb.cells.size() == 5);
	CHECK(!nb.HasErrors());
	CHECK(nb.cells[0].kind == CellKind::MARKDOWN && nb.cells[0].start_line == 2 && nb.cells[0].end_line == 7);
	CHECK(nb.cells[0].content == "# Title\n-- %% not a marker, we are inside a comment");
	CHECK(nb.cells[1].kind == CellKind::INPUT && nb.cells[1].name == "region" && nb.cells[1].widget == "select");
	CHECK(nb.cells[1].Option("label") == "Sales region" && nb.cells[1].Option("from") == "regions.name");
	CHECK(nb.cells[2].name == "monthly" && nb.cells[2].is_query && nb.cells[2].variables.size() == 1);
	CHECK(nb.cells[3].kind == CellKind::CHART && nb.cells[3].name.empty() && nb.cells[3].content == "mark: line");
	CHECK(nb.cells[4].kind == CellKind::SQL && nb.cells[4].name.empty() && !nb.cells[4].is_query);
	CHECK(nb.cells[4].start_line == 17 && nb.cells[4].end_line == 18);
	CHECK(ParseNotebook("SELECT 1;").cells.empty());
}

static void TestParseErrors() {
	auto nb = ParseNotebook("-- %% a\nSELECT 1;\n-- %% a\nSELECT 2;\n-- %% Bad-Name\nSELECT 3;\n"
	                        "-- %% [notebook]\n/* title: x */\n-- %% [markdown]\nnot a comment\n"
	                        "-- %% input x slider\nSET VARIABLE y = 1;\n-- %% chart\n/* mark: bar */\n-- %% [nope]\n");
	CHECK(nb.cells.size() == 8);
	for (auto &cell : nb.cells) {
		CHECK(cell.errors.size() == 1);
	}
}

static void TestScript() {
	auto nb = ParseNotebook("-- %% input region select\nSET VARIABLE region = 'EU';\n"
	                        "-- %% [markdown]\n/* hi */\n"
	                        "-- %% filtered\nSET VARIABLE lim = 5;\n-- pick rows\nSELECT * FROM t WHERE r = $region "
	                        "AND $brush IS NULL LIMIT $lim;\n"
	                        "-- %% snap materialize=table\nFROM filtered\n"
	                        "-- %%\nSELECT 1;\n");
	CHECK(CellSql(nb.cells[0]) == "SET VARIABLE region = 'EU';\n");
	CHECK(CellSql(nb.cells[1]).empty());
	CHECK(CellSql(nb.cells[2]) == "SET VARIABLE lim = 5;\n-- pick rows\nCREATE OR REPLACE TEMP VIEW \"filtered\" AS\n"
	                              "SELECT * FROM t WHERE r = $region AND $brush IS NULL LIMIT $lim;\n");
	CHECK(CellSql(nb.cells[3]) == "CREATE OR REPLACE TEMP TABLE \"snap\" AS\nFROM filtered;\n");
	CHECK(CellSql(nb.cells[4]) == "SELECT 1;\n");
	auto undefined = UndefinedVariables(nb);
	CHECK(undefined.size() == 1 && undefined[0] == "brush");
	CHECK(ExpandNotebook(nb, "x.sql").find("SET VARIABLE \"brush\" = NULL;") != std::string::npos);
}

static void TestFindExpressions() {
	std::string error;
	std::string md = "a {{ $x }} b {{ {'k': {'v': 1}} }} c {{ '}}' /* }} */ }}";
	auto e = FindExpressions(md, error);
	CHECK(error.empty());
	CHECK(e.size() == 3);
	CHECK(e.size() == 3 && e[0].sql == "$x" && e[1].sql == "{'k': {'v': 1}}" && e[2].sql == "'}}' /* }} */");
	CHECK(e.size() == 3 && md.substr(e[0].begin, e[0].end - e[0].begin) == "{{ $x }}");
	CHECK(FindExpressions("no expressions here", error).empty() && error.empty());
	FindExpressions("x {{ 1 + ", error);
	CHECK(error.find("unterminated") != std::string::npos);
	error.clear();
	FindExpressions("{{ }}", error);
	CHECK(error.find("empty") != std::string::npos);

	auto nb = ParseNotebook("-- %% [markdown]\n/*\nTotal for {{ $region }}: {{ (SELECT sum(x) FROM t) }}\n*/\n"
	                        "-- %% [markdown]\n/* {{ oops */\n");
	CHECK(nb.cells[0].expressions.size() == 2 && nb.cells[0].errors.empty());
	CHECK(nb.cells[0].variables.size() == 1 && nb.cells[0].variables[0] == "region");
	CHECK(nb.cells[1].errors.size() == 1);
}

static void TestChartSpec() {
	CHECK(ChartSelection("mark: bar\nselect: {as: picked, type: point, on: x}\n") == "picked");
	CHECK(ChartSelection("select:\n  type: interval\n  as: \"brush\"\nmark: line") == "brush");
	CHECK(ChartSelection("mark: bar\nlayers:\n  - select: {as: nested}") == "");
	CHECK(ChartFilters("filter: picked # from the trend chart") == std::vector<std::string> {"picked"});
	CHECK((ChartFilters("filter: [a, 'b']") == std::vector<std::string> {"a", "b"}));
	CHECK((ChartFilters("filter:\n  - a\n  - b\nmark: bar") == std::vector<std::string> {"a", "b"}));
	CHECK(ChartFilters("mark: bar").empty());
}

//! Stands in for DuckDB's parser in these tests: a word-level reading that is good
//! enough for the simple SQL below.
static References FakeExtract(const std::string &sql) {
	References refs;
	if (sql.find("syntax!") != std::string::npos) {
		refs.error = "syntax error";
		return refs;
	}
	std::string spaced;
	for (char c : sql) {
		spaced += (c == '(' || c == ')' || c == ',' || c == ';') ? ' ' : static_cast<char>(std::tolower(c));
	}
	std::istringstream in(spaced);
	std::vector<std::string> words;
	for (std::string w; in >> w;) {
		words.push_back(w);
	}
	auto add = [](std::vector<std::string> &list, const std::string &name) {
		if (std::find(list.begin(), list.end(), name) == list.end()) {
			list.push_back(name);
		}
	};
	for (size_t i = 0; i + 1 < words.size(); i++) {
		auto &w = words[i];
		auto &next = words[i + 1];
		if ((w == "from" || w == "join" || w == "into") && std::isalpha(static_cast<unsigned char>(next[0]))) {
			add(refs.reads, next);
		} else if ((w == "table" || w == "view") && i > 0 && words[i - 1] != "drop") {
			add(refs.defines, next);
		} else if (w == "variable") {
			add(refs.defines, "$" + next);
		} else if (w == "as" && i > 0 && words[i - 1][0] == '\'') {
			add(refs.defines, next);
		}
	}
	for (auto &name : ReferencedVariables(sql)) {
		add(refs.reads, "$" + name);
	}
	return refs;
}

static Notebook Analyzed(const std::string &text) {
	auto nb = ParseNotebook(text);
	AnalyzeNotebook(nb, FakeExtract);
	return nb;
}

static bool HasMessage(const std::vector<std::string> &messages, const std::string &part) {
	for (auto &m : messages) {
		if (m.find(part) != std::string::npos) {
			return true;
		}
	}
	return false;
}

static void TestGraph() {
	auto nb = Analyzed("-- %% [markdown]\n/* Total: {{ (SELECT sum(n) FROM detail) }} for {{ $region }} */\n"
	                   "-- %% orders\nCREATE TABLE orders AS SELECT 1 AS n;\n"
	                   "-- %% input region select from=regions.name\nSET VARIABLE region = 'EU';\n"
	                   "-- %% filtered\nSELECT * FROM orders WHERE r = $region;\n"
	                   "-- %% chart from=filtered\n/*\nmark: bar\nselect: {as: picked}\n*/\n"
	                   "-- %% detail\nSELECT * FROM filtered JOIN dim ON true WHERE $picked IS NULL;\n");
	auto &md = nb.cells[0], &orders = nb.cells[1], &region = nb.cells[2], &filtered = nb.cells[3], &chart = nb.cells[4],
	     &detail = nb.cells[5];
	CHECK(!nb.HasErrors());
	CHECK(orders.defines == std::vector<std::string> {"orders"});
	CHECK(region.defines == std::vector<std::string> {"$region"});
	CHECK(region.reads == std::vector<std::string> {"regions"});
	CHECK(region.depends_on.empty());
	CHECK((filtered.defines == std::vector<std::string> {"filtered"}));
	CHECK((filtered.reads == std::vector<std::string> {"orders", "$region"}));
	CHECK((filtered.depends_on == std::vector<int> {1, 2}));
	CHECK(chart.defines == std::vector<std::string> {"$picked"});
	CHECK((detail.reads == std::vector<std::string> {"filtered", "dim", "$picked"}));
	CHECK((detail.depends_on == std::vector<int> {3, 4}));
	// Markdown and charts may read cells below them.
	CHECK((md.depends_on == std::vector<int> {2, 5}));
	CHECK((RunOrder(nb) == std::vector<int> {1, 2, 3, 4, 5, 0}));

	// The input's own variable and what a cell creates for itself are not reads.
	auto self = Analyzed("-- %%\nCREATE TABLE t AS SELECT 1; INSERT INTO t SELECT 2;\n");
	CHECK(self.cells[0].reads.empty() && self.cells[0].depends_on.empty());
}

static void TestGraphErrors() {
	auto forward = Analyzed("-- %% a\nSELECT * FROM b;\n-- %% b\nSELECT 1;\n"
	                        "-- %% input x select from=b.col\nSET VARIABLE x = 1;\n");
	CHECK(HasMessage(forward.cells[0].errors, "'b' is defined further down, at line 3"));
	CHECK(forward.cells[1].errors.empty() && forward.cells[2].errors.empty());

	auto later_var = Analyzed("-- %% a\nSELECT $v;\n-- %%\nSET VARIABLE v = 1;\n");
	CHECK(HasMessage(later_var.cells[0].errors, "variable 'v' is defined further down"));

	auto twice = Analyzed("-- %%\nCREATE TABLE t AS SELECT 1;\n-- %%\nCREATE TABLE t AS SELECT 2;\n"
	                      "-- %% t\nSELECT 3;\n");
	for (auto &cell : twice.cells) {
		CHECK(HasMessage(cell.errors, "'t' is defined by more than one cell (lines 1, 3, 5)"));
	}
	// Duplicate cell names are reported once, by the parser.
	auto names = Analyzed("-- %% a\nSELECT 1;\n-- %% a\nSELECT 2;\n");
	CHECK(names.cells[0].errors.size() == 1 && names.cells[1].errors.size() == 1);

	auto cycle = Analyzed("-- %% chart trend from=detail\n/*\nselect: {as: sel}\n*/\n"
	                      "-- %% detail\nSELECT * FROM t WHERE $sel IS NULL;\n");
	CHECK(HasMessage(cycle.cells[0].errors, "dependency cycle between trend, detail"));
	CHECK(HasMessage(cycle.cells[1].errors, "dependency cycle between trend, detail"));

	auto warn = Analyzed("-- %% a\nSELECT $nobody;\n-- %% b\nSELECT syntax!;\n"
	                     "-- %% chart from=a\n/*\nfilter: other\n*/\n");
	CHECK(!warn.HasErrors());
	CHECK(HasMessage(warn.cells[0].warnings, "variable 'nobody' is not set by any cell"));
	CHECK(HasMessage(warn.cells[1].warnings, "could not analyse this cell's SQL"));
	CHECK(HasMessage(warn.cells[2].warnings, "variable 'other' is not set by any cell"));

	// A qualified read depends on the cell that attached the database.
	auto attach = Analyzed("-- %%\nATTACH 'x.db' AS wh;\n-- %% q\nSELECT * FROM wh.orders;\n");
	CHECK((attach.cells[0].defines == std::vector<std::string> {"wh"}));
	CHECK((attach.cells[1].depends_on == std::vector<int> {0}));
}

static void TestParameters() {
	auto nb = ParseNotebook("-- %% input region select\nSET VARIABLE region = 'EU';\n"
	                        "-- %% q\nSET VARIABLE lim = 5;\nSELECT * FROM t WHERE $picked IS NULL LIMIT $lim;\n");
	CHECK(ParameterError(nb, "region").empty());
	CHECK(ParameterError(nb, "picked").empty());
	CHECK(ParameterError(nb, "lim").find("set by the SQL cell at line 3") != std::string::npos);
	CHECK(ParameterError(nb, "nope").find("no input or variable named 'nope'") != std::string::npos);
}

int main() {
	TestSplitStatements();
	TestIsQueryStatement();
	TestReferencedVariables();
	TestParseNotebook();
	TestParseErrors();
	TestScript();
	TestFindExpressions();
	TestChartSpec();
	TestGraph();
	TestGraphErrors();
	TestParameters();
	if (failures) {
		std::cerr << failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "core tests passed" << std::endl;
	return 0;
}
