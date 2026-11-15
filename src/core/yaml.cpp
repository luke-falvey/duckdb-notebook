#include "notebook/yaml.hpp"

#include <cctype>
#include <cstdlib>

namespace notebook {

namespace {

struct Line {
	int indent;
	std::string text;
	int number;
};

std::string Trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
		b++;
	}
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
		e--;
	}
	return s.substr(b, e - b);
}

//! Removes a `#` comment that is outside quotes and starts the line or follows a space.
std::string StripComment(const std::string &s) {
	char quote = 0;
	for (size_t i = 0; i < s.size(); i++) {
		char c = s[i];
		if (quote) {
			if (c == quote) {
				quote = 0;
			}
		} else if (c == '"' || c == '\'') {
			quote = c;
		} else if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
			return s.substr(0, i);
		}
	}
	return s;
}

//! Position of the `:` that ends a map key, or npos. The colon must be followed by a space or
//! the end of the text, and must be outside quotes and brackets.
size_t KeyColon(const std::string &s) {
	char quote = 0;
	int depth = 0;
	for (size_t i = 0; i < s.size(); i++) {
		char c = s[i];
		if (quote) {
			if (c == quote) {
				quote = 0;
			}
		} else if (c == '"' || c == '\'') {
			quote = c;
		} else if (c == '{' || c == '[') {
			depth++;
		} else if (c == '}' || c == ']') {
			depth--;
		} else if (c == ':' && depth == 0 && (i + 1 == s.size() || s[i + 1] == ' ')) {
			return i;
		}
	}
	return std::string::npos;
}

std::string Unquote(const std::string &s, bool &quoted) {
	quoted = false;
	if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
		quoted = true;
		std::string inner = s.substr(1, s.size() - 2);
		if (s.front() == '\'') {
			std::string out;
			for (size_t i = 0; i < inner.size(); i++) {
				out += inner[i];
				if (inner[i] == '\'' && i + 1 < inner.size() && inner[i + 1] == '\'') {
					i++;
				}
			}
			return out;
		}
		std::string out;
		for (size_t i = 0; i < inner.size(); i++) {
			if (inner[i] == '\\' && i + 1 < inner.size()) {
				char e = inner[++i];
				out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
			} else {
				out += inner[i];
			}
		}
		return out;
	}
	return s;
}

struct Parser {
	std::vector<Line> lines;
	size_t pos = 0;
	std::string error;

	void Fail(const std::string &message, int line) {
		if (error.empty()) {
			error = message + " (line " + std::to_string(line) + ")";
		}
	}

	// Flow (inline) values: {a: b}, [a, b], scalars.
	Yaml Flow(const std::string &s, size_t &i, bool in_flow, int line) {
		while (i < s.size() && s[i] == ' ') {
			i++;
		}
		Yaml node;
		if (i < s.size() && (s[i] == '{' || s[i] == '[')) {
			bool is_map = s[i] == '{';
			char close = is_map ? '}' : ']';
			node.type = is_map ? Yaml::Type::MAP : Yaml::Type::LIST;
			i++;
			while (true) {
				while (i < s.size() && s[i] == ' ') {
					i++;
				}
				if (i < s.size() && s[i] == close) {
					i++;
					return node;
				}
				if (i >= s.size()) {
					Fail(std::string("missing '") + close + "'", line);
					return node;
				}
				if (is_map) {
					// key: value
					size_t start = i;
					char quote = 0;
					while (i < s.size()) {
						char c = s[i];
						if (quote) {
							quote = c == quote ? 0 : quote;
						} else if (c == '"' || c == '\'') {
							quote = c;
						} else if (c == ':' || c == ',' || c == '}') {
							break;
						}
						i++;
					}
					bool quoted;
					auto key = Unquote(Trim(s.substr(start, i - start)), quoted);
					Yaml value;
					if (i < s.size() && s[i] == ':') {
						i++;
						value = Flow(s, i, true, line);
					}
					node.entries.emplace_back(key, std::move(value));
				} else {
					node.items.push_back(Flow(s, i, true, line));
				}
				while (i < s.size() && s[i] == ' ') {
					i++;
				}
				if (i < s.size() && s[i] == ',') {
					i++;
				} else if (i < s.size() && s[i] != close) {
					Fail("expected ',' in flow collection", line);
					return node;
				}
			}
		}
		size_t start = i;
		if (i < s.size() && (s[i] == '"' || s[i] == '\'')) {
			char quote = s[i++];
			while (i < s.size()) {
				if (s[i] == quote) {
					if (quote == '\'' && i + 1 < s.size() && s[i + 1] == '\'') {
						i += 2;
						continue;
					}
					break;
				}
				if (quote == '"' && s[i] == '\\') {
					i++;
				}
				i++;
			}
			i++;
		} else {
			while (i < s.size() && !(in_flow && (s[i] == ',' || s[i] == '}' || s[i] == ']'))) {
				i++;
			}
		}
		auto text = Trim(s.substr(start, std::min(i, s.size()) - start));
		if (text.empty() || text == "~" || text == "null") {
			return node;
		}
		node.type = Yaml::Type::SCALAR;
		node.scalar = Unquote(text, node.quoted);
		return node;
	}

	Yaml Inline(const std::string &text, int line) {
		size_t i = 0;
		auto node = Flow(text, i, false, line);
		while (i < text.size() && text[i] == ' ') {
			i++;
		}
		if (i < text.size()) {
			Fail("unexpected text after value: " + text.substr(i), line);
		}
		return node;
	}

