// Tests for the result cache's core (src/core/cache.cpp), included by core_test.cpp, which provides
// CHECK, Analyzed and HasMessage.
#include "notebook/cache.hpp"

static const char *const CACHE_NOTEBOOK =
    "-- %% orders\nCREATE TABLE orders AS SELECT 1 AS n;\n"
    "-- %% input region select options=EU,US\nSET VARIABLE region = 'EU';\n"
    "-- %% filtered rows=10\nSELECT * FROM orders WHERE r = $region;\n\n/* chart\nmark: bar\nx: n\nfilter: picked\n*/\n"
    "-- %% chart from=filtered\n/*\nmark: bar\nselect: {as: picked}\n*/\n"
    "-- %% detail\nSELECT * FROM filtered WHERE n > 0;\n"
    "-- %% [markdown]\n/* Rows: {{ (SELECT count(*) FROM detail) }} */\n"
    "-- %%\nSELECT 42 AS answer;\n";

static std::string Replaced(std::string text, const std::string &from, const std::string &to) {
	auto at = text.find(from);
	CHECK(at != std::string::npos);
	return at == std::string::npos ? text : text.replace(at, from.size(), to);
}

//! Which cells' data and result keys differ between two notebooks with the same cells.
static std::string Changed(const std::vector<CacheKey> &a, const std::vector<CacheKey> &b) {
	std::string changed;
	for (size_t i = 0; i < a.size() && i < b.size(); i++) {
		if (a[i].data != b[i].data) {
			changed += std::to_string(i) + "d ";
		}
		if (a[i].result != b[i].result) {
			changed += std::to_string(i) + "r ";
		}
	}
	return changed;
}

static void TestCacheKeys() {
	CHECK(CacheHash("") == "cbf29ce484222325");
	CHECK(CacheHash("a") == "af63dc4c8601ec8c");
	CHECK(CacheHash("ab") != CacheHash("ba"));

	std::map<std::string, std::string> variables {{"region", CacheVariableKey("VARCHAR", "EU", false)}};
	auto nb = Analyzed(CACHE_NOTEBOOK);
	CHECK(!nb.HasErrors());
	auto keys = CacheKeys(nb, variables);
	CHECK(keys.size() == 7);
	CHECK(keys[0].data.size() == 16 && keys[0].result.size() == 16 && keys[0].data != keys[0].result);
	// The same notebook and variables give the same keys.
	CHECK(Changed(keys, CacheKeys(Analyzed(CACHE_NOTEBOOK), variables)).empty());

	// Editing a cell changes it and everything that reads it, directly or not, and nothing else.
	auto edited = CacheKeys(Analyzed(Replaced(CACHE_NOTEBOOK, "SELECT 1 AS n", "SELECT 2 AS n")), variables);
	CHECK(Changed(keys, edited) == "0d 0r 2d 2r 3d 3r 4d 4r 5d 5r ");
	edited = CacheKeys(Analyzed(Replaced(CACHE_NOTEBOOK, "n > 0", "n > 1")), variables);
	CHECK(Changed(keys, edited) == "4d 4r 5d 5r ");
	// Whitespace inside the SQL is part of the source; a comment line between cells is too.
	edited = CacheKeys(Analyzed(Replaced(CACHE_NOTEBOOK, "n > 0", "n  > 0")), variables);
	CHECK(Changed(keys, edited) == "4d 4r 5d 5r ");

	// A variable's value changes the cells whose SQL reads it, and their readers.
	auto us = variables;
	us["region"] = CacheVariableKey("VARCHAR", "US", false);
	CHECK(Changed(keys, CacheKeys(nb, us)) == "2d 2r 3d 3r 4d 4r 5d 5r ");
	// Unset, NULL and a value of another type are three different things.
	CHECK(Changed(keys, CacheKeys(nb, {})) == "2d 2r 3d 3r 4d 4r 5d 5r ");
	auto null = variables;
	null["region"] = CacheVariableKey("VARCHAR", "", true);
	auto empty = variables;
	empty["region"] = CacheVariableKey("VARCHAR", "", false);
	CHECK(!Changed(CacheKeys(nb, null), CacheKeys(nb, empty)).empty());
	CHECK(!Changed(CacheKeys(nb, null), CacheKeys(nb, {})).empty());
	CHECK(CacheVariableKey("INTEGER", "1", false) != CacheVariableKey("VARCHAR", "1", false));
	// A variable no cell reads changes nothing.
	auto extra = variables;
	extra["unused"] = CacheVariableKey("INTEGER", "1", false);
	CHECK(Changed(keys, CacheKeys(nb, extra)).empty());

	// A selection that only a query cell's chart filters on redraws that cell: what it shows changes,
	// what it computes for the cells below does not.
	auto picked = variables;
	picked["picked"] = CacheVariableKey("INTEGER[]", "[1]", false);
	CHECK(Changed(keys, CacheKeys(nb, picked)) == "2r ");
	// The same for editing its chart spec, its page size or a chart's size.
	edited = CacheKeys(Analyzed(Replaced(CACHE_NOTEBOOK, "x: n", "x: r")), variables);
	CHECK(Changed(keys, edited) == "2r ");
	edited = CacheKeys(Analyzed(Replaced(CACHE_NOTEBOOK, "rows=10", "rows=25")), variables);
	CHECK(Changed(keys, edited) == "2r ");
	// hide=, span= and run= change nothing; an option the cache does not know counts as data.
	edited = CacheKeys(Analyzed(Replaced(CACHE_NOTEBOOK, "rows=10", "rows=10 hide=code span=6 run=manual")), variables);
	CHECK(Changed(keys, edited).empty());
	edited = CacheKeys(Analyzed(Replaced(CACHE_NOTEBOOK, "rows=10", "rows=10 materialize=table")), variables);
	CHECK(Changed(keys, edited) == "2d 2r 3d 3r 4d 4r 5d 5r ");

	// Keys do not depend on where a cell is: a cell added above changes none of them.
	auto moved = CacheKeys(Analyzed(std::string("-- %% [markdown]\n/* New */\n") + CACHE_NOTEBOOK), variables);
	CHECK(moved.size() == 8);
	for (size_t i = 0; i < keys.size(); i++) {
		CHECK(moved[i + 1].data == keys[i].data && moved[i + 1].result == keys[i].result);
	}

	// A cycle is an error on its cells; computing keys for it still ends.
	auto cycle = Analyzed("-- %% chart a from=b\n/*\nmark: bar\nselect: {as: s}\n*/\n-- %% b\nSELECT $s;\n");
	CHECK(cycle.HasErrors());
	CHECK(CacheKeys(cycle, {}).size() == 2);
}

