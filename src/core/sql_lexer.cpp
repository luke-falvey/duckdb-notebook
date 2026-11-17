#include "notebook/sql_lexer.hpp"

#include <cctype>
#include <cstdint>

namespace notebook {

namespace {

bool IsIdentStart(char c) {
	return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool IsIdentChar(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}
bool IsSpace(char c) {
	return std::isspace(static_cast<unsigned char>(c));
}

std::string Trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && IsSpace(s[b])) {
		b++;
	}
	while (e > b && IsSpace(s[e - 1])) {
		e--;
	}
	return s.substr(b, e - b);
}

std::string Upper(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return s;
}

//! A word or single punctuation character found in code, with its parenthesis depth.
struct CodeToken {
	std::string text;
	int depth;
};

//! Keywords and punctuation outside strings and comments. Quoted identifiers and
//! string literals are reported as the placeholder tokens `"` and `'`.
std::vector<CodeToken> CodeTokens(const std::string &sql, size_t limit = SIZE_MAX) {
	std::vector<CodeToken> tokens;
	SqlScanner scan(sql);
	int depth = 0;
	while (!scan.Done() && tokens.size() < limit) {
		auto state = scan.CurrentState();
		size_t start = scan.Position();
		char c = sql[start];
		if (state != SqlScanner::State::CODE) {
			scan.Advance();
			continue;
		}
		if (IsIdentStart(c)) {
			size_t end = start;
			while (end < sql.size() && IsIdentChar(sql[end])) {
				end++;
			}
			tokens.push_back({Upper(sql.substr(start, end - start)), depth});
			while (scan.Position() < end) {
				scan.Advance();
			}
			continue;
		}
		scan.Advance();
		auto after = scan.CurrentState();
		if (after == SqlScanner::State::STRING || after == SqlScanner::State::DOLLAR_STRING) {
			tokens.push_back({"'", depth});
		} else if (after == SqlScanner::State::QUOTED_IDENTIFIER) {
			tokens.push_back({"\"", depth});
		} else if (after == SqlScanner::State::CODE && !IsSpace(c)) {
			if (c == ')' && depth > 0) {
				depth--;
			}
			tokens.push_back({std::string(1, c), depth});
			if (c == '(') {
				depth++;
			}
		}
	}
	return tokens;
}

bool IsQueryKeyword(const std::string &word) {
	static const char *const WORDS[] = {"SELECT",  "FROM",      "VALUES",   "TABLE", "PIVOT",
	                                    "UNPIVOT", "SUMMARIZE", "DESCRIBE", "DESC",  "SHOW"};
	for (auto w : WORDS) {
		if (word == w) {
			return true;
		}
	}
	return false;
}

bool IsEffectKeyword(const std::string &word) {
	static const char *const WORDS[] = {"INSERT", "UPDATE", "DELETE", "MERGE"};
	for (auto w : WORDS) {
		if (word == w) {
			return true;
		}
	}
	return false;
}

} // namespace

size_t SqlScanner::Advance() {
	if (Done()) {
		return 0;
	}
	size_t start = pos;
	char c = text[pos];
	char next = pos + 1 < text.size() ? text[pos + 1] : '\0';
	auto step = [&](size_t n) {
		for (size_t i = 0; i < n && pos < text.size(); i++) {
			if (text[pos] == '\n') {
				line++;
			}
			pos++;
		}
	};
	switch (state) {
	case State::CODE:
		if (c == '-' && next == '-') {
			state = State::LINE_COMMENT;
			step(2);
		} else if (c == '/' && next == '*') {
			state = State::BLOCK_COMMENT;
			block_depth = 1;
			step(2);
		} else if (c == '\'') {
			state = State::STRING;
			step(1);
		} else if (c == '"') {
			state = State::QUOTED_IDENTIFIER;
			step(1);
		} else if (c == '$') {
			// `$$` or `$tag$` opens a dollar-quoted string; `$name` and `$1` are parameters.
			size_t end = pos + 1;
			while (end < text.size() && IsIdentChar(text[end])) {
				end++;
			}
			bool tag_ok = end == pos + 1 || IsIdentStart(text[pos + 1]);
			if (end < text.size() && text[end] == '$' && tag_ok) {
				dollar_tag = text.substr(pos, end - pos + 1);
				state = State::DOLLAR_STRING;
				step(dollar_tag.size());
			} else {
				step(1);
			}
		} else {
			step(1);
		}
		break;
	case State::LINE_COMMENT:
		if (c == '\n') {
			state = State::CODE;
		}
		step(1);
		break;
	case State::BLOCK_COMMENT:
		// DuckDB block comments nest.
		if (c == '/' && next == '*') {
			block_depth++;
			step(2);
		} else if (c == '*' && next == '/') {
			block_depth--;
			step(2);
			if (block_depth == 0) {
				state = State::CODE;
			}
		} else {
			step(1);
		}
		break;
	case State::STRING:
		if (c == '\'' && next == '\'') {
			step(2);
		} else {
			if (c == '\'') {
				state = State::CODE;
			}
			step(1);
		}
		break;
	case State::QUOTED_IDENTIFIER:
		if (c == '"' && next == '"') {
			step(2);
		} else {
			if (c == '"') {
				state = State::CODE;
			}
			step(1);
		}
		break;
	case State::DOLLAR_STRING:
		if (c == '$' && text.compare(pos, dollar_tag.size(), dollar_tag) == 0) {
			step(dollar_tag.size());
			state = State::CODE;
		} else {
			step(1);
		}
		break;
	}
	return pos - start;
}

