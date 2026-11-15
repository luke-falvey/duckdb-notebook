#include "notebook/graph.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>

namespace notebook {

namespace {

std::string Lower(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

std::string TrimSpace(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
		b++;
	}
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
		e--;
	}
	return s.substr(b, e - b);
}

void AddUnique(std::vector<std::string> &values, const std::string &value) {
	if (std::find(values.begin(), values.end(), value) == values.end()) {
		values.push_back(value);
	}
}

//! How a name from `defines` or `reads` is shown in a message.
std::string Describe(const std::string &name) {
	if (!name.empty() && name[0] == '$') {
		return "variable '" + name.substr(1) + "'";
	}
	return "'" + name + "'";
}

std::string CellLabel(const Cell &cell) {
	return cell.name.empty() ? "line " + std::to_string(cell.start_line) : cell.name;
}

// A deliberately small reading of the chart spec's YAML: just enough to find the names a
// chart defines and reads. The chart compiler will replace it with a real parser.

std::string StripYamlScalar(std::string value) {
	auto comment = value.find(" #");
	if (comment != std::string::npos) {
		value = value.substr(0, comment);
	}
	value = TrimSpace(value);
	if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
		value = value.substr(1, value.size() - 2);
	}
	return value;
}

//! Finds `key:` at the start of an unindented line. Returns its inline value and, in
//! `block`, the indented lines that follow it.
bool TopLevelEntry(const std::string &spec, const std::string &key, std::string &value,
                   std::vector<std::string> &block) {
	size_t pos = 0;
	bool found = false;
	while (pos <= spec.size()) {
		size_t end = spec.find('\n', pos);
		if (end == std::string::npos) {
			end = spec.size();
		}
		auto line = spec.substr(pos, end - pos);
		bool indented = !line.empty() && std::isspace(static_cast<unsigned char>(line[0]));
		if (found) {
			if (!indented && !TrimSpace(line).empty()) {
				break;
			}
			block.push_back(line);
		} else if (line.compare(0, key.size() + 1, key + ":") == 0) {
			value = line.substr(key.size() + 1);
			found = true;
		}
		pos = end + 1;
	}
	return found;
}

//! Splits `a, b, {c: d}` on the commas that are not nested in braces or brackets.
std::vector<std::string> SplitFlow(const std::string &inner) {
	std::vector<std::string> parts;
	std::string current;
	int depth = 0;
	char quote = 0;
	for (char c : inner) {
		if (quote) {
			quote = c == quote ? 0 : quote;
		} else if (c == '"' || c == '\'') {
			quote = c;
		} else if (c == '{' || c == '[') {
			depth++;
		} else if (c == '}' || c == ']') {
			depth--;
		} else if (c == ',' && depth == 0) {
			parts.push_back(TrimSpace(current));
			current.clear();
			continue;
		}
		current += c;
	}
	if (!TrimSpace(current).empty()) {
		parts.push_back(TrimSpace(current));
	}
	return parts;
}

std::string MappingValue(const std::vector<std::string> &entries, const std::string &key) {
	for (auto &entry : entries) {
		auto trimmed = TrimSpace(entry);
		if (trimmed.compare(0, key.size() + 1, key + ":") == 0) {
			return StripYamlScalar(trimmed.substr(key.size() + 1));
		}
	}
	return std::string();
}

//! Tarjan's strongly connected components over `depends_on`.
struct Components {
	const Notebook &notebook;
	std::vector<int> index, low;
	std::vector<bool> on_stack;
	std::vector<int> stack;
	std::vector<std::vector<int>> result;
	int counter = 0;

	explicit Components(const Notebook &notebook)
	    : notebook(notebook), index(notebook.cells.size(), -1), low(notebook.cells.size(), 0),
	      on_stack(notebook.cells.size(), false) {
		for (size_t i = 0; i < notebook.cells.size(); i++) {
			if (index[i] < 0) {
				Visit(static_cast<int>(i));
			}
		}
	}

