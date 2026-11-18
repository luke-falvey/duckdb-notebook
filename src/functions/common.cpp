#include "notebook/functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

namespace {

string ReadFile(FileSystem &fs, const string &path) {
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
	return contents;
}

void WriteFile(FileSystem &fs, const string &path, const string &content) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
	handle->Write(const_cast<char *>(content.data()), content.size());
	handle->Sync();
}

} // namespace

string ReadNotebookText(DatabaseInstance &db, const string &source) {
	if (source.compare(0, 3, "db:") == 0) {
		auto name = source.substr(3);
		int32_t version = 0;
		auto at = name.rfind('@');
		if (at != string::npos) {
			version = std::atoi(name.substr(at + 1).c_str());
			name = name.substr(0, at);
		}
		Connection connection(db);
		int32_t found;
		return StoredNotebook(connection, string(), name, version, found);
	}
	return ReadFile(FileSystem::GetFileSystem(db), source);
}

string ReadNotebookText(ClientContext &context, const string &source) {
	if (source.compare(0, 3, "db:") == 0) {
		return ReadNotebookText(DatabaseInstance::GetDatabase(context), source);
	}
	return ReadFile(FileSystem::GetFileSystem(context), source);
}

void WriteTextFile(ClientContext &context, const string &path, const string &content) {
	WriteFile(FileSystem::GetFileSystem(context), path, content);
}

void WriteTextFile(DatabaseInstance &db, const string &path, const string &content) {
	WriteFile(FileSystem::GetFileSystem(db), path, content);
}

notebook::Notebook AnalyzeText(ClientContext &context, const string &text) {
	auto notebook = notebook::ParseNotebook(text);
	notebook::AnalyzeNotebook(notebook, [&](const std::string &sql) { return ExtractReferences(context, sql); });
	return notebook;
}

notebook::Notebook AnalyzeText(DatabaseInstance &db, const string &text) {
	Connection connection(db);
	return AnalyzeText(*connection.context, text);
}

notebook::Notebook ReadNotebook(ClientContext &context, const string &path) {
	return AnalyzeText(context, ReadNotebookText(context, path));
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

unique_ptr<GlobalTableFunctionState> OnceInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<OnceState>();
}

string NamedString(TableFunctionBindInput &input, const char *key) {
	auto found = input.named_parameters.find(key);
	return found == input.named_parameters.end() || found->second.IsNull() ? string()
	                                                                       : found->second.GetValue<string>();
}

bool NamedBool(TableFunctionBindInput &input, const char *key) {
	auto found = input.named_parameters.find(key);
	return found != input.named_parameters.end() && !found->second.IsNull() && found->second.GetValue<bool>();
}

} // namespace duckdb
