#include "notebook/functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/list.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/list.hpp"
#include "notebook/sql_lexer.hpp"

// "Peek under the pen" (docs/DESIGN.md section 19): finds the innermost CTE or sub-query under a
// cursor position and builds the statement that selects from it alone.
//
// The parts, their names and their scopes come from DuckDB's parse tree. The tree records where
// expressions and table references are, but not where a CTE or a sub-query begins and ends, so
// each part is matched to a pair of parentheses: the innermost pair around something the part
// contains whose text parses to the same query. Parentheses come from the SQL scanner and the
// start of a CTE's name from DuckDB's tokenizer; nothing here searches SQL text.
namespace duckdb {

namespace {

//! The CTEs of one query node that a part can see.
struct Frame {
	QueryNode *owner = nullptr;
	//! CTEs [0, visible) of the owner are in scope.
	idx_t visible = 0;
	//! The owner's CTE that the part is, or is inside of.
	optional_idx inside;
};

struct Part {
	bool cte = false;
	string name;
	QueryNode *node = nullptr;
	//! Outermost scope first.
	vector<Frame> frames;
	//! Byte offsets of the part's parentheses, and where the part starts (a CTE starts at its name).
	bool located = false;
	idx_t open = 0;
	idx_t close = 0;
	idx_t start = 0;
};

struct CteEntry {
	string name;
	CommonTableExpressionInfo *info;
};

vector<CteEntry> Ctes(QueryNode &node) {
	vector<CteEntry> result;
	for (auto &cte : node.cte_map.map) {
		result.push_back({cte.first.GetIdentifierName(), cte.second.get()});
	}
	return result;
}

//! Walks a query: every node, table reference and expression in it, sub-queries and CTEs included.
//! Anything it does not know how to walk sets `unknown`, and the caller refuses to peek.
class TreeWalker {
public:
	virtual ~TreeWalker() = default;
	bool unknown = false;
	//! A node that is not a plain query (INSERT ... RETURNING in a CTE, say).
	bool changes_data = false;

	void Node(QueryNode &node, const vector<Frame> &outer) {
		switch (node.type) {
		case QueryNodeType::SELECT_NODE:
		case QueryNodeType::SET_OPERATION_NODE:
		case QueryNodeType::RECURSIVE_CTE_NODE:
			break;
		default:
			changes_data = true;
			return;
		}
		auto frames = outer;
		auto ctes = Ctes(node);
		for (idx_t i = 0; i < ctes.size(); i++) {
			if (!ctes[i].info->query_node) {
				unknown = true;
				continue;
			}
			auto inner = outer;
			Frame frame;
			frame.owner = &node;
			frame.visible = i;
			frame.inside = i;
			inner.push_back(frame);
			OnPart(true, ctes[i].name, *ctes[i].info->query_node, inner);
			Node(*ctes[i].info->query_node, inner);
			for (auto &target : ctes[i].info->key_targets) {
				Expr(target, inner);
			}
			for (auto &aggregate : ctes[i].info->payload_aggregates) {
				Expr(aggregate, inner);
			}
		}
		if (!ctes.empty()) {
			Frame frame;
			frame.owner = &node;
			frame.visible = ctes.size();
			frames.push_back(frame);
		}
		auto expr = [&](unique_ptr<ParsedExpression> &child) {
			Expr(child, frames);
		};
		switch (node.type) {
		case QueryNodeType::SELECT_NODE: {
			auto &select = node.Cast<SelectNode>();
			for (auto &child : select.select_list) {
				expr(child);
			}
			for (auto &child : select.groups.group_expressions) {
				expr(child);
			}
			expr(select.where_clause);
			expr(select.having);
			expr(select.qualify);
			if (select.from_table) {
				Ref(*select.from_table, frames);
			}
			break;
		}
		case QueryNodeType::SET_OPERATION_NODE:
			for (auto &child : node.Cast<SetOperationNode>().children) {
				Node(*child, frames);
			}
			break;
		default: {
			auto &recursive = node.Cast<RecursiveCTENode>();
			Node(*recursive.left, frames);
			Node(*recursive.right, frames);
			for (auto &target : recursive.key_targets) {
				expr(target);
			}
			break;
		}
		}
		ParsedExpressionIterator::EnumerateQueryNodeModifiers(node, expr);
	}

protected:
	virtual void OnPart(bool cte, const string &name, QueryNode &node, const vector<Frame> &frames) {
	}
	virtual void OnRef(TableRef &ref) {
	}
	virtual void OnExpr(ParsedExpression &expr) {
	}

private:
	void Ref(TableRef &ref, const vector<Frame> &frames) {
		OnRef(ref);
		switch (ref.type) {
		case TableReferenceType::BASE_TABLE:
		case TableReferenceType::EMPTY_FROM:
			break;
		case TableReferenceType::SUBQUERY: {
			auto &subquery = ref.Cast<SubqueryRef>();
			OnPart(false, ref.alias.GetIdentifierName(), *subquery.subquery->node, frames);
			Node(*subquery.subquery->node, frames);
			break;
		}
		case TableReferenceType::JOIN: {
			auto &join = ref.Cast<JoinRef>();
			Ref(*join.left, frames);
			Ref(*join.right, frames);
			Expr(join.condition, frames);
			break;
		}
		case TableReferenceType::TABLE_FUNCTION: {
			auto &function = ref.Cast<TableFunctionRef>();
			Expr(function.function, frames);
			if (function.subquery) {
				Node(*function.subquery->node, frames);
			}
			break;
		}
		case TableReferenceType::EXPRESSION_LIST:
			for (auto &row : ref.Cast<ExpressionListRef>().values) {
				for (auto &value : row) {
					Expr(value, frames);
				}
			}
			break;
		case TableReferenceType::PIVOT: {
			auto &pivot = ref.Cast<PivotRef>();
			Ref(*pivot.source, frames);
			for (auto &aggregate : pivot.aggregates) {
				Expr(aggregate, frames);
			}
			break;
		}
		case TableReferenceType::SHOW_REF: {
			auto &show = ref.Cast<ShowRef>();
			if (show.query) {
				Node(*show.query, frames);
			}
			break;
		}
		default:
			unknown = true;
			break;
		}
	}

