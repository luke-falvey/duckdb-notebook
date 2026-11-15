#pragma once

#include "notebook/format.hpp"

#include <functional>
#include <string>
#include <vector>

// The dependency graph between cells (docs/DESIGN.md section 3). Which names a piece of SQL
// defines and reads comes from DuckDB's parser, which core cannot use, so the caller passes
// an extractor in. Everything else (resolving names to cells, ordering rules, cycles, run
// order) lives here and is unit-tested without a database.
namespace notebook {

//! What one piece of SQL defines and reads. Relation names are lower-case and qualified
//! only as far as needed (`orders`, `wh.orders`; `main.` and `temp.` dropped); variables
//! are written with a leading `$`.
struct References {
	std::vector<std::string> defines;
	std::vector<std::string> reads;
	//! Set if the SQL could not be analysed (a syntax error, typically).
	std::string error;
};

//! Analyses SQL text, which may hold several statements.
using ReferenceExtractor = std::function<References(const std::string &sql)>;

//! Fills each cell's `defines`, `reads` and `depends_on`, and reports on the cells:
//!  - errors: a name defined by more than one cell; an SQL or input cell reading something
//!    defined further down; a dependency cycle
//!  - warnings: a variable that no cell sets; SQL the extractor could not analyse
//! Cells with format errors are still analysed as far as possible.
void AnalyzeNotebook(Notebook &notebook, const ReferenceExtractor &extract);

//! The order in which running every cell executes them: file order, except that a
//! markdown or chart cell waits until the cells it reads have run. Needs AnalyzeNotebook.
std::vector<int> RunOrder(const Notebook &notebook);

//! The `as` of a chart spec's top-level `select:`, or empty.
std::string ChartSelection(const std::string &spec);

//! The selection names in a chart spec's top-level `filter:` (a name or a `[a, b]` list).
std::vector<std::string> ChartFilters(const std::string &spec);

} // namespace notebook