	void Visit(int v) {
		index[v] = low[v] = counter++;
		stack.push_back(v);
		on_stack[v] = true;
		for (int w : notebook.cells[v].depends_on) {
			if (index[w] < 0) {
				Visit(w);
				low[v] = std::min(low[v], low[w]);
			} else if (on_stack[w]) {
				low[v] = std::min(low[v], index[w]);
			}
		}
		if (low[v] == index[v]) {
			std::vector<int> component;
			int w;
			do {
				w = stack.back();
				stack.pop_back();
				on_stack[w] = false;
				component.push_back(w);
			} while (w != v);
			result.push_back(std::move(component));
		}
	}
};

void CollectReferences(Cell &cell, const ReferenceExtractor &extract) {
	auto analyse = [&](const std::string &sql, const std::string &what) {
		auto refs = extract(sql);
		if (!refs.error.empty()) {
			cell.warnings.push_back("could not analyse " + what + ", so what it reads is unknown: " + refs.error);
		}
		for (auto &name : refs.defines) {
			AddUnique(cell.defines, name);
		}
		for (auto &name : refs.reads) {
			AddUnique(cell.reads, name);
		}
	};
	switch (cell.kind) {
	case CellKind::NOTEBOOK:
		break;
	case CellKind::SQL:
		if (cell.is_query && !cell.name.empty()) {
			cell.defines.push_back(cell.name);
		}
		if (!cell.body.empty()) {
			analyse(cell.body, "this cell's SQL");
		}
		break;
	case CellKind::INPUT: {
		if (!cell.name.empty()) {
			cell.defines.push_back("$" + cell.name);
		}
		analyse(cell.body, "this input's default");
		// from=<cell>.<column>: the input reads the cell.
		auto from = cell.Option("from");
		auto dot = from.rfind('.');
		if (dot != std::string::npos && dot > 0) {
			AddUnique(cell.reads, Lower(from.substr(0, dot)));
		}
		break;
	}
	case CellKind::MARKDOWN:
		for (auto &expression : cell.expressions) {
			analyse("SELECT (" + expression.sql + ")", "{{ " + expression.sql + " }}");
		}
		break;
	case CellKind::CHART: {
		auto from = cell.Option("from");
		if (!from.empty()) {
			AddUnique(cell.reads, Lower(from));
		}
		for (auto &filter : ChartFilters(cell.content)) {
			AddUnique(cell.reads, "$" + filter);
		}
		auto selection = ChartSelection(cell.content);
		if (!selection.empty()) {
			cell.defines.push_back("$" + selection);
		}
		break;
	}
	}
	// What a cell defines for itself is not a dependency (CREATE TABLE t; INSERT INTO t ...).
	auto &reads = cell.reads;
	reads.erase(std::remove_if(reads.begin(), reads.end(),
	                           [&](const std::string &name) {
		                           return std::find(cell.defines.begin(), cell.defines.end(), name) !=
		                                  cell.defines.end();
	                           }),
	            reads.end());
}

} // namespace

std::string ChartSelection(const std::string &spec) {
	std::string value;
	std::vector<std::string> block;
	if (!TopLevelEntry(spec, "select", value, block)) {
		return std::string();
	}
	value = StripYamlScalar(value);
	if (value.size() >= 2 && value.front() == '{' && value.back() == '}') {
		return MappingValue(SplitFlow(value.substr(1, value.size() - 2)), "as");
	}
	return value.empty() ? MappingValue(block, "as") : std::string();
}

std::vector<std::string> ChartFilters(const std::string &spec) {
	std::string value;
	std::vector<std::string> block;
	std::vector<std::string> names;
	if (!TopLevelEntry(spec, "filter", value, block)) {
		return names;
	}
	value = StripYamlScalar(value);
	if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
		for (auto &part : SplitFlow(value.substr(1, value.size() - 2))) {
			AddUnique(names, StripYamlScalar(part));
		}
	} else if (!value.empty()) {
		names.push_back(value);
	} else {
		for (auto &line : block) {
			auto item = TrimSpace(line);
			if (item.compare(0, 2, "- ") == 0) {
				AddUnique(names, StripYamlScalar(item.substr(2)));
			}
		}
	}
	return names;
}

