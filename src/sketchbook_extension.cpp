#define DUCKDB_EXTENSION_MAIN

#include "sketchbook_extension.hpp"

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

void SketchbookExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string SketchbookExtension::Name() {
	return "sketchbook";
}

std::string SketchbookExtension::Version() const {
#ifdef EXT_VERSION_SKETCHBOOK
	return EXT_VERSION_SKETCHBOOK;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(sketchbook, loader) {
	duckdb::LoadInternal(loader);
}
}
