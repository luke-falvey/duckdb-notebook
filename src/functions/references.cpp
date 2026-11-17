#include "notebook/functions.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/parameter_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/list.hpp"
#include "duckdb/parser/statement/list.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"
#include "duckdb/parser/tableref/list.hpp"

// Finds what SQL defines and reads by walking DuckDB's parse tree, without binding it: the
// tables a cell reads need not exist yet when the notebook is opened. This resolves spike S1
// of docs/DESIGN.md. The graph logic itself is in src/core/graph.cpp.
namespace duckdb {

namespace {

//! `main.` and `temp.` are where unqualified names live, so they are dropped: `main.t`,
//! `temp.main.t` and `t` all match a cell that creates `t`.
string NormalizeName(const QualifiedName &name) {
	vector<string> parts;
	for (auto &part : name.Path()) {
		if (!part.empty()) {
			parts.push_back(StringUtil::Lower(part.GetIdentifierName()));
		}
	}
	if (parts.size() >= 2 && parts[0] == "temp") {
		parts.erase(parts.begin());
	}
	if (parts.size() >= 2 && parts[parts.size() - 2] == "main") {
		parts.erase(parts.end() - 2);
	}
	return StringUtil::Join(parts, ".");
}

class ReferenceWalker {
public:
	notebook::References result;

	void Statement(SQLStatement &statement) {
		switch (statement.type) {
		case StatementType::SELECT_STATEMENT:
			Node(*statement.Cast<SelectStatement>().node);
			break;
		case StatementType::INSERT_STATEMENT:
			Node(*statement.Cast<InsertStatement>().node);
			break;
		case StatementType::UPDATE_STATEMENT:
			Node(*statement.Cast<UpdateStatement>().node);
			break;
		case StatementType::DELETE_STATEMENT:
			Node(*statement.Cast<DeleteStatement>().node);
			break;
		case StatementType::MERGE_INTO_STATEMENT:
			Node(*statement.Cast<MergeIntoStatement>().node);
			break;
		case StatementType::COPY_STATEMENT:
			Copy(*statement.Cast<CopyStatement>().info);
			break;
		case StatementType::CREATE_STATEMENT:
			Create(*statement.Cast<CreateStatement>().info);
			break;
		case StatementType::SET_STATEMENT: {
			auto &set = statement.Cast<SetStatement>();
			if (set.scope == SetScope::VARIABLE && set.set_type == SetType::SET) {
				auto &variable = statement.Cast<SetVariableStatement>();
				Expr(variable.value);
				Add(result.defines, "$" + variable.name.GetIdentifierName());
			}
			break;
		}
		case StatementType::ATTACH_STATEMENT: {
			auto &info = *statement.Cast<AttachStatement>().info;
			Expr(info.parsed_path);
			auto alias = info.name.GetIdentifierName();
			auto path = info.path;
			if (path.empty() && info.parsed_path &&
			    info.parsed_path->GetExpressionClass() == ExpressionClass::CONSTANT) {
				auto value = info.parsed_path->Cast<ConstantExpression>().GetLiteral().ToValue();
				path = value.IsNull() ? string() : value.ToString();
			}
			if (alias.empty()) {
				// ATTACH 'dir/sales.duckdb' is attached as `sales`.
				alias = path.substr(path.find_last_of("/\\") + 1);
				alias = alias.substr(0, alias.find('.'));
			}
			if (!alias.empty()) {
				Add(result.defines, StringUtil::Lower(alias));
			}
			break;
		}
		case StatementType::DROP_STATEMENT:
			AddRead(NormalizeName(statement.Cast<DropStatement>().info->GetQualifiedName()));
			break;
		case StatementType::EXPLAIN_STATEMENT:
			Statement(*statement.Cast<ExplainStatement>().stmt);
			break;
		case StatementType::CALL_STATEMENT:
			Expr(statement.Cast<CallStatement>().function);
			break;
		case StatementType::MULTI_STATEMENT:
			for (auto &child : statement.Cast<MultiStatement>().statements) {
				Statement(*child);
			}
			break;
		default:
			// PRAGMA, LOAD, INSTALL, transactions and the like neither define nor read cells.
			break;
		}
	}

	//! Drops reads of CTE names, which are local to the statement. CTE names are collected
	//! for the whole statement rather than per scope: a CTE that shadows a real table hides
	//! reads of that table elsewhere in the same statement.
	void Finish() {
		auto &reads = result.reads;
		reads.erase(
		    std::remove_if(reads.begin(), reads.end(), [&](const string &name) { return cte_names.count(name) > 0; }),
		    reads.end());
	}

private:
	std::set<string> cte_names;

	static void Add(std::vector<std::string> &list, const string &name) {
		if (!name.empty() && std::find(list.begin(), list.end(), name) == list.end()) {
			list.push_back(name);
		}
	}
	void AddRead(const string &name) {
		Add(result.reads, name);
	}

