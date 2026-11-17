#include "duckdb/function/table_function.hpp"
#include "notebook/functions.hpp"

// notebook_start and notebook_stop for builds without the HTTP server: DuckDB-WASM, which has no
// sockets to listen on and no threads to serve from. CMakeLists.txt compiles this file instead of
// server.cpp there (NOTEBOOK_SERVER=OFF). The functions keep their signatures, so a query that uses
// them binds and then says why it cannot run.
namespace duckdb {

namespace {

[[noreturn]] void Unavailable(const char *function) {
	throw NotImplementedException("%s: the notebook server is not part of this build of the notebook extension "
	                              "(DuckDB-WASM has no sockets to serve from). Use notebook_export to write html "
	                              "instead",
	                              function);
}

unique_ptr<FunctionData> StartBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	Unavailable("notebook_start");
}

unique_ptr<FunctionData> StopBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	Unavailable("notebook_stop");
}

void UnavailableScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
}

} // namespace

void RegisterNotebookServer(ExtensionLoader &loader) {
	TableFunctionSet starts("notebook_start");
	for (auto arguments : {vector<LogicalType> {}, vector<LogicalType> {LogicalType::VARCHAR}}) {
		TableFunction start("notebook_start", arguments, UnavailableScan, StartBind);
		start.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
			options.Add("port", LogicalType::INTEGER).Add("open", LogicalType::BOOLEAN);
		});
		starts.AddFunction(start);
	}
	loader.RegisterFunction(starts);
	loader.RegisterFunction(TableFunction("notebook_stop", {}, UnavailableScan, StopBind));
}

} // namespace duckdb
