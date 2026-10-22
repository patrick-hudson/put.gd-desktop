#pragma once
// Immutable artifacts under <dataRoot>\artifacts\<operation>\<item>: private
// spools written once, hashed, recoverable from a per-batch manifest, with
// bounded previews under <dataRoot>\previews (docs/architecture.md).
#include "core/Operation.h"
#include "core/Settings.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace putgd {
// What an acquisition delivered: local files, text, verified PNG bytes (editor),
// a clipboard image (png or dib bytes) or a capture already written as a PNG file.
struct Payload {
    std::string kind; // files, text, png, image, capture, recording
    std::vector<std::string> files;
    std::string bytes;
    std::string imageFormat; // png or dib for kind image
    std::string imagePath;   // kind capture: a PNG the capture helper wrote
    std::string stagingDirectory; // kind capture/recording: removed once the file was taken
    std::string mime;             // kind recording: the export's MIME type; imagePath is its file
};
struct Acquisition { std::vector<Artifact> artifacts; Error error; };
class ArtifactStore {
public:
    explicit ArtifactStore(std::string dataRoot);
    Acquisition acquire(const std::string& operationId, const Payload&, const Settings&, const std::string& action, const std::string& createdIsoUtc, long long maximumObjectBytes);
    Error verify(const Artifact&) const;
    Acquisition recover(const std::string& operationId) const;
    void discardIncomplete();
    Error remove(const std::string& operationId);
    // Makes (or finds) the bounded 256-px preview of an image artifact; empty when none.
    std::string preview(const Artifact&, long long budgetBytes, long long maximumPixels);
    void clearPreviews();
    long long bytes() const;
    std::string root() const { return artifacts_; }
    std::string previewsRoot() const { return previews_; }
    static std::string mimeFor(const std::string& path);
private:
    std::string dataRoot_, artifacts_, previews_;
};
}
