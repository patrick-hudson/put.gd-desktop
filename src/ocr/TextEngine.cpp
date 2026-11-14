#include "TextEngine.h"
#include <QCryptographicHash>
#include <QFile>
#include <tesseract/baseapi.h>
#include <tesseract/ocrclass.h>
#include <tesseract/resultiterator.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <mutex>
#include <vector>

namespace putgd::ocr {
namespace {
// Tesseract only ever sees this name: every file is resolved by readData.
constexpr char dataRoot[]="/putgd-ocr/tessdata/";
constexpr int lstmEntry=17;  // TESSDATA_LSTM
constexpr uint32_t knownEntries=24;  // TESSDATA_NUM_ENTRIES in Tesseract 5

// bytes and sha256 are the file's; data is what Tesseract is handed, the
// entries the LSTM engine reads (lstmImage).
struct DataFile { QString language,path;qint64 bytes=0;QByteArray sha256;std::vector<char> data;qint64 kept=0; };
struct Resolver {
 QStringList directories;
 QList<DataFile> files;
 qint64 total=0;
 QString overLimit;          // the language that crossed a limit
 std::mutex lock;            // engines start on several threads
 void reset(){files.clear();total=0;overLimit.clear();}
};
// The job's LanguageData; Tesseract's FileReader is a plain function.
Resolver* resolver=nullptr;

bool readWhole(const QString& path,qint64 maximum,std::vector<char>& data,bool& tooLarge){
 // O_NONBLOCK: a FIFO named like a data file must not hang the helper in open.
 const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
 if(fd<0)return false;
 struct stat s{};
 if(::fstat(fd,&s)||!S_ISREG(s.st_mode)||s.st_size<1){::close(fd);return false;}
 if(s.st_size>maximum){tooLarge=true;::close(fd);return false;}
 data.resize(size_t(s.st_size));size_t done=0;
 while(done<data.size()){
  const auto n=::read(fd,data.data()+done,data.size()-done);
  if(n<0&&errno==EINTR)continue;
  if(n<=0){::close(fd);data.clear();return false;}
  done+=size_t(n);
 }
 ::close(fd);return true;
}
// The traineddata container: an entry count, a table of 64-bit offsets, the
// entries. It must hold an LSTM model, the only engine put.gd runs.
bool plausibleData(const std::vector<char>& data){
 if(data.size()<4)return false;
 uint32_t entries=0;std::memcpy(&entries,data.data(),4);
 if(entries<=uint32_t(lstmEntry)||entries>1000)return false;
 const size_t header=4+size_t(entries)*8;if(header>data.size())return false;
 std::vector<int64_t> offsets(entries);std::memcpy(offsets.data(),data.data()+4,size_t(entries)*8);
 // Tesseract's loader reads the present entries one after another from the
 // end of the table, each up to the next present one: the first must start
 // there and none may go backwards.
 int64_t previous=-1;
 for(auto offset:offsets){
  if(offset==-1)continue;
  if(offset<int64_t(header)||offset>int64_t(data.size()))return false;
  if(previous<0?offset!=int64_t(header):offset<previous)return false;
  previous=offset;
 }
 const auto lstm=offsets[lstmEntry];if(lstm<0)return false;
 int64_t end=int64_t(data.size());
 for(uint32_t i=lstmEntry+1;i<entries;++i)if(offsets[i]>=0){end=offsets[i];break;}
 return end>lstm;
}
// The entries Tesseract 5.3 and 5.5 read for OEM_LSTM_ONLY (tessedit.cpp
// init_tesseract_lang_data, LSTMRecognizer::Load, Dict::LoadLSTM): the
// language config, the ambiguities and the params model, which a build with
// the legacy engine still loads, and the LSTM model, its dictionaries,
// unicharset and recoder, and the version. The legacy classifier's templates,
// unicharset, shape table and dictionaries are read only with the legacy
// engine (program_editup is given no data for LSTM only).
constexpr int lstmReads[]={0/*LANG_CONFIG*/,2/*AMBIGS*/,16/*PARAMS_MODEL*/,17/*LSTM*/,18/*LSTM_PUNC_DAWG*/,
                           19/*LSTM_SYSTEM_DAWG*/,20/*LSTM_NUMBER_DAWG*/,21/*LSTM_UNICHARSET*/,22/*LSTM_RECODER*/,23/*VERSION*/};
// A traineddata image of plausible data (plausibleData) with only those
// entries, and the offset table rewritten: a model that also holds the legacy
// engine's data (Arch's English, 23 MB against the 4 MB LSTM-only file) costs
// no more memory than its LSTM part, in this cache and in every engine's copy.
std::vector<char> lstmImage(const std::vector<char>& data){
 uint32_t entries=0;std::memcpy(&entries,data.data(),4);
 std::vector<int64_t> offsets(entries);std::memcpy(offsets.data(),data.data()+4,size_t(entries)*8);
 const auto size=[&](uint32_t i)->int64_t{
  if(offsets[i]<0)return -1;
  for(uint32_t j=i+1;j<entries;++j)if(offsets[j]>=0)return offsets[j]-offsets[i];
  return int64_t(data.size())-offsets[i];
 };
 const uint32_t count=std::min(entries,knownEntries);
 std::vector<int64_t> table(count,-1);
 int64_t at=4+int64_t(count)*8,total=at;
 for(const int i:lstmReads)if(uint32_t(i)<count&&size(uint32_t(i))>=0){table[size_t(i)]=total;total+=size(uint32_t(i));}
 std::vector<char> out(static_cast<size_t>(total),char(0));
 std::memcpy(out.data(),&count,4);std::memcpy(out.data()+4,table.data(),size_t(count)*8);
 for(const int i:lstmReads)if(table.size()>size_t(i)&&table[size_t(i)]>=0){
  const auto n=size(uint32_t(i));std::memcpy(out.data()+at,data.data()+offsets[size_t(i)],size_t(n));at+=n;
 }
 return out;
}
bool readData(const char* filename,std::vector<char>* data){
 if(!resolver||!filename||!data)return false;
 const QString name=QFile::decodeName(filename);
 if(!name.startsWith(QLatin1String(dataRoot))||!name.endsWith(QLatin1String(".traineddata")))return false;
 const QString language=name.mid(int(std::strlen(dataRoot))).chopped(12);
 if(!validLanguage(language))return false;
 const std::lock_guard<std::mutex> guard(resolver->lock);
 // A language asked for again, by Tesseract or by another engine, comes from memory.
 for(const auto& file:std::as_const(resolver->files))if(file.language==language){*data=file.data;return true;}
 if(resolver->files.size()>=maximumLanguages){resolver->overLimit=language;return false;}
 for(const auto& directory:resolver->directories){
  const auto path=directory+QLatin1Char('/')+language+QLatin1String(".traineddata");
  bool tooLarge=false;std::vector<char> bytes;
  if(!readWhole(path,maximumLanguageBytes-resolver->total,bytes,tooLarge)){
   if(tooLarge){resolver->overLimit=language;return false;}
   continue;
  }
  if(!plausibleData(bytes))continue;  // a broken user file falls back to the next directory
  DataFile file{language,path,qint64(bytes.size()),QCryptographicHash::hash(QByteArrayView(bytes.data(),qsizetype(bytes.size())),QCryptographicHash::Sha256).toHex(),lstmImage(bytes)};
  resolver->total+=file.bytes;file.kept=qint64(file.data.size());
  *data=file.data;resolver->files.append(std::move(file));
  return true;
 }
 return false;
}
// Without the dictionary, none of the word lists is loaded: system and frequent
// words, punctuation, numbers, unambiguous words and bigrams.
std::vector<std::string> initNames(const EngineOptions& o){return o.dictionary?std::vector<std::string>{}:std::vector<std::string>{"load_system_dawg","load_freq_dawg","load_punc_dawg","load_number_dawg","load_unambig_dawg","load_bigram_dawg"};}
std::vector<std::string> initValues(const EngineOptions& o){return o.dictionary?std::vector<std::string>{}:std::vector<std::string>(6,"0");}
}
LanguageData::LanguageData(const QStringList& directories){delete resolver;resolver=new Resolver;resolver->directories=directories;}
LanguageData::~LanguageData(){delete resolver;resolver=nullptr;}
void LanguageData::release(){
 if(!resolver)return;
 const std::lock_guard<std::mutex> guard(resolver->lock);
 for(auto& f:resolver->files)std::vector<char>().swap(f.data);
}
qint64 LanguageData::bytesBeyondEnglish()const{
 qint64 total=0;if(!resolver)return 0;
 for(const auto& f:std::as_const(resolver->files))if(f.language!=QLatin1String("eng"))total+=f.kept;
 return total;
}
qint64 LanguageData::bytesHanded()const{
 qint64 total=0;if(!resolver)return 0;
 for(const auto& f:std::as_const(resolver->files))total+=f.kept;
 return total;
}
QStringList LanguageData::files()const{
 QStringList out;if(!resolver)return out;
 for(const auto& f:std::as_const(resolver->files))out.append(f.language+QLatin1String(".traineddata ")+QString::number(f.bytes)+QLatin1Char(' ')+QString::fromLatin1(f.sha256));
 return out;
}
TextEngine::TextEngine()=default;
TextEngine::~TextEngine(){if(api_)api_->End();}
QString TextEngine::version(){return QString::fromLatin1(tesseract::TessBaseAPI::Version());}
QString TextEngine::start(const QStringList& languages,bool strict,const EngineOptions& options,QString& failedLanguage){
 failedLanguage.clear();used_.clear();skipped_.clear();options_=options;
 if(languages.isEmpty()||languages.size()>maximumLanguages){failedLanguage=languages.value(maximumLanguages);return QStringLiteral("language_unavailable");}
 for(const auto& language:languages)if(!validLanguage(language)||languages.count(language)>1){failedLanguage=language;return QStringLiteral("language_unavailable");}
 if(!resolver)return QStringLiteral("engine_unavailable");
 auto names=initNames(options),values=initValues(options);
 QStringList remaining=languages;
 while(!remaining.isEmpty()){
  {const std::lock_guard<std::mutex> guard(resolver->lock);resolver->reset();}
  if(api_)api_->End();
  api_=std::make_unique<tesseract::TessBaseAPI>();
  const auto joined=remaining.join(QLatin1Char('+')).toLatin1();
  const int status=api_->Init(dataRoot,0,joined.constData(),tesseract::OEM_LSTM_ONLY,nullptr,0,&names,&values,false,readData);
  std::vector<std::string> loaded;if(status==0)api_->GetLoadedLanguagesAsVector(&loaded);
  QStringList loadedNames;for(const auto& l:loaded)loadedNames.append(QString::fromStdString(l));
  QString missing;for(const auto& l:remaining)if(!loadedNames.contains(l)){missing=l;break;}
  if(!resolver->overLimit.isEmpty()){failedLanguage=resolver->overLimit;api_.reset();return QStringLiteral("language_unavailable");}
  if(status==0&&missing.isEmpty()){
   used_=loadedNames;for(const auto& l:languages)if(!remaining.contains(l))skipped_.append(l);
   return {};
  }
  if(strict){failedLanguage=status==0?missing:remaining.first();api_.reset();return QStringLiteral("language_unavailable");}
  if(status==0){
   // The primary loaded, another did not: Tesseract skipped it already.
   used_=loadedNames;for(const auto& l:languages)if(!loadedNames.contains(l))skipped_.append(l);
   return {};
  }
  remaining.removeFirst();
 }
 failedLanguage=languages.first();api_.reset();
 return QStringLiteral("language_unavailable");
}
QString TextEngine::startLike(const TextEngine& first){
 options_=first.options_;used_=first.used_;skipped_=first.skipped_;
 if(!resolver||used_.isEmpty())return QStringLiteral("engine_unavailable");
 auto names=initNames(options_),values=initValues(options_);
 api_=std::make_unique<tesseract::TessBaseAPI>();
 const auto joined=used_.join(QLatin1Char('+')).toLatin1();
 if(api_->Init(dataRoot,0,joined.constData(),tesseract::OEM_LSTM_ONLY,nullptr,0,&names,&values,false,readData)!=0){api_.reset();return QStringLiteral("engine_unavailable");}
 return {};
}
void TextEngine::prepare(const GreyImage& band,double scale){
 api_->SetPageSegMode(tesseract::PageSegMode(options_.pageSegmentation));
 api_->SetVariable("thresholding_method",QByteArray::number(options_.thresholding).constData());
 api_->SetVariable("tessedit_do_invert",options_.invertLines?"1":"0");
 api_->SetImage(reinterpret_cast<const unsigned char*>(band.pixels.constData()),band.width,band.height,1,band.width);
 api_->SetSourceResolution(options_.resolution>0?options_.resolution:std::clamp(int(96*scale),70,600));
}
BandText TextEngine::recognize(const GreyImage& band,double scale,int deadlineMs,bool* cutShort,bool* failed){
 BandText out;if(cutShort)*cutShort=false;if(failed)*failed=false;
 if(!api_){if(failed)*failed=true;return out;}
 if(band.width<1||band.height<1)return out;
 prepare(band,scale);
 // The monitor's deadline stops recognition between words. Tesseract then
 // returns -1 with the words before the deadline recognized; the iterator
 // below gives exactly those.
 tesseract::ETEXT_DESC monitor;if(deadlineMs>0)monitor.set_deadline_msecs(deadlineMs);
 const int status=api_->Recognize(deadlineMs>0?&monitor:nullptr);
 const bool late=deadlineMs>0&&monitor.deadline_exceeded();
 if(cutShort)*cutShort=late;
 if(status!=0&&!late){api_->Clear();if(failed)*failed=true;return out;}
 std::unique_ptr<tesseract::ResultIterator> it(api_->GetIterator());
 if(it){
  int block=-1;
  it->Begin();
  do{
   if(it->Empty(tesseract::RIL_WORD))continue;
   if(it->IsAtBeginningOf(tesseract::RIL_BLOCK))++block;
   if(out.lines.isEmpty()||it->IsAtBeginningOf(tesseract::RIL_TEXTLINE)){
    int l=0,t=0,r=0,b=0;it->BoundingBox(tesseract::RIL_TEXTLINE,&l,&t,&r,&b);
    out.lines.append({QRect(l,t,std::max(1,r-l),std::max(1,b-t)),std::max(0,block)});
   }
   std::unique_ptr<char[]> text(it->GetUTF8Text(tesseract::RIL_WORD));
   // After a deadline, words it never reached have no text.
   if(!text||!*text.get())continue;
   int l=0,t=0,r=0,b=0;it->BoundingBox(tesseract::RIL_WORD,&l,&t,&r,&b);
   out.words.append({QString::fromUtf8(text.get()),QRect(l,t,std::max(1,r-l),std::max(1,b-t)),it->Confidence(tesseract::RIL_WORD),int(out.lines.size()-1)});
  }while(it->Next(tesseract::RIL_WORD));
 }
 api_->Clear();
 return out;
}
}
