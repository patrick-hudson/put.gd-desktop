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
    Stdio(qint64 lineLimit, QString prefix, QString journal)
        : maximumLine(lineLimit), prefix_(std::move(prefix)), journal_(std::move(journal)) {}
    std::atomic_bool cancelled{false};
    void emitLine(const QJsonObject& value);
    Error checkpoint(const QJsonObject& journal);
    void progress(const QString& phase, qint64 sent, qint64 total);
    using Run = std::function<QJsonObject(QJsonObject&, Error&, bool&)>;
    int run(int argc, char** argv, const char* name, const char* manifest, bool (*initialize)(), const Run& run);
private:
    QByteArray readLine(bool& eof);
    void readControl();
    qint64 maximumLine;
    QString prefix_, journal_;
    std::mutex controlMutex, outputMutex;
    std::condition_variable controlChanged;
    std::deque<QJsonObject> acks;
    bool parentGone = false;
    QElapsedTimer last_;
};
}
