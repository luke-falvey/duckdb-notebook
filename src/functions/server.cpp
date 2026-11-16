#include "httplib.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "notebook/chart.hpp"
#include "notebook/functions.hpp"
#include "notebook/graph.hpp"
#include "notebook/script.hpp"
#include "notebook/session.hpp"

#include <atomic>
#include <random>
#include <thread>

// The notebook server (docs/DESIGN.md section 4): an HTTP server on 127.0.0.1 inside the user's
// DuckDB process, serving the embedded frontend and a JSON API.
//
// Lifetime: the server never owns the database. It holds a weak_ptr to the DatabaseInstance and
// opens a connection per request (a Runner replays the notebook session's variables and views),
// so closing the database is never blocked by the server. The server object lives in the
// database's object cache, which is destroyed with the database, and stops the server then.
//
// Security: requests must come to 127.0.0.1 or localhost on the server's port (Host header), from
// the server's own origin if an Origin header is sent, and carry the random token created at start
// (X-Notebook-Token header or token= parameter). This blocks other sites and DNS rebinding.
namespace duckdb {

using notebook::Json;
namespace http = duckdb_httplib;

namespace {

thread_local bool in_server_thread = false;

struct Session {
	string source;
	string text;
	SessionState state;
	//! Serialises work on the session: one request runs its cells at a time.
	std::mutex lock;
	std::mutex running_lock;
	shared_ptr<ClientContext> running;
};

struct ServerState {
	weak_ptr<DatabaseInstance> db;
	string token;
	string host = "127.0.0.1";
	int port = 0;
	http::Server http;
	std::atomic<bool> stopping {false};
	std::mutex sessions_lock;
	std::map<string, shared_ptr<Session>> sessions;
};

string RandomToken() {
	std::random_device random;
	string token;
	const char *digits = "0123456789abcdef";
	for (int i = 0; i < 32; i++) {
		token += digits[random() % 16];
	}
	return token;
}

shared_ptr<DatabaseInstance> Database(ServerState &state) {
	auto db = state.db.lock();
	if (!db) {
		throw InvalidInputException("The database this notebook server belonged to has been closed");
	}
	return db;
}

void SendJson(http::Response &res, const Json &json, int status = 200) {
	res.status = status;
	res.set_header("Cache-Control", "no-store");
	res.set_content(json.Dump(), "application/json");
}

void SendError(http::Response &res, const string &message, int status = 400) {
	SendJson(res, Json::Object().Set("error", Json::String(message)), status);
}

Json Body(const http::Request &req) {
	string error;
	auto body = notebook::ParseJson(req.body.empty() ? "{}" : req.body, error);
	if (!error.empty()) {
		throw InvalidInputException("Request body is not JSON: %s", error);
	}
	return body;
}

string Param(const http::Request &req, const string &key) {
	return req.has_param(key) ? req.get_param_value(key) : string();
}

notebook::Notebook Analyze(DatabaseInstance &db, const string &text) {
	Connection connection(db);
	return AnalyzeText(*connection.context, text);
}

Json CellsJson(const notebook::Notebook &notebook) {
	auto cells = Json::Array();
	for (auto &cell : notebook.cells) {
		cells.Push(CellJson(cell));
	}
	return cells;
}

Json NotebookJson(DatabaseInstance &db, Session &session) {
	auto notebook = Analyze(db, session.text);
	return Json::Object()
	    .Set("source", Json::String(session.source))
	    .Set("text", Json::String(session.text))
	    .Set("preamble", Json::String(notebook.preamble))
	    .Set("title", Json::String(NotebookTitle(notebook, session.source)))
	    .Set("cells", CellsJson(notebook));
}

shared_ptr<Session> GetSession(ServerState &state, const string &source) {
	if (source.empty()) {
		throw InvalidInputException("No notebook given: add nb=<path> or nb=db:<name> to the URL");
	}
	std::lock_guard<std::mutex> guard(state.sessions_lock);
	auto found = state.sessions.find(source);
	if (found != state.sessions.end()) {
		return found->second;
	}
	auto db = Database(state);
	auto session = make_shared_ptr<Session>();
	session->source = source;
	bool exists = source.compare(0, 3, "db:") == 0 || FileSystem::GetFileSystem(*db).FileExists(source);
	if (exists) {
		session->text = ReadNotebookText(*db, source);
	} else {
		// A new notebook: it becomes a file when first saved.
		session->text = "-- %% [markdown]\n/*\n# Untitled\n*/\n\n-- %%\nSELECT 42 AS answer;\n";
	}
	state.sessions[source] = session;
	return session;
}

const notebook::Cell &CellAt(const notebook::Notebook &notebook, const string &index) {
	auto i = std::atoi(index.c_str());
	if (index.empty() || i < 0 || i >= static_cast<int>(notebook.cells.size())) {
		throw InvalidInputException("No cell %s", index);
	}
	return notebook.cells[NumericCast<idx_t>(i)];
}

//! Remembers which context is running so /api/interrupt can stop it.
struct RunningGuard {
	Session &session;
	RunningGuard(Session &session, Runner &runner) : session(session) {
		std::lock_guard<std::mutex> guard(session.running_lock);
		session.running = runner.Conn().context;
	}
	~RunningGuard() {
		std::lock_guard<std::mutex> guard(session.running_lock);
		session.running.reset();
	}
};

string ContentType(const string &name) {
	if (StringUtil::EndsWith(name, ".js")) {
		return "text/javascript; charset=utf-8";
	}
	if (StringUtil::EndsWith(name, ".css")) {
		return "text/css; charset=utf-8";
	}
	if (StringUtil::EndsWith(name, ".html")) {
		return "text/html; charset=utf-8";
	}
	if (StringUtil::EndsWith(name, ".woff2")) {
		return "font/woff2";
	}
	return "application/octet-stream";
}

//! Wraps a handler: resolves the server state, turns exceptions into JSON errors.
template <class F>
http::Server::Handler Handler(weak_ptr<ServerState> weak, F f) {
	return [weak, f](const http::Request &req, http::Response &res) {
		auto state = weak.lock();
		if (!state) {
			SendError(res, "server stopped", 503);
			return;
		}
		try {
			f(*state, req, res);
		} catch (std::exception &ex) {
			ErrorData error(ex);
			SendError(res, error.RawMessage());
		}
	};
}

void Routes(shared_ptr<ServerState> &shared) {
	weak_ptr<ServerState> weak = shared;
	auto &server = shared->http;

	server.set_pre_routing_handler([weak](const http::Request &req, http::Response &res) {
		in_server_thread = true;
		auto state = weak.lock();
		if (!state || state->stopping) {
			res.status = 503;
			return http::Server::HandlerResponse::Handled;
		}
		auto port = std::to_string(state->port);
		auto host = req.get_header_value("Host");
		if (host != "127.0.0.1:" + port && host != "localhost:" + port) {
			res.status = 403;
			res.set_content("Forbidden host", "text/plain");
			return http::Server::HandlerResponse::Handled;
		}
		auto origin = req.get_header_value("Origin");
		if (!origin.empty() && origin != "http://127.0.0.1:" + port && origin != "http://localhost:" + port) {
			res.status = 403;
			res.set_content("Forbidden origin", "text/plain");
			return http::Server::HandlerResponse::Handled;
		}
		if (StringUtil::StartsWith(req.path, "/assets/")) {
			return http::Server::HandlerResponse::Unhandled;
		}
		auto token = req.get_header_value("X-Notebook-Token");
		if (token.empty()) {
			token = Param(req, "token");
		}
		// Compare in constant time.
		unsigned diff = token.size() != state->token.size();
		for (idx_t i = 0; i < token.size() && i < state->token.size(); i++) {
			diff |= static_cast<unsigned>(token[i] ^ state->token[i]);
		}
		if (diff) {
			res.status = 403;
			res.set_content("Missing or wrong token. Open the URL that notebook_start() returned.", "text/plain");
			return http::Server::HandlerResponse::Handled;
		}
		return http::Server::HandlerResponse::Unhandled;
	});

	server.Get("/", [](const http::Request &, http::Response &res) {
		res.set_header("Cache-Control", "no-store");
		res.set_content(GetAsset("index.html"), "text/html; charset=utf-8");
	});

	server.Get(R"(/assets/([\w.-]+))", [](const http::Request &req, http::Response &res) {
		auto name = req.path.substr(8);
		auto &asset = GetAsset(name);
		if (asset.empty() || name == "viewer.html") {
			res.status = 404;
			return;
		}
		res.set_content(asset, ContentType(name));
	});

	server.Get("/api/notebook", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		           auto session = GetSession(state, Param(req, "nb"));
		           std::lock_guard<std::mutex> guard(session->lock);
		           auto db = Database(state);
		           SendJson(res, NotebookJson(*db, *session));
	           }));

