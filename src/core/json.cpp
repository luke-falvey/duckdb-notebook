#include "notebook/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace notebook {

Json Json::Bool(bool value) {
	Json j;
	j.type = Type::BOOL;
	j.boolean = value;
	return j;
}

Json Json::Number(double value) {
	Json j;
	j.type = Type::NUMBER;
	j.number = value;
	return j;
}

Json Json::String(std::string value) {
	Json j;
	j.type = Type::STRING;
	j.text = std::move(value);
	return j;
}

Json Json::Raw(std::string json) {
	Json j;
	j.type = Type::RAW;
	j.text = std::move(json);
	return j;
}

Json Json::Array() {
	Json j;
	j.type = Type::ARRAY;
	return j;
}

Json Json::Object() {
	Json j;
	j.type = Type::OBJECT;
	return j;
}

Json &Json::Push(Json value) {
	type = Type::ARRAY;
	items.push_back(std::move(value));
	return *this;
}

Json &Json::Set(const std::string &key, Json value) {
	type = Type::OBJECT;
	for (auto &entry : entries) {
		if (entry.first == key) {
			entry.second = std::move(value);
			return *this;
		}
	}
	entries.emplace_back(key, std::move(value));
	return *this;
}

const Json *Json::Get(const std::string &key) const {
	for (auto &entry : entries) {
		if (entry.first == key) {
			return &entry.second;
		}
	}
	return nullptr;
}

std::string JsonQuote(const std::string &text) {
	std::string out = "\"";
	for (unsigned char c : text) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		case '<':
			out += "\\u003c";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	return out + "\"";
}

std::string FormatNumber(double number) {
	if (std::isnan(number)) {
		return "NaN";
	}
	if (std::isinf(number)) {
		return number > 0 ? "Infinity" : "-Infinity";
	}
	if (number == std::floor(number) && std::fabs(number) < 1e15) {
		return std::to_string(static_cast<long long>(number));
	}
	// The shortest of 15, 16 or 17 significant digits that reads back as the same double: any double
	// within 15 digits of a shorter decimal prints as that decimal (0.1, 268.18), never 0.10000000000000001.
	char buf[32];
	for (int precision = 15; precision <= 17; precision++) {
		snprintf(buf, sizeof(buf), "%.*g", precision, number);
		if (precision == 17 || std::strtod(buf, nullptr) == number) {
			break;
		}
	}
	return buf;
}

std::string Json::Dump() const {
	switch (type) {
	case Type::NUL:
		return "null";
	case Type::BOOL:
		return boolean ? "true" : "false";
	case Type::NUMBER:
		// JSON has no infinity or NaN: they travel as the strings JavaScript's Number() reads back.
		return std::isfinite(number) ? FormatNumber(number) : JsonQuote(FormatNumber(number));
	case Type::STRING:
		return JsonQuote(text);
	case Type::RAW:
		return text;
	case Type::ARRAY: {
		std::string out = "[";
		for (size_t i = 0; i < items.size(); i++) {
			out += (i ? "," : "") + items[i].Dump();
		}
		return out + "]";
	}
	case Type::OBJECT: {
		std::string out = "{";
		for (size_t i = 0; i < entries.size(); i++) {
			out += (i ? "," : "") + JsonQuote(entries[i].first) + ":" + entries[i].second.Dump();
		}
		return out + "}";
	}
	}
	return "null";
}

namespace {

//! How deeply arrays and objects may nest in parsed JSON.
constexpr int JSON_MAX_DEPTH = 256;

struct JsonParser {
	explicit JsonParser(const std::string &s) : s(s) {
	}
	const std::string &s;
	size_t pos = 0;
	std::string error;
	//! Arrays and objects open around the value being parsed. Parse recurses once per level, so
	//! nesting is bounded (JSON_MAX_DEPTH) to keep a crafted request or cache file off the stack's end.
	int depth = 0;

	//! Enters an array or object; false (with the error set) when that is one level too many.
	bool Enter() {
		if (++depth > JSON_MAX_DEPTH) {
			return Fail("JSON nested more than " + std::to_string(JSON_MAX_DEPTH) + " levels deep");
		}
		return true;
	}

	void Space() {
		while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\n' || s[pos] == '\r' || s[pos] == '\t')) {
			pos++;
		}
	}

	bool Fail(const std::string &message) {
		if (error.empty()) {
			error = message + " at offset " + std::to_string(pos);
		}
		return false;
	}

