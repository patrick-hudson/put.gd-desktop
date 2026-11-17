// The text-recognition job contract (core/TextJobProtocol.cpp): the control frames
// and the output file the temporary putgd-ocr helper hands the resident, and the
// sanitizer every recognized string passes through before it is copied or shown.
// The helper is a separate, untrusted process, so each decoder is fed raw JSON.
#include "FuzzSupport.h"
#include "core/TextJobProtocol.h"
#include <QJsonDocument>
using namespace putgd;
namespace tj = putgd::textjob;

namespace {
bool forbidden(const QString& text, bool joinersAllowed) {
    for (qsizetype i = 0; i < text.size(); ++i) {
        char32_t c = text.at(i).unicode();
        if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) { c = QChar::surrogateToUcs4(text.at(i), text.at(i + 1)); ++i; }
        else if (text.at(i).isSurrogate()) return true;
        if (c == '\t' || c == '\n') continue;
        if (c < 0x20 || (c >= 0x7f && c <= 0x9f) || c == 0x2028 || c == 0x2029) return true;
        if (QChar::category(c) == QChar::Other_Format && !(joinersAllowed && (c == 0x200c || c == 0x200d))) return true;
    }
    return false;
}
}

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    qInstallMessageHandler(fuzz::quiet);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz::Input in(data, size);
    const auto choice = in.byte() % 9;
    if (choice == 0) {
        // Text in, sanitized text out.
        const auto text = QString::fromUtf8(in.rest());
        const auto clean = tj::sanitizeText(text), code = tj::sanitizeCodeContent(text);
        FUZZ_REQUIRE(!forbidden(clean, true));
        FUZZ_REQUIRE(!forbidden(code, false));
        tj::validLanguage(text);
        tj::languageForLocale(text);
        tj::validLanguages(text.split(','));
        tj::defaultLanguages(text.split(','), text, {{"eng", "system", "", 1}, {"deu", "user", "", 1}});
        return 0;
    }
    if (choice == 1) {
        // The 32-byte input header and the size it promises.
        tj::InputHeader header;
        const auto bytes = in.part();
        const qint64 total = qint64(in.number(0xffff)) * in.number(0xffff);
        if (tj::decodeInputHeader(bytes, total, header)) {
            FUZZ_REQUIRE(tj::admitted(header.width, header.height, header.stride));
            FUZZ_REQUIRE(tj::encodeInputHeader(header) == bytes.left(tj::inputHeaderBytes));
            FUZZ_REQUIRE(tj::inputBytes(header) + tj::inputHeaderBytes == total);
        }
        tj::admitted(int(in.number(0xffff)) - 100, int(in.number(0xffff)), int(in.number(0xffff)));
        return 0;
    }
    const auto document = QJsonDocument::fromJson(in.rest());
    if (!document.isObject()) return 0;
    const auto object = document.object();
    const auto nonce = object["nonce"].toString();
    const auto build = object["build"].toObject();
    switch (choice) {
    case 2: { QString n; tj::InputHeader h; tj::Options o; tj::decodeBootstrap(object, build, n, h, o); break; }
    case 3: tj::validEnvelope(object, nonce); tj::validReady(object, nonce, build); break;
    case 4: { Error e; qint64 bytes = 0; QString sha; tj::decodeCheckpoint(object, nonce, e, bytes, sha); break; }
    default: {
        // The output file, as the helper wrote it. What decodes is bounded and clean, and
        // survives the encoder's cap.
        const auto header = tj::InputHeader{int(object["w"].toInt(200)), int(object["h"].toInt(100)), int(object["s"].toInt(200))};
        tj::Options options;
        options.words = choice & 1;
        options.candidates = choice & 2;
        TextResult result;
        const auto json = QJsonDocument(object.contains("result") ? object["result"].toObject() : object).toJson(QJsonDocument::Compact);
        if (tj::decodeResult(json, header, options, result)) {
            FUZZ_REQUIRE(!forbidden(result.text, true));
            for (const auto& w : result.words) FUZZ_REQUIRE(!forbidden(w.text, true) && w.rect.right() < header.width && w.rect.bottom() < header.height);
            FUZZ_REQUIRE(result.words.size() <= tj::maximumWords && result.codes.size() <= tj::maximumCodes);
            const auto text = tj::copyText(result);
            FUZZ_REQUIRE(!forbidden(text, true));
            tj::notification(result, "copied");
            tj::wordsJson(result.words);
            tj::codesJson(result.codes);
            const auto encoded = tj::encodeResult(result, options);
            FUZZ_REQUIRE(encoded.size() <= tj::maximumOutputBytes);
        }
    }
    }
    return 0;
}