	// Update the session's text (and optionally save it). Returns the analysis of the new text.
	server.Post("/api/notebook", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		            auto body = Body(req);
		            auto session = GetSession(state, body.Get("nb") ? body.Get("nb")->Str() : Param(req, "nb"));
		            std::lock_guard<std::mutex> guard(session->lock);
		            auto db = Database(state);
		            if (auto text = body.Get("text")) {
			            session->text = text->Str();
		            }
		            string note;
		            if (body.Get("save") && body.Get("save")->Truthy()) {
			            if (session->source.compare(0, 3, "db:") == 0) {
				            Connection connection(*db);
				            auto name = session->source.substr(3);
				            name = name.substr(0, name.rfind('@'));
				            bool saved;
				            auto version =
				                SaveNotebookVersion(connection, string(), name, session->text,
				                                    "saved from the notebook UI", true, saved, note, session->source);
				            note = saved ? "Saved as version " + std::to_string(version) : "No changes to save";
			            } else {
				            WriteTextFile(*db, session->source, session->text);
				            note = "Saved " + session->source;
			            }
		            }
		            auto json = NotebookJson(*db, *session);
		            json.Set("note", Json::String(note));
		            SendJson(res, json);
	            }));

	server.Post(
	    "/api/run", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		    auto body = Body(req);
		    auto session = GetSession(state, body.Get("nb") ? body.Get("nb")->Str() : Param(req, "nb"));
		    std::lock_guard<std::mutex> guard(session->lock);
		    if (auto text = body.Get("text")) {
			    if (text->GetType() == Json::Type::STRING) {
				    session->text = text->Str();
			    }
		    }
		    auto db = Database(state);
		    Runner runner(*db, session->state);
		    RunningGuard running(*session, runner);
		    auto notebook = AnalyzeText(*runner.Conn().context, session->text);
		    runner.ApplyMetadata(notebook, session->source);
		    runner.InitializeVariables(notebook, {});
		    RunnerOptions options;
		    options.charts = true;
		    options.mark_values = true;
		    options.page_rows = body.Get("page_rows") ? static_cast<idx_t>(body.Get("page_rows")->Num()) : 100;
		    std::set<int> wanted;
		    auto cells = body.Get("cells");
		    bool all = !cells || cells->IsNull();
		    if (!all) {
			    for (auto &item : cells->Items()) {
				    wanted.insert(static_cast<int>(item.Num()));
			    }
		    }
		    auto results = Json::Array();
		    for (auto index : notebook::RunOrder(notebook)) {
			    if (!all && !wanted.count(index)) {
				    continue;
			    }
			    auto &cell = notebook.cells[NumericCast<idx_t>(index)];
			    auto result = Json::Object().Set("idx", Json::Number(index));
			    if (!cell.errors.empty()) {
				    result.Set("status", Json::String("error")).Set("error", Json::String(cell.errors[0]));
				    results.Push(result);
				    continue;
			    }
			    Json data;
			    auto outcome = runner.Run(notebook, cell, options, &data);
			    result.Set("status", Json::String(outcome.status));
			    result.Set("error", outcome.status == "error" ? Json::String(outcome.error) : Json());
			    result.Set("row_count",
			               outcome.row_count >= 0 ? Json::Number(static_cast<double>(outcome.row_count)) : Json());
			    result.Set("elapsed_ms", outcome.elapsed_ms >= 0 ? Json::Number(outcome.elapsed_ms) : Json());
			    if (outcome.has_output) {
				    result.Set("output", Json::String(outcome.output));
			    }
			    if (!data.IsNull()) {
				    result.Set("data", data);
			    }
			    if (cell.kind == notebook::CellKind::INPUT) {
				    auto value = runner.Conn().Query("SELECT getvariable(" + notebook::QuoteString(cell.name) + ")");
				    if (!value->HasError()) {
					    if (auto chunk = value->Fetch()) {
						    result.Set("value", ValueToJson(chunk->GetValue(0, 0)));
					    }
				    }
			    }
			    if (cell.kind == notebook::CellKind::INPUT &&
			        (cell.widget == "select" || cell.widget == "multiselect")) {
				    auto choices = Json::Array();
				    for (auto &choice : InputChoices(runner.Conn(), cell, 1000)) {
					    choices.Push(ValueToJson(choice));
				    }
				    result.Set("choices", choices);
			    }
			    results.Push(result);
		    }
		    SendJson(res, Json::Object().Set("results", results).Set("cells", CellsJson(notebook)));
	    }));

	server.Post("/api/variable", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		            auto body = Body(req);
		            auto session = GetSession(state, body.Get("nb") ? body.Get("nb")->Str() : Param(req, "nb"));
		            std::lock_guard<std::mutex> guard(session->lock);
		            auto name = body.Get("name") ? body.Get("name")->Str() : string();
		            if (name.empty()) {
			            throw InvalidInputException("variable needs a name");
		            }
		            auto db = Database(state);
		            Runner runner(*db, session->state);
		            auto notebook = AnalyzeText(*runner.Conn().context, session->text);
		            string type;
		            if (auto cell = body.Get("cell")) {
			            if (!cell->IsNull()) {
				            type =
				                VariableSqlType(runner.Conn(), notebook,
				                                CellAt(notebook, std::to_string(static_cast<int>(cell->Num()))), name);
			            }
		            }
		            auto value = body.Get("value") ? *body.Get("value") : Json();
		            auto sql = "SET VARIABLE " + notebook::QuoteIdentifier(name) + " = " +
		                       notebook::JsonToSqlLiteral(value, type);
		            auto result = runner.Conn().Query(sql);
		            if (result->HasError()) {
			            throw InvalidInputException("%s", result->GetError());
		            }
		            SendJson(res, Json::Object().Set("ok", Json::Bool(true)).Set("sql", Json::String(sql)));
	            }));

	server.Get("/api/page", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		           auto session = GetSession(state, Param(req, "nb"));
		           std::lock_guard<std::mutex> guard(session->lock);
		           auto db = Database(state);
		           Runner runner(*db, session->state);
		           auto notebook = AnalyzeText(*runner.Conn().context, session->text);
		           auto sql = Runner::DisplaySql(CellAt(notebook, Param(req, "cell")));
		           if (sql.empty()) {
			           throw InvalidInputException("That cell has no result to page through");
		           }
		           auto limit = std::max<idx_t>(1, std::min<idx_t>(10000, std::atoll(Param(req, "limit").c_str())));
		           auto offset = std::atoll(Param(req, "offset").c_str());
		           string error;
		           auto page = runner.Page("SELECT * FROM (" + sql + ") LIMIT " + std::to_string(limit) + " OFFSET " +
		                                       std::to_string(offset),
		                                   limit, error);
		           if (!error.empty()) {
			           throw InvalidInputException("%s", error);
		           }
		           SendJson(res, page);
	           }));

	server.Get("/api/options", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		           auto session = GetSession(state, Param(req, "nb"));
		           std::lock_guard<std::mutex> guard(session->lock);
		           auto db = Database(state);
		           Runner runner(*db, session->state);
		           auto notebook = AnalyzeText(*runner.Conn().context, session->text);
		           auto values = Json::Array();
		           for (auto &value : InputChoices(runner.Conn(), CellAt(notebook, Param(req, "cell")), 1000)) {
			           values.Push(ValueToJson(value));
		           }
		           SendJson(res, Json::Object().Set("values", values));
	           }));

	server.Get(
	    "/api/catalog", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		    auto session = GetSession(state, Param(req, "nb"));
		    std::lock_guard<std::mutex> guard(session->lock);
		    auto db = Database(state);
		    Runner runner(*db, session->state);
		    auto tables = Json::Array();
		    auto result = runner.Conn().Query(
		        "SELECT t.database_name, t.schema_name, t.table_name, t.kind, list({'name': c.column_name, 'type': "
		        "c.data_type} ORDER BY c.column_index) FROM (SELECT database_name, schema_name, table_name, 'table' AS "
		        "kind "
		        "FROM duckdb_tables() WHERE NOT internal UNION ALL SELECT database_name, schema_name, view_name, "
		        "'view' FROM "
		        "duckdb_views() WHERE NOT internal) t LEFT JOIN duckdb_columns() c USING (database_name, schema_name, "
		        "table_name) WHERE t.schema_name <> '_notebook' GROUP BY ALL ORDER BY ALL");
		    if (result->HasError()) {
			    throw InvalidInputException("%s", result->GetError());
		    }
		    auto current = runner.Conn().Query("SELECT current_database()");
		    string current_db = current->HasError() ? "" : current->Fetch()->GetValue(0, 0).ToString();
		    while (auto chunk = result->Fetch()) {
			    for (idx_t r = 0; r < chunk->size(); r++) {
				    auto database = chunk->GetValue(0, r).ToString(), schema = chunk->GetValue(1, r).ToString();
				    auto name = chunk->GetValue(2, r).ToString();
				    auto qualified = notebook::QuoteIdentifier(name);
				    if (!(database == current_db || database == "temp") || schema != "main") {
					    qualified = notebook::QuoteIdentifier(database) + "." + notebook::QuoteIdentifier(schema) +
					                "." + qualified;
				    }
				    tables.Push(Json::Object()
				                    .Set("database", Json::String(database))
				                    .Set("schema", Json::String(schema))
				                    .Set("name", Json::String(name))
				                    .Set("kind", Json::String(chunk->GetValue(3, r).ToString()))
				                    .Set("qualified", Json::String(qualified))
				                    .Set("columns", ValueToJson(chunk->GetValue(4, r))));
			    }
		    }
		    SendJson(res, Json::Object().Set("tables", tables));
	    }));

	server.Get("/api/notebooks", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		           auto db = Database(state);
		           auto files = Json::Array(), stored = Json::Array(), open = Json::Array();
		           for (auto &file : FileSystem::GetFileSystem(*db).Glob("*.sql")) {
			           files.Push(Json::String(file.path));
		           }
		           Connection connection(*db);
		           auto result = connection.Query("SELECT DISTINCT name FROM _notebook.versions ORDER BY name");
		           if (!result->HasError()) {
			           while (auto chunk = result->Fetch()) {
				           for (idx_t r = 0; r < chunk->size(); r++) {
					           stored.Push(Json::String(chunk->GetValue(0, r).ToString()));
				           }
			           }
		           }
		           {
			           std::lock_guard<std::mutex> guard(state.sessions_lock);
			           for (auto &session : state.sessions) {
				           open.Push(Json::String(session.first));
			           }
		           }
		           SendJson(res, Json::Object().Set("files", files).Set("stored", stored).Set("open", open));
	           }));

	server.Post("/api/interrupt", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		            auto body = Body(req);
		            auto session = GetSession(state, body.Get("nb") ? body.Get("nb")->Str() : Param(req, "nb"));
		            std::lock_guard<std::mutex> guard(session->running_lock);
		            if (session->running) {
			            session->running->Interrupt();
		            }
		            SendJson(res, Json::Object().Set("ok", Json::Bool(true)));
	            }));

	server.Get(
	    "/api/export", Handler(weak, [](ServerState &state, const http::Request &req, http::Response &res) {
		    auto session = GetSession(state, Param(req, "nb"));
		    string text;
		    {
			    std::lock_guard<std::mutex> guard(session->lock);
			    text = session->text;
		    }
		    auto db = Database(state);
		    ExportOptions options;
		    options.format = Param(req, "format").empty() ? "html" : Param(req, "format");
		    options.interactive = Param(req, "interactive");
		    options.hide = Param(req, "hide") == "none" ? string() : Param(req, "hide");
		    string note;
		    auto document = ExportDocument(*db, text, session->source, options, note);
		    auto stem = session->source.substr(session->source.find_last_of("/:") + 1);
		    stem = stem.substr(0, stem.rfind('.'));
		    auto extension = options.format == "md" ? ".md" : options.format == "sql" ? ".expanded.sql" : ".html";
		    auto type = options.format == "md"    ? "text/markdown; charset=utf-8"
		                : options.format == "sql" ? "application/sql"
		                                          : "text/html; charset=utf-8";
		    res.set_header("Content-Disposition", "inline; filename=\"" + stem + extension + "\"");
		    res.set_content(document, type);
	    }));

	// Server-sent events: reports edits to the file made by another program. Each stream lasts
	// about 30 seconds; EventSource reconnects by itself.
	server.Get("/api/events", Handler(weak, [weak](ServerState &state, const http::Request &req, http::Response &res) {
		           auto session = GetSession(state, Param(req, "nb"));
		           auto source = session->source;
		           res.set_header("Cache-Control", "no-store");
		           auto started = std::chrono::steady_clock::now();
		           auto last = std::make_shared<string>();
		           res.set_chunked_content_provider("text/event-stream", [weak, session, source, started,
		                                                                  last](size_t offset, http::DataSink &sink) {
			           auto state = weak.lock();
			           if (!state || state->stopping ||
			               std::chrono::steady_clock::now() - started > std::chrono::seconds(30)) {
				           sink.done();
				           return true;
			           }
			           if (offset == 0) {
				           sink.write("retry: 1000\n\n", 13);
			           }
			           std::this_thread::sleep_for(std::chrono::milliseconds(500));
			           if (source.compare(0, 3, "db:") == 0) {
				           sink.write(": ping\n\n", 8);
				           return true;
			           }
			           auto db = state->db.lock();
			           if (!db) {
				           sink.done();
				           return true;
			           }
			           string text;
			           try {
				           if (!FileSystem::GetFileSystem(*db).FileExists(source)) {
					           sink.write(": ping\n\n", 8);
					           return true;
				           }
				           text = ReadNotebookText(*db, source);
			           } catch (...) {
				           sink.write(": ping\n\n", 8);
				           return true;
			           }
			           if (last->empty()) {
				           *last = text;
			           }
			           if (text != *last) {
				           *last = text;
				           auto event =
				               "event: changed\ndata: " + Json::Object().Set("text", Json::String(text)).Dump() +
				               "\n\n";
				           sink.write(event.data(), event.size());
			           } else {
				           sink.write(": ping\n\n", 8);
			           }
			           return true;
		           });
	           }));
}