	//! The block starting at `pos`, whose lines are indented exactly `indent`.
	Yaml Block(int indent) {
		Yaml node;
		if (pos >= lines.size()) {
			return node;
		}
		auto &first = lines[pos];
		bool is_list = first.text == "-" || first.text.compare(0, 2, "- ") == 0;
		if (is_list) {
			node.type = Yaml::Type::LIST;
			while (pos < lines.size() && lines[pos].indent == indent &&
			       (lines[pos].text == "-" || lines[pos].text.compare(0, 2, "- ") == 0)) {
				auto &line = lines[pos];
				auto rest = Trim(line.text.substr(1));
				if (rest.empty()) {
					pos++;
					if (pos < lines.size() && lines[pos].indent > indent) {
						node.items.push_back(Block(lines[pos].indent));
					} else {
						node.items.emplace_back();
					}
				} else if (KeyColon(rest) != std::string::npos && rest[0] != '{' && rest[0] != '[') {
					// `- key: value` starts a map whose keys line up with `key`.
					int item_indent = indent + static_cast<int>(line.text.size() - rest.size());
					line.indent = item_indent;
					line.text = rest;
					node.items.push_back(Block(item_indent));
				} else {
					pos++;
					node.items.push_back(Inline(rest, line.number));
				}
			}
			return node;
		}
		if (KeyColon(first.text) == std::string::npos) {
			pos++;
			return Inline(first.text, first.number);
		}
		node.type = Yaml::Type::MAP;
		while (pos < lines.size() && lines[pos].indent == indent) {
			auto line = lines[pos++];
			auto colon = KeyColon(line.text);
			if (colon == std::string::npos) {
				Fail("expected 'key: value'", line.number);
				return node;
			}
			bool quoted;
			auto key = Unquote(Trim(line.text.substr(0, colon)), quoted);
			auto rest = Trim(line.text.substr(colon + 1));
			Yaml value;
			if (!rest.empty()) {
				value = Inline(rest, line.number);
			} else if (pos < lines.size() &&
			           (lines[pos].indent > indent ||
			            (lines[pos].indent == indent && lines[pos].text.compare(0, 1, "-") == 0))) {
				value = Block(lines[pos].indent);
			}
			node.entries.emplace_back(key, std::move(value));
		}
		if (pos < lines.size() && lines[pos].indent > indent) {
			Fail("unexpected indentation", lines[pos].number);
		}
		return node;
	}
};

bool IsNumber(const std::string &s) {
	if (s.empty()) {
		return false;
	}
	char *end = nullptr;
	std::strtod(s.c_str(), &end);
	return end == s.c_str() + s.size() && (std::isdigit(static_cast<unsigned char>(s.back())) || s.back() == '.');
}

} // namespace

const Yaml *Yaml::Get(const std::string &key) const {
	if (type != Type::MAP) {
		return nullptr;
	}
	for (auto &entry : entries) {
		if (entry.first == key) {
			return &entry.second;
		}
	}
	return nullptr;
}

std::string Yaml::Str(const std::string &key, const std::string &fallback) const {
	auto value = Get(key);
	return value && value->type == Type::SCALAR ? value->scalar : fallback;
}

std::vector<std::string> Yaml::Strings() const {
	std::vector<std::string> result;
	if (type == Type::SCALAR) {
		result.push_back(scalar);
	} else if (type == Type::LIST) {
		for (auto &item : items) {
			if (item.type == Type::SCALAR) {
				result.push_back(item.scalar);
			}
		}
	}
	return result;
}

Json Yaml::ToJson() const {
	switch (type) {
	case Type::NUL:
		return Json::Null();
	case Type::SCALAR:
		if (!quoted) {
			if (scalar == "true" || scalar == "false") {
				return Json::Bool(scalar == "true");
			}
			if (IsNumber(scalar)) {
				return Json::Number(std::strtod(scalar.c_str(), nullptr));
			}
		}
		return Json::String(scalar);
	case Type::LIST: {
		auto result = Json::Array();
		for (auto &item : items) {
			result.Push(item.ToJson());
		}
		return result;
	}
	case Type::MAP: {
		auto result = Json::Object();
		for (auto &entry : entries) {
			result.Set(entry.first, entry.second.ToJson());
		}
		return result;
	}
	}
	return Json::Null();
}

Yaml ParseYaml(const std::string &text, std::string &error) {
	Parser parser;
	size_t start = 0;
	int number = 0;
	while (start <= text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) {
			end = text.size();
		}
		number++;
		auto raw = StripComment(text.substr(start, end - start));
		int indent = 0;
		while (indent < static_cast<int>(raw.size()) && raw[indent] == ' ') {
			indent++;
		}
		auto content = Trim(raw);
		if (!content.empty()) {
			if (indent < static_cast<int>(raw.size()) && raw[indent] == '\t') {
				parser.Fail("tabs are not allowed for indentation", number);
			}
			parser.lines.push_back({indent, content, number});
		}
		start = end + 1;
	}
	Yaml result;
	if (!parser.lines.empty()) {
		result = parser.Block(parser.lines[0].indent);
		if (parser.pos < parser.lines.size()) {
			parser.Fail("unexpected text", parser.lines[parser.pos].number);
		}
	}
	error = parser.error;
	return result;
}

} // namespace notebook