void AnalyzeNotebook(Notebook &notebook, const ReferenceExtractor &extract) {
	auto &cells = notebook.cells;
	for (auto &cell : cells) {
		cell.defines.clear();
		cell.reads.clear();
		cell.depends_on.clear();
		CollectReferences(cell, extract);
	}

	// One definition per name.
	std::map<std::string, std::vector<int>> definers;
	for (auto &cell : cells) {
		for (auto &name : cell.defines) {
			definers[name].push_back(cell.index);
		}
	}
	for (auto &entry : definers) {
		if (entry.second.size() < 2) {
			continue;
		}
		std::string lines;
		for (int index : entry.second) {
			lines += (lines.empty() ? "" : ", ") + std::to_string(cells[index].start_line);
		}
		for (int index : entry.second) {
			auto &cell = cells[index];
			// Two cells with the same name are already reported by the parser.
			auto duplicate_name = "duplicate cell name '" + entry.first + "'";
			if (std::find(cell.errors.begin(), cell.errors.end(), duplicate_name) != cell.errors.end()) {
				continue;
			}
			cell.errors.push_back(Describe(entry.first) + " is defined by more than one cell (lines " + lines + ")");
		}
	}

	// Resolve reads to the cells that define them.
	for (auto &cell : cells) {
		std::set<int> depends_on;
		for (auto &name : cell.reads) {
			if (name.empty()) {
				continue;
			}
			// `wh.orders` depends on the cell that defines it, or else on the one that
			// attached `wh` or created schema `wh`.
			auto lookup = name;
			auto found = definers.find(lookup);
			while (found == definers.end() && lookup.find('.') != std::string::npos) {
				lookup = lookup.substr(0, lookup.rfind('.'));
				found = definers.find(lookup);
			}
			if (found == definers.end()) {
				// Relations that no cell defines are catalog objects and need no cell.
				if (name[0] == '$') {
					cell.warnings.push_back(Describe(name) + " is not set by any cell, so it reads as NULL");
				}
				continue;
			}
			int definer = found->second.front();
			if (definer == cell.index) {
				continue;
			}
			depends_on.insert(definer);
			bool ordered = cell.kind == CellKind::SQL || cell.kind == CellKind::INPUT;
			if (ordered && definer > cell.index) {
				cell.errors.push_back(Describe(name) + " is defined further down, at line " +
				                      std::to_string(cells[definer].start_line) +
				                      "; SQL and input cells can only use cells above them");
			}
		}
		cell.depends_on.assign(depends_on.begin(), depends_on.end());
	}

	// Cycles. SQL and input cells only read upwards, so a cycle runs through a chart, or
	// through a forward reference that is already an error.
	Components components(notebook);
	for (auto &component : components.result) {
		if (component.size() < 2) {
			continue;
		}
		std::sort(component.begin(), component.end());
		std::string members;
		for (int index : component) {
			members += (members.empty() ? "" : ", ") + CellLabel(cells[index]);
		}
		for (int index : component) {
			cells[index].errors.push_back("dependency cycle between " + members);
		}
	}
}

std::vector<int> RunOrder(const Notebook &notebook) {
	struct Key {
		int anchor;
		int deferred;
		int index;
		bool operator<(const Key &other) const {
			return std::tie(anchor, deferred, index) < std::tie(other.anchor, other.deferred, other.index);
		}
	};
	std::vector<Key> keys;
	for (auto &cell : notebook.cells) {
		Key key {cell.index, 0, cell.index};
		if (cell.kind == CellKind::MARKDOWN || cell.kind == CellKind::CHART) {
			for (int dependency : cell.depends_on) {
				if (dependency > key.anchor) {
					key.anchor = dependency;
					key.deferred = 1;
				}
			}
		}
		keys.push_back(key);
	}
	std::sort(keys.begin(), keys.end());
	std::vector<int> order;
	for (auto &key : keys) {
		order.push_back(key.index);
	}
	return order;
}

} // namespace notebook
