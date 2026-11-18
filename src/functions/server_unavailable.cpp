#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "notebook/functions.hpp"

// Stands in for server.cpp in builds without host features (NOTEBOOK_HOST_FEATURES=OFF, the default
// for WebAssembly; see CMakeLists.txt). The HTTP server needs sockets and threads, which DuckDB-WASM
// does not have. notebook_start and notebook_stop keep their signatures, so a script that calls them
// binds and fails with this message rather than with "function does not exist".
namespace duckdb {

namespace {

[[noreturn]] void Unavailable(const char *name) {
	throw NotImplementedException(
	    "%s: the notebook server is not available in this build of the sketchbook extension. It needs sockets "
	    "and threads, which DuckDB-WASM does not have. notebook_cells, notebook_sql, notebook_run, "
	    "notebook_chart_query and notebook_export (except pdf) work here",
	    name);
}

unique_ptr<FunctionData> StartBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	Unavailable("notebook_start");
}

unique_ptr<FunctionData> StopBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	Unavailable("notebook_stop");
}

void Never(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
}

} // namespace

void RegisterNotebookServer(ExtensionLoader &loader) {
	TableFunctionSet starts("notebook_start");
	for (auto arguments : {vector<LogicalType> {}, vector<LogicalType> {LogicalType::VARCHAR}}) {
		TableFunction start("notebook_start", arguments, Never, StartBind);
		start.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
			options.Add("port", LogicalType::INTEGER).Add("open", LogicalType::BOOLEAN);
		});
		starts.AddFunction(start);
	}
	loader.RegisterFunction(starts);
	loader.RegisterFunction(TableFunction("notebook_stop", {}, Never, StopBind));
}

} // namespace duckdb
