#pragma once
#include "OcrImage.h"
#include <QByteArray>
#include <QString>
#include <cstddef>

// The two memfds the parent hands putgd-ocr at fixed descriptor numbers (#1428
// decision 2): the sealed grey input and the output it writes its JSON into.
namespace putgd::ocr {
inline constexpr int inputDescriptor=3,outputDescriptor=4;
// The input's layout: "PGRY", then width, height and stride as little-endian
// 32-bit numbers, then height rows of stride bytes of 8-bit grey.
inline constexpr size_t inputHeaderBytes=16;
class InputImage final {
public:
 InputImage()=default;
 ~InputImage();
 InputImage(const InputImage&)=delete;
 InputImage& operator=(const InputImage&)=delete;
 // Maps a memfd sealed against write, shrink and grow whose size is exactly
 // the header and its rows. Returns the error code: input_excessive or
 // text_job_io; empty on success.
 QString map(int fd,qint64 maximumPixels);
 GreyView view()const{return view_;}
private:
 void* base_=nullptr;size_t size_=0;GreyView view_;
};
// The output memfd: writable, sealable and empty.
bool outputReady(int fd);
// Writes all of bytes through the inherited descriptor; never by path.
bool writeOutput(int fd,const QByteArray& bytes);
}
