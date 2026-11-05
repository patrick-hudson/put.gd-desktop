// The UI catalog's fixed build identity. The footer and the About window show
// the version, so reference images (tests/gui/catalog-references) need the
// same one in every build. putgd-ui-catalog compiles this file; its definitions
// come before the static putgd_build_identity archive, so the generated
// build-identity/BuildInfo.cpp is never linked in.
#include "core/BuildInfo.h"
namespace putgd {
const char* buildVersion() noexcept { return "26.615.1200-dev+g0123456789ab"; }
const char* buildManifestJson() noexcept { return R"putgd({
  "schemaVersion": 1,
  "version": "26.615.1200-dev+g0123456789ab",
  "baseVersion": "26.615.1200",
  "packageVersion": "26.615.1200~dev+g0123456789ab",
  "gitCommit": "0123456789abcdef0123456789abcdef01234567",
  "gitTree": "89abcdef0123456789abcdef0123456789abcdef",
  "commitCount": 1200,
  "dirty": false,
  "sourceProvenance": "git"
}
)putgd"; }
}