std::vector<Statement> SplitStatements(const std::string &sql) {
	std::vector<Statement> result;
	SqlScanner scan(sql);
	size_t start = 0;
	bool has_code = false;
	int first_code_line = 1;
	auto flush = [&](size_t end) {
		if (has_code) {
			result.push_back({Trim(sql.substr(start, end - start)), first_code_line});
		}
		has_code = false;
	};
	while (!scan.Done()) {
		auto state = scan.CurrentState();
		size_t at = scan.Position();
		int line = scan.Line();
		char c = sql[at];
		scan.Advance();
		if (state != SqlScanner::State::CODE) {
			continue;
		}
		if (c == ';') {
			flush(at);
			start = scan.Position();
			continue;
		}
		// A character that opens a comment is not code; anything else non-blank is.
		auto after = scan.CurrentState();
		bool opens_comment = after == SqlScanner::State::LINE_COMMENT || after == SqlScanner::State::BLOCK_COMMENT;
		if (!IsSpace(c) && !opens_comment && !has_code) {
			has_code = true;
			first_code_line = line;
		}
	}
	flush(sql.size());
	return result;
}

bool IsQueryStatement(const std::string &statement) {
	auto tokens = CodeTokens(statement);
	size_t i = 0;
	// Leading parentheses: (SELECT ...) UNION ...
	while (i < tokens.size() && tokens[i].text == "(") {
		i++;
	}
	if (i >= tokens.size()) {
		return false;
	}
	if (tokens[i].text != "WITH") {
		return IsQueryKeyword(tokens[i].text);
	}
	// WITH cte AS (...) [, ...] <main statement>: CTE bodies are parenthesised, so the
	// main statement's verb is the first query or DML keyword back at the WITH's depth.
	int depth = tokens[i].depth;
	for (i++; i < tokens.size(); i++) {
		if (tokens[i].depth != depth) {
			continue;
		}
		if (IsEffectKeyword(tokens[i].text)) {
			return false;
		}
		if (IsQueryKeyword(tokens[i].text)) {
			return true;
		}
	}
	return false;
}

std::vector<ParenPair> ParenPairs(const std::string &sql) {
	std::vector<ParenPair> pairs;
	std::vector<size_t> open;
	SqlScanner scan(sql);
	while (!scan.Done()) {
		auto state = scan.CurrentState();
		size_t at = scan.Position();
		scan.Advance();
		if (state != SqlScanner::State::CODE) {
			continue;
		}
		if (sql[at] == '(') {
			open.push_back(pairs.size());
			pairs.push_back({at, std::string::npos});
		} else if (sql[at] == ')' && !open.empty()) {
			pairs[open.back()].close = at;
			open.pop_back();
		}
	}
	std::vector<ParenPair> balanced;
	for (auto &pair : pairs) {
		if (pair.close != std::string::npos) {
			balanced.push_back(pair);
		}
	}
	return balanced;
}

