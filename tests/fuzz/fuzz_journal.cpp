// What the resident reads back after a crash or a power cut: the operation journal's
// rows (core/Operation.cpp, with the S3 upload and deletion journals inside them)
// and the receipts in history. A damaged row must never stop the resident from
// starting, so every shape of JSON is read, then written back out, then read again.
#include "FuzzSupport.h"
#include "core/Operation.h"
#include <QJsonDocument>
using namespace putgd;

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    qInstallMessageHandler(fuzz::quiet);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz::Input in(data, size);
    const auto mode = in.byte();
    const auto document = QJsonDocument::fromJson(in.rest());
    if (!document.isObject()) return 0;
    const auto object = document.object();
    switch (mode % 4) {
    case 0: {
        const auto operation = Operation::fromJson(object);
        const auto written = operation.toJson();
        operation.publicJson();
        operation.stateName();
        operation.active();
        const auto again = Operation::fromJson(written);
        // What was written reads back as what was written.
        FUZZ_REQUIRE(again.toJson() == written);
        for (const auto& item : operation.items) {
            item.linkAvailable();
            item.linkFinishable();
            item.uploadRequest(operation.destination, std::optional<int>(1000));
        }
        break;
    }
    case 1: {
        const auto item = OperationItem::fromJson(object);
        const auto written = item.toJson();
        FUZZ_REQUIRE(OperationItem::fromJson(written).toJson() == written);
        item.linkAvailable();
        item.linkFinishable();
        break;
    }
    case 2: {
        const auto receipt = StorageReceipt::fromJson(object);
        receipt.validate();
        const auto written = receipt.toJson();
        FUZZ_REQUIRE(StorageReceipt::fromJson(written).toJson() == written);
        break;
    }
    default:
        UploadProgress::fromJson(object);
        Destination::fromJson(object).toJson();
        break;
    }
    return 0;
}
