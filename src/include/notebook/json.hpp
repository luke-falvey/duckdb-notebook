#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace notebook {

//! A JSON value, built by core (chart render descriptions, export payloads) and written out with
//! Dump. Objects keep insertion order so output is stable and diffable.
class Json {
public:
	enum class Type { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT, RAW };

	Json() = default;
	static Json Null() {
		return Json();
	}
	static Json Bool(bool value);
	static Json Number(double value);
	static Json String(std::string value);
	//! Already-serialised JSON text, inserted verbatim.
	static Json Raw(std::string json);
	static Json Array();
	static Json Object();

	Type GetType() const {
		return type;
	}
	bool IsNull() const {
		return type == Type::NUL;
	}
	const std::string &Str() const {
		return text;
	}
	double Num() const {
		return number;
	}
	bool Truthy() const {
		return type == Type::BOOL && boolean;
	}

	//! Arrays.
	Json &Push(Json value);
	const std::vector<Json> &Items() const {
		return items;
	}
	//! Objects: sets or replaces a key.
	Json &Set(const std::string &key, Json value);
	const Json *Get(const std::string &key) const;
	const std::vector<std::pair<std::string, Json>> &Entries() const {
		return entries;
	}
	size_t Size() const {
		return type == Type::ARRAY ? items.size() : entries.size();
	}

	std::string Dump() const;

private:
	Type type = Type::NUL;
	bool boolean = false;
	double number = 0;
	std::string text;
	std::vector<Json> items;
	std::vector<std::pair<std::string, Json>> entries;
};

//! `"text"` with JSON escapes; safe to embed in HTML because `<` is escaped too.
std::string JsonQuote(const std::string &text);

//! Parses JSON text (request bodies). Sets `error` and returns null on failure.
Json ParseJson(const std::string &text, std::string &error);

} // namespace notebook
