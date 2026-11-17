// Tests for src/core, which has no DuckDB dependency. Build and run with `make core_test`.
// End-to-end behaviour through SQL is covered by test/sql/*.test.
#include "notebook/chart.hpp"
#include "notebook/document.hpp"
#include "notebook/format.hpp"
#include "notebook/json.hpp"
#include "notebook/yaml.hpp"
#include "notebook/graph.hpp"
#include "notebook/script.hpp"
#include "notebook/sql_lexer.hpp"

#include <cstdio>
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

static void TestParenPairs() {
	// Parentheses in strings, quoted identifiers, dollar strings and comments are not code.
	std::string sql = "WITH a AS (SELECT ')' AS \"(\", $$)$$ /* ( */ FROM t -- (\n) SELECT (1 + (2)) FROM a";
	auto pairs = ParenPairs(sql);
	CHECK(pairs.size() == 3);
	CHECK(sql.substr(pairs[0].open, pairs[0].close - pairs[0].open + 1).rfind("(SELECT ')'", 0) == 0);
	CHECK(sql[pairs[0].close] == ')' && sql.substr(pairs[0].close, 9) == ") SELECT ");
	CHECK(sql.substr(pairs[1].open, pairs[1].close - pairs[1].open + 1) == "(1 + (2))");
	CHECK(sql.substr(pairs[2].open, pairs[2].close - pairs[2].open + 1) == "(2)");
	// Unmatched parentheses are left out, whichever side is missing.
	CHECK(ParenPairs("SELECT (1").empty());
	CHECK(ParenPairs("SELECT 1)").empty());
	CHECK(ParenPairs("SELECT (1))) + (2").size() == 1);
	CHECK(ParenPairs("").empty());
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
	// Every selection in a notebook, by name.
	auto selecting =
	    ParseNotebook("-- %% a\nSELECT 1 AS x\n/* chart\nmark: bar\nx: x\nselect: {as: pick, type: point}\n*/\n"
	                  "-- %% b\nSELECT 2 AS x\n/* chart\nmark: line\nx: x\n*/\n");
	auto selections = NotebookSelections(selecting);
	CHECK(selections.size() == 1 && selections["pick"].type == "point" && selections["pick"].channels.size() == 1);
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

static void TestYaml() {
	std::string error;
	auto y = ParseYaml("title: Monthly # comment\nmark: line\nx: {field: ts, timeunit: month}\n"
	                   "layers:\n  - {mark: bar, y: 1}\n  - mark: rule\n    y: {field: v, agg: avg}\n"
	                   "tags: [a, 'b, c', \"d\"]\nempty:\nquoted: \"1\"\n",
	                   error);
	CHECK(error.empty());
	CHECK(y.Str("title") == "Monthly" && y.Str("mark") == "line");
	CHECK(y.Get("x") && y.Get("x")->Str("timeunit") == "month");
	auto layers = y.Get("layers");
	CHECK(layers && layers->IsList() && layers->items.size() == 2);
	CHECK(layers && layers->items[1].Str("mark") == "rule" && layers->items[1].Get("y")->Str("agg") == "avg");
	CHECK((y.Get("tags")->Strings() == std::vector<std::string> {"a", "b, c", "d"}));
	CHECK(y.Get("empty") && y.Get("empty")->type == Yaml::Type::NUL);
	CHECK(y.Get("quoted")->ToJson().GetType() == Json::Type::STRING);
	CHECK(y.Get("layers")->items[0].Get("y")->ToJson().GetType() == Json::Type::NUMBER);
	ParseYaml("a: {b: 1", error);
	CHECK(error.find("missing '}'") != std::string::npos);
}

static void TestJson() {
	auto j = Json::Object().Set("a", Json::Number(1.5)).Set("b", Json::String("x\"</script>\n"));
	j.Set("c", Json::Array().Push(Json::Bool(true)).Push(Json()));
	CHECK(j.Dump() == "{\"a\":1.5,\"b\":\"x\\\"\\u003c/script>\\n\",\"c\":[true,null]}");
	std::string error;
	auto parsed = ParseJson(j.Dump(), error);
	CHECK(error.empty() && parsed.Get("b")->Str() == "x\"</script>\n" && parsed.Get("c")->Size() == 2);
	CHECK(ParseJson("{\"u\": \"\\u00e9\"}", error).Get("u")->Str() == "\xc3\xa9");
	ParseJson("{1}", error);
	CHECK(!error.empty());
}

static void TestLiterals() {
	std::string error;
	CHECK(JsonToSqlLiteral(ParseJson("\"it's\"", error)) == "'it''s'");
	CHECK(JsonToSqlLiteral(ParseJson("5", error), "DOUBLE") == "5::DOUBLE");
	CHECK(JsonToSqlLiteral(ParseJson("[\"a\", \"b\"]", error), "VARCHAR[]") == "['a'::VARCHAR, 'b'::VARCHAR]");
	CHECK(JsonToSqlLiteral(ParseJson("{\"lo\": \"2026-01-01\", \"hi\": null}", error), "STRUCT(lo DATE, hi DATE)") ==
	      "{'lo': '2026-01-01'::DATE, 'hi': NULL::DATE}");
	CHECK(JsonToSqlLiteral(ParseJson("{\"x\": {\"lo\": 1, \"hi\": 2}}", error),
	                       "STRUCT(x STRUCT(lo DOUBLE, hi DOUBLE))") == "{'x': {'lo': 1::DOUBLE, 'hi': 2::DOUBLE}}");
	CHECK(JsonToSqlLiteral(Json()) == "NULL");
	CHECK(IsConnectionState("CREATE OR REPLACE TEMP VIEW v AS SELECT 1"));
	CHECK(IsConnectionState("SET threads = 2") && IsConnectionState("USE other"));
	CHECK(!IsConnectionState("SET VARIABLE x = 1") && !IsConnectionState("CREATE TABLE t (a INT)"));
	CHECK(RewriteVariables("SELECT $a, '$b', $\"c d\", $1 -- $e") ==
	      "SELECT getvariable('a'), '$b', getvariable('c d'), $1 -- $e");
}

static void TestChartCompiler() {
	ChartContext context;
	context.columns = {{"ts", "TIMESTAMP"}, {"region", "VARCHAR"}, {"amount", "DECIMAL(18,2)"}, {"n", "BIGINT"}};
	context.row_count = 100;
	auto bar = CompileChart("mark: bar\nx: region\ny: {field: amount, agg: sum}\n", "orders", context);
	CHECK(bar.error.empty() && bar.queries.size() == 1);
	CHECK(bar.queries[0].find("sum(\"amount\") AS \"y\"") != std::string::npos);
	CHECK(bar.queries[0].find("GROUP BY ALL") != std::string::npos);
	CHECK(bar.render.Get("layers")->Items()[0].Get("encoding")->Get("x")->Get("type")->Str() == "nominal");

	auto hist = CompileChart("mark: histogram\nx: amount\n", "orders", context);
	CHECK(hist.error.empty() && hist.queries[0].find("__bin_x") != std::string::npos);
	CHECK(hist.queries[0].find("count(*) AS \"y\"") != std::string::npos);

	auto month =
	    CompileChart("mark: line\nx: {field: ts, timeunit: month}\ny: {field: n, agg: p95}\n", "orders", context);
	CHECK(month.queries[0].find("date_trunc('month', \"ts\")") != std::string::npos);
	CHECK(month.queries[0].find("quantile_cont(\"n\", 0.95)") != std::string::npos);

	context.row_count = 1000000;
	auto big = CompileChart("mark: line\nx: ts\ny: n\n", "orders", context);
	CHECK(big.render.Get("layers")->Items()[0].Get("m4") != nullptr);
	auto scatter = CompileChart("mark: point\nx: n\ny: amount\n", "orders", context);
	CHECK(scatter.render.Get("layers")->Items()[0].Get("binned") != nullptr);

	context.selections["picked"] = ChartSelectionInfo("mark: bar\nx: region\nselect: {as: picked, type: point}\n");
	CHECK(context.selections["picked"].fields[0] == "region");
	auto filtered = CompileChart("mark: bar\nx: region\ny: {agg: count}\nfilter: picked\n", "orders", context);
	CHECK(filtered.queries[0].find("list_contains(getvariable('picked'), \"region\")") != std::string::npos);

	auto pivot =
	    CompileChart("type: pivot\nrows: [region]\ncolumns: ts\nvalues: {field: n, agg: max}\n", "wh.orders", context);
	CHECK(pivot.error.empty() && pivot.queries[0].find("PIVOT") != std::string::npos);
	CHECK(pivot.queries[0].find("\"wh\".\"orders\"") != std::string::npos);

	CHECK(CompileChart("mark: bar\nx: {field: nope}\n", "orders", context).error.find("no column") !=
	      std::string::npos);
	CHECK(CompileChart("type: sankey\nsource: region\n", "orders", context).error == "sankey chart needs target:");
	CHECK(CompileChart("mark: bar\nfilter: unknown\n", "orders", context).error.find("not the selection") !=
	      std::string::npos);
	CHECK(TypeClass("TIMESTAMP WITH TIME ZONE") == "temporal" && TypeClass("DECIMAL(9,2)") == "quantitative");
}

static void TestChartReferences() {
	ChartContext context;
	context.columns = {{"ts", "TIMESTAMP"}, {"region", "VARCHAR"}, {"amount", "DECIMAL(18,2)"}, {"n", "BIGINT"}};
	context.row_count = 100;
	std::string base = "mark: line\nx: {field: ts, timeunit: month}\ny: {field: amount, agg: sum}\n";

	// A fixed value needs no query; an aggregate is computed in DuckDB over the chart's marks.
	auto fixed = CompileChart(base + "rules:\n  - {y: 1200, label: Target}\n  - {x: 2026-06-01, label: Launch}\n",
	                          "orders", context);
	CHECK(fixed.error.empty() && fixed.queries.size() == 1);
	auto &rules = fixed.render.Get("rules")->Items();
	CHECK(rules.size() == 2 && rules[0].Get("axis")->Str() == "y" && rules[0].Get("value")->Num() == 1200);
	CHECK(rules[0].Get("label")->Str() == "Target" && rules[1].Get("value")->Str() == "2026-06-01");
	CHECK(fixed.render.Get("reference_data") == nullptr);

	auto avg =
	    CompileChart(base + "rules:\n  - {y: avg, label: Average, color: red}\n  - {y: p90}\n", "orders", context);
	CHECK(avg.error.empty() && avg.queries.size() == 2);
	CHECK(avg.render.Get("reference_data")->Num() == 1);
	CHECK(avg.render.Get("rules")->Items()[0].Get("column")->Str() == "r0");
	CHECK(avg.render.Get("rules")->Items()[0].Get("agg")->Str() == "avg");
	CHECK(avg.render.Get("rules")->Items()[0].Get("color")->Str() == "red");
	// The average of the monthly sums, not of the rows.
	CHECK(avg.queries[1].find("__marks0 AS (WITH __src AS") != std::string::npos);
	CHECK(avg.queries[1].find("sum(\"amount\") AS \"y\"") != std::string::npos);
	CHECK(avg.queries[1].find("(SELECT avg(\"y\") FROM __marks0) AS r0") != std::string::npos);
	CHECK(avg.queries[1].find("(SELECT quantile_cont(\"y\", 0.90) FROM __marks0) AS r1") != std::string::npos);

	// Bands take two ends, each a value or an aggregate; a column of the source can be aggregated too.
	auto bands = CompileChart(base + "bands:\n  - {y: [p25, p75], label: Middle half}\n  - {y: [0, {field: n, agg: "
	                                 "max}]}\n  - {x: [2026-03-01, 2026-04-01]}\n",
	                          "orders", context);
	CHECK(bands.error.empty() && bands.queries.size() == 2);
	auto &band = bands.render.Get("bands")->Items();
	CHECK(band.size() == 3 && band[0].Get("from")->Get("column")->Str() == "r0");
	CHECK(band[0].Get("to")->Get("column")->Str() == "r1" && band[0].Get("label")->Str() == "Middle half");
	CHECK(band[1].Get("from")->Get("value")->Num() == 0 && band[1].Get("to")->Get("column")->Str() == "r2");
	CHECK(bands.queries[1].find("(SELECT max(\"n\") FROM (SELECT * FROM \"orders\")) AS r2") != std::string::npos);
	CHECK(band[2].Get("axis")->Str() == "x" && band[2].Get("to")->Get("value")->Str() == "2026-04-01");

	// A downsampled line is averaged before it is downsampled.
	context.row_count = 1000000;
	auto big = CompileChart("mark: line\nx: ts\ny: n\nrules:\n  - {y: median}\n", "orders", context);
	CHECK(big.error.empty() && big.render.Get("layers")->Items()[0].Get("m4") != nullptr);
	CHECK(big.queries[1].find("__px") == std::string::npos &&
	      big.queries[1].find("median(\"y\")") != std::string::npos);
	context.row_count = 100;

	// A category axis takes a category; "max" in quotes is a value, not an aggregate.
	auto cat =
	    CompileChart("mark: bar\nx: region\ny: {field: amount, agg: sum}\nrules:\n  - {x: EU}\n  - {x: \"max\"}\n",
	                 "orders", context);
	CHECK(cat.error.empty() && cat.queries.size() == 1);
	CHECK(cat.render.Get("rules")->Items()[1].Get("value")->Str() == "max");

	auto fails = [&](const std::string &extra, const std::string &message) {
		auto plan = CompileChart(base + extra, "orders", context);
		if (plan.error.find(message) == std::string::npos) {
			printf("    expected '%s', got '%s'\n", message.c_str(), plan.error.c_str());
		}
		return plan.error.find(message) != std::string::npos && plan.queries.empty();
	};
	CHECK(fails("rules: {y: avg}\n", "rules: must be a list"));
	CHECK(fails("rules:\n  - avg\n", "each item is a mapping"));
	CHECK(fails("rules:\n  - {label: x}\n", "needs x: or y:"));
	CHECK(fails("rules:\n  - {x: 2026-01-01, y: 3}\n", "needs x: or y:"));
	CHECK(fails("rules:\n  - {y: lots}\n", "must be a number or one of avg"));
	CHECK(fails("rules:\n  - {y: avg, lable: Average}\n", "unknown key 'lable'"));
	CHECK(fails("rules:\n  - {x: avg}\n", "not defined for dates"));
	CHECK(fails("rules:\n  - {y: {field: nope, agg: max}}\n", "no column named 'nope'"));
	CHECK(fails("rules:\n  - {y: {field: n}}\n", "needs field: and agg:"));
	CHECK(fails("bands:\n  - {y: 3}\n", "y must be [from, to]"));
	CHECK(fails("bands:\n  - {y: [1, 2, 3]}\n", "y must be [from, to]"));
	CHECK(CompileChart("mark: bar\nx: region\ny: {field: amount, agg: sum}\nrules:\n  - {x: avg}\n", "orders", context)
	          .error.find("needs a numeric or date x axis") != std::string::npos);
	CHECK(CompileChart("mark: arc\ntheta: {field: amount, agg: sum}\ncolor: region\nrules:\n  - {y: 3}\n", "orders",
	                   context)
	          .error.find("no axes") != std::string::npos);
	CHECK(CompileChart("mark: boxplot\nx: region\ny: amount\nrules:\n  - {y: avg}\n", "orders", context)
	          .error.find("not available on a boxplot") != std::string::npos);
	CHECK(CompileChart("mark: boxplot\nx: region\ny: amount\nrules:\n  - {y: 10}\n", "orders", context).error.empty());
}

static void TestChartNotes() {
	ChartContext context;
	context.columns = {{"ts", "TIMESTAMP"}, {"region", "VARCHAR"}, {"amount", "DECIMAL(18,2)"}, {"n", "BIGINT"}};
	context.row_count = 100;
	std::string base = "mark: line\nx: {field: ts, timeunit: month}\ny: {field: amount, agg: sum}\n";
	auto plan = CompileChart(base + "notes:\n  - {x: 2026-06-01, y: 1200, text: \"Launch week\"}\n"
	                                "  - {x: 2026-08-01, text: dip, circle: true, side: below, dx: 10, dy: -4}\n"
	                                "  - {x: [2026-03-01, 2026-05-01], text: Promo, color: red}\n",
	                         "orders", context);
	// Notes are positions for the renderer: no query, values typed by their axis.
	CHECK(plan.error.empty() && plan.queries.size() == 1);
	auto &notes = plan.render.Get("notes")->Items();
	CHECK(notes.size() == 3 && notes[0].Get("x")->Str() == "2026-06-01" && notes[0].Get("y")->Num() == 1200);
	CHECK(notes[0].Get("text")->Str() == "Launch week" && notes[0].Get("circle") == nullptr);
	CHECK(notes[1].Get("y") == nullptr && notes[1].Get("circle")->Truthy() && notes[1].Get("side")->Str() == "below");
	CHECK(notes[1].Get("dx")->Num() == 10 && notes[1].Get("dy")->Num() == -4);
	CHECK(notes[2].Get("x")->Items().size() == 2 && notes[2].Get("x")->Items()[1].Str() == "2026-05-01");
	CHECK(notes[2].Get("color")->Str() == "red");

	// A category position, on either axis.
	auto bars = CompileChart("mark: bar\nx: {field: amount, agg: sum}\ny: region\nnotes:\n  - {y: EU, text: ours}\n",
	                         "orders", context);
	CHECK(bars.error.empty() && bars.render.Get("notes")->Items()[0].Get("y")->Str() == "EU");

	auto fails = [&](const std::string &extra, const std::string &message) {
		auto bad = CompileChart(base + extra, "orders", context);
		if (bad.error.find(message) == std::string::npos) {
			printf("    expected '%s', got '%s'\n", message.c_str(), bad.error.c_str());
		}
		return bad.error.find(message) != std::string::npos && bad.queries.empty();
	};
	CHECK(fails("notes: {x: 1}\n", "notes: must be a list"));
	CHECK(fails("notes:\n  - hello\n", "each item is a mapping"));
	CHECK(fails("notes:\n  - {text: hello}\n", "needs x: or y:"));
	CHECK(fails("notes:\n  - {x: 2026-06-01}\n", "needs text:"));
	CHECK(fails("notes:\n  - {x: 2026-06-01, y: high, text: a}\n", "y must be a number, not 'high'"));
	CHECK(fails("notes:\n  - {x: 2026-06-01, text: a, side: north}\n", "side must be above, below, left or right"));
	CHECK(fails("notes:\n  - {x: 2026-06-01, text: a, dx: far}\n", "dx must be a number"));
	CHECK(fails("notes:\n  - {x: 2026-06-01, text: a, label: b}\n", "unknown key 'label'"));
	CHECK(fails("notes:\n  - {x: [2026-06-01, 2026-07-01], y: [1, 2]}\n", "only one of x and y"));
	CHECK(fails("notes:\n  - {x: [2026-06-01], text: a}\n", "must be [from, to]"));
	CHECK(fails("notes:\n  - {x: [2026-06-01, 2026-07-01], circle: true}\n", "circle needs a point"));
	CHECK(CompileChart("mark: arc\ntheta: {field: amount, agg: sum}\ncolor: region\nnotes:\n  - {x: 1, text: a}\n",
	                   "orders", context)
	          .error.find("no axes") != std::string::npos);
	CHECK(CompileChart("mark: histogram\nx: amount\nnotes:\n  - {x: 40, text: most orders}\n", "orders", context)
	          .error.empty());
}

static void TestCellOptions() {
	auto nb = ParseNotebook("-- %% a hide=code rows=25\nSELECT 1;\n-- %% b hide=everything\nSELECT 2;\n"
	                        "-- %% c rows=0\nSELECT 3;\n-- %% d rows=lots\nSELECT 4;\n-- %% e hide=all\nSELECT 5;\n");
	CHECK(nb.cells[0].errors.empty() && nb.cells[0].Option("rows") == "25");
	CHECK(nb.cells[1].errors.size() == 1 && nb.cells[1].errors[0].find("hide=everything") != std::string::npos);
	CHECK(nb.cells[2].errors.size() == 1 && nb.cells[3].errors.size() == 1);
	CHECK(nb.cells[4].errors.empty());
	// screen= says where a cell goes when the notebook is presented.
	auto shown =
	    ParseNotebook("-- %% a screen=new\nSELECT 1;\n-- %% b screen=join\nSELECT 2;\n-- %% c screen=skip\nSELECT 3;\n"
	                  "-- %% d screen=next\nSELECT 4;\n");
	CHECK(shown.cells[0].errors.empty() && shown.cells[1].errors.empty() && shown.cells[2].errors.empty());
	CHECK(shown.cells[1].Option("screen") == "join");
	CHECK(shown.cells[3].errors.size() == 1 && shown.cells[3].errors[0].find("screen=next") != std::string::npos);
	// from=<relation>.<column> splits at the last dot.
	auto inputs =
	    ParseNotebook("-- %% input r widget=select from=main.orders.region\n'EU'\n-- %% input s widget=text\n''\n");
	std::string relation, column;
	CHECK(inputs.cells[0].InputFrom(relation, column) && relation == "main.orders" && column == "region");
	CHECK(!inputs.cells[1].InputFrom(relation, column));
}

static void TestQueryCharts() {
	// Chart comments after the query are charts; the SQL is what is left.
	auto nb = Analyzed("-- %% monthly\nSELECT 1 AS m, 2 AS n -- totals\n/* a note */\n/* chart\nmark: line\nx: m\n*/\n"
	                   "/*chart mark: bar */\n");
	auto &cell = nb.cells[0];
	CHECK(cell.errors.empty());
	CHECK(cell.charts.size() == 2);
	CHECK(cell.charts[0] == "mark: line\nx: m");
	CHECK(cell.charts[1] == "mark: bar");
	CHECK(cell.sql == "SELECT 1 AS m, 2 AS n -- totals\n/* a note */");
	CHECK(cell.is_query);
	CHECK(ChartFrom(cell) == "monthly");
	CHECK(CellSql(cell).find("chart") == std::string::npos);

	// Unnamed: the chart reads the query itself.
	nb = Analyzed("-- %%\nSELECT 1 AS m -- note\n/* chart\nmark: bar\n*/\n");
	CHECK(ChartFrom(nb.cells[0]) == "(\nSELECT 1 AS m -- note\n)");

	// A chart comment before the query's end is a comment, with a warning; one inside a string is text.
	nb = Analyzed("-- %% a\nSELECT 1 /* chart\nmark: bar */ AS m\n-- %% b\nSELECT '/* chart */' AS m\n");
	CHECK(nb.cells[0].charts.empty() && HasMessage(nb.cells[0].warnings, "only at the end"));
	CHECK(nb.cells[1].charts.empty() && nb.cells[1].warnings.empty());
	// /* charts */ or /* chartreuse */ is not a chart.
	nb = Analyzed("-- %% a\nSELECT 1 AS m\n/* chartreuse */\n");
	CHECK(nb.cells[0].charts.empty());

	// Only a query can be charted.
	nb = Analyzed("-- %% t\nCREATE TABLE t AS SELECT 1 AS m;\n/* chart\nmark: bar\n*/\n");
	CHECK(HasMessage(nb.cells[0].errors, "ends in a query"));

	// A chart's selection is defined by the cell; filters are read without ordering or cycles.
	nb = Analyzed("-- %% detail\nSELECT 1 AS m\n/* chart\nmark: bar\nfilter: picked\n*/\n"
	              "-- %% overview\nSELECT * FROM detail\n/* chart\nmark: bar\nselect: {as: picked}\n*/\n"
	              "/* chart\nmark: point\nfilter: picked\n*/\n"
	              "-- %% below\nSELECT * FROM detail WHERE m = $picked.lo\n");
	CHECK(nb.cells[0].errors.empty() && nb.cells[1].errors.empty() && nb.cells[2].errors.empty());
	CHECK(std::find(nb.cells[1].defines.begin(), nb.cells[1].defines.end(), "$picked") != nb.cells[1].defines.end());
	CHECK(std::find(nb.cells[0].reads.begin(), nb.cells[0].reads.end(), "$picked") != nb.cells[0].reads.end());
	CHECK(nb.cells[0].depends_on.empty());
	CHECK(std::find(nb.cells[1].reads.begin(), nb.cells[1].reads.end(), "$picked") != nb.cells[1].reads.end());
	CHECK(nb.cells[2].depends_on == std::vector<int>({0, 1}));

	// A cell's SQL cannot read its own chart's selection.
	nb = Analyzed("-- %% a\nSELECT 1 AS m WHERE $picked IS NULL\n/* chart\nmark: bar\nselect: {as: picked}\n*/\n");
	CHECK(HasMessage(nb.cells[0].errors, "its own chart sets"));

	// Chart cells keep working: their spec is their one chart.
	nb = Analyzed("-- %% a\nSELECT 1 AS m\n-- %% chart from=a\n/*\nmark: bar\n*/\n");
	CHECK(nb.cells[1].charts.size() == 1 && nb.cells[1].charts[0] == "mark: bar" && ChartFrom(nb.cells[1]) == "a");
}

static void TestDocument() {
	// The title: [notebook] title:, else the first # heading, else the file name.
	CHECK(NotebookTitle(ParseNotebook("-- %% [notebook]\n/*\ntitle: Sales\n*/\n-- %% [markdown]\n/*\n# Other\n*/\n"),
	                    "x.sql") == "Sales");
	CHECK(NotebookTitle(ParseNotebook("-- %% [markdown]\n/*\n# Revenue review\ntext\n*/\n"), "x.sql") ==
	      "Revenue review");
	CHECK(NotebookTitle(ParseNotebook("-- %% q\nSELECT 1;\n"), "/data/monthly.report.sql") == "monthly.report");
	CHECK(NotebookTitle(ParseNotebook("-- %% q\nSELECT 1;\n"), ".hidden") == ".hidden");

	// The Markdown export of a payload: prose, fenced code, tables of at most 20 rows, inputs and hide=.
	std::string error;
	auto payload = ParseJson(R"({"hide": "", "cells": [
	  {"kind": "markdown", "output": "# Title"},
	  {"kind": "sql", "sql": "SELECT a FROM t", "options": {},
	   "table": {"columns": [{"name": "a|b"}], "rows": [[1], ["x\ny"]], "total": 30}},
	  {"kind": "sql", "sql": "SELECT 2", "options": {"hide": "code"}, "error": "boom"},
	  {"kind": "input", "name": "region", "options": {"label": "Region"}, "value": "EU"},
	  {"kind": "chart", "options": {}, "chart": {"render": {"title": "Sales"}, "datasets": [{"columns": [{"name": "n"}], "rows": [[3]]}]}}
	]})",
	                         error);
	CHECK(error.empty());
	auto md = MarkdownDocument(payload);
	CHECK(md.find("# Title\n\n") == 0);
	CHECK(md.find("```sql\nSELECT a FROM t\n```") != std::string::npos);
	CHECK(md.find("| a\\|b |\n| --- |\n| 1 |\n| x y |\n") != std::string::npos);
	CHECK(md.find("30 rows in total.") != std::string::npos);
	CHECK(md.find("SELECT 2") == std::string::npos && md.find("> Error: boom") != std::string::npos);
	CHECK(md.find("**Region**: EU") != std::string::npos);
	CHECK(md.find("*Sales* (chart data; charts are drawn in the HTML export)\n\n| n |") != std::string::npos);
	payload.Set("hide", Json::String("output"));
	md = MarkdownDocument(payload);
	CHECK(md.find("boom") == std::string::npos && md.find("| 1 |") == std::string::npos && md.find("# Title") == 0);
}

static void TestTimingSetting() {
	auto nb = ParseNotebook("-- %% [notebook]\n/*\ntitle: T\ntiming: false\n*/\n");
	CHECK(nb.cells[0].errors.empty());
	nb = ParseNotebook("-- %% [notebook]\n/*\ntiming: sometimes\n*/\n");
	CHECK(HasMessage(nb.cells[0].errors, "timing: sometimes is not valid"));
}

#include "cache_test.hpp"

int main() {
	TestSplitStatements();
	TestParenPairs();
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
	TestYaml();
	TestJson();
	TestLiterals();
	TestChartCompiler();
	TestChartReferences();
	TestChartNotes();
	TestCellOptions();
	TestDocument();
	TestQueryCharts();
	TestTimingSetting();
	TestCache();
	if (failures) {
		std::cerr << failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "core tests passed" << std::endl;
	return 0;
}
