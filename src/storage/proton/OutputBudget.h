#pragma once
#include <QtGlobal>
#include <algorithm>
namespace putgd {
// Bound bursts and buffered frames, while allowing sustained coalesced progress.
class ProtonOutputBudget {
public:
 static constexpr qsizetype MaximumBurstBytes=2*1024*1024;
 bool consume(qsizetype bytes,qint64 elapsedMs){
  if(bytes<0||bytes>MaximumBurstBytes||elapsedMs<lastMs_)return false;
  // Saturate elapsed credit before multiplying, including after long idle gaps.
  const auto credit=std::min(elapsedMs-lastMs_,FullRefillMs)*RefillBytesPerSecond+fraction_;
  remaining_=std::min<qint64>(MaximumBurstBytes,remaining_+credit/1000);
  fraction_=credit%1000;lastMs_=elapsedMs;
  if(bytes>remaining_)return false;
  remaining_-=bytes;return true;
 }
private:
 static constexpr qint64 RefillBytesPerSecond=1024;
 static constexpr qint64 FullRefillMs=MaximumBurstBytes*1000/RefillBytesPerSecond;
 qint64 remaining_=MaximumBurstBytes,lastMs_=0,fraction_=0;
};
}