class NotebookServer : public ObjectCacheEntry {
public:
	explicit NotebookServer(shared_ptr<DatabaseInstance> db) : db(db) {
	}
	~NotebookServer() override {
		Stop();
	}

	static string ObjectType() {
		return "notebook_server";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		// Not evictable: the server lives until it is stopped or the database closes.
		return optional_idx();
	}

	bool Running() const {
		return state && !state->stopping;
	}

	void Start(int port) {
		std::lock_guard<std::mutex> guard(lock);
		if (Running()) {
			return;
		}
		auto fresh = make_shared_ptr<ServerState>();
		fresh->db = db;
		fresh->token = RandomToken();
		fresh->http.set_keep_alive_max_count(100);
		Routes(fresh);
		if (port > 0) {
			if (!fresh->http.bind_to_port(fresh->host, port)) {
				throw IOException("notebook_start: cannot listen on 127.0.0.1:%d (in use?)", port);
			}
			fresh->port = port;
		} else {
			fresh->port = fresh->http.bind_to_any_port(fresh->host);
			if (fresh->port <= 0) {
				throw IOException("notebook_start: cannot listen on 127.0.0.1");
			}
		}
		state = fresh;
		thread = std::thread([fresh]() { fresh->http.listen_after_bind(); });
		state->http.wait_until_ready();
	}