static void TestCacheIds() {
	auto nb = Analyzed(CACHE_NOTEBOOK);
	auto ids = CacheCellIds(nb);
	CHECK(ids.size() == 7 && ids[0] == "orders" && ids[1] == "region" && ids[4] == "detail");
	// Unnamed cells are known by their text, wherever they are.
	CHECK(ids[5].size() == 17 && ids[5][0] == '@' && ids[6][0] == '@' && ids[5] != ids[6]);
	auto moved = CacheCellIds(Analyzed(std::string("-- %%\nSELECT 0;\n") + CACHE_NOTEBOOK));
	CHECK(moved[6] == ids[5] && moved[7] == ids[6]);
	auto edited = CacheCellIds(Analyzed(Replaced(CACHE_NOTEBOOK, "42 AS answer", "43 AS answer")));
	CHECK(edited[6] != ids[6] && edited[5] == ids[5]);
	// Two unnamed cells with the same text get different ids.
	auto twins = CacheCellIds(Analyzed("-- %%\nSELECT 1;\n-- %%\nSELECT 1;\n"));
	CHECK(twins[0] != twins[1] && twins[1] == twins[0] + ".2");
	CHECK(std::string(CACHE_STATE_ID) == "@state");

	CHECK(CacheFileName("monthly_2") == "monthly_2");
	CHECK(CacheFileName("@0123abcd.2") == "@0123abcd.2");
	CHECK(CacheFileName("../a b") == "..%2Fa%20b");

	CHECK((SettableVariables(nb) == std::set<std::string> {"picked", "region"}));
}

