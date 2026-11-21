//===----------------------------------------------------------------------===//
// Notebook sources: a file path, or `db:<name>[@<version>]` for a notebook stored in the database.
// Plain C++; no DuckDB headers.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>

namespace notebook {

//! What `db:<name>[@<version>]` names. `version` is 0 for the newest.
struct StoredSource {
	std::string name;
	int32_t version = 0;
	//! Why the source is not a valid stored-notebook reference; empty when it is.
	std::string error;
};

//! True if `source` names a stored notebook (starts with `db:`), whether or not it is well formed.
bool IsStoredSource(const std::string &source);

//! Parses `db:<name>[@<version>]`. The name must not be empty; a version, when given, is a whole
//! number from 1 to 2147483647. Never throws: problems are in `error`. Only for stored sources.
StoredSource ParseStoredSource(const std::string &source);

//! Why `name` cannot be the name a notebook is stored under; empty when it can. It must not be empty or blank, and
//! must not hold `@`, which in `db:<name>@<version>` starts the version: `report@2` would be version 2 of `report`.
std::string CheckStoredName(const std::string &name);

//! Why `version` is not a valid stored version (1 or more); empty when it is.
std::string CheckStoredVersion(int64_t version);

} // namespace notebook
