// Portable contract checks for the Windows image editor (docs/image-editor.md):
// geometry, bounded history, the agent command contract, the sanitized raster
// pipeline (crop, redaction, blur, hidden colour) and the PNG chunk check.
// GDI+ annotations and WIC encoding are Windows-only; a stored-deflate PNG
// writer stands in for the encoder so byte identity is checked end to end.
#include "common/Json.h"
#include "core/SanitizedPng.h"
#include "editor/EditDocument.h"
#include "editor/EditorCommands.h"
#include "editor/Export.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

using namespace putgd;
using namespace putgd::editor;
static int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #__VA_ARGS__); ++failures; } } while (0)

namespace {
// Deterministic stand-in for GDI+ text metrics: 0.6 em per UTF-16 unit, 1.2 em line.
const TextMeasurer measure = [](const std::string& text, const std::string&, int pixels) {
    return TextSize{int(utf16Length(text) * size_t(pixels) * 6 / 10), pixels * 12 / 10};
};
Raster noise(int w, int h, unsigned seed, bool alpha = false) {
    Raster r(w, h, alpha);
    std::mt19937 random(seed);
    for (auto& p : r.pixels) { p = random(); if (!alpha) p |= 0xFF000000u; }
    return r;
}
json::Array parseArray(const char* text) { return json::parse(text).toArray(); }
bool run(EditDocument& doc, const char* text, std::string* code = nullptr, json::Array* created = nullptr) {
    json::Array ids;
    std::string c, m;
    const bool ok = commands::apply(doc, parseArray(text), measure, ids, c, m);
    if (code) *code = c;
    if (created) *created = ids;
    return ok;
}
std::string codeOf(EditDocument& doc, const char* text) { std::string code; run(doc, text, &code); return code; }

// Minimal PNG writer (stored deflate blocks, filter 0) for the portable tests.
void be32(std::string& out, uint32_t v) { for (int s = 24; s >= 0; s -= 8) out.push_back(char(v >> s & 255)); }
void chunk(std::string& out, const char* type, const std::string& data) {
    be32(out, uint32_t(data.size()));
    const std::string body = std::string(type, 4) + data;
    out += body;
    be32(out, uint32_t(png::crc32(body)));
}
std::string storedPng(const Raster& r) {
    std::string raw;
    for (int y = 0; y < r.height; ++y) {
        raw.push_back(0);
        for (int x = 0; x < r.width; ++x) {
            const auto p = r.row(y)[x];
            raw.push_back(char(redOf(p))); raw.push_back(char(greenOf(p))); raw.push_back(char(blueOf(p))); raw.push_back(char(alphaOf(p)));
        }
    }
    std::string z = "\x78\x01";
    size_t at = 0;
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(char(n & 255)); z.push_back(char(n >> 8));
        z.push_back(char(~n & 255)); z.push_back(char((~n >> 8) & 255));
        z.append(raw, at, n);
        at += n;
    } while (at < raw.size());
    uint32_t a = 1, b = 0;
    for (const unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    be32(z, b << 16 | a);
    std::string out(png::signature);
    std::string ihdr;
    be32(ihdr, uint32_t(r.width)); be32(ihdr, uint32_t(r.height));
    ihdr += std::string("\x08\x06\x00\x00\x00", 5);
    chunk(out, "IHDR", ihdr);
    chunk(out, "tEXt", std::string("Comment\0secret", 14)); // reader metadata the export must drop
    chunk(out, "IDAT", z);
    chunk(out, "tIME", std::string(7, '\x01'));
    chunk(out, "IEND", "");
    return out;
}
// The portable part of the export path: steps 1-3, step 5, encode, sanitize.
std::string exportPortable(const EditDocument& doc, size_t limit = SIZE_MAX) {
    auto raster = doc.renderBase();
    EditDocument::clearHiddenColor(raster);
    std::string out;
    if (exporter::finishPng(storedPng(raster), limit, raster.width, raster.height, out)) return {};
    return out;
}
}

