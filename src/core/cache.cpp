#include "notebook/cache.hpp"

#include "notebook/chart.hpp"
#include "notebook/graph.hpp"
#include "notebook/yaml.hpp"
#include "notebook/text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>

namespace notebook {

const int CACHE_FORMAT = 2;
const size_t CACHE_MAX_RESULT_BYTES = 4 * 1024 * 1024;
const char *const CACHE_STATE_ID = "@state";

namespace {

//! Builds the text a key is the hash of. Every field is written with its length, so two different
//! lists of fields never produce the same text.
struct KeyText {
	std::string text;

	KeyText &Add(const std::string &field) {
		text += std::to_string(field.size());
		text += ':';
		text += field;
		return *this;
	}
};

//! Options that never change a result: where and whether the cell is shown, and when it runs.
bool IsLayoutOption(const std::string &key) {
	return key == "hide" || key == "span" || key == "run" || key == "screen";
}

//! Options that change what a cell shows but not what the cells reading it get.
bool IsDisplayOption(const std::string &key) {
	return key == "rows" || key == "width" || key == "height" || key == "label" || key == "placeholder";
}

//! Selections that only a query cell's charts read (filter:), not its SQL.
std::vector<std::string> ChartOnlyVariables(const Cell &cell) {
	std::vector<std::string> result;
	if (cell.kind != CellKind::SQL) {
		return result;
	}
	for (auto &spec : cell.charts) {
		for (auto &name : ChartFilters(spec)) {
			if (!Contains(cell.variables, name) && !Contains(result, name)) {
				result.push_back(name);
			}
		}
	}
	return result;
}

void AddVariable(KeyText &text, const std::string &name, const std::map<std::string, std::string> &variables) {
	auto found = variables.find(name);
	text.Add(name).Add(found == variables.end() ? "unset" : "set").Add(found == variables.end() ? "" : found->second);
}

struct KeyBuilder {
	const Notebook &notebook;
	const std::map<std::string, std::string> &variables;
	std::vector<CacheKey> keys;
	//! 0: not started, 1: being computed, 2: done.
	std::vector<int> state;

	KeyBuilder(const Notebook &notebook, const std::map<std::string, std::string> &variables)
	    : notebook(notebook), variables(variables), keys(notebook.cells.size()), state(notebook.cells.size(), 0) {
	}

	const std::string &DataKey(size_t index) {
		static const std::string cycle = "cycle";
		if (state[index] == 1) {
			// A dependency cycle is already an error on its cells; the key only has to be stable.
			return cycle;
		}
		if (state[index] == 0) {
			Compute(index);
		}
		return keys[index].data;
	}

