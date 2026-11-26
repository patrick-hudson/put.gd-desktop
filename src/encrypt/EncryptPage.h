#pragma once
#include "core/Error.h"
#include <QByteArray>
#include <QString>
// The encrypted-link viewer page (#2079). src/encrypt/viewer.html is the one viewer: its exact
// bytes are compiled in and pinned by SHA-256 (viewerSha256, checked by a test and by
// tools/encrypt/pin_viewer.py --check). An encrypted page is that file with its marker
// `<!--putgd-sealed-->` replaced by one data element, and nothing else changed:
//  inline  <script id="putgd-sealed" type="application/octet-stream" data-form="inline">
//          BASE64(container)</script>
//  split   <script id="putgd-sealed" type="application/octet-stream" data-form="split"
//          data-bin="NAME.bin"></script>, and the CSP's connect-src 'none' becomes
//          connect-src 'self' so the page can fetch NAME.bin beside it.
// Qt Core only: the resident may parse pages without linking libcrypto.
namespace putgd::encrypt {
extern const char viewerSha256[];
// The compiled-in viewer, byte for byte.
QByteArray viewer();
// Whether the compiled-in viewer still has the pinned hash.
bool viewerIntact();
// The page around its data element.
QByteArray pagePrefix(bool split);
QByteArray pageSuffix();
QByteArray dataOpen(bool split,const QString& bin={});
inline QByteArray dataClose() {return QByteArrayLiteral("</script>");}
// A data file name the split page may name: 1 to 128 of [A-Za-z0-9_-], then ".bin".
bool validBinName(const QString&);
// The Test check's page (#2079): the "probe" vector of tests/fixtures/encrypt/vectors.json as an
// inline page. Its key is public (probeFragment), so it proves only how a destination serves a page.
QByteArray probePage();
QString probeFragment();
struct ParsedPage {bool split=false;QString bin;QByteArray base64;};
// Checks that `page` is exactly the pinned viewer around one well-formed data element.
// encrypt_page_changed when anything else differs: it is not put.gd's viewer.
Error parsePage(const QByteArray& page,ParsedPage& parsed);
}
