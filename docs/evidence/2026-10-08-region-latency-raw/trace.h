#pragma once
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <time.h>
namespace putgd_capture_probe {
inline thread_local std::string operation;
inline std::atomic<unsigned long long> nextSurface{0},nextHotkey{0};
inline long long now(){timespec value{};clock_gettime(CLOCK_MONOTONIC,&value);return value.tv_sec*1000000000LL+value.tv_nsec;}
inline void setOperation(const char* id){operation=id;}
inline void trace(const char* phase,const char* id=nullptr,unsigned long long ordinal=0){
 const auto timestamp=now();static std::mutex mutex;std::lock_guard guard(mutex);
 std::fprintf(stderr,"PUTGD_CAPTURE_TRACE {\"phase\":\"%s\",\"tNs\":%lld,\"operationId\":\"%s\",\"ordinal\":%llu}\n",phase,timestamp,id?id:operation.c_str(),ordinal);
}
struct Span {
 const char* end;std::string id;unsigned long long ordinal;
 Span(const char* begin,const char* end,const char* id=nullptr,unsigned long long ordinal=0):end(end),id(id?id:operation),ordinal(ordinal){trace(begin,this->id.c_str(),ordinal);}
 ~Span(){trace(end,id.c_str(),ordinal);}
};
}
