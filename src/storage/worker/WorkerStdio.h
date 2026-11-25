#pragma once
#include "core/StorageProvider.h"
#include <QElapsedTimer>
#include <condition_variable>
#include <deque>
#include <mutex>
namespace putgd::worker {
// One request per process; the control thread lives until run() exits the process.
class Stdio {
public:
    Stdio(qint64 lineLimit, QString prefix) : maximumLine(lineLimit), prefix_(std::move(prefix)) {}
    std::atomic_bool cancelled{false};
    // Extra parallel requests the resident lends to this upload right now (a {"lanes":n}
    // control line, #1792); zero until it sends one. Values outside [0, maximumLanes) are ignored.
    std::atomic_int lanes{0};
    int maximumLanes = 1;
    void emitLine(const QJsonObject& value);
    Error checkpoint(const QJsonObject& journal);
    void progress(const QString& phase, qint64 sent, qint64 total);
    using Run = std::function<QJsonObject(QJsonObject&, Error&, bool&)>;
    int run(int argc, char** argv, const char* name, const char* manifest, bool (*initialize)(), const Run& run);
private:
    QByteArray readLine(bool& eof);
    void readControl();
    qint64 maximumLine;
    QString prefix_;
    std::mutex controlMutex, outputMutex;
    std::condition_variable controlChanged;
    std::deque<QJsonObject> acks;
    bool parentGone = false;
    QElapsedTimer last_;
};
}
