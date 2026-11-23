#include "Cores.h"
#include <QDir>
#include <QFile>
#include <algorithm>
#include <map>
#include <sched.h>

namespace putgd::ocr {
namespace {
QByteArray readSmall(const QString& path){
 QFile file(path);
 if(!file.open(QIODevice::ReadOnly))return {};
 return file.read(4096).trimmed();
}
// The CPUs above the slowest group of `values` (CPU -> value), or none.
QList<int> aboveSlowest(const std::map<int,qint64>& values){
 QList<qint64> distinct;
 for(const auto& [cpu,value]:values)if(!distinct.contains(value))distinct.append(value);
 std::sort(distinct.begin(),distinct.end());
 for(int i=1;i<distinct.size();++i){
  // A step of a sixth or more ends the slowest group.
  if(distinct[i-1]>0&&distinct[i]*6>=distinct[i-1]*7){
   QList<int> out;
   for(const auto& [cpu,value]:values)if(value>=distinct[i])out.append(cpu);
   return out;
  }
 }
 return {};
}
}
QList<int> cpuList(const QByteArray& text){
 QList<int> out;
 if(text.trimmed().isEmpty())return out;
 for(const auto& part:text.trimmed().split(',')){
  const auto range=part.trimmed().split('-');
  bool ok1=false,ok2=true;
  const int from=range.value(0).toInt(&ok1);
  const int to=range.size()==2?range.value(1).toInt(&ok2):from;
  if(!ok1||!ok2||range.size()>2||from<0||to<from||to>=CPU_SETSIZE)return {};
  for(int c=from;c<=to;++c)if(!out.contains(c))out.append(c);
 }
 std::sort(out.begin(),out.end());
 return out;
}
QList<int> performanceCpus(const QString& root){
 const auto core=cpuList(readSmall(root+QStringLiteral("/cpu_core/cpus")));
 if(!core.isEmpty()&&!cpuList(readSmall(root+QStringLiteral("/cpu_atom/cpus"))).isEmpty())return core;
 const QDir cpus(root+QStringLiteral("/system/cpu"));
 QList<int> numbers;
 for(const auto& name:cpus.entryList({QStringLiteral("cpu*")},QDir::Dirs|QDir::NoDotAndDotDot)){
  bool ok=false;const int n=name.mid(3).toInt(&ok);
  if(ok&&n>=0&&n<CPU_SETSIZE)numbers.append(n);
 }
 if(numbers.size()<2)return {};
 for(const auto& file:{QStringLiteral("acpi_cppc/highest_perf"),QStringLiteral("cpufreq/cpuinfo_max_freq")}){
  std::map<int,qint64> values;bool every=true;
  for(const int n:std::as_const(numbers)){
   bool ok=false;const qint64 v=readSmall(cpus.filePath(QStringLiteral("cpu%1/").arg(n)+file)).toLongLong(&ok);
   if(!ok||v<=0){every=false;break;}
   values[n]=v;
  }
  if(every)return aboveSlowest(values);
 }
 return {};
}
QList<int> preferredCpus(const QList<int>& allowed,const QString& root){
 QList<int> preferred;
 for(const int c:performanceCpus(root))if(allowed.contains(c))preferred.append(c);
 if(preferred.isEmpty()||preferred.size()>=allowed.size())return {};
 QList<QByteArray> cores;
 for(const int c:std::as_const(preferred)){
  const QString topology=root+QStringLiteral("/system/cpu/cpu%1/topology/").arg(c);
  const auto package=readSmall(topology+QStringLiteral("physical_package_id")),core=readSmall(topology+QStringLiteral("core_id"));
  // Without topology the cores cannot be counted: every CPU, as on a uniform machine.
  if(package.isEmpty()||core.isEmpty())return {};
  const QByteArray id=package+'/'+core;
  if(!cores.contains(id))cores.append(id);
 }
 if(cores.size()<minimumPreferredCores)return {};
 return preferred;
}
QList<int> preferredCpus(){
 cpu_set_t set;CPU_ZERO(&set);
 if(::sched_getaffinity(0,sizeof set,&set)!=0)return {};
 QList<int> allowed;for(int c=0;c<CPU_SETSIZE;++c)if(CPU_ISSET(c,&set))allowed.append(c);
 return preferredCpus(allowed);
}
}
