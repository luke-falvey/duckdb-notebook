#include "notebook/functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"

namespace duckdb {

notebook::Notebook ReadNotebook(ClientContext &context, const string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto size = handle->GetFileSize();
	string contents(size, '\0');
	idx_t total = 0;
	while (total < size) {
		auto n = handle->Read(&contents[total], size - total);
		if (n <= 0) {
			throw IOException("Could not read notebook \"%s\"", path);
		}
		total += NumericCast<idx_t>(n);
	}
	auto notebook = notebook::ParseNotebook(contents);
	notebook::AnalyzeNotebook(notebook, [&](const std::string &sql) { return ExtractReferences(context, sql); });
	return notebook;
}

void ThrowIfInvalid(const notebook::Notebook &notebook, const string &path) {
	for (auto &cell : notebook.cells) {
		if (!cell.errors.empty()) {
			// Report one error here; notebook_cells reports all of them as data.
			throw InvalidInputException("Notebook \"%s\" has errors. First one, at line %d: %s\n"
			                            "List them all with: SELECT start_line, errors FROM notebook_cells('%s') "
			                            "WHERE len(errors) > 0",
			                            path, cell.start_line, cell.errors[0], path);
		}
	}
}

} // namespace duckdb
