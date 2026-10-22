#pragma once
// SQLite through the copy Windows ships (System32\winsqlite3.dll), loaded at
// runtime: no bundled database engine, nothing mapped until the journal opens.
// Only the handful of entry points the history store needs are declared.
#include <cstdint>
#include <string>
#include <vector>

namespace putgd::sqlite {
struct sqlite3; struct sqlite3_stmt;
bool available();           // the DLL loaded and exports resolved
std::string libraryVersion();
class Statement;
class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    bool open(const std::string& path);
    void close();
    bool exec(const std::string& sql);
    bool begin() { return exec("BEGIN IMMEDIATE"); }
    bool commit() { return exec("COMMIT"); }
    void rollback() { exec("ROLLBACK"); }
    long long changes() const;
    std::string lastError() const;
    sqlite3* handle() const { return db_; }
private:
    sqlite3* db_ = nullptr;
};
class Statement {
public:
    Statement(Database&, const std::string& sql);
    ~Statement();
    Statement(const Statement&) = delete;
    bool valid() const { return stmt_ != nullptr; }
    Statement& bind(int index, const std::string& text);
    Statement& bind(int index, long long value);
    Statement& bindBlob(int index, const std::string& bytes);
    Statement& bindNull(int index);
    // step(): true while a row is available; `done` becomes true at SQLITE_DONE.
    bool step();
    bool done() const { return done_; }
    bool failed() const { return failed_; }
    bool exec() { step(); return !failed_; } // for statements without rows
    std::string text(int column) const;
    long long integer(int column) const;
    std::string blob(int column) const;
    bool isNull(int column) const;
    void reset();
private:
    sqlite3_stmt* stmt_ = nullptr;
    bool done_ = false, failed_ = false;
};
}
