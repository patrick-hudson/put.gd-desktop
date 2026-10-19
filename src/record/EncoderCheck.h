#pragma once
#include <QJsonObject>
#include <QStringList>

namespace putgd::record {
// Registry presence is not proof: Fedora's openh264enc and libopenh264 load the
// noopenh264 placeholder and fail on the first frame, and stock VA drivers list
// encoders without the entry points. These checks start each candidate on a
// tiny synthetic frame instead. They open no capture source or audio device.

// GStreamer live H.264 encoders from `present`, in order, that encode two test
// frames; `failures` receives the reason for each one that did not.
QStringList usableLiveEncoders(const QStringList& present, QJsonObject& failures);
// FFmpeg video encoders from `names` that open at a small size (VA-API ones on
// a VA device and frame pool, as the export does).
QStringList usableExportEncoders(const QStringList& names, QJsonObject& failures);
// Whether FFmpeg can open its H.264 decoder, which every preview and export of
// the working clip needs; `decoder` receives its name.
bool h264DecoderUsable(QString& decoder, QString& failure);
// One user-facing sentence on how to get an H.264 encoder or decoder on this
// system: distribution packages, or the AppImage's host-driver requirement.
QString h264Hint(const QString& reason);
}
