// Tests for notebook sources (src/core/source.cpp), included by core_test.cpp, which provides CHECK.
#include "notebook/source.hpp"

static void TestStoredSource() {
	CHECK(IsStoredSource("db:revenue"));
	CHECK(!IsStoredSource("revenue.sql"));
	CHECK(!IsStoredSource("DB:revenue"));

	auto plain = ParseStoredSource("db:revenue");
	CHECK(plain.error.empty() && plain.name == "revenue" && plain.version == 0);
	auto pinned = ParseStoredSource("db:revenue@12");
	CHECK(pinned.error.empty() && pinned.name == "revenue" && pinned.version == 12);
	// Only the last @ separates the version.
	auto at = ParseStoredSource("db:a@b@3");
	CHECK(at.error.empty() && at.name == "a@b" && at.version == 3);
	auto largest = ParseStoredSource("db:n@2147483647");
	CHECK(largest.error.empty() && largest.version == 2147483647);

	for (auto bad : {"db:n@", "db:n@abc", "db:n@0", "db:n@-1", "db:n@+1", "db:n@1.5", "db:n@ 1", "db:n@2147483648",
	                 "db:n@99999999999"}) {
		auto parsed = ParseStoredSource(bad);
		CHECK(!parsed.error.empty());
		CHECK(parsed.error.find("a version is a whole number from 1") != std::string::npos);
	}
	CHECK(ParseStoredSource("db:").error.find("names no notebook") != std::string::npos);
	CHECK(ParseStoredSource("db:@3").error.find("names no notebook") != std::string::npos);

	CHECK(CheckStoredVersion(1).empty());
	CHECK(!CheckStoredVersion(0).empty());
	CHECK(!CheckStoredVersion(-4).empty());
	CHECK(!CheckStoredVersion(int64_t(1) << 40).empty());
}
