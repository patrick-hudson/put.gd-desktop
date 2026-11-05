#include "record/PortalScreenCast.h"
#include <QtTest>
using namespace putgd::record;

// The portal's Start response carries streams as a(ua{sv}) with (ii) position
// and size; the helper reads the node, geometry and source type. The D-Bus
// structure form is exercised against the live portal; the list form here.
class PortalStreams : public QObject {
    Q_OBJECT
private slots:
    void readsNodeGeometryAndType() {
        const auto monitor = PortalScreenCast::parseStream(92, {{"position", QVariantList{0, 0}}, {"size", QVariantList{1920, 1080}}, {"source_type", 1u}});
        QCOMPARE(monitor.node, 92u); QCOMPARE(monitor.position, QPoint(0, 0)); QCOMPARE(monitor.size, QSize(1920, 1080)); QCOMPARE(monitor.sourceType, 1u);
        const auto window = PortalScreenCast::parseStream(7, {{"position", QVariantList{1920, 0}}, {"size", QVariantList{2560, 1440}}, {"source_type", 2u}});
        QCOMPARE(window.position, QPoint(1920, 0)); QCOMPARE(window.size, QSize(2560, 1440)); QCOMPARE(window.sourceType, 2u);
        // A stream without geometry (some compositors send it later) is still usable by node.
        const auto bare = PortalScreenCast::parseStream(3, {});
        QCOMPARE(bare.node, 3u); QVERIFY(bare.size.isEmpty());
        QVERIFY(PortalScreenCast::parseStreams(QVariant()).isEmpty());
    }
};
QTEST_GUILESS_MAIN(PortalStreams)
#include "test_portal_streams.moc"