static void geometryTests() {
    CHECK((Rect{0, 0, 10, 10}.intersected({5, 5, 10, 10}) == Rect{5, 5, 5, 5}));
    CHECK(Rect{0, 0, 10, 10}.intersected({10, 0, 5, 5}).empty());
    CHECK((Rect::spanning({5, 7}, {2, 3}) == Rect{2, 3, 4, 5}));
    CHECK((EditDocument::clampRect({-5, -5, 20, 8}, {0, 0, 10, 10}) == Rect{0, 0, 10, 3}));
    CHECK((Shape{Shape::Kind::Rectangle, 0, {10, 10, 5, 5}}.bounds() == Rect{6, 6, 13, 13}));
    CHECK(colorName(0xDC2626) == "#dc2626");
    Rgb c = 0; CHECK(parseColor("#A1b2C3", c) && c == 0xa1b2c3); CHECK(!parseColor("#12345", c)); CHECK(!parseColor("123456", c));
    CHECK(utf16Length("a\xc3\xa9\xf0\x9f\x98\x80") == 4);
    CHECK(truncateUtf16("ab\xf0\x9f\x98\x80", 3) == "ab");
    CHECK((exporter::fitWithin(4000, 1000, 1024).width == 1024 && exporter::fitWithin(4000, 1000, 1024).height == 256));
    CHECK((exporter::fitWithin(500, 300, 1024).width == 500));
}

static void historyTests() {
    EditDocument doc(noise(64, 48, 1));
    CHECK(!doc.modified() && !doc.canUndo());
    Shape s; s.kind = Shape::Kind::Rectangle; s.rect = {1, 2, 3, 4};
    const auto id = doc.add(s);
    CHECK(id == 1 && doc.canUndo() && doc.modified());
    auto moved = *doc.find(id); moved.rect.x = 9;
    CHECK(doc.replace(moved)); CHECK(!doc.replace(moved)); // unchanged
    CHECK(doc.undo() && doc.find(id)->rect.x == 1 && doc.canRedo());
    CHECK(doc.redo() && doc.find(id)->rect.x == 9);
    // A gesture previews live and commits one step.
    doc.beginGesture();
    for (int i = 0; i < 5; ++i) { auto p = *doc.find(id); p.rect.y = 10 + i; doc.preview(p); }
    doc.commitGesture();
    CHECK(doc.find(id)->rect.y == 14 && doc.undo() && doc.find(id)->rect.y == 2);
    doc.beginGesture(); doc.add(s); doc.cancelGesture();
    CHECK(doc.shapes().size() == 1);
    CHECK(doc.setCrop({10, 10, 20, 20}) && doc.crop() == (Rect{10, 10, 20, 20}));
    CHECK(!doc.setCrop({100, 100, 5, 5}));
    CHECK(doc.remove(id) && !doc.remove(id));
    // 200 steps at most; the oldest is evicted first.
    EditDocument many(noise(32, 32, 2));
    for (int i = 0; i < 250; ++i) { Shape r; r.kind = Shape::Kind::Redact; r.rect = {i % 30, 0, 1, 1}; many.add(r); }
    int steps = 0; while (many.undo()) ++steps;
    CHECK(steps == EditDocument::maximumHistory && many.historyTruncated() && many.shapes().size() == 50);
    // 4 MiB of geometry at most.
    EditDocument heavy(noise(32, 32, 3));
    Shape t; t.kind = Shape::Kind::Text; t.rect = {0, 0, 5, 5}; t.text = std::string(500, 'x'); t.fontFamily = std::string(100, 'f');
    for (int i = 0; i < 150; ++i) heavy.add(t);
    CHECK(heavy.historyBytes() <= EditDocument::maximumHistoryBytes && heavy.historyTruncated());
    // Clamps on add: text length and blur strength.
    EditDocument clamp(noise(16, 16, 4));
    Shape b; b.kind = Shape::Kind::Blur; b.rect = {0, 0, 4, 4}; b.strength = 500;
    CHECK(clamp.find(clamp.add(b))->strength == EditDocument::maximumBlur);
}