std::vector<std::string> ReferencedVariables(const std::string &sql) {
	std::vector<std::string> names;
	auto add = [&](const std::string &name) {
		for (auto &existing : names) {
			if (existing == name) {
				return;
			}
		}
		names.push_back(name);
	};
	SqlScanner scan(sql);
	while (!scan.Done()) {
		auto state = scan.CurrentState();
		size_t at = scan.Position();
		scan.Advance();
		if (state != SqlScanner::State::CODE || sql[at] != '$' ||
		    scan.CurrentState() == SqlScanner::State::DOLLAR_STRING) {
			continue;
		}
		size_t p = at + 1;
		if (p < sql.size() && sql[p] == '"') {
			size_t end = p + 1;
			std::string name;
			while (end < sql.size()) {
				if (sql[end] == '"' && end + 1 < sql.size() && sql[end + 1] == '"') {
					name += '"';
					end += 2;
				} else if (sql[end] == '"') {
					break;
				} else {
					name += sql[end++];
				}
			}
			if (!name.empty()) {
				add(name);
			}
		} else if (p < sql.size() && IsIdentStart(sql[p])) {
			size_t end = p;
			while (end < sql.size() && IsIdentChar(sql[end])) {
				end++;
			}
			add(sql.substr(p, end - p));
		}
	}
	return names;
}

bool IsConnectionState(const std::string &statement) {
	auto tokens = CodeTokens(statement, 6);
	if (tokens.empty()) {
		return false;
	}
	auto &first = tokens[0].text;
	if (first == "USE") {
		return true;
	}
	if (first == "SET" || first == "RESET") {
		return !(tokens.size() > 1 && tokens[1].text == "VARIABLE");
	}
	if (first == "CREATE") {
		for (size_t i = 1; i < tokens.size(); i++) {
			if (tokens[i].text == "TEMP" || tokens[i].text == "TEMPORARY") {
				return true;
			}
		}
	}
	return false;
}

bool IsSetVariable(const std::string &statement, std::string &name) {
	auto tokens = CodeTokens(statement, 4);
	if (tokens.size() < 4 || tokens[0].text != "SET" || tokens[1].text != "VARIABLE" || tokens[3].text != "=") {
		return false;
	}
	if (!IsIdentStart(tokens[2].text[0])) {
		return false;
	}
	// CodeTokens upper-cases words; recover the name as written.
	auto stripped = StripLeadingComments(statement);
	size_t p = 0;
	for (int word = 0; word < 2; word++) {
		while (p < stripped.size() && IsIdentChar(stripped[p])) {
			p++;
		}
		while (p < stripped.size() && IsSpace(stripped[p])) {
			p++;
		}
	}
	size_t end = p;
	while (end < stripped.size() && IsIdentChar(stripped[end])) {
		end++;
	}
	name = stripped.substr(p, end - p);
	return !name.empty();
}

std::string StripLeadingComments(const std::string &sql) {
	SqlScanner scan(sql);
	while (!scan.Done()) {
		auto state = scan.CurrentState();
		size_t at = scan.Position();
		if (state == SqlScanner::State::CODE && !IsSpace(sql[at])) {
			// Peek: does this character open a comment?
			SqlScanner probe = scan;
			probe.Advance();
			auto after = probe.CurrentState();
			if (after != SqlScanner::State::LINE_COMMENT && after != SqlScanner::State::BLOCK_COMMENT) {
				return sql.substr(at);
			}
		}
		scan.Advance();
	}
	return std::string();
}

bool SingleBlockComment(const std::string &text, std::string &inner) {
	auto trimmed = Trim(text);
	if (trimmed.size() < 4 || trimmed.compare(0, 2, "/*") != 0) {
		return false;
	}
	SqlScanner scan(trimmed);
	scan.Advance();
	while (!scan.Done() && scan.CurrentState() == SqlScanner::State::BLOCK_COMMENT) {
		scan.Advance();
	}
	if (!scan.Done() || scan.CurrentState() != SqlScanner::State::CODE) {
		// Text continues after the comment closes, or the comment never closes.
		return false;
	}
	inner = trimmed.substr(2, trimmed.size() - 4);
	// Drop the newline that conventionally follows `/*` and precedes `*/`.
	if (!inner.empty() && inner.front() == '\n') {
		inner.erase(0, 1);
	}
	if (!inner.empty() && inner.back() == '\n') {
		inner.pop_back();
	}
	return true;
}

} // namespace notebook
