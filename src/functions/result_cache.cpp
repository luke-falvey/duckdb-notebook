#include "notebook/result_cache.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "notebook/functions.hpp"
#include "notebook/script.hpp"

namespace duckdb {

using notebook::CacheEntry;
using notebook::CacheState;
using notebook::Json;

namespace {

const char *const CACHE_TABLE = "_notebook.cache";

string Now() {
	// `YYYY-MM-DD hh:mm:ss`, UTC.
	return Timestamp::ToString(Timestamp::GetCurrentTimestamp()).substr(0, 19);
}

//! Runs a statement with parameters. Throws on any error; callers catch.
unique_ptr<QueryResult> Execute(Connection &connection, const string &sql, vector<Value> values = {}) {
	auto prepared = connection.Prepare(sql);
	if (prepared->HasError()) {
		throw InvalidInputException("%s", prepared->GetError());
	}
	auto result = prepared->Execute(values);
	if (result->HasError()) {
		result->ThrowError();
	}
	return result;
}

bool CacheTableExists(Connection &connection) {
	auto result = connection.Query("SELECT 1 FROM duckdb_tables() WHERE database_name = current_database() AND "
	                               "schema_name = '_notebook' AND table_name = 'cache'");
	if (result->HasError()) {
		return false;
	}
	auto chunk = result->Fetch();
	return chunk && chunk->size() > 0;
}

//! A file's text, or false if it is missing, unreadable or larger than an entry can be.
bool ReadSmallFile(FileSystem &fs, const string &path, string &text) {
	try {
		if (!fs.FileExists(path)) {
			return false;
		}
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
		auto size = NumericCast<idx_t>(handle->GetFileSize());
		if (size > notebook::CACHE_MAX_RESULT_BYTES + 4096) {
			return false;
		}
		text.assign(size, '\0');
		idx_t total = 0;
		while (total < size) {
			auto n = handle->Read(&text[total], size - total);
			if (n <= 0) {
				return false;
			}
			total += NumericCast<idx_t>(n);
		}
		return true;
	} catch (...) { // NOLINT: an unreadable cache is no cache
		return false;
	}
}

} // namespace

// ---- The store -------------------------------------------------------------------------------------

ResultCacheStore::ResultCacheStore(DatabaseInstance &db, const string &source) : db(db) {
	if (source.compare(0, 3, "db:") == 0) {
		in_database = true;
		name = source.substr(3);
		name = name.substr(0, name.rfind('@'));
		available = !name.empty();
		return;
	}
	try {
		auto &fs = FileSystem::GetFileSystem(db);
		// A cache beside a remote notebook would be written over the network on every run.
		if (source.empty() || FileSystem::IsRemoteFile(source) || !fs.FileExists(source)) {
			return;
		}
		auto slash = source.find_last_of("/\\");
		auto file = slash == string::npos ? source : source.substr(slash + 1);
		auto dot = file.rfind('.');
		auto stem = dot == string::npos || dot == 0 ? file : file.substr(0, dot);
		root = (slash == string::npos ? string() : source.substr(0, slash + 1)) + ".duckdb-notebook";
		directory = root + fs.PathSeparator(source) + notebook::CacheFileName(stem);
		available = true;
	} catch (...) { // NOLINT
	}
}

string ResultCacheStore::FilePath(const string &id) const {
	return directory + "/" + notebook::CacheFileName(id) + ".json";
}

//! Makes the place entries are written to: the directories (with a .gitignore, so the cache never
//! needs one written by hand), or the table.
bool ResultCacheStore::Prepare() {
	if (prepared) {
		return true;
	}
	if (!available) {
		return false;
	}
	try {
		if (in_database) {
			Connection connection(db);
			Execute(connection, "CREATE SCHEMA IF NOT EXISTS _notebook");
			Execute(connection,
			        string("CREATE TABLE IF NOT EXISTS ") + CACHE_TABLE +
			            " (name VARCHAR NOT NULL, cell VARCHAR NOT NULL, format INTEGER NOT NULL, key "
			            "VARCHAR NOT NULL, data VARCHAR, saved_at TIMESTAMP NOT NULL, result VARCHAR NOT NULL, "
			            "PRIMARY KEY (name, cell))");
			// A table made before entries kept their data key (cache format 1).
			Execute(connection, string("ALTER TABLE ") + CACHE_TABLE + " ADD COLUMN IF NOT EXISTS data VARCHAR");
		} else {
			auto &fs = FileSystem::GetFileSystem(db);
			if (!fs.DirectoryExists(root)) {
				fs.CreateDirectory(root);
			}
			auto ignore = root + "/.gitignore";
			if (!fs.FileExists(ignore)) {
				WriteTextFile(db, ignore, "# Results cached by the DuckDB notebook extension. Safe to delete.\n*\n");
			}
			if (!fs.DirectoryExists(directory)) {
				fs.CreateDirectory(directory);
			}
		}
		prepared = true;
	} catch (...) { // NOLINT: nowhere to write means no cache
		available = false;
	}
	return prepared;
}

void ResultCacheStore::Load(const std::vector<string> &ids, std::map<string, CacheEntry> &entries, CacheState &state) {
	if (!available) {
		return;
	}
	try {
		if (in_database) {
			Connection connection(db);
			if (!CacheTableExists(connection)) {
				return;
			}
			auto result = Execute(
			    connection,
			    string("SELECT cell, key, strftime(saved_at, '%Y-%m-%d %H:%M:%S'), result, coalesce(data, '') FROM ") +
			        CACHE_TABLE + " WHERE name = ? AND format = ? AND length(result) <= ?",
			    {Value(name), Value::INTEGER(notebook::CACHE_FORMAT),
			     Value::BIGINT(NumericCast<int64_t>(notebook::CACHE_MAX_RESULT_BYTES))});
			while (auto chunk = result->Fetch()) {
				for (idx_t r = 0; r < chunk->size(); r++) {
					auto id = chunk->GetValue(0, r).ToString();
					auto text = chunk->GetValue(3, r).ToString();
					if (id == notebook::CACHE_STATE_ID) {
						notebook::ParseCacheState(text, state);
					} else if (std::find(ids.begin(), ids.end(), id) != ids.end() && notebook::ValidCacheResult(text)) {
						entries[id] = CacheEntry {chunk->GetValue(1, r).ToString(), chunk->GetValue(4, r).ToString(),
						                          chunk->GetValue(2, r).ToString(), std::move(text)};
					}
				}
			}
			return;
		}
		auto &fs = FileSystem::GetFileSystem(db);
		if (!fs.DirectoryExists(directory)) {
			return;
		}
		string text;
		if (ReadSmallFile(fs, FilePath(notebook::CACHE_STATE_ID), text)) {
			notebook::ParseCacheState(text, state);
		}
		for (auto &id : ids) {
			CacheEntry entry;
			if (ReadSmallFile(fs, FilePath(id), text) && notebook::ParseCacheEntry(text, id, entry)) {
				entries[id] = std::move(entry);
			}
		}
	} catch (...) { // NOLINT: an unreadable cache is no cache
	}
}

bool ResultCacheStore::Write(const string &id, const string &key, const string &data, const string &saved_at,
                             const string &text) {
	if (!Prepare()) {
		return false;
	}
	try {
		if (in_database) {
			Connection connection(db);
			Execute(connection,
			        string("INSERT OR REPLACE INTO ") + CACHE_TABLE +
			            " (name, cell, format, key, data, saved_at, result) VALUES (?, ?, ?, ?, ?, ?::TIMESTAMP, ?)",
			        {Value(name), Value(id), Value::INTEGER(notebook::CACHE_FORMAT), Value(key), Value(data),
			         Value(saved_at), Value(text)});
			return true;
		}
		// Written beside the entry and moved over it, so a reader never sees half an entry.
		auto &fs = FileSystem::GetFileSystem(db);
		auto path = FilePath(id);
		WriteTextFile(db, path + ".tmp", text);
		fs.MoveFile(path + ".tmp", path);
		return true;
	} catch (...) { // NOLINT: writing the cache must not fail a run
		return false;
	}
}

bool ResultCacheStore::Put(const string &id, const CacheEntry &entry) {
	return Write(id, entry.key, entry.data, entry.saved_at,
	             in_database ? entry.result : notebook::DumpCacheEntry(id, entry));
}

bool ResultCacheStore::PutState(const CacheState &state) {
	return Write(notebook::CACHE_STATE_ID, string("state"), string(), Now(), notebook::DumpCacheState(state));
}

void ResultCacheStore::Remove(const string &id) {
	if (!available) {
		return;
	}
	try {
		if (in_database) {
			Connection connection(db);
			if (CacheTableExists(connection)) {
				Execute(connection, string("DELETE FROM ") + CACHE_TABLE + " WHERE name = ? AND cell = ?",
				        {Value(name), Value(id)});
			}
			return;
		}
		FileSystem::GetFileSystem(db).TryRemoveFile(FilePath(id));
	} catch (...) { // NOLINT
	}
}

//! The ids of everything stored for this notebook (files: the names of its .json files).
vector<string> ResultCacheStore::StoredIds() {
	vector<string> stored;
	if (in_database) {
		Connection connection(db);
		if (CacheTableExists(connection)) {
			auto result =
			    Execute(connection, string("SELECT cell FROM ") + CACHE_TABLE + " WHERE name = ?", {Value(name)});
			while (auto chunk = result->Fetch()) {
				for (idx_t r = 0; r < chunk->size(); r++) {
					stored.push_back(chunk->GetValue(0, r).ToString());
				}
			}
		}
		return stored;
	}
	auto &fs = FileSystem::GetFileSystem(db);
	if (fs.DirectoryExists(directory)) {
		fs.ListFiles(directory, [&](const string &file, bool is_directory) {
			// Only what this extension writes there is ever touched.
			if (!is_directory && (StringUtil::EndsWith(file, ".json") || StringUtil::EndsWith(file, ".json.tmp"))) {
				stored.push_back(file);
			}
		});
	}
	return stored;
}

void ResultCacheStore::Prune(const std::vector<string> &ids) {
	if (!available) {
		return;
	}
	try {
		std::set<string> keep;
		keep.insert(in_database ? string(notebook::CACHE_STATE_ID)
		                        : notebook::CacheFileName(notebook::CACHE_STATE_ID) + ".json");
		for (auto &id : ids) {
			keep.insert(in_database ? id : notebook::CacheFileName(id) + ".json");
		}
		for (auto &stored : StoredIds()) {
			if (keep.count(stored)) {
				continue;
			}
			if (in_database) {
				Remove(stored);
			} else {
				FileSystem::GetFileSystem(db).TryRemoveFile(directory + "/" + stored);
			}
		}
	} catch (...) { // NOLINT
	}
}

idx_t ResultCacheStore::Clear() {
	idx_t removed = 0;
	if (!available) {
		return removed;
	}
	try {
		auto state_name = in_database ? string(notebook::CACHE_STATE_ID)
		                              : notebook::CacheFileName(notebook::CACHE_STATE_ID) + ".json";
		auto stored = StoredIds();
		for (auto &entry : stored) {
			removed += entry != state_name && !StringUtil::EndsWith(entry, ".tmp");
		}
		if (in_database) {
			if (!stored.empty()) {
				Connection connection(db);
				Execute(connection, string("DELETE FROM ") + CACHE_TABLE + " WHERE name = ?", {Value(name)});
			}
			return removed;
		}
		auto &fs = FileSystem::GetFileSystem(db);
		for (auto &file : stored) {
			fs.TryRemoveFile(directory + "/" + file);
		}
		// The notebook's directory goes if nothing else is in it; `.duckdb-notebook` itself stays.
		bool empty = true;
		if (fs.DirectoryExists(directory)) {
			fs.ListFiles(directory, [&](const string &, bool) { empty = false; });
			if (empty) {
				fs.RemoveDirectory(directory);
			}
		}
	} catch (...) { // NOLINT
	}
	return removed;
}

// ---- A session's cache ------------------------------------------------------------------------------

void ResultCache::Load(DatabaseInstance &db, const string &source, const notebook::Notebook &notebook) {
	entries.clear();
	state = CacheState();
	if (!notebook::CacheEnabled(notebook)) {
		return;
	}
	ResultCacheStore store(db, source);
	store.Load(notebook::CacheCellIds(notebook), entries, state);
}

void ResultCache::RestoreVariables(DatabaseInstance &db, const notebook::Notebook &notebook,
                                   SessionState &session) const {
	if (state.variables.empty()) {
		return;
	}
	unique_ptr<Connection> connection;
	for (auto &name : notebook::SettableVariables(notebook)) {
		auto found = state.variables.find(name);
		if (found == state.variables.end() || session.variables.count(name)) {
			continue;
		}
		try {
			// Only the type's name is parsed and the text cast to it: no statement runs.
			if (!connection) {
				connection = make_uniq<Connection>(db);
			}
			auto type = TransformStringToLogicalType(found->second.type, *connection->context);
			session.variables[name] =
			    found->second.is_null ? Value(type) : Value(found->second.text).DefaultCastAs(type);
		} catch (...) { // NOLINT: a value that cannot be restored leaves the variable unset
		}
	}
}

void ResultCache::UpdateVariables(const SessionState &session) {
	for (auto &variable : session.variables) {
		auto &recorded = state.variables[variable.first];
		recorded.type = variable.second.type().ToString();
		recorded.is_null = variable.second.IsNull();
		recorded.text = recorded.is_null ? string() : variable.second.ToString();
	}
}

std::map<string, string> ResultCache::Variables(const SessionState &session) const {
	auto keys = notebook::CacheVariableKeys(state);
	for (auto &variable : session.variables) {
		bool is_null = variable.second.IsNull();
		keys[variable.first] = notebook::CacheVariableKey(variable.second.type().ToString(),
		                                                  is_null ? string() : variable.second.ToString(), is_null);
	}
	return keys;
}

void ResultCache::SetShown(const string &name, const Json &value) {
	state.variables[name].shown = value;
}

void ResultCache::Record(DatabaseInstance &db, const string &source, const notebook::Notebook &notebook,
                         const SessionState &session, const vector<RanCell> &ran) {
	if (!notebook::CacheEnabled(notebook)) {
		if (!entries.empty() || !emptied) {
			Clear(db, source);
			emptied = true;
		}
		return;
	}
	emptied = false;
	for (auto &cell : ran) {
		if (!cell.value.IsNull() && cell.index >= 0 && NumericCast<idx_t>(cell.index) < notebook.cells.size()) {
			SetShown(notebook.cells[NumericCast<idx_t>(cell.index)].name, cell.value);
		}
	}
	UpdateVariables(session);
	auto keys = notebook::CacheKeys(notebook, notebook::CacheVariableKeys(state));
	auto ids = notebook::CacheCellIds(notebook);
	ResultCacheStore store(db, source);
	auto now = Now();
	for (auto &cell : ran) {
		if (cell.index < 0 || NumericCast<idx_t>(cell.index) >= ids.size()) {
			continue;
		}
		auto &id = ids[NumericCast<idx_t>(cell.index)];
		string text;
		if (!cell.redraw.IsNull()) {
			// Only the charts were drawn again: the rest of the stored result still stands.
			auto found = entries.find(id);
			// Only the charts are new: the rows in the entry are those of its data key. If the data has changed
			// since, the entry stays as it is, stale.
			if (found == entries.end() || found->second.data != keys[NumericCast<idx_t>(cell.index)].data) {
				continue;
			}
			string error;
			auto merged = notebook::ParseJson(found->second.result, error);
			for (auto &part : cell.redraw.Entries()) {
				merged.Set(part.first, part.second);
			}
			text = merged.Dump();
		} else {
			text = cell.result;
		}
		if (!notebook::ValidCacheResult(text)) {
			// Too large to keep (or not a result at all): better no result than an older one.
			if (entries.erase(id)) {
				store.Remove(id);
			}
			continue;
		}
		auto &entry = entries[id];
		entry.key = keys[NumericCast<idx_t>(cell.index)].result;
		entry.data = keys[NumericCast<idx_t>(cell.index)].data;
		entry.saved_at = now;
		entry.result = std::move(text);
		store.Put(id, entry);
	}
	for (auto entry = entries.begin(); entry != entries.end();) {
		entry = std::find(ids.begin(), ids.end(), entry->first) == ids.end() ? entries.erase(entry) : std::next(entry);
	}
	if (!entries.empty() || !ran.empty()) {
		store.Prune(ids);
		store.PutState(state);
	}
}

idx_t ResultCache::Clear(DatabaseInstance &db, const string &source) {
	auto held = entries.size();
	entries.clear();
	ResultCacheStore store(db, source);
	return MaxValue<idx_t>(held, store.Clear());
}

Json ResultCache::CachedJson(const notebook::Notebook &notebook, const SessionState &session) const {
	auto cached = Json::Array();
	for (auto &match : notebook::MatchCache(notebook, Variables(session), entries)) {
		cached.Push(Json::Object()
		                .Set("idx", Json::Number(match.index))
		                .Set("stale", Json::Bool(match.stale))
		                .Set("saved_at", Json::String(match.entry->saved_at))
		                .Set("result", Json::Raw(match.entry->result)));
	}
	return cached;
}

Json ResultCache::ShownJson(const notebook::Notebook &notebook) const {
	auto shown = Json::Object();
	for (auto &name : notebook::SettableVariables(notebook)) {
		auto found = state.variables.find(name);
		if (found != state.variables.end() && !found->second.shown.IsNull()) {
			shown.Set(name, found->second.shown);
		}
	}
	return shown;
}

RanCell RanFromResult(int index, const string &status, const Json &result) {
	RanCell done;
	done.index = index;
	done.status = status;
	if (auto value = result.Get("value")) {
		done.value = *value;
	}
	done.result = result.Dump();
	return done;
}

// ---- notebook_cache(path) and notebook_cache_clear(path) --------------------------------------------

namespace {

struct CacheRow {
	string cell;
	int idx;
	string kind, status, saved_at;
	int64_t bytes;
};

struct CacheBindData : public TableFunctionData {
	string path;
	vector<CacheRow> rows;
};

struct CacheScanState : public GlobalTableFunctionState {
	idx_t next = 0;
};

unique_ptr<FunctionData> CacheBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<CacheBindData>();
	result->path = input.inputs[0].GetValue<string>();
	auto notebook = ReadNotebook(context, result->path);
	ResultCache cache;
	cache.Load(DatabaseInstance::GetDatabase(context), result->path, notebook);
	auto ids = notebook::CacheCellIds(notebook);
	SessionState none;
	for (auto &match : notebook::MatchCache(notebook, cache.Variables(none), cache.Entries())) {
		auto &cell = notebook.cells[NumericCast<idx_t>(match.index)];
		result->rows.push_back({ids[NumericCast<idx_t>(match.index)], match.index, notebook::CellKindName(cell.kind),
		                        match.stale ? "stale" : "fresh", match.entry->saved_at,
		                        NumericCast<int64_t>(match.entry->result.size())});
	}
	auto column = [&](const char *name, LogicalType type) {
		names.emplace_back(name);
		return_types.push_back(std::move(type));
	};
	// The name the result is stored under: the cell's name, or `@<hash of its text>`.
	column("cell", LogicalType::VARCHAR);
	column("idx", LogicalType::INTEGER);
	column("kind", LogicalType::VARCHAR);
	// `fresh`, or `stale` when the cell, something it reads or a variable it reads has changed since.
	column("status", LogicalType::VARCHAR);
	column("saved_at", LogicalType::TIMESTAMP);
	column("bytes", LogicalType::BIGINT);
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> CacheInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<CacheScanState>();
}

void CacheScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<CacheBindData>();
	auto &state = data.global_state->Cast<CacheScanState>();
	for (idx_t count = 0; state.next < bind.rows.size() && count < STANDARD_VECTOR_SIZE; count++) {
		auto &row = bind.rows[state.next++];
		output.data[0].Append(Value(row.cell));
		output.data[1].Append(Value::INTEGER(row.idx));
		output.data[2].Append(Value(row.kind));
		output.data[3].Append(Value(row.status));
		output.data[4].Append(Value(row.saved_at).DefaultCastAs(LogicalType::TIMESTAMP));
		output.data[5].Append(Value::BIGINT(row.bytes));
	}
}

struct ClearBindData : public TableFunctionData {
	string path;
};

unique_ptr<FunctionData> ClearBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<ClearBindData>();
	result->path = input.inputs[0].GetValue<string>();
	names.emplace_back("removed");
	return_types.push_back(LogicalType::BIGINT);
	return std::move(result);
}

void ClearScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<CacheScanState>();
	if (state.next++ > 0) {
		return;
	}
	ResultCacheStore store(DatabaseInstance::GetDatabase(context), data.bind_data->Cast<ClearBindData>().path);
	output.data[0].Append(Value::BIGINT(NumericCast<int64_t>(store.Clear())));
}

} // namespace

void RegisterNotebookCache(ExtensionLoader &loader) {
	loader.RegisterFunction(TableFunction("notebook_cache", {LogicalType::VARCHAR}, CacheScan, CacheBind, CacheInit));
	loader.RegisterFunction(
	    TableFunction("notebook_cache_clear", {LogicalType::VARCHAR}, ClearScan, ClearBind, CacheInit));
}

} // namespace duckdb