	void Expr(unique_ptr<ParsedExpression> &expression, const vector<Frame> &frames) {
		if (!expression) {
			return;
		}
		auto &expr = *expression;
		OnExpr(expr);
		if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
			auto &subquery = *expr.Cast<SubqueryExpression>().SubqueryMutable();
			OnPart(false, string(), *subquery.node, frames);
			Node(*subquery.node, frames);
		}
		ParsedExpressionIterator::EnumerateChildren(expr,
		                                            [&](unique_ptr<ParsedExpression> &child) { Expr(child, frames); });
	}
};

class PartCollector : public TreeWalker {
public:
	vector<Part> parts;

protected:
	void OnPart(bool cte, const string &name, QueryNode &node, const vector<Frame> &frames) override {
		Part part;
		part.cte = cte;
		part.name = name;
		part.node = &node;
		part.frames = frames;
		parts.push_back(std::move(part));
	}
};

//! What a query reads and calls, and somewhere in the text that is inside it.
class Reads : public TreeWalker {
public:
	//! Unqualified table names, lower-case: the only names a CTE can answer to.
	std::set<string> tables;
	//! Functions by lower-case name, table functions included.
	std::set<string> functions;
	QueryLocation span;

protected:
	void OnRef(TableRef &ref) override {
		span = span.Merge(ref.query_location);
		if (ref.type == TableReferenceType::BASE_TABLE) {
			auto &name = ref.Cast<BaseTableRef>().GetQualifiedName();
			if (name.Catalog().GetIdentifierName().empty() && name.Schema().GetIdentifierName().empty()) {
				tables.insert(StringUtil::Lower(name.Name().GetIdentifierName()));
			}
		}
	}
	void OnExpr(ParsedExpression &expr) override {
		if (expr.HasQueryLocation()) {
			span = span.Merge(expr.GetQueryLocation());
		}
		if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
			functions.insert(StringUtil::Lower(expr.Cast<FunctionExpression>().FunctionName().GetIdentifierName()));
		} else if (expr.GetExpressionClass() == ExpressionClass::WINDOW) {
			functions.insert(StringUtil::Lower(expr.Cast<WindowExpression>().FunctionName().GetIdentifierName()));
		}
	}
};

