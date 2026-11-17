#pragma once

#include <set>

#include "duckdb.hpp"
#include "notebook/cache.hpp"
#include "notebook/session.hpp"

// The result cache as the extension keeps it (docs/FORMAT.md section 8). What a key covers and whether
// an entry is fresh is decided in src/core/cache.cpp; this is where entries live and when they are
// written.
//
// A notebook file keeps its cache in `.duckdb-notebook/<file name without extension>/` beside it; a
// notebook stored in a database (`db:<name>`) keeps it in that database's `_notebook.cache` table.
// The cache is only ever a convenience: nothing here throws. A place that cannot be read gives no
// entries, a place that cannot be written is left alone, and an entry that is not whole is ignored.
namespace duckdb {

//! Where one notebook's entries are kept.
class ResultCacheStore {
public:
	ResultCacheStore(DatabaseInstance &db, const string &source);

	//! False for a notebook with nowhere to keep a cache: a remote file, or one not saved yet.
	bool Available() const {
		return available;
	}

	//! The entries of the given cells that exist and are whole, and the state if there is one.
	void Load(const std::vector<string> &ids, std::map<string, notebook::CacheEntry> &entries,
	          notebook::CacheState &state);
	//! False if the entry could not be written (a read-only directory or database).
	bool Put(const string &id, const notebook::CacheEntry &entry);
	bool PutState(const notebook::CacheState &state);
	void Remove(const string &id);
	//! Removes every entry that is not one of `ids` (cells that were renamed or deleted).
	void Prune(const std::vector<string> &ids);
	//! Removes the notebook's whole cache. Returns how many cell results there were.
	idx_t Clear();

private:
	DatabaseInstance &db;
	bool available = false;
	bool in_database = false;
	//! Files: the notebook's directory inside `.duckdb-notebook`, and that directory.
	string directory, root;
	//! Database: the stored notebook's name.
	string name;
	bool prepared = false;

	bool Prepare();
	string FilePath(const string &id) const;
	bool Write(const string &id, const string &key, const string &data, const string &saved_at, const string &text);
	vector<string> StoredIds();
};

//! What one run did to a cell, as the cache needs it.
struct RanCell {
	int index = 0;
	//! `ok`, `error` or `skipped`.
	string status;
	//! The cell's whole result as JSON text, as the editor receives it. Empty for a redraw.
	string result;
	//! A redraw (charts drawn again without running the SQL): `{charts, status, error}` to merge
	//! into the cell's stored result.
	notebook::Json redraw;
	//! Input cells: the value the input now has, as the browser shows it.
	notebook::Json value;
};

//! A notebook's cache as a session holds it: every entry of the current cells in memory, written
//! through to the store as cells run. An open notebook shows its results again after a page reload
//! even where nothing can be written.
class ResultCache {
public:
	//! Reads the notebook's entries, unless the notebook switches the cache off.
	void Load(DatabaseInstance &db, const string &source, const notebook::Notebook &notebook);

	//! Gives a session that has not run anything the last values of the notebook's inputs and
	//! selections, so it agrees with the results shown. Leaves the variables the session already has.
	//! Returns the names it set. Runs no SQL.
	std::set<string> RestoreVariables(DatabaseInstance &db, const notebook::Notebook &notebook,
	                                  SessionState &session) const;

	//! Values for CacheKeys: the session's variables, and for those it has not set, the ones recorded
	//! with the cached results.
	std::map<string, string> Variables(const SessionState &session) const;

	//! Stores the results of the cells that ran, drops entries of cells that no longer exist, and
	//! records the session's variables. With `cache: false` it removes the cache instead.
	void Record(DatabaseInstance &db, const string &source, const notebook::Notebook &notebook,
	            const SessionState &session, const vector<RanCell> &ran);

	//! The browser set a variable: remembers the value it holds.
	void SetShown(const string &name, const notebook::Json &value);

	//! Forgets everything, here and in the store. Returns how many cell results there were.
	idx_t Clear(DatabaseInstance &db, const string &source);

	//! `[{idx, stale, saved_at, result}]` for the cells that have a result, and `{name: value}` for
	//! the input and selection values that go with them.
	notebook::Json CachedJson(const notebook::Notebook &notebook, const SessionState &session) const;
	notebook::Json ShownJson(const notebook::Notebook &notebook) const;

	const std::map<string, notebook::CacheEntry> &Entries() const {
		return entries;
	}
	const notebook::CacheState &State() const {
		return state;
	}

private:
	std::map<string, notebook::CacheEntry> entries;
	notebook::CacheState state;
	//! Set once a notebook with `cache: false` has had its store emptied.
	bool emptied = false;

	void UpdateVariables(const SessionState &session);
};

//! What a cell that ran (RunForEditor) leaves for the cache: its status, its result serialised once,
//! and an input's value.
RanCell RanFromResult(int index, const string &status, const notebook::Json &result);

//! notebook_cache(path) and notebook_cache_clear(path).
void RegisterNotebookCache(ExtensionLoader &loader);

} // namespace duckdb
