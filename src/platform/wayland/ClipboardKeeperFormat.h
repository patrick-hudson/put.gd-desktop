// What the resident hands putgd-clipboard-keeper on standard input: an anonymous
// memory file, read by both C++ and C. All integers are native-endian.
//
//   magic[8], uint32 count,
//   count times: uint32 nameBytes (with its NUL), uint32 zero, uint64 dataBytes,
//                name, data
#pragma once
#define PUTGD_KEEPER_MAGIC "PUTGDCK1"
#define PUTGD_KEEPER_MAGIC_BYTES 8u
// Larger results are not kept: quitting then loses them, as it did before.
#define PUTGD_KEEPER_MAX_BYTES (64u*1024u*1024u)
#define PUTGD_KEEPER_MAX_TYPES 16u
// Every result carries one empty type with this prefix. It marks the selection
// as this exact copy, so the keeper never replaces anybody else's.
#define PUTGD_KEEPER_TOKEN_PREFIX "application/x-putgd-result-"
