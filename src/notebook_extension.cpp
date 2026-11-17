#define DUCKDB_EXTENSION_MAIN

#include "notebook_extension.hpp"

#include "notebook/functions.hpp"
#include "notebook/result_cache.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	RegisterNotebookCells(loader);
	RegisterNotebookSql(loader);
	RegisterNotebookRun(loader);
	RegisterNotebookChartQuery(loader);
	RegisterNotebookStorage(loader);
	RegisterNotebookExport(loader);
	RegisterNotebookServer(loader);
	RegisterNotebookPeek(loader);
	RegisterNotebookCache(loader);
}

void NotebookExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string NotebookExtension::Name() {
	return "notebook";
}

std::string NotebookExtension::Version() const {
#ifdef EXT_VERSION_NOTEBOOK
	return EXT_VERSION_NOTEBOOK;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(notebook, loader) {
	duckdb::LoadInternal(loader);
}
}
