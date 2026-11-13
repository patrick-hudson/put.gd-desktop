#pragma once
#include <QJsonValue>
#include <QString>
#include <QtGlobal>
#include <optional>
#include <utility>
namespace putgd {
// The S3 driver's saved values (#1435): the request stage an error names, and the
// phases of its multipart-upload and deletion journals. Each is read once into an
// enum; the names are the journal's and the wire's strings, so never rename one.
// Qt-only; no AWS types.
enum class S3Stage {
    NameHead, ObjectHead, CurrentHead, UploadPut, UploadCreate, UploadPart, UploadListParts, UploadComplete, UploadAbort,
    DeleteVersioning, DeleteCurrentHead, DeleteMarker, DeleteVersion, DeleteObject
};
// A multipart upload journal names its phase once the upload exists.
enum class S3UploadPhase { Parts, Completing };
// A deletion journal; None is a journal with no phase yet.
enum class S3DeletePhase {
    None, MarkerIntent, MarkerUnknown, MarkerFailed, MarkerCancelled, MarkerDone,
    VersionReady, VersionIntent, VersionUnknown, VersionFailed, VersionCancelled,
    ObjectIntent, ObjectUnknown, ObjectFailed, ObjectCancelled, Done
};
inline constexpr std::pair<S3Stage, const char*> s3StageNames[] {
    {S3Stage::NameHead, "name_head"}, {S3Stage::ObjectHead, "object_head"}, {S3Stage::CurrentHead, "current_head"},
    {S3Stage::UploadPut, "upload_put"}, {S3Stage::UploadCreate, "upload_create"}, {S3Stage::UploadPart, "upload_part"},
    {S3Stage::UploadListParts, "upload_list_parts"}, {S3Stage::UploadComplete, "upload_complete"}, {S3Stage::UploadAbort, "upload_abort"},
    {S3Stage::DeleteVersioning, "delete_versioning"}, {S3Stage::DeleteCurrentHead, "delete_current_head"},
    {S3Stage::DeleteMarker, "delete_marker"}, {S3Stage::DeleteVersion, "delete_version"}, {S3Stage::DeleteObject, "delete_object"}};
inline constexpr std::pair<S3UploadPhase, const char*> s3UploadPhaseNames[] {
    {S3UploadPhase::Parts, "parts"}, {S3UploadPhase::Completing, "completing"}};
inline constexpr std::pair<S3DeletePhase, const char*> s3DeletePhaseNames[] {
    {S3DeletePhase::None, ""}, {S3DeletePhase::MarkerIntent, "marker_intent"}, {S3DeletePhase::MarkerUnknown, "marker_unknown"},
    {S3DeletePhase::MarkerFailed, "marker_failed"}, {S3DeletePhase::MarkerCancelled, "marker_cancelled"}, {S3DeletePhase::MarkerDone, "marker_done"},
    {S3DeletePhase::VersionReady, "version_ready"}, {S3DeletePhase::VersionIntent, "version_intent"}, {S3DeletePhase::VersionUnknown, "version_unknown"},
    {S3DeletePhase::VersionFailed, "version_failed"}, {S3DeletePhase::VersionCancelled, "version_cancelled"},
    {S3DeletePhase::ObjectIntent, "object_intent"}, {S3DeletePhase::ObjectUnknown, "object_unknown"},
    {S3DeletePhase::ObjectFailed, "object_failed"}, {S3DeletePhase::ObjectCancelled, "object_cancelled"}, {S3DeletePhase::Done, "done"}};
namespace s3journal {
template<class E, std::size_t N> QString name(const std::pair<E, const char*> (&names)[N], E value) {
    for (const auto& [entry, text] : names)
        if (entry == value) return QString::fromLatin1(text);
    return {};
}
// Anything that is not one of the names, including a value of another JSON type, is unreadable.
template<class E, std::size_t N> std::optional<E> parse(const std::pair<E, const char*> (&names)[N], const QJsonValue& value) {
    if (!value.isString()) return std::nullopt;
    for (const auto& [entry, text] : names)
        if (value.toString() == QLatin1String(text)) return entry;
    return std::nullopt;
}
}
inline QString s3StageName(S3Stage stage) { return s3journal::name(s3StageNames, stage); }
inline std::optional<S3Stage> s3Stage(const QJsonValue& value) { return s3journal::parse(s3StageNames, value); }
inline QString s3UploadPhaseName(S3UploadPhase phase) { return s3journal::name(s3UploadPhaseNames, phase); }
inline std::optional<S3UploadPhase> s3UploadPhase(const QJsonValue& value) { return s3journal::parse(s3UploadPhaseNames, value); }
inline QString s3DeletePhaseName(S3DeletePhase phase) { return s3journal::name(s3DeletePhaseNames, phase); }
// A journal without a phase is None, as is the empty string an older journal may hold.
inline std::optional<S3DeletePhase> s3DeletePhase(const QJsonValue& value) {
    return value.isUndefined() ? std::optional(S3DeletePhase::None) : s3journal::parse(s3DeletePhaseNames, value);
}
// What a saved deletion phase means for the next attempt, one row per phase.
struct S3DeleteRule {
    enum class Kind { Start, Marker, Version, Object, Done };
    Kind kind = Kind::Start;
    // A marker DELETE may have taken effect; its identity is unknown, so none is sent again.
    bool markerUnresolved = false;
    // The removing DELETE was sent: a missing object is this deletion's own result.
    bool removalSent = false;
    // The protective marker exists or is no longer needed; go straight to the version.
    bool markerSettled = false;
};
inline S3DeleteRule s3DeleteRule(S3DeletePhase phase) {
    using P = S3DeletePhase;
    using K = S3DeleteRule::Kind;
    switch (phase) {
    case P::None: return {K::Start, false, false, false};
    case P::MarkerIntent: return {K::Marker, true, false, false};
    case P::MarkerUnknown: return {K::Marker, true, false, false};
    case P::MarkerFailed: return {K::Marker, false, false, false};
    case P::MarkerCancelled: return {K::Marker, false, false, false};
    case P::MarkerDone: return {K::Marker, false, false, true};
    case P::VersionReady: return {K::Version, false, false, false};
    case P::VersionIntent: return {K::Version, false, true, true};
    case P::VersionUnknown: return {K::Version, false, true, true};
    case P::VersionFailed: return {K::Version, false, false, false};
    case P::VersionCancelled: return {K::Version, false, false, false};
    case P::ObjectIntent: return {K::Object, false, true, false};
    case P::ObjectUnknown: return {K::Object, false, true, false};
    case P::ObjectFailed: return {K::Object, false, false, false};
    case P::ObjectCancelled: return {K::Object, false, false, false};
    case P::Done: return {K::Done, false, false, false};
    }
    Q_UNREACHABLE();
    return {};
}
}