	void Stop() {
		std::lock_guard<std::mutex> guard(lock);
		if (!state) {
			return;
		}
		state->stopping = true;
		state->http.stop();
		if (thread.joinable()) {
			// Stopped from one of the server's own threads (the database closed while a request
			// held the last reference to it): joining would wait on ourselves.
			if (in_server_thread) {
				thread.detach();
			} else {
				thread.join();
			}
		}
		{
			std::lock_guard<std::mutex> sessions(state->sessions_lock);
			state->sessions.clear();
		}
		state.reset();
	}

	string Url(const string &source) {
		string url = "http://127.0.0.1:" + std::to_string(state->port) + "/?token=" + state->token;
		if (!source.empty()) {
			url += "&nb=" + StringUtil::URLEncode(source);
		}
		return url;
	}

	int Port() const {
		return state ? state->port : 0;
	}
	string Token() const {
		return state ? state->token : string();
	}

private:
	weak_ptr<DatabaseInstance> db;
	shared_ptr<ServerState> state;
	std::thread thread;
	std::mutex lock;
};

const char *const CACHE_KEY = "duckdb_notebook_server";

struct StartBindData : public TableFunctionData {
	string source;
	int port = 0;
	bool open = true;
};

struct OnceState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<FunctionData> StartBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<StartBindData>();
	if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
		result->source = input.inputs[0].GetValue<string>();
	}
	for (auto &entry : input.named_parameters) {
		auto key = entry.first.GetIdentifierName();
		if (entry.second.IsNull()) {
			continue;
		}
		if (key == "port") {
			result->port = entry.second.GetValue<int32_t>();
		} else if (key == "open") {
			result->open = entry.second.GetValue<bool>();
		}
	}
	names.emplace_back("url");
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("port");
	return_types.push_back(LogicalType::INTEGER);
	names.emplace_back("token");
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> OnceInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<OnceState>();
}

