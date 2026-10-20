#pragma once
#include <QByteArray>
#include <QJsonObject>
namespace putgd::crash {
// Parses the "putgd-crash<TAB>..." lines CrashHandler writes (docs/diagnostics.md)
// into {process, pid, thread, version, signal, signalName, code, faultAddress,
// pc, time, frames[{index, address, module, offset}], modules[{index, loadBias,
// buildId, file}], complete}. Addresses are "0x..." strings; `offset` is the
// address in the module's own address space, what eu-addr2line takes. Other
// lines are ignored; text with no record gives an empty object.
QJsonObject parseRecord(const QByteArray& text);
// Whether a line belongs to a crash record.
bool isRecordLine(const QByteArray& line);
}