static void commandTests() {
    EditDocument doc(noise(200, 100, 5));
    json::Array created;
    CHECK(run(doc, R"([{"op":"add","shape":{"kind":"redact","rect":[-10,-10,30,30]}},
                      {"op":"add","shape":{"kind":"blur","rect":[50,10,40,40],"strength":8}},
                      {"op":"add","shape":{"kind":"arrow","from":[1,1],"to":[50,60],"color":"#2563EB","width":6}},
                      {"op":"add","shape":{"kind":"rectangle","rect":[5,5,20,20]}},
                      {"op":"add","shape":{"kind":"highlight","rect":[100,10,30,10]}},
                      {"op":"add","shape":{"kind":"text","at":[10,70],"text":"Hello","fontPixels":20,"font":"Segoe UI"}}])", nullptr, &created));
    CHECK(created.size() == 6 && created[0].toInteger() == 1 && created[5].toInteger() == 6);
    CHECK((doc.find(1)->rect == Rect{0, 0, 20, 20}));          // redaction clamped to the image
    CHECK(doc.find(1)->color == 0x000000);
    CHECK(doc.find(5)->color == defaultHighlightColor);
    CHECK((doc.find(6)->rect == Rect{10, 70, 61, 24}));        // box follows the text: 5*12+1 x 24
    const auto state = commands::state(doc);
    CHECK(state.value("width").toInt() == 200 && state.value("height").toInt() == 100 && state.value("canUndo").toBool() && !state.value("canRedo").toBool());
    const auto& shapes = state.value("shapes").arrayRef();
    CHECK(shapes.size() == 6);
    CHECK(shapes[2]["color"] == "#2563eb" && shapes[2]["width"].toInt() == 6 && shapes[2]["from"].arrayRef().size() == 2);
    CHECK(shapes[1]["strength"].toInt() == 8 && !shapes[1].objectRef().contains("color"));
    CHECK(shapes[5]["text"] == "Hello" && shapes[5]["font"] == "Segoe UI" && shapes[5]["fontPixels"].toInt() == 20 && shapes[5]["at"][0].toInt() == 10);
    CHECK(json::serialize(state.value("crop")) == "[0,0,200,100]");

    // Every validation failure is edit_command_invalid and leaves the document unchanged.
    const char* invalid[] = {
        R"([])",
        R"([{"op":"add","shape":{"kind":"circle","rect":[0,0,5,5]}}])",
        R"([{"op":"add","shape":{"kind":"redact","rect":[0,0,5,5],"color":"#ff0000"}}])",
        R"([{"op":"add","shape":{"kind":"blur","rect":[0,0,5,5],"strength":1}}])",
        R"([{"op":"add","shape":{"kind":"blur","rect":[0,0,5,5],"strength":65}}])",
        R"([{"op":"add","shape":{"kind":"rectangle","rect":[0,0,5,5],"width":0}}])",
        R"([{"op":"add","shape":{"kind":"rectangle","rect":[0,0,5,5],"width":2.5}}])",
        R"([{"op":"add","shape":{"kind":"rectangle","rect":[0,0,0,5]}}])",
        R"([{"op":"add","shape":{"kind":"rectangle","rect":[0,0,5]}}])",
        R"([{"op":"add","shape":{"kind":"rectangle","rect":[0,0,5,5],"color":"red"}}])",
        R"([{"op":"add","shape":{"kind":"rectangle","rect":[300,300,5,5]}}])",
        R"([{"op":"add","shape":{"kind":"rectangle","rect":[0,0,5,5],"extra":1}}])",
        R"([{"op":"add","shape":{"kind":"rectangle","from":[0,0],"to":[5,5]}}])",
        R"([{"op":"add","shape":{"kind":"arrow","from":[3,3],"to":[3,3]}}])",
        R"([{"op":"add","shape":{"kind":"arrow","from":[3,3]}}])",
        R"([{"op":"add","shape":{"kind":"text","at":[1,1],"text":"   "}}])",
        R"([{"op":"add","shape":{"kind":"text","at":[1,1],"text":"a\nb"}}])",
        R"([{"op":"add","shape":{"kind":"text","at":[1,1],"text":"a","fontPixels":7}}])",
        R"([{"op":"add","shape":{"kind":"text","at":[1,1],"text":"a","fontPixels":201}}])",
        R"([{"op":"add","shape":{"kind":"text","at":[1,1],"text":"a","font":"bad\u0007font"}}])",
        R"([{"op":"add","shape":{"kind":"text","at":[1,1]}}])",
        R"([{"op":"add","shape":{"kind":"rectangle","rect":[0,0,5,5],"font":"Arial"}}])",
        R"([{"op":"add"}])",
        R"([{"op":"add","shape":{"kind":"rectangle","rect":[0,0,5,5]},"x":1}])",
        R"([{"op":"crop","rect":[500,500,10,10]}])",
        R"([{"op":"resetCrop","x":1}])",
        R"([{"op":"spin"}])",
        R"([3])",
        R"([{"op":"update","shapeId":1,"shape":{"kind":"blur"}}])",
        R"([{"op":"add","shape":{"kind":"rectangle","rect":[1,1,5,5]}},{"op":"add","shape":{"kind":"bogus"}}])",
    };
    const auto before = json::serialize(commands::state(doc));
    for (const auto* text : invalid) {
        const auto code = codeOf(doc, text);
        if (code != "edit_command_invalid") std::fprintf(stderr, "  unexpected code '%s' for %s\n", code.c_str(), text);
        CHECK(code == "edit_command_invalid");
    }
    {
        std::string many = "[";
        for (int i = 0; i < 101; ++i) many += std::string(i ? "," : "") + R"({"op":"resetCrop"})";
        many += "]";
        CHECK(codeOf(doc, many.c_str()) == "edit_command_invalid");
        std::string text500 = R"([{"op":"add","shape":{"kind":"text","at":[1,1],"text":")" + std::string(501, 'a') + R"("}}])";
        CHECK(codeOf(doc, text500.c_str()) == "edit_command_invalid");
        std::string font101 = R"([{"op":"add","shape":{"kind":"text","at":[1,1],"text":"a","font":")" + std::string(101, 'f') + R"("}}])";
        CHECK(codeOf(doc, font101.c_str()) == "edit_command_invalid");
    }
    CHECK(json::serialize(commands::state(doc)) == before); // atomic: all or nothing
    CHECK(codeOf(doc, R"([{"op":"update","shapeId":99,"shape":{"rect":[0,0,5,5]}}])") == "edit_shape_missing");
    CHECK(codeOf(doc, R"([{"op":"remove","shapeId":99}])") == "edit_shape_missing");
    CHECK(codeOf(doc, R"([{"op":"redo"}])") == "edit_history_empty");

    // update keeps unspecified fields; text re-measures; one undo step per command.
    CHECK(run(doc, R"([{"op":"update","shapeId":6,"shape":{"text":"Hi","color":"#16a34a"}}])"));
    CHECK(doc.find(6)->text == "Hi" && doc.find(6)->color == 0x16a34a && doc.find(6)->fontPixels == 20 && (doc.find(6)->rect == Rect{10, 70, 25, 24}));
    CHECK(run(doc, R"([{"op":"crop","rect":[10,10,100,50]},{"op":"remove","shapeId":4}])"));
    CHECK((doc.crop() == Rect{10, 10, 100, 50}) && !doc.find(4));
    CHECK(run(doc, R"([{"op":"undo"},{"op":"undo"}])"));
    CHECK(doc.find(4) && (doc.crop() == Rect{0, 0, 200, 100}));
    CHECK(run(doc, R"([{"op":"crop","rect":[-50,-50,100,100]},{"op":"resetCrop"}])"));
    CHECK((doc.crop() == Rect{0, 0, 200, 100}));
    CHECK(run(doc, R"([{"op":"crop","rect":[-50,-50,100,100]}])") && (doc.crop() == Rect{0, 0, 50, 50}));
    CHECK(commands::validFontFamily("") && commands::validFontFamily("Noto Sans CJK JP") && !commands::validFontFamily("a\xc2\x85") && !commands::validFontFamily("\xff"));
}

static void renderTests() {
    // Crop copy, opaque redaction, blur confined to its rectangle.
    Raster base(8, 6, false);
    for (int y = 0; y < 6; ++y) for (int x = 0; x < 8; ++x) base.row(y)[x] = argb(255, x * 30, y * 40, 7);
    EditDocument doc(base);
    CHECK(run(doc, R"([{"op":"add","shape":{"kind":"redact","rect":[1,1,2,2]}},{"op":"crop","rect":[1,0,6,5]}])"));
    auto out = doc.renderBase();
    CHECK(out.width == 6 && out.height == 5);
    CHECK(out.row(1)[0] == 0xFF000000u && out.row(2)[1] == 0xFF000000u && out.row(0)[0] == base.row(0)[1] && out.row(1)[2] == base.row(1)[3]);
    CHECK(run(doc, R"([{"op":"add","shape":{"kind":"blur","rect":[4,0,3,6],"strength":4}}])"));
    out = doc.renderBase();
    bool outsideUntouched = true, insideChanged = false;
    for (int y = 0; y < 5; ++y) for (int x = 0; x < 6; ++x) {
        const bool inBlur = x + 1 >= 4 && x + 1 < 7;
        const bool inMask = x + 1 >= 1 && x + 1 < 3 && y >= 1 && y < 3;
        if (!inBlur && !inMask) outsideUntouched = outsideUntouched && out.row(y)[x] == base.row(y)[x + 1];
        if (inBlur) insideChanged = insideChanged || out.row(y)[x] != base.row(y)[x + 1];
    }
    CHECK(outsideUntouched && insideChanged);
    // Hidden colour of fully transparent pixels is cleared.
    Raster alpha(2, 1, true);
    alpha.pixels = {0x00FF8040u, 0x80FF8040u};
    EditDocument::clearHiddenColor(alpha);
    CHECK(alpha.pixels[0] == 0 && alpha.pixels[1] == 0x80FF8040u);
    // Area-average downscale without colour bleeding from transparent pixels.
    Raster quad(2, 2, true);
    quad.pixels = {0xFFFF0000u, 0x0000FF00u, 0xFFFF0000u, 0x0000FF00u};
    const auto half = EditDocument::scaled(quad, quad.rect(), 1, 1);
    CHECK(half.width == 1 && redOf(half.pixels[0]) == 255 && greenOf(half.pixels[0]) == 0 && alphaOf(half.pixels[0]) == 128);
    // Display blur tiles stay within about 1.5 MP and show redactions black.
    EditDocument big(noise(3000, 2000, 6));
    CHECK(run(big, R"([{"op":"add","shape":{"kind":"blur","rect":[0,0,3000,2000]}},{"op":"add","shape":{"kind":"redact","rect":[0,0,1500,1000]}}])"));
    const auto tile = big.blurPreview(*big.find(1), 1.0);
    CHECK(tile.width * double(tile.height) <= EditDocument::blurPreviewPixels * 1.01 && tile.width > 0);
    CHECK(tile.row(tile.height / 8)[tile.width / 8] == 0xFF000000u);
}

static void invarianceTests() {
    // Two inputs that differ only inside a mask export byte-identical PNGs, with
    // blur overlapping and adjacent to the mask, an annotation over it and crop through it.
    for (const bool alpha : {false, true}) {
        auto a = noise(120, 90, 7, alpha), b = a;
        const Rect mask{30, 20, 41, 33};
        std::mt19937 random(99);
        for (int y = mask.y; y < mask.bottom(); ++y) for (int x = mask.x; x < mask.right(); ++x) b.row(y)[x] = random() | (alpha ? 0 : 0xFF000000u);
        CHECK(a.pixels != b.pixels);
        const char* edit = R"([{"op":"add","shape":{"kind":"redact","rect":[30,20,41,33]}},
                              {"op":"add","shape":{"kind":"blur","rect":[60,10,40,30],"strength":24}},
                              {"op":"add","shape":{"kind":"blur","rect":[71,20,20,33],"strength":64}},
                              {"op":"add","shape":{"kind":"blur","rect":[10,53,30,20],"strength":2}},
                              {"op":"add","shape":{"kind":"rectangle","rect":[25,15,50,40]}},
                              {"op":"crop","rect":[45,5,70,80]}])";
        EditDocument docA(a), docB(b);
        CHECK(run(docA, edit) && run(docB, edit));
        const auto pngA = exportPortable(docA), pngB = exportPortable(docB);
        CHECK(!pngA.empty() && pngA == pngB);
        int w = 0, h = 0;
        CHECK(!png::inspect(pngA, &w, &h) && w == 70 && h == 80);
        CHECK(pngA.find("tEXt") == std::string::npos && pngA.find("tIME") == std::string::npos && pngA.find("secret") == std::string::npos);
        // Without the redaction the outputs differ (the check is not vacuous).
        EditDocument plainA(a), plainB(b);
        CHECK(run(plainA, R"([{"op":"crop","rect":[45,5,70,80]}])") && run(plainB, R"([{"op":"crop","rect":[45,5,70,80]}])"));
        CHECK(exportPortable(plainA) != exportPortable(plainB));
    }
    // Tiny and edge masks: 1x1 at a corner and a full-width strip.
    auto a = noise(17, 9, 8), b = a;
    b.row(0)[0] ^= 0x00FFFFFFu; b.row(8)[16] ^= 0x00123456u;
    for (int x = 0; x < 17; ++x) b.row(4)[x] ^= 0x00010101u;
    EditDocument docA(a), docB(b);
    const char* edges = R"([{"op":"add","shape":{"kind":"redact","rect":[0,0,1,1]}},{"op":"add","shape":{"kind":"redact","rect":[16,8,5,5]}},
                           {"op":"add","shape":{"kind":"redact","rect":[-3,4,40,1]}},{"op":"add","shape":{"kind":"blur","rect":[0,0,17,9],"strength":6}}])";
    CHECK(run(docA, edges) && run(docB, edges));
    CHECK(exportPortable(docA) == exportPortable(docB));
}

