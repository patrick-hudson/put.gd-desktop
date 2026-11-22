#pragma once
#include "Error.h"
#include <QJsonObject>
#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>

// #1893: capture style. A still capture placed on a styled canvas when it is exported:
// padding, a backdrop (colour, two-colour gradient, picture or none), rounded corners and a
// soft shadow. The captured pixels are never changed. Off by default; with it absent or off,
// nothing in the capture, upload, local copy or editor output changes.
//
// This header is data and geometry only (no painting): the resident validates and routes it,
// and the drawing lives in src/core/CaptureStyleRender (its own library), which only putgd-ui links.
namespace putgd {
struct CaptureStyle {
    bool enabled = false;
    QString backdrop = "gradient";          // color, gradient, picture or none
    QString color = "#383633";              // the colour, or the gradient's first stop
    QString colorEnd = "#1c1c1b";           // the gradient's second stop
    QString picture;                        // absolute path of the backdrop picture (kind picture)
    QString padding = "medium";             // small, medium or large
    bool corners = true;
    bool shadow = true;
    bool operator==(const CaptureStyle&) const = default;

    // The settings document's preferences.captureStyle, written with every field.
    QJsonObject toJson() const;
    // A validated object (validate() passed); missing optional fields keep their defaults.
    static CaptureStyle fromJson(const QJsonObject&);
    // preferences.captureStyle as stored: `pointer` names the field in errors.
    static Error validate(const QJsonValue&, const QString& pointer = QStringLiteral("/preferences/captureStyle"));
    // The same look without `enabled`: what a preset or the editor compares.
    bool sameLook(const CaptureStyle& other) const;

    // Presets: graphite (the first look), amber, paper and clear. Each is a full look.
    struct Preset;
    static const QList<Preset>& presets();
    static CaptureStyle preset(const QString& id);   // enabled, with that preset's look; graphite for an unknown id
    QString presetId() const;                         // the preset with this look, or "custom"
    static QStringList paddingNames() { return {"small", "medium", "large"}; }
    static QStringList backdropNames() { return {"color", "gradient", "picture", "none"}; }

    // Geometry, all in output pixels, from the shorter edge of the capture.
    struct Geometry {
        int padding = 0, radius = 0;
        double sigma = 0, offsetY = 0;      // shadow blur (Gaussian sigma) and its downward offset
        QSize canvas;                       // the styled image
        bool valid() const { return canvas.isValid() && !canvas.isEmpty(); }
    };
    // `level` overrides the padding name (0 small, 1 medium, 2 large); -1 uses `padding`.
    Geometry geometry(const QSize& capture, int level = -1) const;
    // The geometry for this capture within the limits: the padding steps down one size at a
    // time while the styled image would pass `maxPixels` pixels or `maxEdge` on a side.
    // Invalid when even small padding does not fit.
    Geometry fitted(const QSize& capture, qint64 maxPixels, int maxEdge = 32767) const;
};
struct CaptureStyle::Preset { QString id, label; CaptureStyle style; };
}
