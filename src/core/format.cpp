#include "notebook/format.hpp"

#include "notebook/sql_lexer.hpp"

#include <cctype>
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

void CheckBody(Cell &cell) {
	switch (cell.kind) {
	case CellKind::NOTEBOOK:
	case CellKind::MARKDOWN:
	case CellKind::CHART:
		if (!SingleBlockComment(cell.body, cell.content)) {
			cell.errors.push_back(std::string(CellKindName(cell.kind)) +
			                      " cell body must be a single /* ... */ block comment");
		}
		if (cell.kind == CellKind::CHART && cell.Option("from").empty()) {
			cell.errors.push_back("chart cell needs from=<cell or table>");
		}
		break;
	case CellKind::INPUT: {
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
		auto statements = SplitStatements(cell.body);
		cell.is_query = !statements.empty() && IsQueryStatement(statements.back().text);
		cell.variables = ReferencedVariables(cell.body);
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

	// A final newline ends the last line; it does not start another one.
	int total_lines = scan.Line() - (!text.empty() && text.back() == '\n' ? 1 : 0);
	std::map<std::string, int> first_use;
	for (size_t m = 0; m < markers.size(); m++) {
		Cell cell;
		cell.index = static_cast<int>(m);
		cell.start_line = markers[m].line;
		cell.end_line = m + 1 < markers.size() ? markers[m + 1].line - 1 : total_lines;
		size_t head_start = markers[m].line_start + 5;
		ParseHead(text.substr(head_start, markers[m].line_end - head_start), cell);

		size_t body_start = markers[m].line_end;
		size_t body_end = m + 1 < markers.size() ? markers[m + 1].line_start : text.size();
		cell.body = TrimBlankLines(text.substr(body_start, body_end - body_start));
		CheckBody(cell);

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
