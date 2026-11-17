#pragma once

#include "notebook/format.hpp"
#include "notebook/json.hpp"

#include <string>
#include <vector>

namespace notebook {

//! Wraps a name in double quotes, doubling any quote inside it.
std::string QuoteIdentifier(const std::string &name);

//! The SQL that running a cell means. Empty for cells that only display something.
//!  - named query cell: its leading statements, then the last one wrapped as
//!    CREATE OR REPLACE TEMP VIEW <name> (or TEMP TABLE with materialize=table)
//!  - input cell: its SET VARIABLE statement
//!  - anything else: the body unchanged
std::string CellSql(const Cell &cell);

//! The relation a cell's charts read, as a chart `from`: a chart cell's from=, a named query
//! cell's name, or an unnamed query cell's query in parentheses. Empty for other cells.
std::string ChartFrom(const Cell &cell);

//! Variables that cells read with `$name` but that no input cell or SET VARIABLE
//! in the notebook defines (chart selections, typically). `$name` on an unset
//! variable is an error in DuckDB, so the runner starts these as NULL.
std::vector<std::string> UndefinedVariables(const Notebook &notebook);

//! Empty if `name` can be given as a parameter to a run, otherwise why not. Parameters
//! override input defaults and set variables that nothing in the notebook sets (chart
//! selections); a variable that an SQL cell sets would be overwritten, so it is refused.
std::string ParameterError(const Notebook &notebook, const std::string &name);

//! A SQL literal for a JSON value sent by the browser, cast to `type` when given. Arrays become
//! lists and objects become structs, each element cast to the matching part of a LIST or STRUCT
//! type: `{"lo": "2026-01-01", "hi": "2026-02-01"}` with `STRUCT(lo DATE, hi DATE)` or with `DATE`
//! (applied to each field).
std::string JsonToSqlLiteral(const Json &value, const std::string &type = std::string());

//! A string as a SQL string literal.
std::string QuoteString(const std::string &text);

//! A plain SQL script equivalent to running every cell in file order. It needs no
//! extension: this is the `sql` export described in docs/FORMAT.md section 7.
std::string ExpandNotebook(const Notebook &notebook, const std::string &source_name);

} // namespace notebook
