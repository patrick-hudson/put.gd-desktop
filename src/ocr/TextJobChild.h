#pragma once
#include "core/TextJobProtocol.h"
#include <QStringList>
#include <functional>

// The child half of the text-job contract (#1428): the control handshake, the
// sealed input memfd and the output memfd, for one job. The putgd-ocr helper
// calls runChild after installing its confinement; the test fixture helper
// (tests/fixtures/text_job_fixture.cpp) calls it with a canned engine. Qt Core
// only. Everything about recognition itself belongs to the engine.
namespace putgd::textjob {
// The mapped input, read-only (the memfd is sealed), valid until the engine
// returns: `stride` bytes a row of 8-bit grey and `colourStride` bytes a row of
// 8-bit RGB. releaseColour(top, bottom) lets the mapped colour rows go once read
// (the pages leave the resident set; reading them again maps them again).
struct InputImage {
 const unsigned char* data=nullptr;
 int width=0,height=0,stride=0;
 const unsigned char* colour=nullptr;
 int colourStride=0;
 std::function<void(int,int)> releaseColour;
};
// A child error of childErrorCodes(); language is named only for language_unavailable.
struct ChildFailure {
 QString code,language;
 explicit operator bool() const {return !code.isEmpty();}
};
// Recognizes one image. dataDirectories is the parent's data path in lookup
// order. Returns no failure and fills the result, or one child error.
using Engine=std::function<ChildFailure(const InputImage&,const Options&,const QStringList& dataDirectories,TextResult&)>;
// Runs one job over standard input and output and the two fixed descriptors,
// then returns the process's exit status: 0 only after the parent accepted the
// checkpoint. startupFailure is a child error found before any input was read
// (helper_sandbox_unavailable): the job then answers it and runs no engine.
int runChild(int& argc,char** argv,const Engine& engine,const QString& startupFailure={});
}
