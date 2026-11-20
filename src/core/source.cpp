#include "notebook/source.hpp"

#include <limits>

namespace notebook {

bool IsStoredSource(const std::string &source) {
	return source.compare(0, 3, "db:") == 0;
}

std::string CheckStoredVersion(int64_t version) {
	if (version < 1 || version > std::numeric_limits<int32_t>::max()) {
		return "version " + std::to_string(version) + " is not valid: versions start at 1";
	}
	return std::string();
}

StoredSource ParseStoredSource(const std::string &source) {
	StoredSource result;
	auto rest = IsStoredSource(source) ? source.substr(3) : source;
	auto at = rest.rfind('@');
	result.name = at == std::string::npos ? rest : rest.substr(0, at);
	if (result.name.empty()) {
		result.error = "\"" + source + "\" names no notebook: write db:<name> or db:<name>@<version>";
		return result;
	}
	if (at == std::string::npos) {
		return result;
	}
	auto digits = rest.substr(at + 1);
	int64_t version = 0;
	bool valid = !digits.empty() && digits.size() <= 10;
	for (auto c : digits) {
		valid = valid && c >= '0' && c <= '9';
		if (valid) {
			version = version * 10 + (c - '0');
		}
	}
	if (!valid || !CheckStoredVersion(version).empty()) {
		result.error = "\"" + source + "\" has version \"" + digits +
		               "\": a version is a whole number from 1 (db:<name>@<version>), or leave it out for the newest";
		return result;
	}
	result.version = static_cast<int32_t>(version);
	return result;
}

} // namespace notebook