static void pngTests() {
    Raster r(3, 2, true);
    r.pixels = {0xFF102030u, 0x00000000u, 0x80405060u, 0xFFFFFFFFu, 0xFF000000u, 0x01020304u};
    const auto encoded = storedPng(r);
    CHECK(png::inspect(encoded).code == "png_chunk_refused");
    std::string out;
    CHECK(!exporter::finishPng(encoded, SIZE_MAX, 3, 2, out) && !png::inspect(out));
    CHECK(exporter::finishPng(encoded, 10, 3, 2, out).code == "edit_export_failed");          // over the output limit
    CHECK(exporter::finishPng(encoded, SIZE_MAX, 2, 2, out).code == "edit_export_failed");    // wrong size
    CHECK(exporter::finishPng("not a png", SIZE_MAX, 3, 2, out).code == "edit_export_failed");
    // Animated PNG: an acTL chunk before the first IDAT.
    std::string animated(png::signature);
    std::string ihdr(13, '\0'); ihdr[3] = 1; ihdr[7] = 1; ihdr[8] = 8; ihdr[9] = 6;
    chunk(animated, "IHDR", ihdr);
    chunk(animated, "acTL", std::string(8, '\0'));
    chunk(animated, "IDAT", "x");
    CHECK(exporter::animatedPng(animated) && !exporter::animatedPng(encoded) && !exporter::animatedPng("\xff\xd8\xff"));
}