static void TestCacheSetting() {
	CHECK(CacheEnabled(ParseNotebook("-- %%\nSELECT 1;\n")));
	CHECK(CacheEnabled(ParseNotebook("-- %% [notebook]\n/*\ntitle: T\n*/\n")));
	auto nb = ParseNotebook("-- %% [notebook]\n/*\ntitle: T\ncache: false\n*/\n");
	CHECK(nb.cells[0].errors.empty() && !CacheEnabled(nb));
	nb = ParseNotebook("-- %% [notebook]\n/*\ncache: true\n*/\n");
	CHECK(nb.cells[0].errors.empty() && CacheEnabled(nb));
	nb = ParseNotebook("-- %% [notebook]\n/*\ncache: never\n*/\n");
	CHECK(HasMessage(nb.cells[0].errors, "cache: never is not valid"));
}

static void TestCacheEntries() {
	CacheEntry entry;
	entry.key = "0123456789abcdef";
	entry.saved_at = "2026-11-17 10:00:00";
	entry.result = "{\"status\":\"ok\",\"row_count\":2,\"elapsed_ms\":1.5,\"data\":{\"columns\":[{\"name\":\"a\","
	               "\"type\":\"INTEGER\"}],\"rows\":[[1],[2]],\"total\":2,\"truncated\":false}}";
	CHECK(ValidCacheResult(entry.result));
	auto text = DumpCacheEntry("monthly", entry);
	CacheEntry read;
	CHECK(ParseCacheEntry(text, "monthly", read));
	CHECK(read.key == entry.key && read.saved_at == entry.saved_at && read.result == entry.result);

	// Anything that is not a whole entry of this format for this cell is ignored.
	CacheEntry untouched;
	untouched.key = "kept";
	CHECK(!ParseCacheEntry(text, "other", untouched));
	for (size_t cut : {size_t(0), size_t(1), text.size() / 2, text.size() - 1}) {
		CHECK(!ParseCacheEntry(text.substr(0, cut), "monthly", untouched));
	}
	CHECK(!ParseCacheEntry("not json at all", "monthly", untouched));
	CHECK(!ParseCacheEntry("[1, 2]", "monthly", untouched));
	CHECK(!ParseCacheEntry(Replaced(text, "\"format\":1", "\"format\":0"), "monthly", untouched));
	CHECK(!ParseCacheEntry(Replaced(text, "\"format\":1", "\"format\":2"), "monthly", untouched));
	CHECK(!ParseCacheEntry(Replaced(text, "\"format\":1,", ""), "monthly", untouched));
	CHECK(!ParseCacheEntry(Replaced(text, "\"key\":\"0123456789abcdef\"", "\"key\":7"), "monthly", untouched));
	CHECK(!ParseCacheEntry(Replaced(text, "\"status\":\"ok\"", "\"status\":\"skipped\""), "monthly", untouched));
	CHECK(!ParseCacheEntry(Replaced(text, "[[1],[2]]", "[[1],[2,3]]"), "monthly", untouched));
	CHECK(!ParseCacheEntry(Replaced(text, "[[1],[2]]", "\"rows\""), "monthly", untouched));
	CHECK(untouched.key == "kept");

	// The shapes the renderers are given.
	CHECK(ValidCacheResult("{\"status\":\"error\",\"error\":\"boom\"}"));
	CHECK(!ValidCacheResult("{\"status\":\"error\"}"));
	CHECK(ValidCacheResult("{\"status\":\"ok\",\"output\":\"# Title\"}"));
	CHECK(!ValidCacheResult("{\"status\":\"ok\",\"output\":7}"));
	CHECK(ValidCacheResult("{\"status\":\"ok\",\"value\":\"EU\",\"choices\":[\"EU\",\"US\"]}"));
	CHECK(
	    ValidCacheResult("{\"status\":\"ok\",\"charts\":[{\"render\":{\"type\":\"bar\"},\"datasets\":[{\"columns\":[],"
	                     "\"rows\":[]}]},{\"error\":\"no such column\"}]}"));
	CHECK(!ValidCacheResult("{\"status\":\"ok\",\"charts\":[{\"render\":{\"type\":\"bar\"}}]}"));
	CHECK(!ValidCacheResult("{\"status\":\"ok\",\"charts\":{}}"));
	CHECK(ValidCacheResult("{\"status\":\"ok\",\"data\":{\"render\":{},\"datasets\":[]}}"));
	CHECK(!ValidCacheResult("{\"status\":\"ok\",\"data\":[1]}"));
	CHECK(!ValidCacheResult("\"ok\""));

	// One result has a size limit; a larger one is not an entry.
	std::string big = "{\"status\":\"ok\",\"output\":\"" + std::string(CACHE_MAX_RESULT_BYTES, 'x') + "\"}";
	CHECK(!ValidCacheResult(big));
	entry.result = big;
	CHECK(!ParseCacheEntry(DumpCacheEntry("monthly", entry), "monthly", untouched));

	CacheState state;
	state.variables["region"] = CacheVariable {"VARCHAR", "US", false, Json::String("US")};
	state.variables["cutoff"] = CacheVariable {"DATE", "", true, Json()};
	CacheState parsed;
	CHECK(ParseCacheState(DumpCacheState(state), parsed));
	CHECK(parsed.variables.size() == 2 && parsed.variables["region"].text == "US" &&
	      parsed.variables["region"].shown.Str() == "US" && parsed.variables["cutoff"].is_null &&
	      parsed.variables["cutoff"].type == "DATE" && parsed.variables["cutoff"].shown.IsNull());
	CHECK(CacheVariableKeys(parsed)["region"] == CacheVariableKey("VARCHAR", "US", false));
	CHECK(!ParseCacheState("{\"format\":1}", parsed));
	CHECK(!ParseCacheState("{\"format\":1,\"variables\":{\"a\":{\"type\":\"X\"}}}", parsed));
	CHECK(!ParseCacheState(DumpCacheState(state).substr(0, 20), parsed));
	CHECK(parsed.variables.size() == 2);
}

