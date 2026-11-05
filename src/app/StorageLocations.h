#pragma once
#include "core/Error.h"
#include <functional>
namespace putgd {
// The resident owns <GenericConfigLocation>/putgd (config.json) and
// <GenericDataLocation>/putgd (journal, artifacts, previews). Earlier releases
// resolved Qt's app-scoped locations with organization and application both
// "putgd", so they used the same paths with putgd/putgd doubled (#56).
//
// prepare() moves such a legacy tree into place before anything opens it.
// Each root is decided on its own: a root already in use is never merged with
// its legacy tree. The legacy tree is first renamed to a staging name inside the
// new root, then its entries are renamed into place one by one; every step is an
// atomic rename on one filesystem, and a staging directory found at startup is
// always finished before anything opens the root.
struct StorageLocations {
 QString configRoot,dataRoot;
 QJsonObject report; // diagnostics "locations": active roots, migration state, retained legacy paths
 Error error;        // startup must not continue; nothing was removed
 QString configPath() const {return configRoot+"/config.json";}
 static StorageLocations prepare();
 // Test seam: beforeMove runs ahead of each rename and of removing the emptied
 // staging folder (to is empty); returning false stops there exactly as an
 // interrupted process would.
 using MoveHook=std::function<bool(const QString& from,const QString& to)>;
 static StorageLocations prepare(const QString& genericConfig,const QString& genericData,const MoveHook& beforeMove={});
 static QString legacyRoot(const QString& root) {return root+"/putgd";}
 static QString stagingRoot(const QString& root) {return root+"/.putgd-migration";}
};
}
