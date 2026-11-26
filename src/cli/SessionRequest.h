#pragma once
#include <QJsonObject>
#include <QString>
namespace putgd::cli {
// Prints one JSON line on standard output.
void output(const QJsonObject&);
// An error response for this request ID, printed; returns 1.
int fail(const QString& requestId,const QString& code,const QString& message,const QJsonObject& details={});
// Sends one session request to the resident at `socket` (empty: the default socket),
// prints the resident's response and returns the exit code: 0 when ok.
int sendSessionRequest(const QString& socket,const QString& requestId,const QString& method,const QJsonObject& params);
}