//! True if `text` is one query and it is the query `node` holds.
bool SameQuery(ClientContext &context, const string &text, QueryNode &node) {
	try {
		Parser parser(context);
		parser.ParseQuery(text);
		if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
			return false;
		}
		auto &candidate = *parser.statements[0]->Cast<SelectStatement>().node;
		return candidate.Equals(&node) || candidate.ToString() == node.ToString();
	} catch (std::exception &) {
		return false;
	}
}

//! A recursive CTE is written as a set operation, which is how its text parses on its own.
bool SameRecursiveQuery(ClientContext &context, const string &text, QueryNode &node) {
	if (node.type != QueryNodeType::RECURSIVE_CTE_NODE) {
		return false;
	}
	auto &recursive = node.Cast<RecursiveCTENode>();
	try {
		Parser parser(context);
		parser.ParseQuery(text);
		if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
			return false;
		}
		auto &candidate = *parser.statements[0]->Cast<SelectStatement>().node;
		if (candidate.type != QueryNodeType::SET_OPERATION_NODE) {
			return false;
		}
		auto &children = candidate.Cast<SetOperationNode>().children;
		return children.size() == 2 && children[0]->ToString() == recursive.left->ToString() &&
		       children[1]->ToString() == recursive.right->ToString();
	} catch (std::exception &) {
		return false;
	}
}

//! Finds each part's parentheses: the innermost pair around something inside the part whose text
//! is the part's query.
void Locate(ClientContext &context, const string &sql, const std::vector<notebook::ParenPair> &pairs,
            vector<Part> &parts) {
	for (auto &part : parts) {
		Reads reads;
		reads.Node(*part.node, {});
		if (!reads.span.IsValid()) {
			continue;
		}
		// Pairs are ordered by where they open, so the innermost one around a place is the last.
		for (idx_t i = pairs.size(); i-- > 0;) {
			auto &pair = pairs[i];
			if (pair.open >= reads.span.Start() || pair.close < reads.span.End()) {
				continue;
			}
			auto inner = sql.substr(pair.open + 1, pair.close - pair.open - 1);
			if (SameQuery(context, inner, *part.node) || SameRecursiveQuery(context, inner, *part.node)) {
				part.located = true;
				part.open = pair.open;
				part.close = pair.close;
				part.start = pair.open;
				break;
			}
		}
	}
}

bool IsWord(const string &sql, idx_t start, idx_t end, const char *word) {
	return StringUtil::CIEquals(sql.substr(start, end - start), word);
}

//! A CTE starts at its name, so the cursor can be on `name AS (` as well as inside the parentheses.
//! Between what comes before the CTE (a sibling's closing parenthesis, or the opening one of the
//! query that holds it) and its own parenthesis, the name is the first token after the last
//! `WITH`, `RECURSIVE` or comma that is not inside a column list.
void StartAtNames(const string &sql, idx_t statement_start, vector<Part> &parts) {
	vector<SimplifiedToken> tokens;
	try {
		tokens = Parser::Tokenize(sql);
	} catch (std::exception &) {
		return;
	}
	for (auto &part : parts) {
		if (!part.cte || !part.located) {
			continue;
		}
		idx_t from = statement_start;
		for (auto &other : parts) {
			if (!other.located || &other == &part) {
				continue;
			}
			if (other.close < part.open) {
				from = MaxValue(from, other.close + 1);
			} else if (other.open < part.open && other.close > part.close) {
				from = MaxValue(from, other.open + 1);
			}
		}
		bool want_name = false;
		int depth = 0;
		for (idx_t i = 0; i < tokens.size(); i++) {
			auto start = tokens[i].start;
			if (start < from) {
				continue;
			}
			if (start >= part.open) {
				break;
			}
			auto end = i + 1 < tokens.size() ? tokens[i + 1].start : sql.size();
			while (end > start && StringUtil::CharacterIsSpace(sql[end - 1])) {
				end--;
			}
			auto type = tokens[i].type;
			if (type == SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT) {
				continue;
			}
			if (type == SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD &&
			    (IsWord(sql, start, end, "with") || IsWord(sql, start, end, "recursive"))) {
				want_name = true;
				depth = 0;
				continue;
			}
			if (type == SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR) {
				if (IsWord(sql, start, end, "(")) {
					depth++;
				} else if (IsWord(sql, start, end, ")")) {
					depth--;
				} else if (IsWord(sql, start, end, ",") && depth == 0) {
					want_name = true;
					continue;
				}
			}
			if (want_name) {
				part.start = start;
				want_name = false;
			}
		}
	}
}

