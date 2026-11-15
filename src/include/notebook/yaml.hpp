#pragma once

#include "notebook/json.hpp"

#include <string>
#include <utility>
#include <vector>

namespace notebook {

//! A small YAML subset, enough for chart specs and the [notebook] cell: block maps and lists by
//! indentation, flow maps `{a: 1}` and lists `[a, b]`, plain and quoted scalars, `#` comments.
//! Anchors, tags, multi-document streams and block scalars (`|`, `>`) are not supported.
struct Yaml {
	enum class Type { NUL, SCALAR, LIST, MAP };
	Type type = Type::NUL;
	std::string scalar;
	//! Scalars: true if written in quotes, so `"1"` stays a string.
	bool quoted = false;
	std::vector<Yaml> items;
	std::vector<std::pair<std::string, Yaml>> entries;

	bool IsMap() const {
		return type == Type::MAP;
	}
	bool IsList() const {
		return type == Type::LIST;
	}
	bool IsScalar() const {
		return type == Type::SCALAR;
	}
	//! Map lookup; null if absent or not a map.
	const Yaml *Get(const std::string &key) const;
	//! Scalar value of a key, or `fallback`.
	std::string Str(const std::string &key, const std::string &fallback = std::string()) const;
	//! The scalar items of a list, or the scalar itself as a one-item list.
	std::vector<std::string> Strings() const;
	//! As JSON: unquoted numbers, booleans and null become typed JSON values.
	Json ToJson() const;
};

//! Parses YAML text. Sets `error` (with a line number) on malformed input.
Yaml ParseYaml(const std::string &text, std::string &error);

} // namespace notebook
