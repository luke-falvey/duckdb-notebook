#include "notebook/format.hpp"

#include "notebook/sql_lexer.hpp"
#include "notebook/yaml.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

namespace notebook {

namespace {

const char *const MARKER = "-- %%";

bool IsValidName(const std::string &name) {
	if (name.empty() || !(std::islower(static_cast<unsigned char>(name[0])) || name[0] == '_')) {
		return false;
	}
	for (char c : name) {
		if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_')) {
			return false;
		}
	}
	return true;
}

std::string TrimBlankLines(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
		b++;
	}
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
		e--;
	}
	return s.substr(b, e - b);
}

//! Splits a marker head on whitespace, keeping double-quoted sections together:
//! `input region select label="Sales region"` -> 4 tokens.
std::vector<std::string> SplitHead(const std::string &head, bool &unterminated_quote) {
	std::vector<std::string> tokens;
	std::string current;
	bool in_quotes = false, has_token = false;
	for (char c : head) {
		if (c == '"') {
			in_quotes = !in_quotes;
			has_token = true;
		} else if (std::isspace(static_cast<unsigned char>(c)) && !in_quotes) {
			if (has_token) {
				tokens.push_back(current);
			}
			current.clear();
			has_token = false;
		} else {
			current += c;
			has_token = true;
		}
	}
	if (has_token) {
		tokens.push_back(current);
	}
	unterminated_quote = in_quotes;
	return tokens;
}

void ParseHead(const std::string &head, Cell &cell) {
	bool unterminated = false;
	auto tokens = SplitHead(head, unterminated);
	if (unterminated) {
		cell.errors.push_back("unterminated quote in cell marker");
	}
	size_t i = 0;
	bool takes_name = true;
	if (!tokens.empty()) {
		const auto &first = tokens[0];
		if (first == "[notebook]") {
			cell.kind = CellKind::NOTEBOOK;
			takes_name = false;
			i = 1;
		} else if (first == "[markdown]") {
			cell.kind = CellKind::MARKDOWN;
			takes_name = false;
			i = 1;
		} else if (!first.empty() && first[0] == '[') {
			cell.errors.push_back("unknown cell kind " + first + "; expected [notebook] or [markdown]");
			takes_name = false;
			i = 1;
		} else if (first == "input") {
			cell.kind = CellKind::INPUT;
			i = 1;
		} else if (first == "chart") {
			cell.kind = CellKind::CHART;
			i = 1;
		}
	}
	// Optional name: the next token, unless it is an option.
	if (takes_name && i < tokens.size() && tokens[i].find('=') == std::string::npos) {
		cell.name = tokens[i++];
		if (!IsValidName(cell.name)) {
			cell.errors.push_back("invalid cell name '" + cell.name + "'; names match [a-z_][a-z0-9_]*");
		} else if (cell.name == "input" || cell.name == "chart") {
			cell.errors.push_back("'" + cell.name + "' is reserved and cannot be a cell name");
		}
	}
	if (cell.kind == CellKind::INPUT) {
		if (cell.name.empty()) {
			cell.errors.push_back("input cell needs a name: -- %% input <name> <widget>");
		}
		if (i < tokens.size() && tokens[i].find('=') == std::string::npos) {
			cell.widget = tokens[i++];
		} else {
			cell.errors.push_back("input cell needs a widget: -- %% input <name> <widget>");
		}
	}
	for (; i < tokens.size(); i++) {
		auto eq = tokens[i].find('=');
		if (eq == std::string::npos || eq == 0) {
			cell.errors.push_back("unexpected '" + tokens[i] + "' in cell marker; options are written key=value");
			continue;
		}
		cell.options.emplace_back(tokens[i].substr(0, eq), tokens[i].substr(eq + 1));
	}
}

//! Options whose values the notebook depends on are checked here; the rest are free-form.
void CheckOptions(Cell &cell) {
	for (auto &option : cell.options) {
		auto &key = option.first;
		auto &value = option.second;
		if (key == "hide" && value != "code" && value != "output" && value != "all" && value != "none") {
			cell.errors.push_back("hide=" + value + " is not valid; use hide=code, hide=output or hide=all");
		} else if (key == "rows") {
			bool digits = !value.empty() && value.size() <= 5;
			for (char c : value) {
				digits = digits && std::isdigit(static_cast<unsigned char>(c));
			}
			if (!digits || std::atoi(value.c_str()) < 1 || std::atoi(value.c_str()) > 10000) {
				cell.errors.push_back("rows=" + value + " is not valid; rows is a page size from 1 to 10000");
			}
		}
	}
}

