#pragma once
// The one sanitized render path of the Windows editor (docs/image-editor.md,
// "Sanitized export"): Upload, Save a copy, the agent preview and saveCopy all
// call exportPng/exportPreview. Steps 1-3 (crop, redaction, blur) are portable
// (EditDocument::renderBase); annotations are drawn with GDI+, the PNG is
// encoded with WIC, then png::stripDisallowedChunks and png::inspect run on
// every result. The portable helpers below are tested on Linux; everything
// under _WIN32 needs GDI+/WIC.
#include "common/Error.h"
#include "editor/EditDocument.h"
#include <string>
#include <string_view>

namespace putgd::editor::exporter {
inline constexpr size_t maximumPreviewBytes = 6 * 1024 * 1024;

// Target size that fits `width` x `height` inside maxEdge x maxEdge keeping the
// aspect ratio; unchanged when it already fits.
TextSize fitWithin(int width, int height, int maxEdge);
// Final step of every export: drop chunks outside the allow list, then require
// a structurally valid PNG with the expected size and at most `limit` bytes.
// Errors: edit_export_failed.
Error finishPng(std::string_view encoded, size_t limit, int width, int height, std::string& out);
// Applies an EXIF orientation (1-8) to a decoded raster.
Raster oriented(Raster source, int orientation);
// True when a PNG declares animation (an acTL chunk before the first IDAT).
bool animatedPng(std::string_view bytes);

#ifdef _WIN32
// Bold text metrics shared by the canvas, commands and export (GDI+).
TextSize measureText(const std::string& text, const std::string& family, int pixels);
// The default family the editor uses for "" (Segoe UI, else GDI+'s generic sans-serif).
std::string defaultFontFamily();
// Full-resolution sanitized raster: steps 1-5 except encoding.
Raster render(const EditDocument&);
// WIC PNG encoder (24-bit when every pixel is opaque, else 32-bit RGBA).
bool encodePng(const Raster&, std::string& out);
// Sanitized PNG of the whole edit. Errors: edit_export_failed.
Error exportPng(const EditDocument&, size_t limit, std::string& png, int& width, int& height);
// The same render scaled so its longer edge is at most maxEdge.
Error exportPreview(const EditDocument&, int maxEdge, std::string& png, int& width, int& height);
// Decodes one static PNG or JPEG (chosen by signature) into a normalized
// 32-bit raster in sRGB with the EXIF orientation applied. Errors:
// source_unsupported, input_excessive (more than maxPixels pixels).
Error decodeImage(std::string_view bytes, long long maxPixels, Raster& out);
#endif
}

#ifdef _WIN32
namespace Gdiplus { class Graphics; }
namespace putgd::editor::exporter {
// Draws one annotation (arrow, rectangle, highlight or text) in image
// coordinates through the graphics' current transform; redact and blur are
// raster steps and draw nothing here.
void paintAnnotation(Gdiplus::Graphics&, const Shape&);
}
#endif
