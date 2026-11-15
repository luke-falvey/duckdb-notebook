// Tests for src/core, which has no DuckDB dependency. Build and run with `make core_test`.
// End-to-end behaviour through SQL is covered by test/sql/*.test.
#include "notebook/format.hpp"
#include "notebook/script.hpp"
#include "notebook/sql_lexer.hpp"

#include <iostream>

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

int main() {
	TestSplitStatements();
	TestIsQueryStatement();
	TestReferencedVariables();
	TestParseNotebook();
	TestParseErrors();
	TestScript();
	if (failures) {
		std::cerr << failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "core tests passed" << std::endl;
	return 0;
}
