#include "platform/kde/KdeKeyboardMap.h"
#include <QtTest>
#include <QTemporaryFile>
#include <xkbcommon/xkbcommon.h>
#include <wayland-client-protocol.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
using namespace putgd;
class KeyboardMapTest:public QObject {
 Q_OBJECT
 xkb_context* context_=nullptr;
 QByteArray map_;
 int descriptor(const QByteArray& bytes){QTemporaryFile file; if(!file.open()||file.write(bytes)!=bytes.size()||!file.flush())return -1;return dup(file.handle());}
 bool closed(int fd){return fcntl(fd,F_GETFD)==-1&&errno==EBADF;}
private slots:
 void initTestCase(){context_=xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES);QVERIFY(context_);xkb_context_set_log_level(context_,XKB_LOG_LEVEL_CRITICAL);xkb_rule_names names{};names.layout="us";auto* map=xkb_keymap_new_from_names(context_,&names,XKB_KEYMAP_COMPILE_NO_FLAGS);QVERIFY(map);char* text=xkb_keymap_get_as_string(map,XKB_KEYMAP_FORMAT_TEXT_V1);QVERIFY(text);map_=QByteArray(text);map_.append(char(0));free(text);xkb_keymap_unref(map);}
 void cleanupTestCase(){xkb_context_unref(context_);}
 void acceptsMapAndClosesDescriptor(){const int fd=descriptor(map_);QVERIFY(fd>=0);auto* map=readKdeKeyboardMap(context_,WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,fd,uint32_t(map_.size()));QVERIFY(map);QVERIFY(closed(fd));QCOMPARE(xkb_keymap_num_layouts(map),xkb_layout_index_t(1));xkb_keymap_unref(map);}
 void compiledMapOwnsContextAfterParsing(){auto* context=xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES);QVERIFY(context);const int fd=descriptor(map_);QVERIFY(fd>=0);auto* map=readKdeKeyboardMap(context,1,fd,uint(map_.size()));QVERIFY(map);xkb_context_unref(context);QVERIFY(closed(fd));QCOMPARE(QString::fromUtf8(xkb_keymap_layout_get_name(map,0)),QString("English (US)"));auto* state=xkb_state_new(map);QVERIFY(state);QCOMPARE(xkb_state_key_get_one_sym(state,10),xkb_keysym_t('1'));xkb_state_unref(state);xkb_keymap_unref(map);}
 void malformedInputs_data(){QTest::addColumn<QByteArray>("bytes");QTest::addColumn<uint>("size");QTest::addColumn<uint>("format");QTest::newRow("unsupported-format")<<map_<<uint(map_.size())<<uint(0);QTest::newRow("truncated-fd")<<map_<<uint(map_.size()+1)<<uint(1);QTest::newRow("too-large")<<map_<<uint(1024*1024+1)<<uint(1);QTest::newRow("no-terminator")<<QByteArray("bad")<<uint(3)<<uint(1);QTest::newRow("interior-nul")<<QByteArray("a\0b\0",4)<<uint(4)<<uint(1);QTest::newRow("malformed-map")<<QByteArray("bad\0",4)<<uint(4)<<uint(1);QTest::newRow("empty")<<QByteArray()<<uint(0)<<uint(1);}
 void malformedInputs(){QFETCH(QByteArray,bytes);QFETCH(uint,size);QFETCH(uint,format);const int fd=descriptor(bytes);QVERIFY(fd>=0);QVERIFY(!readKdeKeyboardMap(context_,format,fd,size));QVERIFY(closed(fd));}
 void nullContextClosesDescriptor(){const int fd=descriptor(map_);QVERIFY(fd>=0);QVERIFY(!readKdeKeyboardMap(nullptr,1,fd,uint(map_.size())));QVERIFY(closed(fd));}
 void rejectsPipeWithoutBlocking(){int pipe[2];QVERIFY(::pipe(pipe)==0);QVERIFY(!readKdeKeyboardMap(context_,1,pipe[0],uint(map_.size())));QVERIFY(closed(pipe[0]));close(pipe[1]);}
};
QTEST_GUILESS_MAIN(KeyboardMapTest)
#include "test_kde_keyboard_map.moc"
