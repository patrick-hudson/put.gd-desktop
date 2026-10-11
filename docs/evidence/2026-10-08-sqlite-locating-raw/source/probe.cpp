// Standalone locating probe only. No production source, settings or history input.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>

using Clock=std::chrono::steady_clock;
qint64 nowNs(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
void require(bool value,const char* phase){if(!value)throw std::runtime_error(phase);}
template<class F> void timed(QJsonObject& row,const QString& name,F work){
 const auto start=nowNs();try{work();}catch(...){row[name]=double(nowNs()-start);row["failedPhase"]=name;throw;}row[name]=double(nowNs()-start);
}
void exec(QSqlDatabase& db,const QString& sql){
 QSqlQuery query(db);require(query.exec(sql),"synthetic_sql_failed");query.finish();
}
QVariant scalar(QSqlDatabase& db,const QString& sql){
 QSqlQuery query(db);require(query.exec(sql)&&query.next(),"synthetic_scalar_failed");
 auto result=query.value(0);query.finish();return result;
}
qint64 rssKiB(){
 QFile status("/proc/self/status");require(status.open(QIODevice::ReadOnly),"self_status_failed");
 for(const auto& line:status.readAll().split('\n'))if(line.startsWith("VmRSS:"))
  return line.mid(6).trimmed().split(' ').first().toLongLong();
 return -1;
}
struct Connection{
 QString name;QSqlDatabase db;std::unique_ptr<QSqlQuery> insert;
 Connection(const QString& path,int cacheKiB,QJsonObject& row,bool validate){
  name=QUuid::createUuid().toString(QUuid::WithoutBraces);
  try{
  timed(row,"connectionSetupNs",[&]{db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(path);db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=10000");});
  timed(row,"openNs",[&]{require(db.open(),"synthetic_open_failed");});
  timed(row,"permissionsNs",[&]{require(QFile::setPermissions(path,QFile::ReadOwner|QFile::WriteOwner),"private_file_failed");});
  timed(row,"foreignKeysNs",[&]{exec(db,"PRAGMA foreign_keys=ON");});
  if(validate)timed(row,"userVersionNs",[&]{require(scalar(db,"PRAGMA user_version").toInt()==4,"synthetic_version_failed");});
  timed(row,"policyNs",[&]{exec(db,"PRAGMA synchronous=FULL");exec(db,QString("PRAGMA cache_size=-%1").arg(cacheKiB));});
  }catch(...){close();throw;}
 }
 ~Connection(){close();}
 void close(){if(name.isEmpty())return;insert.reset();db.close();db=QSqlDatabase{};QSqlDatabase::removeDatabase(name);name.clear();}
 void write(const QString& id,const QString& state,bool reuse,QJsonObject& row){
  timed(row,"beginNs",[&]{require(db.transaction(),"synthetic_begin_failed");});
  if(!insert)insert=std::make_unique<QSqlQuery>(db);
  if(!reuse||!prepared){
   timed(row,"prepareNs",[&]{require(insert->prepare("INSERT INTO operations VALUES(?,?) ON CONFLICT(id) DO UPDATE SET data=excluded.data"),"synthetic_prepare_failed");});
   prepared=true;
  }else row["prepareNs"]=0;
  const auto bytes=QJsonDocument(QJsonObject{{"state",state},{"created","2026-10-08T00:00:00.000Z"},{"fixture",QString(256,'x')}}).toJson(QJsonDocument::Compact);
  timed(row,"bindNs",[&]{insert->bindValue(0,id);insert->bindValue(1,bytes);});
  timed(row,"executeNs",[&]{require(insert->exec(),"synthetic_execute_failed");insert->finish();});
  timed(row,"commitNs",[&]{require(db.commit(),"synthetic_commit_failed");});
 }
private:bool prepared=false;
};
QJsonObject memoryStage(Connection& connection){
 QJsonObject row;row["rssBeforeKiB"]=rssKiB();
 {require(connection.db.transaction(),"seed_begin_failed");
  QSqlQuery seed(connection.db);require(seed.prepare("INSERT INTO cache_fixture VALUES(?,?)"),"seed_prepare_failed");
  const QByteArray data(256,'z');
  for(int i=0;i<10000;++i){seed.bindValue(0,i);seed.bindValue(1,data);require(seed.exec(),"seed_execute_failed");seed.finish();}
  require(connection.db.commit(),"seed_commit_failed");seed.finish();}
 auto scan=[&]{QSqlQuery query(connection.db);require(query.exec("SELECT data FROM cache_fixture"),"scan_failed");
  qint64 bytes=0;int count=0;while(query.next()){bytes+=query.value(0).toByteArray().size();++count;}
  require(count==10000&&bytes==2560000,"scan_count_failed");query.finish();};
 timed(row,"scanBeforeShrinkNs",scan);row["rssAfterScanKiB"]=rssKiB();
 timed(row,"shrinkNs",[&]{exec(connection.db,"PRAGMA shrink_memory");});
 row["rssAfterShrinkKiB"]=rssKiB();timed(row,"scanAfterShrinkNs",scan);
 row["rssAfterRescanKiB"]=rssKiB();row["cacheKiBSetting"]=scalar(connection.db,"PRAGMA cache_size").toInt();
 row["pageBytes"]=scalar(connection.db,"PRAGMA page_size").toInt();row["pageCount"]=scalar(connection.db,"PRAGMA page_count").toInt();
 row["limit"]="Process RSS is not SQLite allocation accounting; no glibc trim. Synthetic 10000-row scan only.";
 return row;
}
QJsonObject lockStage(const QString& path,int cacheKiB){
 std::promise<bool> held;auto ready=held.get_future();QJsonObject writerRow;
 std::jthread writer([&]{
  bool signalled=false;
  try{
   Connection writer(path,cacheKiB,writerRow,true);
   exec(writer.db,"BEGIN EXCLUSIVE");
   exec(writer.db,"INSERT INTO operations VALUES('held-writer','{}')");
   writerRow["heldStartNs"]=double(nowNs());held.set_value(true);signalled=true;
   std::this_thread::sleep_for(std::chrono::milliseconds(100));
   writerRow["commitStartNs"]=double(nowNs());
   timed(writerRow,"commitNs",[&]{require(writer.db.commit(),"held_writer_commit_failed");});
   writerRow["releasedNs"]=double(nowNs());
  }catch(const std::exception& error){writerRow["error"]=QString::fromUtf8(error.what());if(!signalled)held.set_value(false);}
 });
 require(ready.wait_for(std::chrono::seconds(12))==std::future_status::ready&&ready.get(),"held_writer_setup_failed");
 QJsonObject readerRow;readerRow["beginNs"]=double(nowNs());
 {Connection reader(path,cacheKiB,readerRow,true);reader.write("following-writer","accepted",false,readerRow);readerRow["endNs"]=double(nowNs());}
 writer.join();
 require(!writerRow.contains("error"),"held_writer_failed");
 return {{"writer",writerRow},{"readerThenWriter",readerRow},{"injectedHoldMs",100},
  {"limit","Deliberately held BEGIN EXCLUSIVE models lock overlap, not measured fsync latency or the product commit."}};
}
int main(int argc,char** argv){
 umask(0077);QCoreApplication app(argc,argv);const auto args=app.arguments();
 if(args.size()<3||args.size()>6){qCritical("usage: sqlite-latency-probe NEW_OUTPUT_DIR reopen|connection|statements [512|2000] [--wal] [--lock]");return 2;}
 const auto output=args[1],mode=args[2];
 if(mode!="reopen"&&mode!="connection"&&mode!="statements"){qCritical("invalid mode");return 2;}
 int cacheKiB=2000;bool wal=false,lock=false;
 for(int i=3;i<args.size();++i){if(args[i]=="--wal")wal=true;else if(args[i]=="--lock")lock=true;
  else if(args[i]=="512"||args[i]=="2000")cacheKiB=args[i].toInt();else {qCritical("invalid option");return 2;}}
 QJsonObject report{{"schemaVersion",1},{"case",mode},{"requestedJournal",wal?"WAL":"DELETE"},{"cacheKiB",cacheKiB},
  {"qtVersion",qVersion()},{"warmups",5},{"scoredSamples",30},{"state","not_completed"},
  {"scope","Standalone Qt SQL synthetic locating; no production services/desktop/clipboard/network. Not a product memory/timing gate."}};
 bool ownsOutput=false;
 try{
  require(!QFile::exists(output)&&QDir().mkdir(output),"new_output_directory_required");
  ownsOutput=true;
  require(QFile::setPermissions(output,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner),"private_directory_failed");
  const auto path=QDir(output).filePath("synthetic.sqlite");
  {QFile file(path);require(file.open(QIODevice::WriteOnly|QIODevice::NewOnly),"new_synthetic_file_failed");
   require(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner),"private_file_failed");}
  QJsonObject setup;
  {Connection connection(path,cacheKiB,setup,false);
   const auto actual=scalar(connection.db,wal?"PRAGMA journal_mode=WAL":"PRAGMA journal_mode=DELETE").toString();
   require(actual==(wal?"wal":"delete"),"journal_mode_failed");report["actualJournal"]=actual;
   exec(connection.db,"CREATE TABLE operations(id TEXT PRIMARY KEY,data BLOB)");
   exec(connection.db,"CREATE INDEX operations_created ON operations(json_extract(data,'$.created') DESC)");
   exec(connection.db,"CREATE TABLE cache_fixture(id INTEGER PRIMARY KEY,data BLOB)");
   exec(connection.db,"PRAGMA user_version=4");
   report["sqliteVersion"]=scalar(connection.db,"SELECT sqlite_version()").toString();
   report["sqliteSourceId"]=scalar(connection.db,"SELECT sqlite_source_id()").toString();
   report["synchronous"]=scalar(connection.db,"PRAGMA synchronous").toInt();
   QSqlQuery options(connection.db);require(options.exec("PRAGMA compile_options"),"compile_options_failed");
   QJsonArray values;while(options.next())values.append(options.value(0).toString());report["compileOptions"]=values;
  }
  report["setup"]=setup;QJsonArray samples;
  std::unique_ptr<Connection> persistent;
  if(mode!="reopen"){QJsonObject phases;persistent=std::make_unique<Connection>(path,cacheKiB,phases,true);report["persistentOpen"]=phases;}
  for(int ordinal=0;ordinal<35;++ordinal){
   QJsonObject sample{{"ordinal",ordinal+1},{"warmup",ordinal<5}};QJsonArray phases;const auto begin=nowNs();
   for(const auto& state:QStringList{"accepted","acquiring","cancelled"}){
    QJsonObject row{{"state",state}};std::unique_ptr<Connection> temporary;
    try{
    auto* connection=persistent.get();
    if(!connection){temporary=std::make_unique<Connection>(path,cacheKiB,row,true);connection=temporary.get();}
    connection->write(QString("fixture-%1").arg(ordinal),state,mode=="statements",row);
    if(temporary)timed(row,"closeNs",[&]{temporary.reset();});
    phases.append(row);
    }catch(...){phases.append(row);sample["phases"]=phases;sample["wallNs"]=double(nowNs()-begin);report["failedSample"]=sample;throw;}
   }
   sample["wallNs"]=double(nowNs()-begin);sample["phases"]=phases;samples.append(sample);report["samples"]=samples;
  }
  report["samples"]=samples;
  if(persistent){QJsonObject close;timed(close,"closeNs",[&]{persistent.reset();});report["persistentClose"]=close;}
  {QJsonObject phases;Connection connection(path,cacheKiB,phases,true);
   require(scalar(connection.db,"SELECT count(*) FROM operations").toInt()==35,"operation_count_failed");
   require(scalar(connection.db,"SELECT count(*) FROM operations WHERE json_extract(data,'$.state')='cancelled'").toInt()==35,"cancel_state_failed");
   report["memory"]=memoryStage(connection);}
  if(lock)report["controlledLock"]=lockStage(path,cacheKiB);
  {QJsonObject phases;Connection connection(path,cacheKiB,phases,true);
   require(scalar(connection.db,"PRAGMA integrity_check").toString()=="ok","integrity_check_failed");}
  report["state"]="completed";report["integrityVerified"]=true;
 }catch(const std::exception& error){report["state"]="failed";report["error"]=QString::fromUtf8(error.what());}
 // A failed attempt is also preserved; never silently convert it into a valid result.
 if(ownsOutput){
  QSaveFile file(QDir(output).filePath("results.json"));file.setDirectWriteFallback(false);
  const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented);
  if(!file.open(QIODevice::WriteOnly)||!file.setPermissions(QFile::ReadOwner|QFile::WriteOwner)||file.write(bytes)!=bytes.size()||!file.commit())return 3;
 }
 return report["state"]=="completed"?0:1;
}
