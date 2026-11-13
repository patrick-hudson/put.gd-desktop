#include "Recognize.h"
#include "Sanitize.h"
#include <cmath>

namespace putgd::ocr {
namespace {
void string(QByteArray& out,const QString& text){
 out+='"';
 for(const char c:text.toUtf8()){
  const auto u=uchar(c);
  if(c=='"')out+="\\\"";else if(c=='\\')out+="\\\\";else if(c=='\n')out+="\\n";else if(c=='\t')out+="\\t";
  else if(u<0x20){static const char hex[]="0123456789abcdef";out+="\\u00";out+=hex[u>>4];out+=hex[u&15];}
  else out+=c;
 }
 out+='"';
}
void rect(QByteArray& out,const QRect& r){
 out+="{\"x\":"+QByteArray::number(r.x())+",\"y\":"+QByteArray::number(r.y())+",\"width\":"+QByteArray::number(r.width())+",\"height\":"+QByteArray::number(r.height())+'}';
}
QByteArray word(const Word& w,bool& cut){
 QByteArray out("{\"text\":");string(out,clampUtf8(w.text,maximumWordBytes,cut));
 out+=",\"rect\":";rect(out,w.rect);
 out+=",\"confidence\":"+QByteArray::number(int(std::lround(w.confidence)))+'}';
 return out;
}
QByteArray code(const Code& c){
 QByteArray out("{\"format\":");string(out,c.format);
 out+=c.text?",\"kind\":\"text\",\"text\":":",\"kind\":\"binary\"";
 if(c.text)string(out,c.content);
 out+=",\"length\":"+QByteArray::number(c.length)+",\"quad\":[";
 for(int i=0;i<4;++i){if(i)out+=',';out+="{\"x\":"+QByteArray::number(c.quad[i].x())+",\"y\":"+QByteArray::number(c.quad[i].y())+'}';}
 out+="]}";
 return out;
}
void strings(QByteArray& out,const QStringList& list){out+='[';for(int i=0;i<list.size();++i){if(i)out+=',';string(out,list[i]);}out+=']';}
}
QByteArray encodeResult(const JobResult& result,const JobOptions& options,qint64 outputLimit){
 bool truncated=result.truncated;
 QList<QByteArray> words,candidates,codes;
 if(options.words)for(const auto& w:result.recognition.words){if(!w.kept)continue;if(words.size()>=maximumWords){truncated=true;break;}words.append(word(w,truncated));}
 if(options.candidates)for(const auto& w:result.recognition.words){if(candidates.size()>=maximumWords){truncated=true;break;}candidates.append(word(w,truncated));}
 for(const auto& c:result.recognition.codes)codes.append(code(c));
 QString text=result.text;
 const auto assemble=[&](qsizetype nWords,qsizetype nCandidates,qsizetype nCodes,const QString& t,bool cut){
  QByteArray out("{\"text\":");string(out,t);
  if(options.words){out+=",\"words\":[";for(qsizetype i=0;i<nWords;++i){if(i)out+=',';out+=words[i];}out+=']';}
  out+=",\"codes\":[";for(qsizetype i=0;i<nCodes;++i){if(i)out+=',';out+=codes[i];}out+=']';
  if(options.candidates){out+=",\"candidates\":[";for(qsizetype i=0;i<nCandidates;++i){if(i)out+=',';out+=candidates[i];}out+=']';}
  out+=",\"truncated\":";out+=cut?"true":"false";
  out+=",\"banded\":";out+=result.recognition.banded?"true":"false";
  out+=",\"languagesUsed\":";strings(out,result.used);
  out+=",\"languagesSkipped\":";strings(out,result.skipped);
  out+=",\"engine\":{\"scale\":"+QByteArray::number(result.recognition.scale,'f',3)+",\"bands\":"+QByteArray::number(result.recognition.bands)
     +",\"codesMs\":"+QByteArray::number(result.timings.codesMs)+",\"engineStartMs\":"+QByteArray::number(result.timings.engineStartMs)
     +",\"recognitionMs\":"+QByteArray::number(result.timings.recognitionMs)+",\"totalMs\":"+QByteArray::number(result.timings.totalMs)+"}}";
  return out;
 };
 qsizetype nWords=words.size(),nCandidates=candidates.size(),nCodes=codes.size();
 auto out=assemble(nWords,nCandidates,nCodes,text,truncated);
 if(out.size()<=outputLimit)return out;
 // Over the cap: candidates go first, then words, then codes, then text.
 const auto drop=[&](QList<QByteArray>& items,qsizetype& count,qint64 excess){
  while(count>0&&excess>0){excess-=items[count-1].size()+1;--count;}
  return excess;
 };
 qint64 excess=out.size()-outputLimit;
 excess=drop(candidates,nCandidates,excess);
 if(excess>0)excess=drop(words,nWords,excess);
 if(excess>0)excess=drop(codes,nCodes,excess);
 out=assemble(nWords,nCandidates,nCodes,text,true);
 while(out.size()>outputLimit&&!text.isEmpty()){
  bool cut=false;const auto over=out.size()-outputLimit;
  text=clampUtf8(text,std::max<qsizetype>(0,text.toUtf8().size()-over),cut);
  out=assemble(nWords,nCandidates,nCodes,text,true);
 }
 return out;
}
}
