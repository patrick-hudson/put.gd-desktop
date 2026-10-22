#pragma once
// The two pieces of ISO BMFF (MP4) structure the recording export needs to put
// the index (moov) before the media data, so a shared link plays before the whole
// file has arrived: the top-level box list and the chunk offsets inside moov.
// Media Foundation writes ftyp, uuid, mdat, moov; moving moov ahead of mdat moves
// every chunk by moov's size. No Windows headers: tested on Linux.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace putgd::mp4 {
struct Box { std::string type; uint64_t offset = 0, size = 0; };
// Top-level boxes of a file of `fileSize` bytes; `read(offset, n)` returns up to n
// bytes there. Empty when a header is truncated or a size is impossible.
std::vector<Box> topLevel(uint64_t fileSize, const std::function<std::string(uint64_t, size_t)>& read);
// Adds `shift` to every stco and co64 entry under moov/trak/mdia/minf/stbl.
// False, with `moov` unchanged, when the box is malformed or a 32-bit offset
// would overflow (the caller then keeps the file as written).
bool shiftChunkOffsets(std::string& moov, uint64_t shift);
}