	static void AppendUtf8(std::string &out, unsigned code) {
		if (code < 0x80) {
			out += static_cast<char>(code);
		} else if (code < 0x800) {
			out += static_cast<char>(0xC0 | (code >> 6));
			out += static_cast<char>(0x80 | (code & 0x3F));
		} else if (code < 0x10000) {
			out += static_cast<char>(0xE0 | (code >> 12));
			out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (code & 0x3F));
		} else {
			out += static_cast<char>(0xF0 | (code >> 18));
			out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
			out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (code & 0x3F));
		}
	}

	bool ParseString(std::string &out) {
		pos++; // opening quote
		while (pos < s.size() && s[pos] != '"') {
			char c = s[pos++];
			if (c != '\\') {
				out += c;
				continue;
			}
			if (pos >= s.size()) {
				return Fail("unterminated escape");
			}
			char e = s[pos++];
			switch (e) {
			case 'n':
				out += '\n';
				break;
			case 't':
				out += '\t';
				break;
			case 'r':
				out += '\r';
				break;
			case 'b':
				out += '\b';
				break;
			case 'f':
				out += '\f';
				break;
			case 'u': {
				if (pos + 4 > s.size()) {
					return Fail("bad \\u escape");
				}
				unsigned code = static_cast<unsigned>(std::strtoul(s.substr(pos, 4).c_str(), nullptr, 16));
				pos += 4;
				if (code >= 0xD800 && code < 0xDC00 && pos + 6 <= s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
					unsigned low = static_cast<unsigned>(std::strtoul(s.substr(pos + 2, 4).c_str(), nullptr, 16));
					pos += 6;
					code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
				}
				AppendUtf8(out, code);
				break;
			}
			default:
				out += e;
			}
		}
		if (pos >= s.size()) {
			return Fail("unterminated string");
		}
		pos++;
		return true;
	}

	bool Parse(Json &out) {
		Space();
		if (pos >= s.size()) {
			return Fail("unexpected end of JSON");
		}
		char c = s[pos];
		if (c == '{' || c == '[') {
			if (!Enter()) {
				return false;
			}
			auto parsed = c == '{' ? ParseObject(out) : ParseArray(out);
			depth--;
			return parsed;
		}
		if (c == '"') {
			std::string text;
			if (!ParseString(text)) {
				return false;
			}
			out = Json::String(text);
			return true;
		}
		if (s.compare(pos, 4, "true") == 0) {
			pos += 4;
			out = Json::Bool(true);
			return true;
		}
		if (s.compare(pos, 5, "false") == 0) {
			pos += 5;
			out = Json::Bool(false);
			return true;
		}
		if (s.compare(pos, 4, "null") == 0) {
			pos += 4;
			out = Json::Null();
			return true;
		}
		// JSON numbers only: strtod alone would also take nan, inf, hex and a leading '+'.
		auto digits = pos + (c == '-' ? 1 : 0);
		if ((c != '-' && (c < '0' || c > '9')) ||
		    (digits + 1 < s.size() && s[digits] == '0' && (s[digits + 1] == 'x' || s[digits + 1] == 'X'))) {
			return Fail("unexpected character");
		}
		char *end = nullptr;
		double number = std::strtod(s.c_str() + pos, &end);
		if (end == s.c_str() + pos) {
			return Fail("unexpected character");
		}
		if (!std::isfinite(number)) {
			return Fail("number out of range");
		}
		pos = static_cast<size_t>(end - s.c_str());
		out = Json::Number(number);
		return true;
	}

	bool ParseObject(Json &out) {
		pos++;
		out = Json::Object();
		Space();
		if (pos < s.size() && s[pos] == '}') {
			pos++;
			return true;
		}
		while (true) {
			Space();
			if (pos >= s.size() || s[pos] != '"') {
				return Fail("expected a key");
			}
			std::string key;
			if (!ParseString(key)) {
				return false;
			}
			Space();
			if (pos >= s.size() || s[pos] != ':') {
				return Fail("expected ':'");
			}
			pos++;
			Json value;
			if (!Parse(value)) {
				return false;
			}
			out.Set(key, std::move(value));
			Space();
			if (pos < s.size() && s[pos] == ',') {
				pos++;
				continue;
			}
			if (pos < s.size() && s[pos] == '}') {
				pos++;
				return true;
			}
			return Fail("expected ',' or '}'");
		}
	}

	bool ParseArray(Json &out) {
		pos++;
		out = Json::Array();
		Space();
		if (pos < s.size() && s[pos] == ']') {
			pos++;
			return true;
		}
		while (true) {
			Json value;
			if (!Parse(value)) {
				return false;
			}
			out.Push(std::move(value));
			Space();
			if (pos < s.size() && s[pos] == ',') {
				pos++;
				continue;
			}
			if (pos < s.size() && s[pos] == ']') {
				pos++;
				return true;
			}
			return Fail("expected ',' or ']'");
		}
	}
};

} // namespace

Json ParseJson(const std::string &text, std::string &error) {
	JsonParser parser(text);
	Json result;
	if (!parser.Parse(result)) {
		error = parser.error;
		return Json();
	}
	parser.Space();
	if (parser.pos != text.size()) {
		error = "trailing characters after JSON";
		return Json();
	}
	return result;
}

} // namespace notebook
