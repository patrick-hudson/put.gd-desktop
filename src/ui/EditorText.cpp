#include "EditorWindow.h"
#include <QCoreApplication>
#include <QDir>
#include <QLocale>
#include <QPainter>
#include <QThread>
#include <algorithm>

namespace putgd {
namespace {
QPolygon polygon(const QList<QPoint>& points,const QPoint& offset){QPolygon quad;for(const auto& point:points)quad<<point+offset;return quad;}
QList<editor::RecognizedWord> words(const QList<TextWord>& found,const QPoint& offset){
 QList<editor::RecognizedWord> out;out.reserve(found.size());
 for(const auto& word:found)out.append({word.text,word.rect.translated(offset),word.confidence,0});
 return out;
}
}
EditorTextRecognition::EditorTextRecognition(editor::EditDocument& doc,QObject* parent):QObject(parent),doc_(doc),signature_(editor::TextLayer::inputSignature(doc)) {
 runner_=[](const QImage& grey,const textjob::Options& options,Error& error,TextJob::Cancel cancel){return TextJob().recognize(grey,options,error,std::move(cancel));};
}
EditorTextRecognition::~EditorTextRecognition() {
 // The helper is the worker's child: cancel it and wait until it is reaped.
 if(cancel_)cancel_->store(true);
 if(worker_){worker_->wait();delete worker_.data();}
}
QUrl EditorTextRecognition::openableLink(const QString& content) {
 if(content.isEmpty()||content.size()>2048)return {};
 for(const auto c:content)if(c.unicode()<0x20||(c.unicode()>=0x7f&&c.unicode()<=0x9f))return {};
 const QUrl link(content,QUrl::StrictMode);
 if(!link.isValid()||(link.scheme()!="http"&&link.scheme()!="https")||link.host().isEmpty())return {};
 return link;
}
QString EditorTextRecognition::shown(const QString& content) {
 QString out;
 for(qsizetype i=0;i<content.size();++i){
  char32_t point=content.at(i).unicode();
  if(content.at(i).isHighSurrogate()&&i+1<content.size()&&content.at(i+1).isLowSurrogate()){point=QChar::surrogateToUcs4(content.at(i),content.at(i+1));++i;}
  const bool plain=point==' '||(QChar::isPrint(point)&&QChar::category(point)!=QChar::Other_Format);
  out+=plain?QString::fromUcs4(&point,1):QString("\\u{%1}").arg(uint(point),4,16,QChar('0')).toUpper().replace("\\U{","\\u{");
 }
 return out;
}
void EditorTextRecognition::setRunner(Runner runner) {runner_=std::move(runner);}
QImage EditorTextRecognition::input(const QImage& base,const QRect& crop,const QList<QRect>& redactions) {
 auto part=base.copy(crop);
 if(!redactions.isEmpty()){
  QPainter painter(&part);painter.setCompositionMode(QPainter::CompositionMode_Source);
  for(const auto& mark:redactions)if(const auto area=mark.intersected(crop);!area.isEmpty())painter.fillRect(area.translated(-crop.topLeft()),Qt::black);
 }
 return TextJob::grey(part);
}
void EditorTextRecognition::setActive(bool active) {
 if(active_==active)return;
 active_=active;if(active_)start();
}
void EditorTextRecognition::documentChanged() {
 const auto signature=editor::TextLayer::inputSignature(doc_);
 if(signature==signature_)return;
 // A new revision: the running job stops, its result is never accepted, and
 // nothing of the old result is shown, selected, copied or returned.
 signature_=signature;++revision_;
 if(cancel_)cancel_->store(true);
 layer_.clear();candidates_.clear();lineTops_.clear();truncated_=false;state_="idle";error_={};
 for(auto& done:std::exchange(waiting_,{}))done({"edit_stale","A Redact mark or the crop changed before recognition finished",{}});
 if(active_)start();
 emit changed();
}
void EditorTextRecognition::start() {
 // A result stands for its revision; a failure can be tried again.
 if(state_=="done")return;
 // One job at a time: a cancelled job is reaped before the next one starts.
 if(worker_){if(jobRevision_!=revision_)restart_=true;return;}
 run();
}
void EditorTextRecognition::whenRecognized(std::function<void(Error)> done) {
 if(state_=="done"){done({});return;}
 waiting_.append(std::move(done));start();
}
void EditorTextRecognition::run() {
 restart_=false;jobRevision_=revision_;state_="recognizing";error_={};
 const auto base=doc_.base();const auto crop=doc_.crop();const auto redactions=doc_.redactionRects();
 textjob::Options options;options.words=true;options.candidates=true;
 // The saved preference, else the locale's language and English; installed ones only.
 options.languages=textjob::defaultLanguages(languages_,QLocale::system().name(),textjob::installedLanguages(textjob::dataDirectories(QDir::homePath(),QCoreApplication::applicationDirPath())));
 cancel_=std::make_shared<std::atomic_bool>(false);
 auto result=std::make_shared<TextResult>();auto error=std::make_shared<Error>();
 const auto revision=revision_;const auto runner=runner_;const auto cancel=cancel_;
 // The worker starts and reaps the helper, so it outlives it.
 auto* thread=QThread::create([base,crop,redactions,options,runner,cancel,result,error]{
  const auto grey=input(base,crop,redactions);
  if(cancel->load()){*error=textjob::failure("cancelled");return;}
  *result=runner(grey,options,*error,cancel);
 });
 worker_=thread;
 connect(thread,&QThread::finished,this,[this,thread,revision,result,error]{
  if(worker_==thread)worker_.clear();
  thread->deleteLater();
  finished(revision,*error,result);
 });
 thread->start();
 emit changed();
}
void EditorTextRecognition::finished(quint64 revision,Error error,std::shared_ptr<TextResult> result) {
 // A result for an older revision is rejected, whatever it holds.
 if(revision!=revision_){if(restart_)run();return;}
 if(error){fail(error.code=="cancelled"?Error{"edit_stale","Recognition was cancelled",{}}:error);return;}
 const auto offset=doc_.crop().topLeft();const auto redactions=doc_.redactionRects();
 auto found=words(result->words,offset);auto every=words(result->candidates,offset);
 QList<editor::RecognizedCode> codes;
 for(const auto& code:result->codes)codes.append({code.format,code.binary,code.binary?QString():code.text,code.length,polygon(code.quad,offset)});
 QList<editor::RecognizedCode> none;
 editor::TextLayer::exclude(found,codes,redactions);editor::TextLayer::exclude(every,none,redactions);
 layer_.set(nextIdentity_++,std::move(found),std::move(codes));candidates_=std::move(every);truncated_=result->truncated;
 lineTops_.clear();for(const auto& word:layer_.words()){if(word.line>=lineTops_.size())lineTops_.append(word.rect.top());else lineTops_[word.line]=qMin(lineTops_[word.line],word.rect.top());}
 state_="done";error_={};
 for(auto& done:std::exchange(waiting_,{}))done({});
 emit changed();
}
void EditorTextRecognition::fail(const Error& error) {
 state_="failed";error_=error;
 for(auto& done:std::exchange(waiting_,{}))done(error);
 emit changed();
}
QString EditorTextRecognition::allText() const {
 TextResult copy;const auto& found=layer_.words();
 copy.text=found.isEmpty()?QString():editor::TextLayer::join(found,0,int(found.size())-1);copy.lineTops=lineTops_;
 for(const auto& code:layer_.codes()){TextCode entry;entry.format=code.format;entry.binary=code.binary;entry.text=code.text;entry.length=code.length;for(const auto& point:code.quad)entry.quad.append(point);copy.codes.append(entry);}
 return textjob::copyText(copy);
}
QJsonObject EditorTextRecognition::inspect() const {
 QList<TextWord> found;for(const auto& word:layer_.words())found.append({word.text,word.rect,word.confidence});
 QList<TextCode> codes;for(const auto& code:layer_.codes()){TextCode entry;entry.format=code.format;entry.binary=code.binary;entry.text=code.text;entry.length=code.length;for(const auto& point:code.quad)entry.quad.append(point);codes.append(entry);}
 QJsonObject text{{"state",state_},{"result",layer_.identity()?QJsonValue(double(layer_.identity())):QJsonValue()},{"words",textjob::wordsJson(found)},{"codes",textjob::codesJson(codes)},{"truncated",truncated_},
  {"selection",layer_.hasSelection()?QJsonValue(QJsonObject{{"from",layer_.first()},{"to",layer_.last()}}):QJsonValue()}};
 if(error_)text["error"]=error_.toJson();
 return text;
}
}
