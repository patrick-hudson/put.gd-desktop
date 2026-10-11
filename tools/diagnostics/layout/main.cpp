#include "core/Settings.h"
#include "core/Operation.h"
#include "core/HistoryStore.h"
#include "ui/HistoryModel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtGlobal>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>

namespace {
template<class T> QJsonObject layout(const char* name)
{
    QJsonObject row{{"type", name},
                    {"size_bytes", qint64(sizeof(T))},
                    {"alignment_bytes", qint64(alignof(T))},
                    {"standard_layout", std::is_standard_layout_v<T>}};
    if constexpr (std::is_integral_v<T>) {
        row.insert("value_bits", std::numeric_limits<T>::digits);
        row.insert("signed", std::numeric_limits<T>::is_signed);
        // Strings preserve full-width bounds without JSON number rounding.
        if constexpr (std::numeric_limits<T>::is_signed) {
            row.insert("minimum", QString::number(qlonglong(std::numeric_limits<T>::lowest())));
            row.insert("maximum", QString::number(qlonglong(std::numeric_limits<T>::max())));
        } else {
            row.insert("minimum", QStringLiteral("0"));
            row.insert("maximum", QString::number(qulonglong(std::numeric_limits<T>::max())));
        }
    }
    return row;
}

template<class T> QJsonObject field(const char* name, std::size_t offset)
{
    return {{"member", name}, {"offset_bytes", qint64(offset)},
            {"size_bytes", qint64(sizeof(T))}, {"alignment_bytes", qint64(alignof(T))}};
}

QJsonObject resourceLimits()
{
    using T = putgd::ResourceLimits;
    static_assert(std::is_standard_layout_v<T>, "offsetof requires a standard-layout type");
    auto row = layout<T>("putgd::ResourceLimits");
    QJsonArray fields;
#define RESOURCE_FIELD(member) fields.append(field<decltype(T::member)>(#member, offsetof(T, member)))
    RESOURCE_FIELD(maxFiles);
    RESOURCE_FIELD(pendingOperations);
    RESOURCE_FIELD(concurrentUploads);
    RESOURCE_FIELD(maxFileBytes);
    RESOURCE_FIELD(maxBatchBytes);
    RESOURCE_FIELD(pendingArtifactBytes);
    RESOURCE_FIELD(pendingImageBytes);
    RESOURCE_FIELD(clipboardTextBytes);
    RESOURCE_FIELD(imagePixels);
    RESOURCE_FIELD(imageBytes);
#undef RESOURCE_FIELD
    row.insert("members", fields);
    return row;
}

// Synthetic structs only: no proposal to alter any production type or contract.
struct WideCount { std::uint8_t tag; std::uint32_t count; std::uint64_t total; };
struct NarrowCount { std::uint8_t tag; std::uint16_t count; std::uint64_t total; };
struct Interleaved { std::uint8_t first; std::uint64_t total; std::uint8_t second; };
struct Grouped { std::uint64_t total; std::uint8_t first; std::uint8_t second; };
static_assert(std::is_standard_layout_v<WideCount> && std::is_standard_layout_v<NarrowCount>);
static_assert(std::is_standard_layout_v<Interleaved> && std::is_standard_layout_v<Grouped>);

template<class T> QJsonObject synthetic(const char* name, std::size_t fieldBytes, QJsonArray fields)
{
    auto row = layout<T>(name);
    row.insert("member_payload_bytes", qint64(fieldBytes));
    row.insert("padding_bytes", qint64(sizeof(T) - fieldBytes));
    row.insert("members", fields);
    return row;
}

QJsonObject examples()
{
    const auto wide = synthetic<WideCount>("synthetic::WideCount",
        sizeof(std::uint8_t) + sizeof(std::uint32_t) + sizeof(std::uint64_t),
        {field<std::uint8_t>("tag", offsetof(WideCount, tag)),
         field<std::uint32_t>("count", offsetof(WideCount, count)),
         field<std::uint64_t>("total", offsetof(WideCount, total))});
    const auto narrow = synthetic<NarrowCount>("synthetic::NarrowCount",
        sizeof(std::uint8_t) + sizeof(std::uint16_t) + sizeof(std::uint64_t),
        {field<std::uint8_t>("tag", offsetof(NarrowCount, tag)),
         field<std::uint16_t>("count", offsetof(NarrowCount, count)),
         field<std::uint64_t>("total", offsetof(NarrowCount, total))});
    constexpr auto groupedFields = sizeof(std::uint64_t) + 2 * sizeof(std::uint8_t);
    const auto interleaved = synthetic<Interleaved>("synthetic::Interleaved", groupedFields,
        {field<std::uint8_t>("first", offsetof(Interleaved, first)),
         field<std::uint64_t>("total", offsetof(Interleaved, total)),
         field<std::uint8_t>("second", offsetof(Interleaved, second))});
    const auto grouped = synthetic<Grouped>("synthetic::Grouped", groupedFields,
        {field<std::uint64_t>("total", offsetof(Grouped, total)),
         field<std::uint8_t>("first", offsetof(Grouped, first)),
         field<std::uint8_t>("second", offsetof(Grouped, second))});
    return {{"synthetic_only", true},
            {"narrowing", QJsonObject{{"before", wide}, {"after", narrow},
                {"stride_saved_bytes", qint64(sizeof(WideCount)) - qint64(sizeof(NarrowCount))}}},
            {"reordering", QJsonObject{{"before", interleaved}, {"after", grouped},
                {"stride_saved_bytes", qint64(sizeof(Interleaved)) - qint64(sizeof(Grouped))}}}};
}
}

