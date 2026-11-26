#pragma once
#include "core/Error.h"
#include <QByteArray>
#include <QString>
#include <functional>
#include <optional>
class QIODevice;
// Sealed links (#2079), format version 1. The container is a fixed 96-byte header and
// AES-256-GCM chunks in the STREAM construction: each chunk's nonce and additional data
// carry its index and whether it is the last one, so a reordered, truncated, extended or
// edited container fails to open instead of giving part of a file. OpenSSL does every
// primitive; nothing here is hand-written crypto. Only putgd-cli links this: the resident
// never maps libcrypto. The browser half is the pinned viewer page (SealPage.h), which
// does the same with WebCrypto.
//
//  header  "PGDSEAL1" | version u8 = 1 | flags u8 (bit 0: passphrase) | 0 u16
//          | chunk size u32 BE | PBKDF2 iterations u32 BE (0 without a passphrase)
//          | salt[32] | nonce prefix[7] | 0[5] | key check[32]
//  key check  HMAC-SHA-256(HKDF-SHA-256(K, salt, "put.gd sealed v1 check"), header[0..64))
//  ikm        K, or K || PBKDF2-HMAC-SHA-256(NFC(passphrase) as UTF-8, salt, iterations, 32)
//  file key   HKDF-SHA-256(ikm, salt, "put.gd sealed v1 payload", 32)
//  chunk i    AES-256-GCM(file key, nonce = prefix || u32 BE i || u8 last,
//                         aad = header || u32 BE i || u8 last) || tag[16]
//  plaintext  u32 BE meta length || meta JSON || the file's bytes; the meta JSON is compact
//             UTF-8 with exactly the keys name, size and type, in that order
// Every chunk but the last holds exactly chunk size plaintext bytes; the last holds 1 to
// chunk size. K is the link key: 32 random bytes, carried only in the link's fragment.
namespace putgd::seal {
inline constexpr int headerBytes=96;
inline constexpr int tagBytes=16;
inline constexpr int keyBytes=32;
inline constexpr qint64 chunkBytes=1048576;
inline constexpr int passphraseIterations=600000;
// The largest file sealed into one self-contained page, and the largest sealed at all.
inline constexpr qint64 inlineLimit=67108864LL;
inline constexpr qint64 sealLimit=2147483648LL;
inline constexpr int maximumMetaBytes=4096;
inline constexpr int maximumPassphraseBytes=1024;
// What the header records about the file, and the file's own description inside.
struct Meta {QString name,type;qint64 size=0;};
// Fixed salt and nonce prefix for test vectors only; empty fields are drawn at random.
struct Fixed {QByteArray salt,noncePrefix;};
struct Header {
 bool passphrase=false;quint32 chunkSize=0,iterations=0;
 QByteArray bytes,salt,noncePrefix;
};
// "v1." followed by the key in unpadded base64url (43 characters).
QString fragment(const QByteArray& key);
// The key a fragment (with or without its leading '#') carries; empty when it isn't one.
QByteArray keyFromFragment(QString fragment);
// 32 bytes from OpenSSL's generator; empty when it fails.
QByteArray randomKey();
// The container's length for a plaintext stream of this many bytes.
qint64 containerBytes(qint64 plaintextBytes,qint64 chunkSize=chunkBytes);
// Meta's encoding, the plaintext stream's first bytes.
QByteArray encodeMeta(const Meta&);
using Sink=std::function<bool(const char*,qint64)>;
// Seals `input` (exactly meta.size bytes are read) into sink. A passphrase, when not
// empty, is a second factor beside the key: one line of at most 1024 bytes once NFC
// normalized. Version 1 always writes 600,000 iterations; `iterations` is ignored.
Error seal(QIODevice& input,const Meta& meta,const QByteArray& key,const QString& passphrase,const Sink& sink,
           int iterations=passphraseIterations,const Fixed& fixed={});
// Reads and checks a header; an error says the link is damaged or not a sealed link.
Error parseHeader(const QByteArray& bytes,Header& header);
// Whether this key opens this header (the key check). Says nothing about a passphrase.
bool keyMatches(const Header&,const QByteArray& key);
// Opens a whole container read from `input`, `total` bytes long, writing the file to
// sink. Errors: seal_damaged (wrong key, or changed, cut or reordered bytes),
// seal_passphrase_required, seal_passphrase (a passphrase that doesn't open it).
// `meta` is filled once the first chunk opens. Nothing is written for a chunk that
// fails, but chunks before it were written: write to a temporary file and keep it
// only on success.
Error open(QIODevice& input,qint64 total,const QByteArray& key,const QString& passphrase,const Sink& sink,Meta* meta=nullptr);
}
