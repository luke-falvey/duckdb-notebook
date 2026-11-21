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
	// B22: nor does screen=, which only says where the cell goes when presenting.
	edited = CacheKeys(Analyzed(Replaced(CACHE_NOTEBOOK, "rows=10", "rows=10 screen=new")), variables);
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
	entry.data = "fedcba9876543210";
	entry.saved_at = "2026-11-17 10:00:00";
	entry.result = "{\"status\":\"ok\",\"row_count\":2,\"elapsed_ms\":1.5,\"data\":{\"columns\":[{\"name\":\"a\","
	               "\"type\":\"INTEGER\"}],\"rows\":[[1],[2]],\"total\":2,\"truncated\":false}}";
	CHECK(ValidCacheResult(entry.result));
	auto text = DumpCacheEntry("monthly", entry);
	CacheEntry read;
	CHECK(ParseCacheEntry(text, "monthly", read));
	CHECK(read.key == entry.key && read.data == entry.data && read.saved_at == entry.saved_at &&
	      read.result == entry.result);
	// B26: an entry without its data key is from before it was kept, and is not used.
	CHECK(!ParseCacheEntry(Replaced(text, ",\"data\":\"fedcba9876543210\"", ""), "monthly", read));

	// Anything that is not a whole entry of this format for this cell is ignored.
	CacheEntry untouched;
	untouched.key = "kept";
	CHECK(!ParseCacheEntry(text, "other", untouched));
	for (size_t cut : {size_t(0), size_t(1), text.size() / 2, text.size() - 1}) {
		CHECK(!ParseCacheEntry(text.substr(0, cut), "monthly", untouched));
	}
	CHECK(!ParseCacheEntry("not json at all", "monthly", untouched));
	CHECK(!ParseCacheEntry("[1, 2]", "monthly", untouched));
	auto format = "\"format\":" + std::to_string(CACHE_FORMAT);
	CHECK(!ParseCacheEntry(Replaced(text, format, "\"format\":2"), "monthly", untouched));
	CHECK(!ParseCacheEntry(Replaced(text, format, "\"format\":" + std::to_string(CACHE_FORMAT + 1)), "monthly",
	                       untouched));
	CHECK(!ParseCacheEntry(Replaced(text, format + ",", ""), "monthly", untouched));
	CHECK(!ParseCacheEntry(Replaced(text, "\"key\":\"0123456789abcdef\"", "\"key\":7"), "monthly", untouched));
	CHECK(!ParseCacheEntry(Replaced(text, "\"status\":\"ok\"", "\"status\":\"done\""), "monthly", untouched));
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
	CHECK(!ParseCacheState("{\"format\":" + std::to_string(CACHE_FORMAT) + "}", parsed));
	CHECK(!ParseCacheState("{\"format\":" + std::to_string(CACHE_FORMAT) + ",\"variables\":{\"a\":{\"type\":\"X\"}}}",
	                       parsed));
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
		entries[ids[i]] = CacheEntry {keys[i].result, keys[i].data, "2026-11-17 10:00:00", "{\"status\":\"ok\"}"};
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

static std::string SourcesText(const std::string &sql) {
	auto nb = ParseNotebook("-- %%\n" + sql + "\n");
	auto sources = CellSources(nb.cells[0]);
	std::string text;
	for (auto &file : sources.files) {
		text += file + "|";
	}
	return text + (sources.unchecked ? "unchecked" : "checked");
}

//! CellSources of a cell whose SQL reads from the given table functions (as AnalyzeNotebook would find them).
static bool UncheckedWith(const std::vector<std::string> &functions) {
	auto nb = ParseNotebook("-- %%\nSELECT 1;\n");
	nb.cells[0].table_functions = functions;
	return CellSources(nb.cells[0]).unchecked;
}