//! Table functions that change something: this extension's, and DuckDB's own. DuckDB records no
//! such property for table functions, so ones from other extensions cannot be judged. Scalar
//! functions are judged by their stability instead.
const char *const CHANGING_TABLE_FUNCTIONS[] = {
    "notebook_run",    "notebook_export",      "notebook_save",           "notebook_checkout", "notebook_start",
    "notebook_stop",   "checkpoint",           "force_checkpoint",        "copy_database",     "import_database",
    "export_database", "truncate_duckdb_logs", "create_external_resource"};
const char *const CHANGING_PREFIXES[] = {"enable_", "disable_"};

//! Volatile functions that only compute a value.
const char *const HARMLESS_VOLATILE[] = {"random", "uuid", "gen_random_uuid", "uuidv4", "uuidv7"};

//! The first function in `reads` whose evaluation may have a side effect, or empty.
string SideEffect(ClientContext &context, const Reads &reads) {
	for (auto name : CHANGING_TABLE_FUNCTIONS) {
		if (reads.functions.count(name)) {
			return name;
		}
	}
	for (auto &name : reads.functions) {
		for (auto prefix : CHANGING_PREFIXES) {
			if (StringUtil::StartsWith(name, prefix)) {
				return name;
			}
		}
		bool harmless = false;
		for (auto known : HARMLESS_VOLATILE) {
			harmless = harmless || name == known;
		}
		if (harmless) {
			continue;
		}
		EntryLookupInfo lookup(CatalogType::SCALAR_FUNCTION_ENTRY, QualifiedName(Identifier(name)));
		auto entry = Catalog::GetEntry(context, lookup, OnEntryNotFound::RETURN_NULL);
		if (!entry || entry->type != CatalogType::SCALAR_FUNCTION_ENTRY) {
			continue;
		}
		auto &functions = entry->Cast<ScalarFunctionCatalogEntry>().functions;
		for (idx_t i = 0; i < functions.Size(); i++) {
			if (functions.GetFunctionByOffset(i)->GetStability() == FunctionStability::VOLATILE) {
				return name;
			}
		}
	}
	return string();
}

//! `SELECT * FROM <from>`.
unique_ptr<SelectNode> SelectAll(unique_ptr<TableRef> from) {
	auto select = make_uniq<SelectNode>();
	select->select_list.push_back(make_uniq<StarExpression>());
	select->from_table = std::move(from);
	return select;
}

unique_ptr<TableRef> AsSubquery(unique_ptr<QueryNode> node) {
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(node);
	return make_uniq<SubqueryRef>(std::move(statement));
}

//! The statement that selects from a part alone, with the CTEs it needs from each scope it is in.
//! Scopes nest as they did in the cell, so a name means what it meant there.
unique_ptr<SelectNode> BuildStatement(const Part &part, string &reason) {
	Reads reads;
	reads.Node(*part.node, {});
	unique_ptr<SelectNode> statement;
	if (part.cte) {
		auto table = make_uniq<BaseTableRef>();
		table->SetTable(Identifier(part.name));
		statement = SelectAll(std::move(table));
	} else {
		statement = SelectAll(AsSubquery(part.node->Copy()));
	}
	for (idx_t f = part.frames.size(); f-- > 0;) {
		auto &frame = part.frames[f];
		auto ctes = Ctes(*frame.owner);
		bool own = part.cte && f + 1 == part.frames.size();
		vector<idx_t> chosen;
		if (own) {
			chosen.push_back(frame.inside.GetIndex());
		} else if (frame.inside.IsValid()) {
			auto &around = ctes[frame.inside.GetIndex()];
			if (around.info->query_node->type == QueryNodeType::RECURSIVE_CTE_NODE &&
			    reads.tables.count(StringUtil::Lower(around.name))) {
				// On its own it would read the finished CTE, not the rows of one step.
				reason = "it reads the recursive CTE it is part of";
				return nullptr;
			}
		}
		for (idx_t i = frame.visible; i-- > 0;) {
			if (reads.tables.count(StringUtil::Lower(ctes[i].name))) {
				chosen.push_back(i);
				reads.Node(*ctes[i].info->query_node, {});
			}
		}
		if (chosen.empty()) {
			continue;
		}
		if (!statement->cte_map.map.empty()) {
			statement = SelectAll(AsSubquery(std::move(statement)));
		}
		std::sort(chosen.begin(), chosen.end());
		for (auto i : chosen) {
			statement->cte_map.map.insert(Identifier(ctes[i].name), ctes[i].info->Copy());
		}
	}
	return statement;
}

} // namespace

