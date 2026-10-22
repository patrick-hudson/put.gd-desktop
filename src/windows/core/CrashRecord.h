#pragma once
// The Windows crash record (docs/diagnostics.md, "Crash records"): the text the
// unhandled-exception filter in common/Crash.cpp writes, one tab-separated line
// per fact, and its JSON form for the log, diagnostics and packages.
//
//   putgd-crash  begin      1  <process>  <pid>  <thread>
//   putgd-crash  version    <build version>
//   putgd-crash  exception  0xC0000005  EXCEPTION_ACCESS_VIOLATION  <fault address>  read|write|execute|-
//   putgd-crash  time       <unix seconds>
//   putgd-crash  pc         0x...
//   putgd-crash  frame      <index>  0x<address>  <module index or ->  0x<offset in module>
//   putgd-crash  module     <index>  0x<load address>  <TimeDateStamp><SizeOfImage>  <file>
//   putgd-crash  end        <frame count>
//
// The module's image ID (PE TimeDateStamp as 8 hex digits, then SizeOfImage in
// hex) is the key a symbol server files the image under.
#include "common/Json.h"
#include <string_view>

namespace putgd::crash {
bool isRecordLine(std::string_view);
// {recordVersion, process, pid, thread, version, exceptionCode, exceptionName,
// faultAddress, access, pc, time, frames[], modules[], complete}; empty when
// the text holds no begin line.
json::Object parseRecord(std::string_view);
// EXCEPTION_ACCESS_VIOLATION and the other common codes; empty when unknown.
const char* exceptionName(unsigned long code);
}
