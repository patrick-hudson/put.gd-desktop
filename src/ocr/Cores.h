#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

// Which CPUs the recognition engines prefer on a machine of mixed cores (#1929).
namespace putgd::ocr {
// A sysfs CPU list ("0-3,8,10-11"), sorted, without repeats; empty when malformed.
QList<int> cpuList(const QByteArray& text);
// On a machine whose CPUs are of different kinds (Intel's performance and
// efficiency cores, AMD's Zen 4 or 5 beside Zen 4c or 5c, Arm's big and
// little), the logical CPUs of the faster kinds, from sysfs under `root`:
// - Intel: <root>/cpu_core/cpus, when <root>/cpu_atom/cpus lists CPUs too;
// - otherwise every <root>/system/cpu/cpuN's acpi_cppc/highest_perf, then its
//   cpufreq/cpuinfo_max_freq: the CPUs above the slowest group, where groups
//   are cut at a step of at least a sixth between neighbouring values. AMD's
//   ranking of alike cores (preferred cores) differs by smaller steps, and the
//   8500GE's Zen 4 and Zen 4c read 196 and 144 with the same maximum frequency.
// Empty on a uniform machine, or when sysfs cannot tell (missing, unreadable,
// a CPU without the value).
QList<int> performanceCpus(const QString& root=QStringLiteral("/sys/devices"));
// The CPUs a job's engines are kept to: the faster kinds among `allowed`, when
// they hold at least minimumPreferredCores physical cores (by topology's
// package and core id) and `allowed` holds slower ones too; empty otherwise,
// and when sysfs has no topology for them (every CPU, as on a uniform machine).
// Fewer cores lose more than the faster kind gains: on the 8500GE, whose two
// Zen 4 cores are four logical CPUs, a dense 4K screen read in 2.18 s on those
// four and in 1.94 to 2.05 s on any four CPUs.
inline constexpr int minimumPreferredCores=4;
QList<int> preferredCpus(const QList<int>& allowed,const QString& root=QStringLiteral("/sys/devices"));
// The same for the CPUs this process may run on (its affinity). Read before
// confinement: Landlock hides sysfs.
QList<int> preferredCpus();
// The job's memory budget and its ceiling with other languages' data (#1937).
// By default 320 and 384 MiB. In Unhinged mode the budget grows with the
// engines, 128 MiB and 64 MiB an engine, never under the default. The ceiling,
// 64 MiB above it, stays within half the memory available (MemAvailable) and
// its RLIMIT_DATA (one and a half times) within the inherited hard limit
// (dataLimit, -1 for none). Unknown available memory (-1) keeps the default.
struct MemoryBudgets { qint64 base=0,ceiling=0; };
MemoryBudgets memoryBudgets(bool unhinged,int threads,qint64 available,qint64 dataLimit=-1);
}
