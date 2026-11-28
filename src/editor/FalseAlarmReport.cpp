#include "FalseAlarmReport.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <numeric>

namespace putgd::editor::falseAlarm {
namespace {
QJsonArray box(const QRect& rect){return QJsonArray{rect.x(),rect.y(),rect.width(),rect.height()};}
QJsonObject report(const Input& input,const QList<int>& kept){
 const auto& run=input.run;
 // Old word index -> its index in the list written; -1 when it was left out.
 QList<int> place(run.candidates.size(),-1);for(int i=0;i<kept.size();++i)place[kept.at(i)]=i;
 QJsonArray words;
 for(const int i:kept){
  const auto& word=run.candidates.at(i);
  QJsonObject entry{{"text",word.text},{"box",box(word.rect)},{"confidence",word.confidence},{"line",word.line}};
  if(word.second)entry["second"]=true;
  words.append(entry);
 }
 QJsonArray findings;
 for(qsizetype m=0;m<run.matches.size();++m){
  const auto& match=run.matches.at(m);
  QStringList text;QJsonArray indexes;QRect bounds;QSet<int> lineSet;int lowest=100,sum=0,count=0;
  for(const int w:match.words){
   if(w<0||w>=run.candidates.size())continue;
   const auto& word=run.candidates.at(w);
   text.append(word.text);indexes.append(place.at(w));bounds=bounds.united(word.rect);lineSet.insert(word.line);
   lowest=std::min(lowest,word.confidence);sum+=word.confidence;++count;
  }
  QList<int> lines(lineSet.cbegin(),lineSet.cend());std::sort(lines.begin(),lines.end());
  QJsonArray lineList;for(const int line:lines)lineList.append(line);
  QJsonArray marks;for(const auto& rect:run.marks.value(m))marks.append(box(rect));
  findings.append(QJsonObject{{"kind",match.category},{"automatic",input.cover.contains(match.category)},{"rule",match.rule.isEmpty()?match.category:match.rule},
   {"text",text.join(' ')},{"words",indexes},{"lines",lineList},{"box",box(bounds)},{"marks",marks},{"covered",!marks.isEmpty()},
   {"confidence",QJsonObject{{"lowest",count?lowest:0},{"mean",count?double(sum)/count:0.0}}}});
 }
 QJsonArray codes;for(const auto& [kind,rect]:run.codes)codes.append(QJsonObject{{"kind",kind},{"box",box(rect)}});
 auto check=input.findings;
 check["categories"]=QJsonArray::fromStringList(input.categories);check["automatic"]=QJsonArray::fromStringList(input.cover);
 if(!input.covered.isEmpty())check["covered"]=input.covered;
 if(!input.codeKinds.isEmpty())check["codes"]=input.codeKinds;
 return {{"reportedFrom",input.reportedFrom},{"image",QJsonObject{{"width",input.image.width()},{"height",input.image.height()}}},
  {"check",check},{"recognition",run.recognition},{"findings",findings},
  {"words",QJsonObject{{"total",double(run.candidates.size())},{"listed",double(kept.size())},{"omitted",double(run.candidates.size()-kept.size())},{"list",words}}},
  {"codes",codes}};
}
}
QJsonObject build(const Input& input){
 const auto& run=input.run;
 QList<int> every(run.candidates.size());std::iota(every.begin(),every.end(),0);
 if(run.candidates.size()<=maximumWords){
  auto whole=report(input,every);
  if(QJsonDocument(whole).toJson(QJsonDocument::Compact).size()<=maximumBytes)return whole;
 }
 // Too many words for one report: the lines around each match, which is what a false alarm is read from.
 QSet<int> near;
 for(const auto& match:run.matches)for(const int w:match.words)if(w>=0&&w<run.candidates.size())for(int line=run.candidates.at(w).line-2;line<=run.candidates.at(w).line+2;++line)near.insert(line);
 QList<int> kept;
 for(int i=0;i<run.candidates.size()&&kept.size()<maximumWords;++i)if(near.contains(run.candidates.at(i).line))kept.append(i);
 auto trimmed=report(input,kept);
 // Still over (a page of one enormous line): only the matched words themselves.
 if(QJsonDocument(trimmed).toJson(QJsonDocument::Compact).size()>maximumBytes){
  QSet<int> matched;for(const auto& match:run.matches)for(const int w:match.words)if(w>=0&&w<run.candidates.size())matched.insert(w);
  kept=QList<int>(matched.cbegin(),matched.cend());std::sort(kept.begin(),kept.end());
  if(kept.size()>maximumWords)kept.resize(maximumWords);
  trimmed=report(input,kept);
  // Still over: fewer listed words, halving, until it fits or none are left.
  while(!kept.isEmpty()&&QJsonDocument(trimmed).toJson(QJsonDocument::Compact).size()>maximumBytes){kept.resize(kept.size()/2);trimmed=report(input,kept);}
 }
 return trimmed;
}
}