static void orientationTests() {
    // 3x2 source, pixel value = 10*y + x.
    Raster r(3, 2, false);
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 3; ++x) r.row(y)[x] = uint32_t(10 * y + x);
    auto at = [](const Raster& o, int x, int y) { return int(o.row(y)[x]); };
    const auto o6 = exporter::oriented(r, 6); // rotate 90 clockwise: top row becomes the right column
    CHECK(o6.width == 2 && o6.height == 3 && at(o6, 1, 0) == 0 && at(o6, 1, 2) == 2 && at(o6, 0, 0) == 10);
    const auto o8 = exporter::oriented(r, 8); // rotate 270 clockwise: top row becomes the left column, reversed
    CHECK(o8.width == 2 && at(o8, 0, 2) == 0 && at(o8, 0, 0) == 2 && at(o8, 1, 0) == 12);
    const auto o5 = exporter::oriented(r, 5); // transpose
    CHECK(at(o5, 0, 0) == 0 && at(o5, 1, 0) == 10 && at(o5, 0, 2) == 2);
    const auto o7 = exporter::oriented(r, 7); // transverse
    CHECK(at(o7, 0, 0) == 12 && at(o7, 1, 2) == 0);
    const auto o2 = exporter::oriented(r, 2), o3 = exporter::oriented(r, 3), o4 = exporter::oriented(r, 4);
    CHECK(at(o2, 0, 0) == 2 && at(o3, 0, 0) == 12 && at(o4, 0, 0) == 10 && at(exporter::oriented(r, 1), 2, 1) == 12);
}

int main() {
    geometryTests();
    historyTests();
    commandTests();
    renderTests();
    invarianceTests();
    pngTests();
    orientationTests();
    if (failures) { std::fprintf(stderr, "%d editor check(s) failed\n", failures); return 1; }
    std::printf("windows editor contract: all checks passed\n");
    return 0;
}
