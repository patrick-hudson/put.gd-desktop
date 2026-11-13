#include "TextJobProtocol.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QtEndian>
#include <algorithm>
#include <cstring>

namespace putgd {
QPoint TextCode::centre() const {
 if(quad.isEmpty())return {};
 qint64 x=0,y=0;for(const auto& point:quad){x+=point.x();y+=point.y();}
 return {int(x/quad.size()),int(y/quad.size())};
}
int TextResult::textCodes() const {return int(std::count_if(codes.begin(),codes.end(),[](const TextCode& code){return !code.binary&&!code.text.isEmpty();}));}
namespace textjob {
namespace {
QString decimal(qint64 value){return QString::number(value);}
// A canonical nonnegative decimal string, as the image worker's descriptors are.
bool number(const QJsonValue& value,qint64& out){bool ok=false;if(!value.isString())return false;out=value.toString().toLongLong(&ok);return ok&&out>=0&&QString::number(out)==value.toString();}
bool integer(const QJsonValue& value,qint64 minimum,qint64 maximum,qint64& out){if(!value.isDouble())return false;const double number=value.toDouble();out=value.toInteger();return number==double(out)&&out>=minimum&&out<=maximum;}
bool keys(const QJsonObject& object,const QStringList& names){if(object.size()!=names.size())return false;for(const auto& name:names)if(!object.contains(name))return false;return true;}
QJsonObject limits(){return {{"outputBytes",decimal(maximumOutputBytes)},{"textBytes",decimal(maximumTextBytes)},{"words",decimal(maximumWords)},{"wordBytes",decimal(maximumWordBytes)},{"codes",decimal(maximumCodes)},{"codeBytes",decimal(maximumCodeBytes)}};}
QJsonArray strings(const QStringList& values){QJsonArray array;for(const auto& value:values)array.append(value);return array;}
bool languageList(const QJsonValue& value,int minimum,QStringList& out){
 out.clear();if(!value.isArray()||value.toArray().size()<minimum||value.toArray().size()>maximumLanguages)return false;
 for(const auto& entry:value.toArray()){if(!entry.isString()||!validLanguage(entry.toString())||out.contains(entry.toString()))return false;out.append(entry.toString());}
 return true;
}
// The longest prefix of at most `bytes` UTF-8 bytes that ends on a code point.
QString cutUtf8(const QString& text,qint64 bytes,bool& cut){
 if(text.toUtf8().size()<=bytes)return text;
 cut=true;qsizetype low=0,high=text.size();
 while(low<high){const auto middle=(low+high+1)/2;if(text.left(middle).toUtf8().size()<=bytes)low=middle;else high=middle-1;}
 if(low>0&&text.at(low-1).isHighSurrogate())--low;
 return text.left(low);
}
QJsonObject wordJson(const TextWord& word){return {{"text",word.text},{"rect",QJsonArray{word.rect.x(),word.rect.y(),word.rect.width(),word.rect.height()}},{"confidence",word.confidence}};}
QJsonObject codeJson(const TextCode& code){
 QJsonArray quad;for(const auto& point:code.quad)quad.append(QJsonArray{point.x(),point.y()});
 QJsonObject object{{"format",code.format},{"kind",code.binary?"binary":"text"},{"length",double(code.length)},{"quad",quad}};
 if(!code.binary)object["text"]=code.text;
 return object;
}
QByteArray serialize(const TextResult& result){
 QJsonArray tops;for(const int top:result.lineTops)tops.append(top);
 return QJsonDocument(QJsonObject{{"text",result.text},{"lineTops",tops},{"words",wordsJson(result.words)},{"candidates",wordsJson(result.candidates)},{"codes",codesJson(result.codes)},
  {"truncated",result.truncated},{"banded",result.banded},{"languagesUsed",strings(result.languagesUsed)},{"languagesSkipped",strings(result.languagesSkipped)},
  {"timings",QJsonObject{{"engineStartMs",double(result.engineStartMs)},{"recognizeMs",double(result.recognizeMs)}}}}).toJson(QJsonDocument::Compact);
}
void boundWords(QList<TextWord>& words,bool& cut){
 if(words.size()>maximumWords){words.resize(maximumWords);cut=true;}
 for(auto& word:words)word.text=cutUtf8(word.text,maximumWordBytes,cut);
}
void trimLines(TextResult& result){const auto lines=result.text.isEmpty()?0:result.text.count('\n')+1;if(result.lineTops.size()>lines)result.lineTops.resize(lines);while(result.lineTops.size()<lines)result.lineTops.append(result.lineTops.isEmpty()?0:result.lineTops.last());}
bool decodeWords(const QJsonValue& value,bool wanted,const InputHeader& input,QList<TextWord>& out){
 if(!value.isArray()||value.toArray().size()>maximumWords||(!wanted&&!value.toArray().isEmpty()))return false;
 for(const auto& entry:value.toArray()){
  const auto object=entry.toObject();const auto rect=object["rect"].toArray();qint64 x=0,y=0,w=0,h=0,confidence=0;
  if(!entry.isObject()||!keys(object,{"text","rect","confidence"})||!object["text"].isString()||object["text"].toString().isEmpty()||object["text"].toString().toUtf8().size()>maximumWordBytes
     ||rect.size()!=4||!integer(rect[0],0,input.width-1,x)||!integer(rect[1],0,input.height-1,y)||!integer(rect[2],1,input.width-x,w)||!integer(rect[3],1,input.height-y,h)||!integer(object["confidence"],0,100,confidence))return false;
  const auto text=sanitizeText(object["text"].toString()).remove('\n');
  if(!text.isEmpty())out.append({text,QRect(int(x),int(y),int(w),int(h)),int(confidence)});
 }
 return true;
}
bool format(char32_t c){return QChar::category(c)==QChar::Other_Format;}
QString sanitize(const QString& input,bool joiners){
 QString out;out.reserve(input.size());
 const auto text=input.normalized(QString::NormalizationForm_C);
 for(qsizetype i=0;i<text.size();++i){
  char32_t c=text.at(i).unicode();
  if(text.at(i).isHighSurrogate()&&i+1<text.size()&&text.at(i+1).isLowSurrogate()){c=QChar::surrogateToUcs4(text.at(i),text.at(i+1));++i;}
  else if(text.at(i).isSurrogate())continue; // a lone surrogate is not text
  if(c==0x2028||c==0x2029){out.append('\n');continue;}
  if(c=='\t'||c=='\n'){out.append(QChar(char16_t(c)));continue;}
  // Other_Control is C0, DEL and C1; Other_Format holds the bidirectional
  // controls and the zero-width, invisible and tag characters.
  if(QChar::category(c)==QChar::Other_Control||c==0xFFFE||c==0xFFFF)continue;
  if(format(c)&&!(joiners&&(c==0x200C||c==0x200D)))continue;
  out.append(QChar::fromUcs4(c));
 }
 return out;
}
struct Tongue {const char* locale;const char* language;};
// ISO 639-1 to the Tesseract data file of that language.
constexpr Tongue tongues[]={{"af","afr"},{"ar","ara"},{"az","aze"},{"be","bel"},{"bg","bul"},{"bn","ben"},{"ca","cat"},{"cs","ces"},{"cy","cym"},{"da","dan"},{"de","deu"},{"el","ell"},{"en","eng"},{"es","spa"},{"et","est"},{"eu","eus"},{"fa","fas"},{"fi","fin"},{"fr","fra"},{"ga","gle"},{"gl","glg"},{"he","heb"},{"hi","hin"},{"hr","hrv"},{"hu","hun"},{"hy","hye"},{"id","ind"},{"is","isl"},{"it","ita"},{"ja","jpn"},{"ka","kat"},{"kk","kaz"},{"ko","kor"},{"lt","lit"},{"lv","lav"},{"mk","mkd"},{"ms","msa"},{"mt","mlt"},{"nb","nor"},{"nl","nld"},{"nn","nor"},{"no","nor"},{"pl","pol"},{"pt","por"},{"ro","ron"},{"ru","rus"},{"sk","slk"},{"sl","slv"},{"sq","sqi"},{"sr","srp"},{"sv","swe"},{"sw","swa"},{"ta","tam"},{"te","tel"},{"th","tha"},{"tr","tur"},{"uk","ukr"},{"ur","urd"},{"uz","uzb"},{"vi","vie"}};
}
QStringList redactCategories(){return {"email","phone","ip","card","key"};}
QStringList childErrorCodes(){return {"input_excessive","text_job_io","language_unavailable","engine_unavailable","helper_sandbox_unavailable"};}
QStringList parentErrorCodes(){return {"text_job_timeout","text_job_failed","text_incompatible","text_busy","text_helper_missing"};}
Error failure(const QString& code,const QString& language){
 if(code=="cancelled")return {code,"Text recognition cancelled",{}};
 if(code=="input_excessive")return {code,"The image is too large for text recognition",{}};
 if(code=="text_job_io")return {code,"Text recognition could not read the image or write its result",{}};
 if(code=="language_unavailable")return {code,"Text recognition data for this language is not installed",validLanguage(language)?QJsonObject{{"language",language}}:QJsonObject{}};
 if(code=="engine_unavailable")return {code,"Text recognition needs the Tesseract English data. Install tesseract-ocr-eng (Debian, Ubuntu, Mint), tesseract-langpack-eng (Fedora) or tesseract-data-eng (Arch)",{}};
 if(code=="helper_sandbox_unavailable")return {code,"Text recognition is unavailable: this system cannot confine the recognition helper",{}};
 if(code=="text_job_timeout")return {code,"Text recognition timed out",{}};
 if(code=="text_incompatible")return {code,"The text recognition helper does not match this build of put.gd",{}};
 if(code=="text_busy")return {code,busyNotification,{}};
 if(code=="text_helper_missing")return {code,"Text recognition needs the putgd-ocr helper. Reinstall put.gd",{}};
 return {"text_job_failed","Text recognition failed",{}};
}
QByteArray encodeInputHeader(const InputHeader& header){
 QByteArray bytes(inputHeaderBytes,'\0');std::memcpy(bytes.data(),inputMagic,4);
 const quint32 words[]={inputVersion,quint32(header.width),quint32(header.height),quint32(header.stride)};
 for(int i=0;i<4;++i)qToLittleEndian<quint32>(words[i],bytes.data()+4+4*i);
 return bytes;
}
bool admitted(int width,int height,int stride){return width>=1&&height>=1&&width<=maximumEdge&&height<=maximumEdge&&stride>=width&&stride<=maximumEdge+3&&qint64(width)*height<=maximumInputPixels;}
qint64 inputBytes(const InputHeader& header){return inputHeaderBytes+qint64(header.stride)*header.height;}
bool decodeInputHeader(const QByteArray& bytes,qint64 total,InputHeader& header){
 if(bytes.size()!=inputHeaderBytes||std::memcmp(bytes.constData(),inputMagic,4)!=0||qFromLittleEndian<quint32>(bytes.constData()+4)!=inputVersion)return false;
 for(qsizetype i=20;i<inputHeaderBytes;++i)if(bytes.at(i)!='\0')return false;
 const auto width=qFromLittleEndian<quint32>(bytes.constData()+8),height=qFromLittleEndian<quint32>(bytes.constData()+12),stride=qFromLittleEndian<quint32>(bytes.constData()+16);
 if(width>quint32(maximumEdge)||height>quint32(maximumEdge)||stride>quint32(maximumEdge+3)||!admitted(int(width),int(height),int(stride)))return false;
 header={int(width),int(height),int(stride)};return inputBytes(header)==total;
}
QJsonObject envelope(const QString& nonce){return {{"role",role},{"nonce",nonce},{"generation",generation}};}
bool validEnvelope(const QJsonObject& payload,const QString& nonce){return !nonce.isEmpty()&&payload["role"]==QLatin1String(role)&&payload["nonce"]==nonce&&payload["generation"]==QLatin1String(generation);}
QJsonObject bootstrap(const QString& nonce,const QJsonObject& build,const InputHeader& input,const Options& options){
 auto payload=envelope(nonce);payload["build"]=build;payload["task"]="recognize";
 payload["input"]=QJsonObject{{"bytes",decimal(inputBytes(input))},{"width",decimal(input.width)},{"height",decimal(input.height)},{"stride",decimal(input.stride)}};
 payload["options"]=QJsonObject{{"words",options.words},{"candidates",options.candidates},{"languages",strings(options.languages)},{"languagesRequired",options.languagesRequired}};
 payload["limits"]=limits();return payload;
}
bool decodeBootstrap(const QJsonObject& payload,const QJsonObject& build,QString& nonce,InputHeader& input,Options& options){
 const auto described=payload["input"].toObject(),wanted=payload["options"].toObject();qint64 bytes=0,width=0,height=0,stride=0;
 if(!keys(payload,{"role","nonce","generation","build","task","input","options","limits"})||!payload["nonce"].isString()||!validEnvelope(payload,payload["nonce"].toString())
    ||!payload["build"].isObject()||payload["build"].toObject()!=build||payload["task"]!=QLatin1String("recognize")||payload["limits"].toObject()!=limits()
    ||!keys(described,{"bytes","width","height","stride"})||!number(described["bytes"],bytes)||!number(described["width"],width)||!number(described["height"],height)||!number(described["stride"],stride)
    ||width>maximumEdge||height>maximumEdge||stride>maximumEdge+3||!admitted(int(width),int(height),int(stride))
    ||!keys(wanted,{"words","candidates","languages","languagesRequired"})||!wanted["words"].isBool()||!wanted["candidates"].isBool()||!wanted["languagesRequired"].isBool()||!languageList(wanted["languages"],1,options.languages))return false;
 input={int(width),int(height),int(stride)};if(inputBytes(input)!=bytes)return false;
 options.words=wanted["words"].toBool();options.candidates=wanted["candidates"].toBool();options.languagesRequired=wanted["languagesRequired"].toBool();nonce=payload["nonce"].toString();return true;
}
QJsonObject ready(const QString& nonce,const QJsonObject& build){auto payload=envelope(nonce);payload["build"]=build;return payload;}
bool validReady(const QJsonObject& payload,const QString& nonce,const QJsonObject& build){return payload.size()==4&&validEnvelope(payload,nonce)&&payload["build"].isObject()&&payload["build"].toObject()==build;}
QJsonObject checkpoint(const QString& nonce,const QByteArray& output){
 auto payload=envelope(nonce);payload["ok"]=true;payload["bytes"]=decimal(output.size());
 payload["sha256"]=QString::fromLatin1(QCryptographicHash::hash(output,QCryptographicHash::Sha256).toHex());return payload;
}
QJsonObject checkpointFailure(const QString& nonce,const QString& code,const QString& language){
 auto payload=envelope(nonce);payload["ok"]=false;payload["code"]=code;
 if(code=="language_unavailable"&&validLanguage(language))payload["language"]=language;
 return payload;
}
bool decodeCheckpoint(const QJsonObject& payload,const QString& nonce,Error& error,qint64& bytes,QString& sha256){
 error={};bytes=0;sha256.clear();
 if(!validEnvelope(payload,nonce)||!payload["ok"].isBool())return false;
 if(!payload["ok"].toBool()){
  const auto code=payload["code"].toString();const bool named=payload.contains("language");
  if(payload.size()!=(named?6:5)||!childErrorCodes().contains(code)||(named&&(code!="language_unavailable"||!payload["language"].isString()||!validLanguage(payload["language"].toString()))))return false;
  error=failure(code,payload["language"].toString());return true;
 }
 const auto hash=payload["sha256"].toString();
 if(payload.size()!=6||!number(payload["bytes"],bytes)||bytes<2||bytes>maximumOutputBytes||hash.size()!=64||QByteArray::fromHex(hash.toLatin1()).toHex()!=hash.toLatin1())return false;
 sha256=hash;return true;
}
QJsonArray wordsJson(const QList<TextWord>& words){QJsonArray array;for(const auto& word:words)array.append(wordJson(word));return array;}
QJsonArray codesJson(const QList<TextCode>& codes){QJsonArray array;for(const auto& code:codes)array.append(codeJson(code));return array;}
QStringList codeFormats(){return {"QRCode","MicroQRCode","DataMatrix","Aztec"};}
QByteArray encodeResult(TextResult result,const Options& options){
 bool cut=result.truncated;
 if(!options.words)result.words.clear();
 if(!options.candidates)result.candidates.clear();
 result.text=cutUtf8(result.text,maximumTextBytes,cut);trimLines(result);
 boundWords(result.words,cut);boundWords(result.candidates,cut);
 if(result.codes.size()>maximumCodes){result.codes.resize(maximumCodes);cut=true;}
 for(auto& code:result.codes)if(!code.binary)code.text=cutUtf8(code.text,maximumCodeBytes,cut);
 result.truncated=cut;auto bytes=serialize(result);
 // The output cap governs: candidates go first, then words, then codes, then text.
 while(bytes.size()>maximumOutputBytes){
  result.truncated=true;
  if(!result.candidates.isEmpty())result.candidates.resize(result.candidates.size()/2);
  else if(!result.words.isEmpty())result.words.resize(result.words.size()/2);
  else if(!result.codes.isEmpty())result.codes.resize(result.codes.size()/2);
  else{bool unused=false;result.text=cutUtf8(result.text,result.text.toUtf8().size()/2,unused);trimLines(result);}
  bytes=serialize(result);
 }
 return bytes;
}
bool decodeResult(const QByteArray& json,const InputHeader& input,const Options& options,TextResult& result){
 result={};if(json.size()>maximumOutputBytes)return false;
 QJsonParseError parse;const auto document=QJsonDocument::fromJson(json,&parse);const auto object=document.object();
 if(parse.error!=QJsonParseError::NoError||!document.isObject()||!keys(object,{"text","lineTops","words","candidates","codes","truncated","banded","languagesUsed","languagesSkipped","timings"})
    ||!object["text"].isString()||object["text"].toString().toUtf8().size()>maximumTextBytes||!object["lineTops"].isArray()||!object["truncated"].isBool()||!object["banded"].isBool()
    ||!languageList(object["languagesUsed"],0,result.languagesUsed)||!languageList(object["languagesSkipped"],0,result.languagesSkipped)||(options.languagesRequired&&!result.languagesSkipped.isEmpty()))return false;
 const auto timings=object["timings"].toObject();
 if(!keys(timings,{"engineStartMs","recognizeMs"})||!integer(timings["engineStartMs"],0,3600000,result.engineStartMs)||!integer(timings["recognizeMs"],0,3600000,result.recognizeMs))return false;
 const auto raw=object["text"].toString();const auto lines=raw.isEmpty()?QStringList{}:raw.split('\n');const auto tops=object["lineTops"].toArray();
 if(tops.size()!=lines.size())return false;
 QStringList kept;
 for(qsizetype i=0;i<lines.size();++i){
  qint64 top=0;if(!integer(tops[i],0,input.height,top))return false;
  // A line separator inside a line becomes lines of its own, at the same height.
  for(const auto& line:sanitizeText(lines[i]).split('\n')){kept.append(line);result.lineTops.append(int(top));}
 }
 result.text=kept.join('\n');
 if(!decodeWords(object["words"],options.words,input,result.words)||!decodeWords(object["candidates"],options.candidates,input,result.candidates))return false;
 if(!object["codes"].isArray()||object["codes"].toArray().size()>maximumCodes)return false;
 for(const auto& entry:object["codes"].toArray()){
  const auto code=entry.toObject();const bool binary=code["kind"]==QLatin1String("binary");const auto quad=code["quad"].toArray();TextCode decoded;
  if(!entry.isObject()||!keys(code,binary?QStringList{"format","kind","length","quad"}:QStringList{"format","kind","length","quad","text"})||!codeFormats().contains(code["format"].toString())
     ||(!binary&&(code["kind"]!=QLatin1String("text")||!code["text"].isString()||code["text"].toString().toUtf8().size()>maximumCodeBytes))||!integer(code["length"],0,65536,decoded.length)||quad.size()!=4)return false;
  for(const auto& corner:quad){qint64 x=0,y=0;const auto point=corner.toArray();if(point.size()!=2||!integer(point[0],0,input.width,x)||!integer(point[1],0,input.height,y))return false;decoded.quad.append({int(x),int(y)});}
  decoded.format=code["format"].toString();decoded.binary=binary;if(!binary)decoded.text=sanitizeCodeContent(code["text"].toString());
  result.codes.append(decoded);
 }
 result.truncated=object["truncated"].toBool();result.banded=object["banded"].toBool();return true;
}
QString sanitizeText(const QString& text){return sanitize(text,true);}
QString sanitizeCodeContent(const QString& text){return sanitize(text,false);}
QString copyText(const TextResult& result){
 const auto lines=result.text.isEmpty()?QStringList{}:result.text.split('\n');QList<QStringList> before(lines.size()+1);
 for(const auto& code:result.codes){
  if(code.binary||code.text.isEmpty())continue;
  qsizetype line=0;while(line<lines.size()&&result.lineTops.value(line)<=code.centre().y())++line;
  before[line].append(code.text);
 }
 QStringList out;for(qsizetype i=0;i<=lines.size();++i){out.append(before[i]);if(i<lines.size())out.append(lines[i]);}
 return out.join('\n');
}
QString notification(const TextResult& result,const QString& copyStatus,const QStringList& missing){
 const auto plural=[](qsizetype count,const QString& noun){return QStringLiteral("%1 %2%3").arg(count).arg(noun,count==1?QString():QStringLiteral("s"));};
 const auto characters=result.text.toUcs4().size();const int codes=result.textCodes();QString message;
 if(!characters&&!codes)message=QStringLiteral("No text found");
 else if(copyStatus=="suppressed")return QStringLiteral("Text not copied: the clipboard changed");
 else if(copyStatus!="copied")return QStringLiteral("Text could not be copied");
 else if(characters&&codes)message=QStringLiteral("Copied %1 and %2").arg(plural(characters,"character"),plural(codes,"code"));
 else message=QStringLiteral("Copied %1").arg(characters?plural(characters,"character"):plural(codes,"code"));
 if(!missing.isEmpty())message+=missing.size()==1?QStringLiteral(" (%1 data is missing)").arg(missing.first()):QStringLiteral(" (%1 and %2 data are missing)").arg(missing.mid(0,missing.size()-1).join(", "),missing.last());
 if(result.truncated)message+=QStringLiteral("\nThe text was cut at a size or time limit.");
 return message;
}
bool validLanguage(const QString& identifier){
 // \z, not $: a trailing newline is not part of an identifier.
 static const QRegularExpression pattern(QStringLiteral("\\A[a-z]{3,4}(_[a-z0-9]+){0,2}\\z"));
 return identifier.size()<=32&&identifier!="osd"&&identifier!="equ"&&pattern.match(identifier).hasMatch();
}
bool validLanguages(const QStringList& identifiers){
 if(identifiers.isEmpty()||identifiers.size()>maximumLanguages)return false;
 for(qsizetype i=0;i<identifiers.size();++i)if(!validLanguage(identifiers[i])||identifiers.indexOf(identifiers[i])!=i)return false;
 return true;
}
QList<DataDirectory> dataDirectories(const QString& home,const QString& applicationDirectory){
 QList<DataDirectory> directories;
 if(QDir::isAbsolutePath(home))directories.append({QDir::cleanPath(home+"/.local/share/putgd/tessdata"),"user"});
 // Beside the executables, where the AppImage carries its English data.
 if(QDir::isAbsolutePath(applicationDirectory))directories.append({QDir::cleanPath(applicationDirectory+"/../share/putgd/tessdata"),"packaged"});
 // Debian, Ubuntu and Mint; Fedora; Arch.
 for(const auto* path:{"/usr/share/tesseract-ocr/5/tessdata","/usr/share/tesseract/tessdata","/usr/share/tessdata"})directories.append({QString::fromLatin1(path),"system"});
 return directories;
}
QList<Language> installedLanguages(const QList<DataDirectory>& directories){
 QList<Language> languages;QStringList seen;
 for(const auto& directory:directories){
  const auto files=QDir(directory.path).entryInfoList({"*.traineddata"},QDir::Files|QDir::Readable,QDir::Name);
  for(const auto& file:files){
   const auto id=file.completeBaseName();
   if(languages.size()>=1024||!validLanguage(id)||seen.contains(id)||file.size()<1)continue;
   seen.append(id);languages.append({id,directory.kind,file.absoluteFilePath(),file.size()});
  }
 }
 std::sort(languages.begin(),languages.end(),[](const Language& a,const Language& b){return a.id<b.id;});
 return languages;
}
QString languageForLocale(const QString& locale){
 const auto name=locale.section('.',0,0).section('@',0,0);const auto language=name.section('_',0,0).toLower(),territory=name.section('_',1,1).toUpper();
 if(language=="zh")return territory=="TW"||territory=="HK"||territory=="MO"?QStringLiteral("chi_tra"):QStringLiteral("chi_sim");
 for(const auto& tongue:tongues)if(language==QLatin1String(tongue.locale))return QString::fromLatin1(tongue.language);
 return {};
}
QStringList defaultLanguages(const QStringList& preference,const QString& locale,const QList<Language>& installed){
 if(validLanguages(preference))return preference;
 QStringList languages;const auto local=languageForLocale(locale);
 if(!local.isEmpty()&&std::any_of(installed.begin(),installed.end(),[&](const Language& language){return language.id==local;}))languages.append(local);
 if(!languages.contains("eng"))languages.append("eng");
 return languages;
}
}
}
