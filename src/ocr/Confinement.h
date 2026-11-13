#pragma once
#include <QString>
#include <QStringList>

// putgd-ocr's confinement (#1428 decision 2), installed at the top of main
// before any input is read. System calls only: no libseccomp, no liblandlock.
namespace putgd::ocr {
struct Confinement {
 QStringList readable;          // directories and files the helper may read; missing ones are skipped
 qint64 dataBytes=0;            // RLIMIT_DATA: private writable memory, not the address space
 int cpuSeconds=0;              // RLIMIT_CPU
 qint64 fileBytes=0;            // RLIMIT_FSIZE: the output cap
 int descriptors=32;            // RLIMIT_NOFILE
};
// The Landlock ABI the kernel offers, 0 without Landlock.
int landlockAbi();
// Sanitizer builds skip the memory limit, the dumpable setting and the ptrace
// kill, which the sanitizers need; confinement tests run on unsanitized builds.
bool sanitizedBuild();
// The read-only set every job gets: system libraries, the lib and share/putgd
// directories under the helper's own installation prefix (the AppImage mount;
// never the prefix itself, nor one that is / or a home directory),
// /proc/self, the CPU entries OpenMP reads, plus the language data directories.
QStringList readableDirectories(const QStringList& languageDirectories);
// No new privileges, not dumpable, the resource limits, Landlock (ABI 3 or
// later; at ABI 4 TCP is denied, at ABI 6 signals to and abstract sockets of
// other processes are scoped out) and the seccomp deny-list on every thread.
// Any part that fails returns false with the part's name in failed; the job
// then fails with helper_sandbox_unavailable.
bool confine(const Confinement& confinement,QString& failed,int& abi);
// Lowers RLIMIT_DATA once the job's languages are known. Never raises it.
bool lowerDataLimit(qint64 bytes);
}