PeekTarget FindPeekTarget(ClientContext &context, const string &sql, idx_t cursor) {
	PeekTarget target;
	Parser parser(context);
	try {
		parser.ParseQuery(sql);
	} catch (std::exception &) {
		target.reason = "the SQL does not parse";
		return target;
	}
	// The statement the cursor is in.
	SQLStatement *statement = nullptr;
	for (auto &candidate : parser.statements) {
		auto &location = candidate->stmt_location;
		if (cursor >= location.Start() && cursor <= location.End()) {
			statement = candidate.get();
			break;
		}
	}
	if (!statement) {
		target.reason = "the cursor is not in a statement";
		return target;
	}
	if (statement->type != StatementType::SELECT_STATEMENT) {
		target.reason = "the statement is not a query";
		return target;
	}
	auto &root = *statement->Cast<SelectStatement>().node;
	PartCollector collector;
	collector.Node(root, {});
	if (collector.changes_data) {
		target.reason = "the statement changes data";
		return target;
	}
	if (collector.unknown) {
		target.reason = "the statement has a part that cannot be looked into";
		return target;
	}
	auto &parts = collector.parts;
	Locate(context, sql, notebook::ParenPairs(sql), parts);
	StartAtNames(sql, statement->stmt_location.Start(), parts);

	// The innermost part around the cursor. A caret just after the closing parenthesis still counts.
	Part *found = nullptr;
	for (auto &part : parts) {
		if (!part.located || cursor < part.start || cursor > part.close + 1) {
			continue;
		}
		if (!found || part.close - part.start < found->close - found->start) {
			found = &part;
		}
	}
	if (!found) {
		target.reason = "the cursor is in the main query";
		return target;
	}
	auto built = BuildStatement(*found, target.reason);
	if (!built) {
		return target;
	}
	Reads reads;
	reads.Node(*built, {});
	if (reads.changes_data || reads.unknown) {
		target.reason = "the part cannot be run on its own";
		return target;
	}
	auto effect = SideEffect(context, reads);
	if (!effect.empty()) {
		target.reason = "it calls " + effect + "(), which may have side effects";
		return target;
	}
	target.kind = found->cte ? "cte" : "subquery";
	target.name = found->name;
	target.start = found->start;
	target.end = found->close + 1;
	target.sql = built->ToString();
	return target;
}

namespace {

struct PeekBindData : public TableFunctionData {
	PeekTarget target;
};

struct PeekState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<FunctionData> PeekBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<PeekBindData>();
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw InvalidInputException("notebook_peek needs SQL text and a cursor position");
	}
	auto cursor = input.inputs[1].GetValue<int64_t>();
	if (cursor < 0) {
		throw InvalidInputException("notebook_peek: the cursor position is a byte offset, 0 or more");
	}
	result->target = FindPeekTarget(context, input.inputs[0].GetValue<string>(), NumericCast<idx_t>(cursor));
	names.emplace_back("kind");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("name");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("query");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("start");
	return_types.push_back(LogicalType::BIGINT);
	names.emplace_back("end");
	return_types.push_back(LogicalType::BIGINT);
	names.emplace_back("reason");
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> PeekInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<PeekState>();
}

void PeekScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<PeekState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &target = data.bind_data->Cast<PeekBindData>().target;
	bool found = !target.sql.empty();
	output.data[0].Append(found ? Value(target.kind) : Value());
	output.data[1].Append(found && !target.name.empty() ? Value(target.name) : Value());
	output.data[2].Append(found ? Value(target.sql) : Value());
	output.data[3].Append(found ? Value::BIGINT(NumericCast<int64_t>(target.start)) : Value());
	output.data[4].Append(found ? Value::BIGINT(NumericCast<int64_t>(target.end)) : Value());
	output.data[5].Append(found ? Value() : Value(target.reason));
}

} // namespace

void RegisterNotebookPeek(ExtensionLoader &loader) {
	loader.RegisterFunction(
	    TableFunction("notebook_peek", {LogicalType::VARCHAR, LogicalType::BIGINT}, PeekScan, PeekBind, PeekInit));
}

} // namespace duckdb
