#include "notebook/script.hpp"

#include "notebook/sql_lexer.hpp"

#include <set>

namespace notebook {

namespace {

std::string WithSemicolon(const std::string &sql) {
	return sql.empty() ? sql : sql + ";\n";
}

} // namespace

std::string QuoteIdentifier(const std::string &name) {
	std::string quoted = "\"";
	for (char c : name) {
		quoted += c;
		if (c == '"') {
			quoted += '"';
		}
	}
	return quoted + "\"";
}

std::string QuoteString(const std::string &text) {
	std::string quoted = "'";
	for (char c : text) {
		quoted += c;
		if (c == '\'') {
			quoted += '\'';
		}
	}
	return quoted + "'";
}

namespace {

//! Splits `STRUCT(a DATE, b VARCHAR)` into field types, and `DATE[]` into its element type.
std::string ElementType(const std::string &type) {
	if (type.size() > 2 && type.compare(type.size() - 2, 2, "[]") == 0) {
		return type.substr(0, type.size() - 2);
	}
	return type;
}

std::string FieldType(const std::string &type, const std::string &field) {
	auto open = type.find('(');
	if (type.compare(0, 6, "STRUCT") != 0 || open == std::string::npos) {
		return type;
	}
	int depth = 0;
	size_t start = open + 1;
	for (size_t i = open + 1; i < type.size(); i++) {
		char c = type[i];
		if (c == '(') {
			depth++;
		} else if ((c == ',' || c == ')') && depth == 0) {
			auto part = type.substr(start, i - start);
			while (!part.empty() && part[0] == ' ') {
				part.erase(0, 1);
			}
			auto space = part.find(' ');
			if (space != std::string::npos) {
				auto name = part.substr(0, space);
				if (name.size() >= 2 && name.front() == '"') {
					name = name.substr(1, name.size() - 2);
				}
				if (name == field) {
					return part.substr(space + 1);
				}
			}
			start = i + 1;
		} else if (c == ')') {
			depth--;
		}
	}
	return std::string();
}

} // namespace

std::string JsonToSqlLiteral(const Json &value, const std::string &type) {
	switch (value.GetType()) {
	case Json::Type::NUL:
		return type.empty() ? "NULL" : "NULL::" + type;
	case Json::Type::BOOL:
		return value.Truthy() ? "true" : "false";
	case Json::Type::NUMBER: {
		auto literal = value.Dump();
		return type.empty() ? literal : literal + "::" + type;
	}
	case Json::Type::STRING:
	case Json::Type::RAW: {
		auto literal = QuoteString(value.Str());
		return type.empty() ? literal : literal + "::" + type;
	}
	case Json::Type::ARRAY: {
		std::string out = "[";
		auto element = ElementType(type);
		for (size_t i = 0; i < value.Items().size(); i++) {
			out += (i ? ", " : "") + JsonToSqlLiteral(value.Items()[i], element);
		}
		return out + "]";
	}
	case Json::Type::OBJECT: {
		std::string out = "{";
		bool first = true;
		for (auto &entry : value.Entries()) {
			out += (first ? "" : ", ") + QuoteString(entry.first) + ": " +
			       JsonToSqlLiteral(entry.second, FieldType(type, entry.first));
			first = false;
		}
		return out + "}";
	}
	}
	return "NULL";
}

std::string CellSql(const Cell &cell) {
	if (cell.kind == CellKind::INPUT) {
		std::string result;
		for (auto &statement : SplitStatements(cell.sql)) {
			result += WithSemicolon(statement.text);
		}
		return result;
	}
	if (cell.kind != CellKind::SQL) {
		return std::string();
	}
	auto statements = SplitStatements(cell.sql);
	if (statements.empty()) {
		return std::string();
	}
	std::string result;
	bool wrap = cell.is_query && !cell.name.empty();
	for (size_t i = 0; i + (wrap ? 1 : 0) < statements.size(); i++) {
		result += WithSemicolon(statements[i].text);
	}
	if (wrap) {
		// Comments written above the query stay above the CREATE, where they still read naturally.
		const auto &last = statements.back().text;
		auto query = StripLeadingComments(last);
		result += last.substr(0, last.size() - query.size());
		const char *object = cell.Option("materialize", "view") == "table" ? "TABLE" : "VIEW";
		result += std::string("CREATE OR REPLACE TEMP ") + object + " " + QuoteIdentifier(cell.name) + " AS\n";
		result += WithSemicolon(query);
	}
	return result;
}

std::string ChartFrom(const Cell &cell) {
	if (cell.kind == CellKind::CHART) {
		return cell.Option("from");
	}
	if (cell.kind != CellKind::SQL || !cell.is_query) {
		return std::string();
	}
	if (!cell.name.empty()) {
		return cell.name;
	}
	auto statements = SplitStatements(cell.sql);
	// On lines of their own, so a trailing -- comment cannot swallow the parenthesis.
	return "(\n" + statements.back().text + "\n)";
}

std::vector<std::string> UndefinedVariables(const Notebook &notebook) {
	std::set<std::string> defined;
	for (auto &cell : notebook.cells) {
		if (cell.kind == CellKind::INPUT) {
			defined.insert(cell.name);
		} else if (cell.kind == CellKind::SQL) {
			for (auto &statement : SplitStatements(cell.sql)) {
				std::string name;
				if (IsSetVariable(statement.text, name)) {
					defined.insert(name);
				}
			}
		}
	}
	std::vector<std::string> undefined;
	std::set<std::string> seen;
	for (auto &cell : notebook.cells) {
		for (auto &name : cell.variables) {
			if (!defined.count(name) && seen.insert(name).second) {
				undefined.push_back(name);
			}
		}
	}
	return undefined;
}

std::string ParameterError(const Notebook &notebook, const std::string &name) {
	for (auto &cell : notebook.cells) {
		if (cell.kind == CellKind::INPUT && cell.name == name) {
			return std::string();
		}
	}
	for (auto &undefined : UndefinedVariables(notebook)) {
		if (undefined == name) {
			return std::string();
		}
	}
	for (auto &cell : notebook.cells) {
		if (cell.kind != CellKind::SQL) {
			continue;
		}
		for (auto &statement : SplitStatements(cell.sql)) {
			std::string set;
			if (IsSetVariable(statement.text, set) && set == name) {
				return "variable '" + name + "' is set by the SQL cell at line " + std::to_string(cell.start_line) +
				       ", so it cannot be a parameter; make it an input cell";
			}
		}
	}
	return "the notebook has no input or variable named '" + name + "'";
}

std::string ExpandNotebook(const Notebook &notebook, const std::string &source_name) {
	std::string script = "-- Expanded from " + source_name + ". Runs in any DuckDB 2.0+ client; no extension needed.\n";
	script += "-- Generated by notebook_sql(); edit the notebook, not this file.\n";
	auto undefined = UndefinedVariables(notebook);
	if (!undefined.empty()) {
		script += "\n-- Variables read by cells but set only interactively start as NULL.\n";
		for (auto &name : undefined) {
			script += "SET VARIABLE " + QuoteIdentifier(name) + " = NULL;\n";
		}
	}
	for (auto &cell : notebook.cells) {
		auto sql = CellSql(cell);
		if (sql.empty()) {
			continue;
		}
		script += "\n-- " + std::string(CellKindName(cell.kind)) + " cell";
		if (!cell.name.empty()) {
			script += " " + cell.name;
		}
		script += " (line " + std::to_string(cell.start_line) + ")\n" + sql;
	}
	return script;
}

} // namespace notebook