//! A chart comment's spec: the text after `chart`, without the blank rest of its first line.
bool ChartComment(const std::string &comment, std::string &spec) {
	auto inner = comment.substr(2, comment.size() - 4);
	auto start = inner.find_first_not_of(" \t");
	if (start == std::string::npos || inner.compare(start, 5, "chart") != 0) {
		return false;
	}
	auto rest = inner.substr(start + 5);
	if (!rest.empty() && !std::isspace(static_cast<unsigned char>(rest[0]))) {
		return false;
	}
	auto newline = rest.find('\n');
	auto first = rest.substr(0, newline);
	auto text = first.find_first_not_of(" \t\r");
	if (text == std::string::npos) {
		spec = newline == std::string::npos ? std::string() : rest.substr(newline + 1);
	} else {
		spec = rest.substr(text);
	}
	while (!spec.empty() && std::isspace(static_cast<unsigned char>(spec.back()))) {
		spec.pop_back();
	}
	return true;
}

//! Takes the `/* chart ... */` comments that follow a SQL cell's last statement out of its body.
void SplitCharts(Cell &cell) {
	struct Range {
		size_t begin;
		size_t end;
	};
	const auto &body = cell.body;
	std::vector<Range> comments;
	size_t code_end = 0, open = std::string::npos;
	SqlScanner scan(body);
	while (!scan.Done()) {
		auto before = scan.CurrentState();
		size_t at = scan.Position();
		scan.Advance();
		auto after = scan.CurrentState();
		if (before == SqlScanner::State::CODE && after == SqlScanner::State::BLOCK_COMMENT) {
			open = at;
		} else if (before == SqlScanner::State::BLOCK_COMMENT && after == SqlScanner::State::CODE &&
		           open != std::string::npos) {
			comments.push_back({open, scan.Position()});
			open = std::string::npos;
		} else if (before == SqlScanner::State::CODE && after != SqlScanner::State::LINE_COMMENT &&
		           !std::isspace(static_cast<unsigned char>(body[at]))) {
			code_end = scan.Position();
		}
	}
	std::string sql;
	size_t copied = 0;
	bool misplaced = false;
	for (auto &comment : comments) {
		std::string spec;
		if (!ChartComment(body.substr(comment.begin, comment.end - comment.begin), spec)) {
			continue;
		}
		if (comment.begin < code_end) {
			misplaced = true;
			continue;
		}
		cell.charts.push_back(spec);
		sql += body.substr(copied, comment.begin - copied);
		copied = comment.end;
	}
	sql += body.substr(copied);
	cell.sql = TrimBlankLines(sql);
	if (misplaced) {
		cell.warnings.push_back("a /* chart */ comment draws a chart only at the end of the cell, after its query");
	}
}

void CheckBody(Cell &cell) {
	switch (cell.kind) {
	case CellKind::NOTEBOOK:
	case CellKind::MARKDOWN:
	case CellKind::CHART:
		if (!SingleBlockComment(cell.body, cell.content)) {
			cell.errors.push_back(std::string(CellKindName(cell.kind)) +
			                      " cell body must be a single /* ... */ block comment");
		} else if (cell.kind == CellKind::MARKDOWN) {
			std::string error;
			cell.expressions = FindExpressions(cell.content, error);
			if (!error.empty()) {
				cell.errors.push_back(error);
			}
			for (auto &expression : cell.expressions) {
				for (auto &name : ReferencedVariables(expression.sql)) {
					if (std::find(cell.variables.begin(), cell.variables.end(), name) == cell.variables.end()) {
						cell.variables.push_back(name);
					}
				}
			}
		}
		if (cell.kind == CellKind::CHART && cell.Option("from").empty()) {
			cell.errors.push_back("chart cell needs from=<cell or table>");
		}
		if (cell.kind == CellKind::CHART) {
			cell.charts.push_back(cell.content);
		}
		if (cell.kind == CellKind::NOTEBOOK && cell.errors.empty()) {
			std::string error;
			auto meta = ParseYaml(cell.content, error);
			for (auto key : {"timing", "animate"}) {
				auto flag = meta.Get(key);
				if (flag && (!flag->IsScalar() || (flag->scalar != "true" && flag->scalar != "false"))) {
					cell.errors.push_back(std::string(key) + ": " + flag->scalar + " is not valid; use " + key +
					                      ": true or " + key + ": false");
				}
			}
		}
		break;
	case CellKind::INPUT: {
		cell.sql = cell.body;
		auto statements = SplitStatements(cell.body);
		std::string variable;
		if (statements.size() != 1 || !IsSetVariable(statements[0].text, variable)) {
			cell.errors.push_back("input cell body must be one statement: SET VARIABLE " + cell.name + " = <default>;");
		} else if (variable != cell.name) {
			cell.errors.push_back("input cell '" + cell.name + "' sets variable '" + variable +
			                      "'; the two names must match");
		}
		break;
	}
	case CellKind::SQL: {
		SplitCharts(cell);
		auto statements = SplitStatements(cell.sql);
		cell.is_query = !statements.empty() && IsQueryStatement(statements.back().text);
		cell.variables = ReferencedVariables(cell.sql);
		if (!cell.charts.empty() && !cell.is_query) {
			cell.errors.push_back("only a cell that ends in a query can draw a chart; its last statement must "
			                      "return rows");
		}
		break;
	}
	}
}

} // namespace