int main()
{
    // No Q(Core/Gui)Application, service, model, settings or payload is constructed.
    const QJsonObject result{
        {"schema_version", 1},
        {"environment", QJsonObject{{"compiler_id", LAYOUT_COMPILER_ID},
            {"compiler_version", LAYOUT_COMPILER_VERSION}, {"build_type", LAYOUT_BUILD_TYPE},
            {"cplusplus", qint64(__cplusplus)}, {"qt_compile_version", QT_VERSION_STR},
            {"qt_runtime_version", qVersion()}, {"bits_per_byte", CHAR_BIT},
            {"pointer_size_bytes", qint64(sizeof(void*))},
            {"pointer_width_bits", qint64(sizeof(void*) * CHAR_BIT)}}},
        {"types", QJsonArray{layout<int>("int"), layout<std::int16_t>("std::int16_t"),
            layout<std::int32_t>("std::int32_t"), layout<qint32>("qint32"),
            layout<std::int64_t>("std::int64_t"), layout<qint64>("qint64"),
            layout<std::int_least32_t>("std::int_least32_t"),
            layout<std::int_fast32_t>("std::int_fast32_t"),
            layout<QString>("QString"), layout<QJsonObject>("QJsonObject"),
            resourceLimits(), layout<putgd::Settings>("putgd::Settings"),
            layout<putgd::Operation>("putgd::Operation"),
            layout<putgd::OperationItem>("putgd::OperationItem"),
            layout<putgd::Artifact>("putgd::Artifact"),
            layout<putgd::HistoryQuery>("putgd::HistoryQuery"),
            layout<putgd::HistoryModel>("putgd::HistoryModel")}},
        {"synthetic_examples", examples()},
        {"limitations", QJsonArray{
            "sizeof measures inline object stride, not pointed-to allocations or RSS.",
            "No application objects, data, live population counts or workload were inspected.",
            "Layouts and padding depend on the compiler, ABI, architecture and Qt build.",
            "Synthetic narrower fields have different bounds; no production type was changed."}}
    };
    const auto json = QJsonDocument(result).toJson(QJsonDocument::Indented);
    std::cout.write(json.constData(), json.size());
    return std::cout ? 0 : 1;
}
