#pragma once

#include <string>
#include <vector>

namespace notebook {

//! One SQL statement cut out of a larger text.
struct Statement {
	//! The statement without its terminating semicolon or surrounding whitespace.
	std::string text;
	//! 1-based line of the statement's first token within the text it was cut from.
	int line = 1;
};

//! Walks SQL text while tracking whether the cursor is inside a string, quoted
//! identifier, dollar-quoted string or comment. Everything that needs to "find X
//! outside strings and comments" is built on this.
class SqlScanner {
public:
	enum class State { CODE, LINE_COMMENT, BLOCK_COMMENT, STRING, QUOTED_IDENTIFIER, DOLLAR_STRING };

	explicit SqlScanner(const std::string &text) : text(text) {
	}

	bool Done() const {
		return pos >= text.size();
	}
	//! State of the character at Position(), before it is consumed.
	State CurrentState() const {
		return state;
	}
	size_t Position() const {
		return pos;
	}
	int Line() const {
		return line;
	}
	//! Consumes one lexical step (usually one character) and returns how many bytes it covered.
	size_t Advance();

private:
	const std::string &text;
	size_t pos = 0;
	int line = 1;
	State state = State::CODE;
	int block_depth = 0;
	std::string dollar_tag;
};

//! A balanced `( ... )` in SQL code: byte offsets of its two parentheses.
struct ParenPair {
	size_t open = 0;
	size_t close = 0;
};

//! Every balanced pair of parentheses in code (not in strings, quoted identifiers or comments),
//! ordered by their opening parenthesis. Unmatched parentheses are left out.
std::vector<ParenPair> ParenPairs(const std::string &sql);

//! Splits text into statements on top-level semicolons. Comments stay attached to
//! the statement that follows them; a trailing comment-only fragment is dropped.
std::vector<Statement> SplitStatements(const std::string &sql);

//! True if the statement returns rows and can be wrapped in a view
//! (SELECT, FROM-first, VALUES, TABLE, PIVOT, UNPIVOT, SUMMARIZE, DESCRIBE, SHOW,
//! or WITH ... followed by one of those).
bool IsQueryStatement(const std::string &statement);

//! Names referenced as `$name` or `$"name"` in code (not in strings or comments),
//! in order of first appearance. Positional parameters such as `$1` are ignored.
std::vector<std::string> ReferencedVariables(const std::string &sql);

//! One `$name` or `$"quoted name"` in SQL code: its byte range (from the `$`) and the name it reads.
struct VariableRef {
	size_t start;
	size_t end;
	std::string name;
};
//! Every variable reference in code, in order; strings, comments and `$tag$` strings are skipped.
std::vector<VariableRef> VariableRefs(const std::string &sql);

//! True if the statement changes state that belongs to one connection and is not a variable:
//! `CREATE TEMP ...`, `SET`/`RESET` of a setting, `USE`. A session that runs each request on a
//! fresh connection replays these.
bool IsConnectionState(const std::string &statement);

//! If the statement is `SET VARIABLE <name> = ...`, returns true and the name.
bool IsSetVariable(const std::string &statement, std::string &name);

//! Removes leading whitespace and comments.
std::string StripLeadingComments(const std::string &sql);

//! If the text is exactly one block comment (ignoring surrounding whitespace),
//! returns true and its inner text.
bool SingleBlockComment(const std::string &text, std::string &inner);

} // namespace notebook