static void TestCacheSources() {
	// Table functions: an allow-list. Readers of files are judged by their paths; the rest cannot be checked.
	CHECK(!UncheckedWith({}));
	CHECK(!UncheckedWith({"range", "generate_series", "unnest"}));
	CHECK(!UncheckedWith({"read_csv"}));
	CHECK(UncheckedWith({"st_read"}));
	CHECK(UncheckedWith({"my_table_macro"}));
	CHECK(UncheckedWith({"duckdb_tables"}));
	// File readers' literal paths, lists of them and strings after FROM or JOIN, as written.
	CHECK(SourcesText("SELECT * FROM read_csv('Data/A.csv', header = true)") == "Data/A.csv|checked");
	CHECK(SourcesText("SELECT * FROM READ_PARQUET(['a.parquet', 'b/*.parquet'])") == "a.parquet|b/*.parquet|checked");
	CHECK(SourcesText("FROM 'it''s.csv' JOIN \"t\" USING (k) JOIN 'b.json' USING (k)") == "it's.csv|b.json|checked");
	CHECK(SourcesText("SELECT * FROM read_json_auto($$x.json$$)") == "x.json|checked");
	CHECK(SourcesText("COPY t FROM 'in.csv'; COPY t TO 'out.csv'") == "in.csv|checked");
	CHECK(SourcesText("SELECT * FROM glob('*.csv')") == "*.csv|checked");
	// Only code counts: not strings, comments or a path in a SELECT list.
	CHECK(SourcesText("-- FROM 'a.csv'\nSELECT 'FROM ''b.csv''', 'c.csv' /* read_csv('d.csv') */") == "checked");
	CHECK(SourcesText("INSTALL x FROM 'https://example.org'; SELECT 1") == "checked");
	// A reader given anything but literal paths, and other scans of external data, cannot be checked.
	CHECK(SourcesText("SELECT * FROM read_csv($path)") == "unchecked");
	CHECK(SourcesText("SELECT * FROM read_csv(['a.csv', $more])") == "a.csv|unchecked");
	CHECK(SourcesText("SELECT * FROM delta_scan('t')") == "unchecked");
	CHECK(SourcesText("SELECT * FROM postgres_query('db', 'SELECT 1')") == "unchecked");
	CHECK(SourcesText("SELECT * FROM query('SELECT 1')") == "unchecked");
	CHECK(SourcesText("SELECT * FROM range(10), generate_series(1, 3)") == "checked");

	// Markdown expressions are read too.
	auto md = Analyzed("-- %% [markdown]\n/* {{ (SELECT count(*) FROM 'm.csv') }} */\n");
	CHECK(CacheSourceFiles(md) == std::vector<std::string> {"m.csv"});

	// (`FROM other, read_csv(...)`: the fake extractor would take a reader after FROM for a table.)
	auto nb = Analyzed("-- %% raw\nSELECT * FROM 'events.csv';\n"
	                   "-- %% daily\nSELECT * FROM raw;\n"
	                   "-- %% other\nSELECT 1 AS n;\n"
	                   "-- %% outside\nSELECT * FROM warehouse_orders;\n"
	                   "-- %% below\nSELECT * FROM outside;\n"
	                   "-- %% attached\nATTACH 'wh.duckdb' AS wh;\n"
	                   "-- %% inside\nSELECT * FROM wh.orders;\n"
	                   "-- %% two\nSELECT * FROM other, read_csv(['a.csv', 'b.csv']);\n");
	CHECK(!nb.HasErrors());
	CHECK((CacheSourceFiles(nb) == std::vector<std::string> {"events.csv", "a.csv", "b.csv"}));
	std::map<std::string, std::string> files {{"events.csv", "100:1"}, {"a.csv", "1:1"}, {"b.csv", "2:2"}};
	auto keys = CacheKeys(nb, {}, files);
	auto verified = [](const std::vector<CacheKey> &list) {
		std::string text;
		for (auto &key : list) {
			text += key.verified ? "v" : "-";
		}
		return text;
	};
	// A table no cell defines, and what reads it, cannot be checked; neither can a table of an attached
	// database.
	CHECK(verified(keys) == "vvv--v-v");
	// A file's version changes the cell that names it and everything that reads that cell, and nothing else.
	auto changed = files;
	changed["events.csv"] = "120:2";
	CHECK(Changed(keys, CacheKeys(nb, {}, changed)) == "0d 0r 1d 1r ");
	changed = files;
	changed["b.csv"] = "2:3";
	CHECK(Changed(keys, CacheKeys(nb, {}, changed)) == "7d 7r ");
	// A file without a known version leaves its cell and readers unverified.
	changed = files;
	changed["events.csv"] = "";
	CHECK(verified(CacheKeys(nb, {}, changed)) == "--v--v-v");
	changed.erase("events.csv");
	CHECK(verified(CacheKeys(nb, {}, changed)) == "--v--v-v");

	// MatchCache says so: fresh, unverified or stale.
	auto ids = CacheCellIds(nb);
	std::map<std::string, CacheEntry> entries;
	for (size_t i = 0; i < ids.size(); i++) {
		entries[ids[i]] = CacheEntry {keys[i].result, keys[i].data, "2026-11-17 10:00:00", "{\"status\":\"ok\"}"};
	}
	auto status = [&](const std::map<std::string, std::string> &versions) {
		std::string text;
		for (auto &match : MatchCache(nb, {}, entries, versions)) {
			text += std::string(match.Status()).substr(0, 1);
		}
		return text;
	};
	CHECK(status(files) == "fffuufuf");
	changed = files;
	changed["events.csv"] = "120:2";
	CHECK(status(changed) == "ssfuufuf");
	// Without versions, a cell that names a file is never fresh.
	CHECK(status({}) == "ssfuufus");

	// A version after `?` may still change (modified in the last two seconds): the key covers it, but is not verified.
	changed = files;
	changed["events.csv"] = "?100:1";
	CHECK(Changed(keys, CacheKeys(nb, {}, changed)).empty());
	CHECK(verified(CacheKeys(nb, {}, changed)) == "--v--v-v");
	// A result made then is stored as such, and is at best unverified, even once the version is known; a changed file
	// still makes it stale.
	auto unsure = CacheKeys(nb, {}, changed);
	CHECK(CacheEntryKey(unsure[0]) == "?" + unsure[0].result);
	CHECK(CacheEntryKey(keys[0]) == keys[0].result);
	auto recorded = entries;
	recorded[ids[0]].key = CacheEntryKey(unsure[0]);
	recorded[ids[1]].key = CacheEntryKey(unsure[1]);
	auto status_of = [&](const std::map<std::string, CacheEntry> &list, const std::map<std::string, std::string> &v) {
		std::string text;
		for (auto &match : MatchCache(nb, {}, list, v)) {
			text += std::string(match.Status()).substr(0, 1);
		}
		return text;
	};
	CHECK(status_of(recorded, files) == "uufuufuf");
	changed = files;
	changed["events.csv"] = "120:2";
	CHECK(status_of(recorded, changed) == "ssfuufuf");

	// Each cell can be keyed with the files as they were just before it ran: a file an earlier cell of the run wrote
	// counts as that cell read it. `files` stays for the cells without versions of their own.
	std::map<size_t, std::map<std::string, std::string>> before {{7, {{"a.csv", "1:1"}, {"b.csv", "2:3"}}}};
	auto per_cell = CacheKeys(nb, {}, files, before);
	changed = files;
	changed["b.csv"] = "2:3";
	CHECK(per_cell[7].result == CacheKeys(nb, {}, changed)[7].result);
	CHECK(Changed(keys, per_cell) == "7d 7r ");

	// A view is read when a cell selects from it: the reader's versions key the views it reads too.
	auto views = Analyzed("-- %% v\nSELECT * FROM 'x.csv';\n"
	                      "-- %% w\nCOPY (SELECT 1) TO 'x.csv';\n"
	                      "-- %% r\nSELECT count(*) AS n FROM v;\n"
	                      "-- %% m materialize=table\nSELECT * FROM 'x.csv';\n"
	                      "-- %% s\nSELECT count(*) AS n FROM m;\n");
	CHECK(!views.HasErrors());
	CHECK(CacheViewClosure(views, 2) == std::vector<size_t> {0});
	CHECK(CacheViewClosure(views, 4).empty());
	CHECK(CacheReadFiles(views, 2) == std::vector<std::string> {"x.csv"});
	CHECK(CacheReadFiles(views, 4).empty());
	std::map<std::string, std::string> old_x {{"x.csv", "1:1"}}, new_x {{"x.csv", "2:2"}};
	std::map<size_t, std::map<std::string, std::string>> run {{0, old_x}, {2, new_x}};
	auto ran = CacheKeys(views, {}, old_x, run);
	// r ran after w wrote x.csv: its key is that of the notebook with the new file, as it is when it is opened next.
	CHECK(ran[2].result == CacheKeys(views, {}, new_x)[2].result);
	// v read the old file, and keeps the key it ran with.
	CHECK(ran[0].result == CacheKeys(views, {}, old_x)[0].result);

	// The mark's helpers.
	CHECK(IsUnsure("?1:1") && !IsUnsure("1:1") && !IsUnsure(""));
	CHECK(StripUnsure("?glob|a=1") == "glob|a=1" && StripUnsure("1:1") == "1:1" && StripUnsure("?") == "");
	CHECK(MarkUnsure("1:1") == "?1:1" && MarkUnsure("?1:1") == "?1:1");

	// Only files on this machine are looked at.
	CHECK(IsLocalPath("data/x.csv"));
	CHECK(IsLocalPath("/abs/x.csv"));
	CHECK(IsLocalPath("C:\\data\\x.csv"));
	CHECK(IsLocalPath("FILE:///tmp/x.csv"));
	CHECK(!IsLocalPath("https://example.com/x.csv"));
	CHECK(!IsLocalPath("s3://bucket/*.parquet"));
	CHECK(!IsLocalPath("hf://datasets/x"));
	CHECK(!IsLocalPath(""));
	// A UNC path is a file on another machine: opening it connects there (SMB).
	CHECK(!IsLocalPath("\\\\host\\share\\x.csv"));
	CHECK(!IsLocalPath("//host/share/x.csv"));
	CHECK(IsLocalPath("/x.csv"));
	CHECK(IsLocalPath("\\\\?\\C:\\data\\x.csv"));
	CHECK(IsLocalPath("\\\\.\\pipe\\x"));
	CHECK(!IsLocalPath("\\\\?\\UNC\\host\\share\\x.csv"));
	// A file modified within the last two seconds may still change without its modified time changing.
	CHECK(ModifiedTooRecently(10000000, 10500000));
	CHECK(ModifiedTooRecently(10000000, 11999999));
	CHECK(!ModifiedTooRecently(10000000, 12000000));
	CHECK(ModifiedTooRecently(13000000, 12000000)); // modified "in the future": a clock behind the file system's
}

static void TestCache() {
	TestCacheKeys();
	TestCacheIds();
	TestCacheSetting();
	TestCacheEntries();
	TestCacheFreshness();
	TestCacheSources();
}
