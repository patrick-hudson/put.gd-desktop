// The S3 worker's wire protocol (storage/s3/S3WorkerProtocol.cpp): every request and
// result the resident and the worker decode from each other, the multipart and
// deletion journals those carry across a crash, and the public URL builder.
// Input: one byte choosing the decoder, then JSON. A decoded value must encode
// and decode again to the same bytes.
#include "FuzzSupport.h"
#include "storage/s3/PublicUrl.h"
#include "storage/s3/S3Journal.h"
#include "storage/s3/S3WorkerProtocol.h"
#include <QJsonDocument>
using namespace putgd;
namespace wire = putgd::s3wire;

namespace {
const char* const methods[] = {"upload", "reconcile", "stat", "refreshLink", "remove", "abandon", "testDestination", "checkName", "nothing"};

template <class T>
void roundTrip(const QJsonObject& object) {
    T value;
    if (!wire::decode(object, value)) return;
    const auto again = wire::encode(value);
    T second;
    FUZZ_REQUIRE(wire::decode(again, second));
    FUZZ_REQUIRE(wire::encode(second) == again);
}
}

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    qInstallMessageHandler(fuzz::quiet);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz::Input in(data, size);
    const auto choice = in.byte();
    const auto document = QJsonDocument::fromJson(in.rest());
    if (!document.isObject()) return 0;
    const auto object = document.object();
    switch (choice % 16) {
    case 0: roundTrip<UploadRequest>(object); break;
    case 1: roundTrip<DeleteRequest>(object); break;
    case 2: roundTrip<UploadResult>(object); break;
    case 3: roundTrip<StatResult>(object); break;
    case 4: roundTrip<DeleteResult>(object); break;
    case 5: roundTrip<Error>(object); break;
    case 6: roundTrip<Credentials>(object); break;
    case 7: { NameCheck v; wire::decode(object, v); } break;
    case 8: { Destination v; wire::decode(object, v); } break;
    case 9: { StorageReceipt v; wire::decode(object, v); } break;
    case 10: { Artifact v; wire::decode(object, v); } break;
    case 11: { UploadRequest v; wire::decodeReconcileRequest(object, v); StorageReceipt r; wire::decodeReceiptRequest(object, r); } break;
    case 12: wire::decodeUploadTrace(object); break;
    case 13: wire::decodeTestDestinationResult(object); break;
    case 14: {
        const auto method = QString::fromLatin1(methods[(choice / 16) % (sizeof methods / sizeof *methods)]);
        wire::validateRequest(method, object);
        break;
    }
    default: {
        // The journals as the history database stores them, and the public link a settings object yields.
        s3Stage(object["stage"]); s3UploadPhase(object["phase"]);
        const auto phase = s3DeletePhase(object["phase"]);
        if (phase) { s3DeleteRule(*phase); FUZZ_REQUIRE(!s3DeletePhaseName(*phase).isNull()); }
        const auto key = object["key"].toString();
        publicObjectUrl(object, key);
        s3VirtualAddressing(object);
    }
    }
    return 0;
}
