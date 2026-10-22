#include "common/Sqlite.h"
#include "common/Paths.h"
#include <mutex>
#include <windows.h>

namespace putgd::sqlite {
namespace {
constexpr int SQLITE_OK = 0, SQLITE_ROW = 100, SQLITE_DONE = 101, SQLITE_OPEN_READWRITE = 2, SQLITE_OPEN_CREATE = 4, SQLITE_OPEN_NOMUTEX = 0x8000;
constexpr void* SQLITE_TRANSIENT = reinterpret_cast<void*>(-1);
using open_v2_t = int (*)(const char*, sqlite3**, int, const char*);
using close_t = int (*)(sqlite3*);
using exec_t = int (*)(sqlite3*, const char*, void*, void*, char**);
using prepare_t = int (*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
using finalize_t = int (*)(sqlite3_stmt*);
using step_t = int (*)(sqlite3_stmt*);
using reset_t = int (*)(sqlite3_stmt*);
using bind_text_t = int (*)(sqlite3_stmt*, int, const char*, int, void*);
using bind_blob_t = int (*)(sqlite3_stmt*, int, const void*, int, void*);
using bind_int64_t = int (*)(sqlite3_stmt*, int, long long);
using bind_null_t = int (*)(sqlite3_stmt*, int);
using column_text_t = const unsigned char* (*)(sqlite3_stmt*, int);
using column_bytes_t = int (*)(sqlite3_stmt*, int);
using column_blob_t = const void* (*)(sqlite3_stmt*, int);
using column_int64_t = long long (*)(sqlite3_stmt*, int);
using column_type_t = int (*)(sqlite3_stmt*, int);
using errmsg_t = const char* (*)(sqlite3*);
using changes_t = int (*)(sqlite3*);
using libversion_t = const char* (*)();
using free_t = void (*)(void*);
using busy_timeout_t = int (*)(sqlite3*, int);
struct Api {
    HMODULE module = nullptr;
    open_v2_t open_v2; close_t close; exec_t exec; prepare_t prepare; finalize_t finalize; step_t step; reset_t reset;
    bind_text_t bind_text; bind_blob_t bind_blob; bind_int64_t bind_int64; bind_null_t bind_null;
    column_text_t column_text; column_bytes_t column_bytes; column_blob_t column_blob; column_int64_t column_int64; column_type_t column_type;
    errmsg_t errmsg; changes_t changes; libversion_t libversion; free_t free; busy_timeout_t busy_timeout;
    bool ok = false;
};
Api& api() {
    static Api a;
    static std::once_flag once;
    std::call_once(once, [] {
        wchar_t system[MAX_PATH];
        GetSystemDirectoryW(system, MAX_PATH);
        a.module = LoadLibraryExW((std::wstring(system) + L"\\winsqlite3.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!a.module) return;
        auto get = [&](const char* name) { return GetProcAddress(a.module, name); };
#define PUTGD_SQLITE_BIND(field, symbol) a.field = reinterpret_cast<decltype(a.field)>(get(symbol)); if (!a.field) return;
        PUTGD_SQLITE_BIND(open_v2, "sqlite3_open_v2") PUTGD_SQLITE_BIND(close, "sqlite3_close_v2") PUTGD_SQLITE_BIND(exec, "sqlite3_exec")
        PUTGD_SQLITE_BIND(prepare, "sqlite3_prepare_v2") PUTGD_SQLITE_BIND(finalize, "sqlite3_finalize") PUTGD_SQLITE_BIND(step, "sqlite3_step")
        PUTGD_SQLITE_BIND(reset, "sqlite3_reset") PUTGD_SQLITE_BIND(bind_text, "sqlite3_bind_text") PUTGD_SQLITE_BIND(bind_blob, "sqlite3_bind_blob")
        PUTGD_SQLITE_BIND(bind_int64, "sqlite3_bind_int64") PUTGD_SQLITE_BIND(bind_null, "sqlite3_bind_null") PUTGD_SQLITE_BIND(column_text, "sqlite3_column_text")
        PUTGD_SQLITE_BIND(column_bytes, "sqlite3_column_bytes") PUTGD_SQLITE_BIND(column_blob, "sqlite3_column_blob") PUTGD_SQLITE_BIND(column_int64, "sqlite3_column_int64")
        PUTGD_SQLITE_BIND(column_type, "sqlite3_column_type") PUTGD_SQLITE_BIND(errmsg, "sqlite3_errmsg") PUTGD_SQLITE_BIND(changes, "sqlite3_changes")
        PUTGD_SQLITE_BIND(libversion, "sqlite3_libversion") PUTGD_SQLITE_BIND(free, "sqlite3_free") PUTGD_SQLITE_BIND(busy_timeout, "sqlite3_busy_timeout")
#undef PUTGD_SQLITE_BIND
        a.ok = true;
    });
    return a;
}
}
bool available() { return api().ok; }
std::string libraryVersion() { return api().ok ? api().libversion() : std::string(); }
Database::~Database() { close(); }
bool Database::open(const std::string& path) {
    if (!api().ok) return false;
    close();
    if (api().open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) { close(); return false; }
    api().busy_timeout(db_, 5000);
    return exec("PRAGMA journal_mode=WAL") && exec("PRAGMA synchronous=FULL") && exec("PRAGMA foreign_keys=ON");
}
void Database::close() { if (db_) { api().close(db_); db_ = nullptr; } }
bool Database::exec(const std::string& sql) {
    if (!db_) return false;
    char* message = nullptr;
    const int rc = api().exec(db_, sql.c_str(), nullptr, nullptr, &message);
    if (message) api().free(message);
    return rc == SQLITE_OK;
}
long long Database::changes() const { return db_ ? api().changes(db_) : 0; }
std::string Database::lastError() const { return db_ ? api().errmsg(db_) : "database not open"; }
Statement::Statement(Database& db, const std::string& sql) {
    if (!db.handle() || api().prepare(db.handle(), sql.c_str(), int(sql.size()), &stmt_, nullptr) != SQLITE_OK) { stmt_ = nullptr; failed_ = true; }
}
Statement::~Statement() { if (stmt_) api().finalize(stmt_); }
Statement& Statement::bind(int i, const std::string& t) { if (stmt_) api().bind_text(stmt_, i, t.data(), int(t.size()), SQLITE_TRANSIENT); return *this; }
Statement& Statement::bind(int i, long long v) { if (stmt_) api().bind_int64(stmt_, i, v); return *this; }
Statement& Statement::bindBlob(int i, const std::string& b) { if (stmt_) api().bind_blob(stmt_, i, b.data(), int(b.size()), SQLITE_TRANSIENT); return *this; }
Statement& Statement::bindNull(int i) { if (stmt_) api().bind_null(stmt_, i); return *this; }
bool Statement::step() {
    if (!stmt_) { failed_ = true; return false; }
    const int rc = api().step(stmt_);
    if (rc == SQLITE_ROW) return true;
    done_ = rc == SQLITE_DONE;
    failed_ = !done_;
    return false;
}
std::string Statement::text(int c) const { if (!stmt_) return {}; const auto* t = api().column_text(stmt_, c); return t ? std::string(reinterpret_cast<const char*>(t), size_t(api().column_bytes(stmt_, c))) : std::string(); }
long long Statement::integer(int c) const { return stmt_ ? api().column_int64(stmt_, c) : 0; }
std::string Statement::blob(int c) const { if (!stmt_) return {}; const auto* b = api().column_blob(stmt_, c); return b ? std::string(static_cast<const char*>(b), size_t(api().column_bytes(stmt_, c))) : std::string(); }
bool Statement::isNull(int c) const { return !stmt_ || api().column_type(stmt_, c) == 5; }
void Statement::reset() { if (stmt_) api().reset(stmt_); done_ = failed_ = false; }
}