const char *CellKindName(CellKind kind) {
	switch (kind) {
	case CellKind::NOTEBOOK:
		return "notebook";
	case CellKind::MARKDOWN:
		return "markdown";
	case CellKind::INPUT:
		return "input";
	case CellKind::CHART:
		return "chart";
	case CellKind::SQL:
		return "sql";
	}
	return "sql";
}

std::string Cell::Option(const std::string &key, const std::string &fallback) const {
	for (auto &option : options) {
		if (option.first == key) {
			return option.second;
		}
	}
	return fallback;
}

bool Notebook::HasErrors() const {
	for (auto &cell : cells) {
		if (!cell.errors.empty()) {
			return true;
		}
	}
	return false;
}

std::vector<Expression> FindExpressions(const std::string &markdown, std::string &error) {
	std::vector<Expression> expressions;
	size_t from = 0;
	while (true) {
		size_t open = markdown.find("{{", from);
		if (open == std::string::npos) {
			return expressions;
		}
		// Scan the rest as SQL so braces inside strings and comments are skipped. Struct
		// literals use braces too, so `}}` only closes at brace depth zero.
		std::string rest = markdown.substr(open + 2);
		SqlScanner scan(rest);
		int depth = 0;
		size_t close = std::string::npos;
		while (!scan.Done()) {
			size_t at = scan.Position();
			if (scan.CurrentState() == SqlScanner::State::CODE) {
				if (rest[at] == '{') {
					depth++;
				} else if (rest[at] == '}') {
					if (depth == 0 && at + 1 < rest.size() && rest[at + 1] == '}') {
						close = at;
						break;
					}
					depth = depth > 0 ? depth - 1 : 0;
				}
			}
			scan.Advance();
		}
		if (close == std::string::npos) {
			error = "unterminated {{ in markdown; expressions are written {{ expr }}";
			return expressions;
		}
		Expression expression;
		expression.begin = open;
		expression.end = open + 2 + close + 2;
		expression.sql = TrimBlankLines(rest.substr(0, close));
		if (expression.sql.empty()) {
			error = "empty {{ }} in markdown";
			return expressions;
		}
		expressions.push_back(std::move(expression));
		from = expressions.back().end;
	}
}

Notebook ParseNotebook(const std::string &text) {
	Notebook notebook;

	// A marker is a line starting with `-- %%` while the scanner is in code: the same
	// characters inside a block comment or string literal are content, not a marker.
	struct Marker {
		size_t line_start;
		size_t line_end;
		int line;
	};
	std::vector<Marker> markers;
	SqlScanner scan(text);
	bool at_line_start = true;
	while (!scan.Done()) {
		size_t at = scan.Position();
		if (at_line_start && scan.CurrentState() == SqlScanner::State::CODE && text.compare(at, 5, MARKER) == 0) {
			size_t end = text.find('\n', at);
			if (end == std::string::npos) {
				end = text.size();
			}
			markers.push_back({at, end, scan.Line()});
		}
		at_line_start = text[at] == '\n';
		size_t n = scan.Advance();
		if (n > 1) {
			at_line_start = text[scan.Position() - 1] == '\n';
		}
	}

	notebook.preamble = markers.empty() ? text : text.substr(0, markers[0].line_start);
	// A final newline ends the last line; it does not start another one.
	int total_lines = scan.Line() - (!text.empty() && text.back() == '\n' ? 1 : 0);
	std::map<std::string, int> first_use;
	for (size_t m = 0; m < markers.size(); m++) {
		Cell cell;
		cell.index = static_cast<int>(m);
		cell.start_line = markers[m].line;
		cell.end_line = m + 1 < markers.size() ? markers[m + 1].line - 1 : total_lines;
		size_t head_start = markers[m].line_start + 5;
		cell.head = TrimBlankLines(text.substr(head_start, markers[m].line_end - head_start));
		ParseHead(cell.head, cell);

		size_t body_start = markers[m].line_end;
		size_t body_end = m + 1 < markers.size() ? markers[m + 1].line_start : text.size();
		cell.body = TrimBlankLines(text.substr(body_start, body_end - body_start));
		CheckBody(cell);
		CheckOptions(cell);

		if (cell.kind == CellKind::NOTEBOOK && m != 0) {
			cell.errors.push_back("[notebook] must be the first cell");
		}
		if (!cell.name.empty()) {
			auto found = first_use.find(cell.name);
			if (found == first_use.end()) {
				first_use[cell.name] = cell.index;
			} else {
				cell.errors.push_back("duplicate cell name '" + cell.name + "'");
				auto &original = notebook.cells[found->second];
				std::string message = "duplicate cell name '" + cell.name + "'";
				bool already = false;
				for (auto &e : original.errors) {
					already = already || e == message;
				}
				if (!already) {
					original.errors.push_back(message);
				}
			}
		}
		notebook.cells.push_back(std::move(cell));
	}
	return notebook;
}

} // namespace notebook
