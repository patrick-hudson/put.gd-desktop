// cli/Minisign.cpp: the release signature check behind an AppImage self-update.
// Two properties: it never crashes on any signature file, key or message, and it
// never accepts anything but what the signer signed. The key pair is made at
// start-up, so the fuzzer cannot forge a signature; any acceptance of other
// bytes is a bug.
#include "FuzzSupport.h"
#include "cli/Minisign.h"
#include "fixtures/MinisignFixture.h"
using namespace putgd;

namespace {
MinisignKey* key = nullptr;
QByteArray flip(QByteArray bytes, uint32_t position, uint8_t mask) {
    if (bytes.isEmpty()) return bytes;
    bytes[position % bytes.size()] = char(uint8_t(bytes[position % bytes.size()]) ^ (mask ? mask : 1));
    return bytes;
}
// Accepted input must be exactly what was signed.
void acceptedOnlyIfSigned(const QByteArray& message, const QByteArray& file, const QByteArray& publicKey,
                          const QByteArray& signedMessage, const QString& signedComment) {
    QString comment;
    if (!minisign::verify(message, file, publicKey, &comment)) {
        FUZZ_REQUIRE(message == signedMessage);
        FUZZ_REQUIRE(comment == signedComment);
    }
}
}

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    qInstallMessageHandler(fuzz::quiet);
    key = new MinisignKey;
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz::Input in(data, size);
    const auto mode = in.byte() % 4;
    const auto message = in.part();
    auto trusted = in.part();
    // minisign ends each line at \n and drops a trailing \r, so neither can be part of a genuine comment.
    trusted.replace('\n', ' ').replace('\r', ' ');
    const bool prehashed = in.byte() & 1;
    const auto comment = QString::fromUtf8(trusted);
    const auto publicFile = key->publicFile();
    const auto good = key->signature(message, trusted, prehashed);
    // The genuine pair verifies (this is the regression for the harness itself, too).
    if (mode == 0) {
        QString got;
        FUZZ_REQUIRE(!minisign::verify(message, good, publicFile, &got));
        FUZZ_REQUIRE(got == comment);
        return 0;
    }
    // Arbitrary bytes as the signature file and the key, beside a real pair.
    if (mode == 1) {
        const auto file = in.part(), text = in.rest();
        minisign::verify(message, file, publicFile);
        minisign::verify(message, good, text);
        minisign::verify(message, file, text);
        acceptedOnlyIfSigned(message, file, publicFile, message, comment);
        return 0;
    }
    // One byte of the decoded fields changed: always refused. Then one byte of the
    // text changed: refused, or accepted only for the same message and comment (the
    // untrusted comment and base64 padding bits are not signed).
    const auto at = in.number(0xffff);
    const auto mask = in.byte();
    if (mode == 2) {
        const auto lines = good.split('\n');
        const auto sig = QByteArray::fromBase64(lines[1]);
        const auto global = QByteArray::fromBase64(lines[3]);
        const auto pub = QByteArray::fromBase64(publicFile.split('\n')[1]);
        const auto rebuilt = [&](const QByteArray& s, const QByteArray& c, const QByteArray& g) {
            return lines[0] + "\n" + s.toBase64() + "\ntrusted comment: " + c + "\n" + g.toBase64() + "\n";
        };
        const auto pubFile = [&](const QByteArray& p) { return "untrusted comment: key\n" + p.toBase64() + "\n"; };
        switch (in.byte() % 5) {
        case 0: FUZZ_REQUIRE(minisign::verify(flip(message, at, mask), good, publicFile) || message.isEmpty()); break;
        case 1: FUZZ_REQUIRE(minisign::verify(message, rebuilt(flip(sig, at, mask), trusted, global), publicFile)); break;
        case 2: FUZZ_REQUIRE(minisign::verify(message, rebuilt(sig, trusted, flip(global, at, mask)), publicFile)); break;
        case 3: FUZZ_REQUIRE(trusted.isEmpty() || minisign::verify(message, rebuilt(sig, flip(trusted, at, mask), global), publicFile)); break;
        default: FUZZ_REQUIRE(minisign::verify(message, good, pubFile(flip(pub, at, mask)))); break;
        }
        return 0;
    }
    acceptedOnlyIfSigned(message, flip(good, at, mask), publicFile, message, comment);
    acceptedOnlyIfSigned(message, good, flip(publicFile, at, mask), message, comment);
    return 0;
}
