#pragma once

#include "notebook/format.hpp"
#include "notebook/json.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

// The result cache (docs/FORMAT.md section 8): the last result of each cell, kept so that a notebook
// reopens showing its output without running anything. This file is the part that needs no database:
// what a cell's key covers, how an entry is written and read, and whether an entry is fresh or stale.
// Where entries live (a directory beside the file, or a table in the database) is
// src/functions/result_cache.cpp.
namespace notebook {

//! Written into every entry. An entry with another number is ignored, so changing what a key covers
//! or how a result is laid out only needs this number raised.
extern const int CACHE_FORMAT;

//! The largest result kept for one cell, in bytes of JSON. A larger result is not cached: the cell
//! reopens empty, as every cell did before there was a cache.
extern const size_t CACHE_MAX_RESULT_BYTES;

//! The id of the entry that holds the notebook's variables rather than a cell's result.
extern const char *const CACHE_STATE_ID;

//! 16 hex digits (64-bit FNV-1a). Not cryptographic: it only has to tell texts apart.
std::string CacheHash(const std::string &text);

struct CacheKey {
	//! Covers what the cell computes: its kind, name, SQL (or text) and the options that change its
	//! data, the data keys of the cells it reads, and the values of the variables it reads. The
	//! cells that read this one build their keys from it.
	std::string data;
	//! Covers what the cell shows: the data key plus its chart specs, the options that change the
	//! picture (rows=, width=), and the selections its charts filter on. An entry is fresh when it
	//! was stored under this key.
	std::string result;
};

//! One key per cell, in file order. `variables` maps a variable name (no `$`) to a text that
//! identifies its value and type (CacheVariableKey); a variable that is missing counts as unset.
//! Needs AnalyzeNotebook.
std::vector<CacheKey> CacheKeys(const Notebook &notebook, const std::map<std::string, std::string> &variables);

//! Identifies a variable's value in a key: its SQL type and its text.
std::string CacheVariableKey(const std::string &type, const std::string &text, bool is_null);

//! The name each cell's result is stored under, in file order. A named cell uses its name. An
//! unnamed cell uses `@` and a hash of its own text, so it keeps its result when cells around it
//! move and loses it when it is edited.
std::vector<std::string> CacheCellIds(const Notebook &notebook);

//! An id as a file name: letters, digits and `_ . @ -` as they are, anything else as %XX.
std::string CacheFileName(const std::string &id);

//! False if the [notebook] cell says `cache: false`.
bool CacheEnabled(const Notebook &notebook);

//! Variables the browser can set: the names of input cells and of chart selections.
std::set<std::string> SettableVariables(const Notebook &notebook);

struct CacheEntry {
	//! CacheKey::result of the cell when the result was made.
	std::string key;
	//! When it was made: `YYYY-MM-DD hh:mm:ss`, UTC.
	std::string saved_at;
	//! The cell's result as the editor receives it from a run, as JSON text.
	std::string result;
};

//! True if `json` is a result the editor can show: an object with a status of `ok` or `error` whose
//! parts have the shapes the renderers expect, no larger than CACHE_MAX_RESULT_BYTES.
bool ValidCacheResult(const std::string &json);

//! The text of an entry file.
std::string DumpCacheEntry(const std::string &id, const CacheEntry &entry);
//! Reads an entry file. False (and `entry` untouched) for anything that is not a complete entry
//! of this format for cell `id`: truncated or corrupt text, another format number, another cell.
bool ParseCacheEntry(const std::string &text, const std::string &id, CacheEntry &entry);

struct CacheVariable {
	std::string type;
	std::string text;
	bool is_null = false;
	//! Input and selection variables: the value as the browser holds it. Null JSON when unknown.
	Json shown;
};

//! The notebook's variables when a result was last stored.
struct CacheState {
	std::map<std::string, CacheVariable> variables;
};

std::string DumpCacheState(const CacheState &state);
//! False (and `state` untouched) for corrupt or old text.
bool ParseCacheState(const std::string &text, CacheState &state);

//! The variables of a state as CacheKeys wants them.
std::map<std::string, std::string> CacheVariableKeys(const CacheState &state);

//! A cell that has a cached result.
struct CachedCell {
	int index = 0;
	//! The result was made from other inputs than the cell has now: its own text, a cell it reads,
	//! or a variable it reads has changed since.
	bool stale = false;
	const CacheEntry *entry = nullptr;
};

//! The decision: which cells have a cached result, and whether each is still fresh. Cells with
//! errors have none (they cannot run, so nothing they show could be current).
std::vector<CachedCell> MatchCache(const Notebook &notebook, const std::map<std::string, std::string> &variables,
                                   const std::map<std::string, CacheEntry> &entries);

} // namespace notebook
