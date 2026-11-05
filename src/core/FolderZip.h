#pragma once
#include "Error.h"
#include <QString>
#include <functional>
namespace putgd {
// A streaming ZIP64 writer for selected folders (#1105), separate from the in-memory
// diagnostics ZipWriter: zlib raw deflate into a file through fixed buffers, so memory
// stays bounded for any folder size or entry count.
class FolderZip {
public:
 // Bounds for one selected folder, from a stat-only walk. Never follows symlinks, and
 // skips symlinks, sockets, FIFOs and devices. Keeps no per-entry list, so memory
 // stays flat for any entry count.
 struct Plan {
  QString folder;         // absolute path of the selected folder
  QString archiveName;    // "<folder name>.zip"
  qint64 entries=0;       // file + directory entries, the folder itself included
  qint64 files=0;
  qint64 sourceBytes=0;   // sum of the regular files' sizes
  qint64 finalBound=0;    // upper bound of the finished archive: entry data bound (deflateBound or raw, whichever is larger) + local headers + ZIP64 records + ONE central directory + EOCD records
  qint64 peakBytes=0;     // upper bound of disk in use at peak: finalBound + the spilled central-directory file (both exist during the append)
  Error error;            // e.g. unreadable folder; message names the folder
 };
 static Plan plan(const QString& folder);
 // Writes the archive to `output` (new file, 0600). Walks again, reads each source
 // once, and patches local headers in place (no data descriptors). Spills central-
 // directory records to a sibling temp file and appends them at the end. Removes its
 // partial output and spill file on any failure. Errors name the file at fault.
 // `cancelled` is polled between chunks.
 static Error write(const Plan&,const QString& output,const std::function<bool()>& cancelled={});
};
}
