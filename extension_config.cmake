# This file is included by DuckDB's build system. It specifies which extension to load

duckdb_extension_load(notebook
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# Link it into the build's own `duckdb` shell too, so ./build/release/duckdb has the
# notebook functions without a LOAD. The loadable .duckdb_extension is built either way.
duckdb_extension_statically_link(notebook)
