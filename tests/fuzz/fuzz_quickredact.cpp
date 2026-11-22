// editor/QuickRedact.cpp: Quick redact's matching over recognized words (#1897), which reads screen
// text from any untrusted page. Properties: it never crashes or runs away on any words, every match
// names words that exist, in order, once, and no word belongs to two matches; the marks of a match
// stay inside the bounds they are clamped to.
#include "FuzzSupport.h"
#include "editor/QuickRedact.h"
#include <QSet>
using namespace putgd::editor;

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    qInstallMessageHandler(fuzz::quiet);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz::Input in(data, size);
    // Words: the text split at spaces and line breaks, boxes laid out as a page reads, with the
    // fuzzer choosing each word's width per character and the gap after it.
    const auto text = QString::fromUtf8(in.rest());
    QList<RecognizedWord> words;
    int line = 0, x = 0, seed = 0;
    for (const auto& row : text.split(QLatin1Char('\n'))) {
        for (const auto& word : row.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            seed = (seed * 1103515245 + 12345) & 0x7fffffff;
            const int glyph = 4 + seed % 9, gap = seed % 3 == 0 ? 0 : glyph;
            words.append({word, QRect(x, line * 24 + seed % 3, int(word.size()) * glyph, 16), 90, line});
            x += int(word.size()) * glyph + gap;
            if (words.size() >= 4000) break;
        }
        ++line;x = 0;
        if (words.size() >= 4000) break;
    }
    const auto matches = quickRedact::find(words, quickRedact::categories());
    QSet<int> seen;
    const QRect bounds(0, 0, 4096, 4096);
    for (const auto& match : matches) {
        FUZZ_REQUIRE(!match.words.isEmpty());
        for (int i = 0; i < match.words.size(); ++i) {
            const int w = match.words.at(i);
            FUZZ_REQUIRE(w >= 0 && w < words.size());
            FUZZ_REQUIRE(i == 0 || match.words.at(i - 1) < w);
            FUZZ_REQUIRE(!seen.contains(w));
            seen.insert(w);
        }
        for (const auto& mark : quickRedact::marks(words, match, bounds)) FUZZ_REQUIRE(bounds.contains(mark));
    }
    return 0;
}