static void TestCacheFreshness() {
	std::map<std::string, std::string> variables {{"region", CacheVariableKey("VARCHAR", "EU", false)}};
	auto nb = Analyzed(CACHE_NOTEBOOK);
	auto keys = CacheKeys(nb, variables);
	auto ids = CacheCellIds(nb);
	std::map<std::string, CacheEntry> entries;
	for (size_t i = 0; i < ids.size(); i++) {
		entries[ids[i]] = CacheEntry {keys[i].result, "2026-11-17 10:00:00", "{\"status\":\"ok\"}"};
	}
	auto stale = [&](const Notebook &notebook, const std::map<std::string, std::string> &values) {
		std::string result;
		for (auto &match : MatchCache(notebook, values, entries)) {
			result += std::to_string(match.index) + (match.stale ? "s " : "f ");
		}
		return result;
	};
	// Nothing changed: every result is fresh.
	CHECK(stale(nb, variables) == "0f 1f 2f 3f 4f 5f 6f ");
	CHECK(MatchCache(nb, variables, {}).empty());
	// An input changed: its readers are stale, the input and the cells that do not read it are not.
	auto us = variables;
	us["region"] = CacheVariableKey("VARCHAR", "US", false);
	CHECK(stale(nb, us) == "0f 1f 2s 3s 4s 5s 6f ");
	// An upstream cell changed: it and everything below it are stale.
	CHECK(stale(Analyzed(Replaced(CACHE_NOTEBOOK, "SELECT 1 AS n", "SELECT 2 AS n")), variables) ==
	      "0s 1f 2s 3s 4s 5s 6f ");
	// A brush only makes the cell whose chart filters on it stale.
	auto picked = variables;
	picked["picked"] = CacheVariableKey("INTEGER[]", "[1]", false);
	CHECK(stale(nb, picked) == "0f 1f 2s 3f 4f 5f 6f ");
	// A renamed cell has no result, and what read it under the old name is stale; an edited unnamed
	// cell has none either.
	CHECK(stale(Analyzed(Replaced(CACHE_NOTEBOOK, "%% detail", "%% details")), variables) == "0f 1f 2f 3f 5s 6f ");
	CHECK(stale(Analyzed(Replaced(CACHE_NOTEBOOK, "42 AS answer", "43 AS answer")), variables) == "0f 1f 2f 3f 4f 5f ");
	// A cell with an error shows nothing from the cache.
	CHECK(stale(Analyzed(Replaced(CACHE_NOTEBOOK, "-- %% orders\n", "-- %% orders\n-- %% detail\nSELECT 1;\n")),
	            variables)
	          .find("1") == std::string::npos);
}

static void TestCache() {
	TestCacheKeys();
	TestCacheIds();
	TestCacheSetting();
	TestCacheEntries();
	TestCacheFreshness();
}