	void Compute(size_t index) {
		state[index] = 1;
		auto &cell = notebook.cells[index];
		auto chart_only = ChartOnlyVariables(cell);

		KeyText data;
		data.Add("data").Add(std::to_string(CACHE_FORMAT)).Add(CellKindName(cell.kind)).Add(cell.name).Add(cell.widget);
		for (auto &option : cell.options) {
			if (!IsLayoutOption(option.first) && !IsDisplayOption(option.first)) {
				data.Add(option.first).Add(option.second);
			}
		}
		// A query cell's charts are not part of what it computes for others.
		data.Add(cell.kind == CellKind::SQL ? cell.sql : cell.body);
		for (int dependency : cell.depends_on) {
			if (dependency >= 0 && static_cast<size_t>(dependency) < notebook.cells.size()) {
				data.Add(DataKey(static_cast<size_t>(dependency)));
			}
		}
		std::vector<std::string> read;
		for (auto &name : cell.reads) {
			if (name.size() > 1 && name[0] == '$' && !Contains(chart_only, name.substr(1))) {
				read.push_back(name.substr(1));
			}
		}
		std::sort(read.begin(), read.end());
		for (auto &name : read) {
			AddVariable(data, name, variables);
		}
		keys[index].data = CacheHash(data.text);

		KeyText result;
		result.Add("result").Add(keys[index].data).Add(cell.body);
		for (auto &option : cell.options) {
			if (IsDisplayOption(option.first)) {
				result.Add(option.first).Add(option.second);
			}
		}
		std::sort(chart_only.begin(), chart_only.end());
		for (auto &name : chart_only) {
			AddVariable(result, name, variables);
		}
		keys[index].result = CacheHash(result.text);
		state[index] = 2;
	}
};

bool IsArrayOf(const Json *json, Json::Type type) {
	if (!json || json->GetType() != Json::Type::ARRAY) {
		return false;
	}
	for (auto &item : json->Items()) {
		if (item.GetType() != type) {
			return false;
		}
	}
	return true;
}

//! `{columns: [{name, type}], rows: [[...]]}`, as Runner::Page makes it.
bool ValidPage(const Json &page) {
	if (page.GetType() != Json::Type::OBJECT || !IsArrayOf(page.Get("columns"), Json::Type::OBJECT) ||
	    !IsArrayOf(page.Get("rows"), Json::Type::ARRAY)) {
		return false;
	}
	for (auto &column : page.Get("columns")->Items()) {
		auto name = column.Get("name");
		if (!name || name->GetType() != Json::Type::STRING) {
			return false;
		}
	}
	auto width = page.Get("columns")->Size();
	for (auto &row : page.Get("rows")->Items()) {
		if (row.Size() != width) {
			return false;
		}
	}
	return true;
}

//! `{render: {...}, datasets: [page...]}` or `{error}`, as Runner::Chart makes it.
bool ValidChart(const Json &chart) {
	if (chart.GetType() != Json::Type::OBJECT) {
		return false;
	}
	if (auto error = chart.Get("error")) {
		return error->GetType() == Json::Type::STRING;
	}
	auto render = chart.Get("render");
	if (!render || render->GetType() != Json::Type::OBJECT || !IsArrayOf(chart.Get("datasets"), Json::Type::OBJECT)) {
		return false;
	}
	for (auto &dataset : chart.Get("datasets")->Items()) {
		if (!ValidPage(dataset)) {
			return false;
		}
	}
	return true;
}

bool ValidResult(const Json &result) {
	if (result.GetType() != Json::Type::OBJECT) {
		return false;
	}
	auto status = result.Get("status");
	if (!status || status->GetType() != Json::Type::STRING ||
	    (status->Str() != "ok" && status->Str() != "error" && status->Str() != "skipped")) {
		return false;
	}
	auto error = result.Get("error");
	if (status->Str() == "error" && (!error || error->GetType() != Json::Type::STRING)) {
		return false;
	}
	if (auto output = result.Get("output")) {
		if (output->GetType() != Json::Type::STRING) {
			return false;
		}
	}
	if (auto data = result.Get("data")) {
		// A query cell's page, or a chart cell's chart.
		if (!ValidPage(*data) && !ValidChart(*data)) {
			return false;
		}
	}
	if (auto charts = result.Get("charts")) {
		if (charts->GetType() != Json::Type::ARRAY) {
			return false;
		}
		for (auto &chart : charts->Items()) {
			if (!ValidChart(chart)) {
				return false;
			}
		}
	}
	if (auto choices = result.Get("choices")) {
		if (choices->GetType() != Json::Type::ARRAY) {
			return false;
		}
	}
	return true;
}

const std::string *StringField(const Json &json, const char *key) {
	auto field = json.Get(key);
	return field && field->GetType() == Json::Type::STRING ? &field->Str() : nullptr;
}

bool HasFormat(const Json &json) {
	auto format = json.Get("format");
	return json.GetType() == Json::Type::OBJECT && format && format->GetType() == Json::Type::NUMBER &&
	       format->Num() == CACHE_FORMAT;
}

} // namespace

std::string CacheHash(const std::string &text) {
	uint64_t hash = 14695981039346656037ULL;
	for (unsigned char c : text) {
		hash ^= c;
		hash *= 1099511628211ULL;
	}
	char buffer[17];
	snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
	return buffer;
}

std::string CacheVariableKey(const std::string &type, const std::string &text, bool is_null) {
	KeyText key;
	key.Add(type).Add(is_null ? "null" : "value").Add(is_null ? std::string() : text);
	return key.text;
}

std::vector<CacheKey> CacheKeys(const Notebook &notebook, const std::map<std::string, std::string> &variables) {
	KeyBuilder builder(notebook, variables);
	for (size_t i = 0; i < notebook.cells.size(); i++) {
		builder.DataKey(i);
	}
	return builder.keys;
}

std::vector<std::string> CacheCellIds(const Notebook &notebook) {
	std::vector<std::string> ids;
	std::map<std::string, int> seen;
	for (auto &cell : notebook.cells) {
		if (!cell.name.empty()) {
			ids.push_back(cell.name);
			continue;
		}
		KeyText text;
		text.Add(CellKindName(cell.kind)).Add(cell.head).Add(cell.body);
		auto id = "@" + CacheHash(text.text);
		// Two unnamed cells with the same text are told apart by their order.
		auto count = ++seen[id];
		ids.push_back(count == 1 ? id : id + "." + std::to_string(count));
	}
	return ids;
}

std::string CacheFileName(const std::string &id) {
	std::string name;
	for (unsigned char c : id) {
		if (std::isalnum(c) || c == '_' || c == '.' || c == '@' || c == '-') {
			name += static_cast<char>(c);
		} else {
			char buffer[4];
			snprintf(buffer, sizeof(buffer), "%%%02X", c);
			name += buffer;
		}
	}
	return name;
}

bool CacheEnabled(const Notebook &notebook) {
	for (auto &cell : notebook.cells) {
		if (cell.kind != CellKind::NOTEBOOK) {
			continue;
		}
		std::string error;
		auto meta = ParseYaml(cell.content, error);
		auto cache = meta.Get("cache");
		if (cache && cache->IsScalar() && cache->scalar == "false") {
			return false;
		}
	}
	return true;
}

std::set<std::string> SettableVariables(const Notebook &notebook) {
	std::set<std::string> names;
	for (auto &cell : notebook.cells) {
		if (cell.kind == CellKind::INPUT && !cell.name.empty()) {
			names.insert(cell.name);
		}
		for (auto &spec : cell.charts) {
			auto selection = ChartSelection(spec);
			if (!selection.empty()) {
				names.insert(selection);
			}
		}
	}
	return names;
}

bool ValidCacheResult(const std::string &json) {
	if (json.size() > CACHE_MAX_RESULT_BYTES) {
		return false;
	}
	std::string error;
	auto result = ParseJson(json, error);
	return error.empty() && ValidResult(result);
}

std::string DumpCacheEntry(const std::string &id, const CacheEntry &entry) {
	return Json::Object()
	    .Set("format", Json::Number(CACHE_FORMAT))
	    .Set("cell", Json::String(id))
	    .Set("key", Json::String(entry.key))
	    .Set("saved_at", Json::String(entry.saved_at))
	    .Set("result", Json::Raw(entry.result))
	    .Dump();
}

bool ParseCacheEntry(const std::string &text, const std::string &id, CacheEntry &entry) {
	// The wrapper adds little to the result, so this also bounds what is parsed.
	if (text.size() > CACHE_MAX_RESULT_BYTES + 4096) {
		return false;
	}
	std::string error;
	auto json = ParseJson(text, error);
	if (!error.empty() || !HasFormat(json)) {
		return false;
	}
	auto cell = StringField(json, "cell"), key = StringField(json, "key"), saved_at = StringField(json, "saved_at");
	auto result = json.Get("result");
	if (!cell || *cell != id || !key || key->empty() || !saved_at || !result || !ValidResult(*result)) {
		return false;
	}
	auto dumped = result->Dump();
	if (dumped.size() > CACHE_MAX_RESULT_BYTES) {
		return false;
	}
	entry.key = *key;
	entry.saved_at = *saved_at;
	entry.result = std::move(dumped);
	return true;
}

std::string DumpCacheState(const CacheState &state) {
	auto variables = Json::Object();
	for (auto &entry : state.variables) {
		auto variable = Json::Object();
		variable.Set("type", Json::String(entry.second.type));
		variable.Set("value", entry.second.is_null ? Json::Null() : Json::String(entry.second.text));
		if (!entry.second.shown.IsNull()) {
			variable.Set("shown", entry.second.shown);
		}
		variables.Set(entry.first, std::move(variable));
	}
	return Json::Object().Set("format", Json::Number(CACHE_FORMAT)).Set("variables", std::move(variables)).Dump();
}

bool ParseCacheState(const std::string &text, CacheState &state) {
	if (text.size() > CACHE_MAX_RESULT_BYTES) {
		return false;
	}
	std::string error;
	auto json = ParseJson(text, error);
	if (!error.empty() || !HasFormat(json)) {
		return false;
	}
	auto variables = json.Get("variables");
	if (!variables || variables->GetType() != Json::Type::OBJECT) {
		return false;
	}
	CacheState parsed;
	for (auto &entry : variables->Entries()) {
		auto type = entry.second.GetType() == Json::Type::OBJECT ? StringField(entry.second, "type") : nullptr;
		auto value = type ? entry.second.Get("value") : nullptr;
		if (!type || !value || (!value->IsNull() && value->GetType() != Json::Type::STRING)) {
			return false;
		}
		CacheVariable variable;
		variable.type = *type;
		variable.is_null = value->IsNull();
		variable.text = value->Str();
		if (auto shown = entry.second.Get("shown")) {
			variable.shown = *shown;
		}
		parsed.variables[entry.first] = std::move(variable);
	}
	state = std::move(parsed);
	return true;
}

std::map<std::string, std::string> CacheVariableKeys(const CacheState &state) {
	std::map<std::string, std::string> keys;
	for (auto &entry : state.variables) {
		keys[entry.first] = CacheVariableKey(entry.second.type, entry.second.text, entry.second.is_null);
	}
	return keys;
}

std::vector<CachedCell> MatchCache(const Notebook &notebook, const std::map<std::string, std::string> &variables,
                                   const std::map<std::string, CacheEntry> &entries) {
	std::vector<CachedCell> cached;
	if (entries.empty()) {
		return cached;
	}
	auto keys = CacheKeys(notebook, variables);
	auto ids = CacheCellIds(notebook);
	for (size_t i = 0; i < notebook.cells.size(); i++) {
		auto &cell = notebook.cells[i];
		auto found = entries.find(ids[i]);
		if (found == entries.end() || !cell.errors.empty()) {
			continue;
		}
		CachedCell match;
		match.index = cell.index;
		match.stale = found->second.key != keys[i].result;
		match.entry = &found->second;
		cached.push_back(match);
	}
	return cached;
}

} // namespace notebook
