#include "notebook/document.hpp"

#include "notebook/text.hpp"
#include "notebook/yaml.hpp"

#include <algorithm>
#include <cstdint>

namespace notebook {

namespace {

//! A file name without its directory and extension (`/a/revenue.sql` gives `revenue`).
std::string FileStem(const std::string &path) {
	auto name = path.substr(path.find_last_of("/\\:") + 1);
	auto dot = name.rfind('.');
	return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

std::string Cellish(const Json &value) {
	if (value.IsNull()) {
		return "";
	}
	std::string text = value.GetType() == Json::Type::STRING ? value.Str() : value.Dump();
	return ReplaceAll(ReplaceAll(text, "|", "\\|"), "\n", " ");
}

std::string MarkdownTable(const Json &table, size_t max_rows) {
	auto columns = table.Get("columns");
	auto rows = table.Get("rows");
	if (!columns || !rows || columns->Size() == 0) {
		return "";
	}
	std::string out = "|";
	std::string rule = "|";
	for (auto &column : columns->Items()) {
		out += " " + Cellish(*column.Get("name")) + " |";
		rule += " --- |";
	}
	out += "\n" + rule + "\n";
	size_t count = 0;
	for (auto &row : rows->Items()) {
		if (count++ >= max_rows) {
			break;
		}
		out += "|";
		for (auto &value : row.Items()) {
			out += " " + Cellish(value) + " |";
		}
		out += "\n";
	}
	auto total = table.Get("total");
	if (total && total->Num() > static_cast<double>(std::min<size_t>(max_rows, rows->Size()))) {
		out += "\n" + std::to_string(static_cast<int64_t>(total->Num())) + " rows in total.\n";
	}
	return out;
}

//! A chart in Markdown: its title and the data it draws.
std::string MarkdownChart(const Json &chart) {
	std::string title = "Chart";
	if (chart.Get("render") && chart.Get("render")->Get("title")) {
		title = chart.Get("render")->Get("title")->Str();
	}
	std::string out = "*" + title + "* (chart data; charts are drawn in the HTML export)\n\n";
	if (chart.Get("datasets") && chart.Get("datasets")->Size() > 0) {
		out += MarkdownTable(chart.Get("datasets")->Items()[0], 20) + "\n";
	} else if (chart.Get("error")) {
		out += "> Error: " + chart.Get("error")->Str() + "\n\n";
	}
	return out;
}

bool Hides(const std::string &hide, const char *what) {
	return hide == "all" || hide == what;
}

} // namespace

std::string MarkdownDocument(const Json &payload) {
	auto global_hide = payload.Get("hide") ? payload.Get("hide")->Str() : std::string();
	std::string out;
	for (auto &cell : payload.Get("cells")->Items()) {
		auto kind = cell.Get("kind")->Str();
		auto options = cell.Get("options");
		auto hide = options && options->Get("hide") ? options->Get("hide")->Str() : std::string();
		bool show_code = !Hides(hide, "code") && !Hides(global_hide, "code");
		bool show_output = !Hides(hide, "output") && !Hides(global_hide, "output");
		if (kind == "markdown") {
			if (auto output = cell.Get("output")) {
				out += output->Str() + "\n\n";
			} else if (auto error = cell.Get("error")) {
				out += "> Error: " + error->Str() + "\n\n";
			}
		} else if (kind == "sql") {
			if (show_code) {
				out += "```sql\n" + cell.Get("sql")->Str() + "\n```\n\n";
			}
			auto charts = cell.Get("charts");
			if (show_output && charts && charts->Size() > 0) {
				for (auto &chart : charts->Items()) {
					out += MarkdownChart(chart);
				}
			} else if (show_output) {
				if (cell.Get("error") && !cell.Get("error")->IsNull()) {
					out += "> Error: " + cell.Get("error")->Str() + "\n\n";
				} else if (auto table = cell.Get("table")) {
					out += MarkdownTable(*table, 20) + "\n";
				}
			}
		} else if (kind == "input") {
			auto label = options && options->Get("label") ? options->Get("label")->Str() : cell.Get("name")->Str();
			out += "**" + label + "**: " + Cellish(cell.Get("value") ? *cell.Get("value") : Json()) + "\n\n";
		} else if (kind == "chart" && show_output && cell.Get("chart")) {
			out += MarkdownChart(*cell.Get("chart"));
		}
	}
	return out;
}

std::string NotebookTitle(const Notebook &notebook, const std::string &source) {
	for (auto &cell : notebook.cells) {
		if (cell.kind == CellKind::NOTEBOOK) {
			std::string error;
			auto meta = ParseYaml(cell.content, error);
			auto title = meta.Str("title");
			if (!title.empty()) {
				return title;
			}
		}
	}
	for (auto &cell : notebook.cells) {
		if (cell.kind == CellKind::MARKDOWN && cell.content.compare(0, 2, "# ") == 0) {
			return cell.content.substr(2, cell.content.find('\n') - 2);
		}
	}
	return FileStem(source);
}

} // namespace notebook