void OpenBrowser(const string &url) {
#if defined(__APPLE__)
	auto command = "open '" + url + "' >/dev/null 2>&1 &";
#elif defined(_WIN32)
	auto command = "start \"\" \"" + url + "\"";
#else
	if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
		return;
	}
	auto command = "xdg-open '" + url + "' >/dev/null 2>&1 &";
#endif
	auto status = std::system(command.c_str());
	(void)status;
}

void StartScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<OnceState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &bind = data.bind_data->Cast<StartBindData>();
	auto server = ObjectCache::GetObjectCache(context).GetOrCreate<NotebookServer>(CACHE_KEY, context.db);
	server->Start(bind.port);
	auto url = server->Url(bind.source);
	if (bind.open) {
		OpenBrowser(url);
	}
	output.data[0].Append(Value(url));
	output.data[1].Append(Value::INTEGER(server->Port()));
	output.data[2].Append(Value(server->Token()));
}

unique_ptr<FunctionData> StopBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("stopped");
	return_types.push_back(LogicalType::BOOLEAN);
	return make_uniq<TableFunctionData>();
}

void StopScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<OnceState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto server = ObjectCache::GetObjectCache(context).Get<NotebookServer>(CACHE_KEY);
	bool running = server && server->Running();
	if (server) {
		server->Stop();
	}
	output.data[0].Append(Value::BOOLEAN(running));
}

} // namespace

void RegisterNotebookServer(ExtensionLoader &loader) {
	TableFunctionSet starts("notebook_start");
	for (auto arguments : {vector<LogicalType> {}, vector<LogicalType> {LogicalType::VARCHAR}}) {
		TableFunction start("notebook_start", arguments, StartScan, StartBind, OnceInit);
		start.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
			options.Add("port", LogicalType::INTEGER).Add("open", LogicalType::BOOLEAN);
		});
		starts.AddFunction(start);
	}
	loader.RegisterFunction(starts);
	loader.RegisterFunction(TableFunction("notebook_stop", {}, StopScan, StopBind, OnceInit));
}

} // namespace duckdb
