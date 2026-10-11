#include "platform/x11/X11NativeSnapshot.h"
#include "platform/x11/X11Reply.h"
#include <QTest>
#include <QCoreApplication>
#include <QJsonArray>
#include <cstring>
class NativeSnapshotTests final:public QObject {
 Q_OBJECT
private slots:
 void activeTargetFreezesBeforePersistenceAndRejectsOwnedPid(){
  auto* connection=xcb_connect(nullptr,nullptr);QVERIFY(!xcb_connection_has_error(connection));const auto* screen=xcb_setup_roots_iterator(xcb_get_setup(connection)).data;const auto root=screen->root;
  const auto active=putgd::x11::internAtom(connection,"_NET_ACTIVE_WINDOW"),pid=putgd::x11::internAtom(connection,"_NET_WM_PID");
  auto make=[&](int x){const auto window=xcb_generate_id(connection);const uint32_t values[]{1};xcb_create_window(connection,screen->root_depth,window,root,x,10,50,40,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,XCB_CW_OVERRIDE_REDIRECT,values);xcb_map_window(connection,window);return window;};
  const auto first=make(10),second=make(80);auto sync=[&]{xcb_flush(connection);auto response=putgd::x11::reply(xcb_get_input_focus_reply(connection,xcb_get_input_focus(connection),nullptr));QVERIFY(response);};
  xcb_change_property(connection,XCB_PROP_MODE_REPLACE,root,active,XCB_ATOM_WINDOW,32,1,&first);const uint32_t helper=424242;xcb_change_property(connection,XCB_PROP_MODE_REPLACE,first,pid,XCB_ATOM_CARDINAL,32,1,&helper);sync();
  putgd::X11NativeSnapshot native;QVERIFY(native.available());putgd::Error error;native.freeze("window",{helper},error);QCOMPARE(error.code,QString("capture_target"));error={};const auto frozen=native.freeze("window",{},error);QVERIFY2(!error,qPrintable(error.code));QCOMPARE(frozen["window"].toString(),QString::number(first));
  xcb_change_property(connection,XCB_PROP_MODE_REPLACE,root,active,XCB_ATOM_WINDOW,32,1,&second);sync();QVERIFY(native.unchanged(frozen,error));
  const uint32_t position=20;xcb_configure_window(connection,first,XCB_CONFIG_WINDOW_X,&position);sync();QVERIFY(!native.unchanged(frozen,error));QCOMPARE(error.code,QString("capture_target"));
  xcb_destroy_window(connection,first);xcb_destroy_window(connection,second);xcb_delete_property(connection,root,active);xcb_disconnect(connection);
 }
 void scalePropertyChangeRejectsFrozenNativeLayout(){
  auto* connection=xcb_connect(nullptr,nullptr);QVERIFY(!xcb_connection_has_error(connection));const auto root=xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;const auto property=putgd::x11::internAtom(connection,"RESOURCE_MANAGER");
  auto previous=putgd::x11::reply(xcb_get_property_reply(connection,xcb_get_property(connection,0,root,property,XCB_GET_PROPERTY_TYPE_ANY,0,262145),nullptr));QVERIFY(previous);QVERIFY(!previous->bytes_after);
  putgd::X11NativeSnapshot native;putgd::Error error;const auto frozen=native.freeze("desktop",{},error);QVERIFY2(!error,qPrintable(error.code));const QByteArray changed="Xft.dpi:\t192\n";xcb_change_property(connection,XCB_PROP_MODE_REPLACE,root,property,XCB_ATOM_STRING,8,changed.size(),changed.constData());xcb_flush(connection);auto sync=putgd::x11::reply(xcb_get_input_focus_reply(connection,xcb_get_input_focus(connection),nullptr));QVERIFY(sync);
  const bool rejected=!native.unchanged(frozen,error);const auto code=error.code;
  // This test runs on its private Xvfb server, but restore its metadata regardless.
  if(previous->type==XCB_ATOM_NONE)xcb_delete_property(connection,root,property);else xcb_change_property(connection,XCB_PROP_MODE_REPLACE,root,property,previous->type,previous->format,xcb_get_property_value_length(previous.get())/(previous->format/8),xcb_get_property_value(previous.get()));xcb_flush(connection);xcb_disconnect(connection);
  QVERIFY(rejected);QCOMPARE(code,QString("display_changed"));
 }
};
QTEST_GUILESS_MAIN(NativeSnapshotTests)
#include "test_native_snapshot.moc"