	void Node(QueryNode &node) {
		for (auto &cte : node.cte_map.map) {
			cte_names.insert(StringUtil::Lower(cte.first.GetIdentifierName()));
		}
		switch (node.type) {
		case QueryNodeType::INSERT_QUERY_NODE: {
			auto &insert = node.Cast<InsertQueryNode>();
			// The iterator visits the INSERT's source but not its target.
			AddRead(NormalizeName(insert.qualified_name));
			break;
		}
		case QueryNodeType::COPY_QUERY_NODE: {
			auto &info = *node.Cast<CopyQueryNode>().info;
			if (!info.select_statement) {
				AddRead(NormalizeName(info.GetQualifiedName()));
			}
			break;
		}
		default:
			break;
		}
		ParsedExpressionIterator::EnumerateQueryNodeChildren(
		    node, [&](unique_ptr<ParsedExpression> &child) { Expr(child); }, [&](TableRef &ref) { Ref(ref); });
	}

	void Ref(TableRef &ref) {
		switch (ref.type) {
		case TableReferenceType::BASE_TABLE:
			AddRead(NormalizeName(ref.Cast<BaseTableRef>().GetQualifiedName()));
			break;
		case TableReferenceType::SUBQUERY: {
			// The iterator descends into the subquery itself; only its CTE names are needed here.
			auto &subquery = ref.Cast<SubqueryRef>();
			for (auto &cte : subquery.subquery->node->cte_map.map) {
				cte_names.insert(StringUtil::Lower(cte.first.GetIdentifierName()));
			}
			break;
		}
		case TableReferenceType::SHOW_REF: {
			// DESCRIBE t and SUMMARIZE t name a table rather than holding a query.
			auto &show = ref.Cast<ShowRef>();
			if (!show.query && !show.GetTableName().empty()) {
				AddRead(NormalizeName(show.qualified_name));
			}
			break;
		}
		default:
			break;
		}
	}

	void Expr(unique_ptr<ParsedExpression> &expression) {
		if (!expression) {
			return;
		}
		auto &expr = *expression;
		switch (expr.GetExpressionClass()) {
		case ExpressionClass::PARAMETER: {
			// `$name` reads a variable; `$1` and `?` are positional parameters.
			auto &name = expr.Cast<ParameterExpression>().Identifier().GetIdentifierName();
			if (!name.empty() && !std::all_of(name.begin(), name.end(), StringUtil::CharacterIsDigit)) {
				AddRead("$" + name);
			}
			break;
		}
		case ExpressionClass::FUNCTION: {
			auto &function = expr.Cast<FunctionExpression>();
			auto &arguments = function.GetArguments();
			if (StringUtil::Lower(function.FunctionName().GetIdentifierName()) == "getvariable" &&
			    arguments.size() == 1 &&
			    arguments[0].GetExpression().GetExpressionClass() == ExpressionClass::CONSTANT) {
				auto value = arguments[0].GetExpression().Cast<ConstantExpression>().GetLiteral().ToValue();
				if (value.type().id() == LogicalTypeId::VARCHAR && !value.IsNull()) {
					AddRead("$" + value.GetValue<string>());
				}
			}
			break;
		}
		case ExpressionClass::SUBQUERY:
			Node(*expr.Cast<SubqueryExpression>().SubqueryMutable()->node);
			break;
		default:
			break;
		}
		ParsedExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<ParsedExpression> &child) { Expr(child); });
	}

	void Copy(CopyInfo &info) {
		if (info.select_statement) {
			Node(*info.select_statement);
		} else {
			AddRead(NormalizeName(info.GetQualifiedName()));
		}
		Expr(info.file_path_expression);
	}

	void Create(CreateInfo &info) {
		// PIVOT creates enum types for itself; they are not the user's to depend on.
		if (StringUtil::StartsWith(info.GetQualifiedName().Name().GetIdentifierName(), "__pivot_enum")) {
			return;
		}
		switch (info.type) {
		case CatalogType::TABLE_ENTRY: {
			auto &table = info.Cast<CreateTableInfo>();
			if (table.query) {
				Node(*table.query->node);
			}
			Add(result.defines, NormalizeName(info.GetQualifiedName()));
			break;
		}
		case CatalogType::VIEW_ENTRY: {
			auto &view = info.Cast<CreateViewInfo>();
			if (view.query) {
				Node(*view.query->node);
			}
			Add(result.defines, NormalizeName(info.GetQualifiedName()));
			break;
		}
		case CatalogType::SCHEMA_ENTRY:
			Add(result.defines, StringUtil::Lower(info.Cast<CreateSchemaInfo>().SchemaName().GetIdentifierName()));
			break;
		case CatalogType::INDEX_ENTRY: {
			auto &index = info.Cast<CreateIndexInfo>();
			auto table = info.GetQualifiedName().WithName(index.table);
			AddRead(NormalizeName(table));
			break;
		}
		case CatalogType::MACRO_ENTRY:
		case CatalogType::TABLE_MACRO_ENTRY:
		case CatalogType::SEQUENCE_ENTRY:
		case CatalogType::TYPE_ENTRY:
			// Defined, so two cells cannot both create one. Uses of macros, sequences and types
			// are not tracked as reads yet.
			Add(result.defines, NormalizeName(info.GetQualifiedName()));
			break;
		default:
			break;
		}
	}
};

} // namespace

notebook::References ExtractReferences(ClientContext &context, const string &sql) {
	ReferenceWalker walker;
	try {
		Parser parser(context);
		parser.ParseQuery(sql);
		for (auto &statement : parser.statements) {
			walker.Statement(*statement);
		}
	} catch (std::exception &ex) {
		ErrorData error(ex);
		walker.result.error = error.RawMessage();
	}
	walker.Finish();
	return std::move(walker.result);
}

} // namespace duckdb
