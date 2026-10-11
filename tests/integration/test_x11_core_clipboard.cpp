#include "platform/x11/X11Clipboard.h"
#include "platform/x11/X11Shortcuts.h"
#include "platform/x11/X11Reply.h"
#include <QtTest>
#include <QBuffer>
#include <QImage>
#include <QUrl>
#include <memory>
#include <vector>
#include <algorithm>
#include <cstring>
using namespace putgd;
// Independent requestor on its own XCB connection: no Qt GUI application,
// QClipboard or production import implementation can make these tests pass.
class Receiver final:public QObject {
public:
 Receiver(){
  connection=xcb_connect(nullptr,nullptr);const auto* root=xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
  window=xcb_generate_id(connection);quint32 mask=XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_create_window(connection,XCB_COPY_FROM_PARENT,window,root->root,0,0,1,1,0,XCB_WINDOW_CLASS_INPUT_ONLY,XCB_COPY_FROM_PARENT,XCB_CW_EVENT_MASK,&mask);
  notifier=new QSocketNotifier(xcb_get_file_descriptor(connection),QSocketNotifier::Read,this);
  connect(notifier,&QSocketNotifier::activated,this,[this]{events();});
 }
 ~Receiver(){delete notifier;xcb_disconnect(connection);}
 xcb_atom_t atom(const char* name){return x11::internAtom(connection,name);}
 void request(const char* name,bool noProperty=false,bool hold=false,xcb_timestamp_t time=XCB_CURRENT_TIME){
  target=atom(name);property=noProperty?target:atom("SYNTHETIC_RESULT");paused=hold;done=false;rejected=false;incremental=false;data.clear();type=0;format=0;
  xcb_delete_property(connection,window,property);xcb_convert_selection(connection,window,atom("CLIPBOARD"),target,noProperty?xcb_atom_t(XCB_ATOM_NONE):property,time);xcb_flush(connection);
 }
 void resume(){paused=false;xcb_delete_property(connection,window,property);xcb_flush(connection);}
 void steal(){xcb_set_selection_owner(connection,window,atom("CLIPBOARD"),XCB_CURRENT_TIME);xcb_flush(connection);auto roundtrip=x11::reply(xcb_get_selection_owner_reply(connection,xcb_get_selection_owner(connection,atom("CLIPBOARD")),nullptr));}
 bool done=false,rejected=false,incremental=false,paused=false;QByteArray data;xcb_atom_t type=0;quint8 format=0;
private:
 void read(bool initial){
  auto value=x11::reply(xcb_get_property_reply(connection,xcb_get_property(connection,0,window,property,XCB_GET_PROPERTY_TYPE_ANY,0,1024*1024),nullptr));
  if(!value){rejected=true;done=true;return;}
  if(initial&&value->type==atom("INCR")){incremental=true;if(!paused)resume();return;}
  type=value->type;format=value->format;const int size=xcb_get_property_value_length(value.get());
  data.append(static_cast<const char*>(xcb_get_property_value(value.get())),size);
  if(!incremental||size==0)done=true;
  if(!paused){xcb_delete_property(connection,window,property);xcb_flush(connection);}
 }
 void events(){while(auto* raw=xcb_poll_for_event(connection)){
  const auto event=x11::reply(raw);const auto kind=raw->response_type&127;
  if(kind==XCB_SELECTION_NOTIFY){const auto* notify=reinterpret_cast<xcb_selection_notify_event_t*>(raw);if(notify->property==XCB_ATOM_NONE){rejected=true;done=true;}else read(true);}
  else if(kind==XCB_PROPERTY_NOTIFY&&incremental&&!done){const auto* notify=reinterpret_cast<xcb_property_notify_event_t*>(raw);if(notify->window==window&&notify->atom==property&&notify->state==XCB_PROPERTY_NEW_VALUE)read(false);}
 }}
 xcb_connection_t* connection; xcb_window_t window; xcb_atom_t target=0,property=0;QSocketNotifier* notifier;
};
class CoreClipboardTest final:public QObject {
 Q_OBJECT
 static Error copy(X11Clipboard& owner,const CopyPayload& payload){Error result;bool completed=false;owner.copy(payload,[&](Error error){result=error;completed=true;});if(!completed)return {"test_incomplete","Copy callback was not synchronous",{}};if(result)qWarning().noquote()<<"Synthetic native copy failure:"<<result.code;return result;}

private slots:
 void nativeCoreGuards(){
  X11Clipboard clipboard;QVERIFY(clipboard.available());X11Shortcuts shortcuts;QVERIFY(shortcuts.available());shortcuts.bind({{"region","Ctrl+Shift+1"}});
  const auto old=qgetenv("WAYLAND_DISPLAY");qputenv("WAYLAND_DISPLAY","synthetic-wayland");{X11Clipboard rejected;QVERIFY(!rejected.available());X11Shortcuts noKeys;QVERIFY(!noKeys.available());}if(old.isNull())qunsetenv("WAYLAND_DISPLAY");else qputenv("WAYLAND_DISPLAY",old);
 }
 void textTargetsTimestampAndNoneProperty(){
  X11Clipboard owner;CopyPayload payload;payload.kind="text";payload.text=QString::fromUtf8("synthetic café\nsecond line");QVERIFY(!copy(owner,payload));
  Receiver receiver;receiver.request("TARGETS");QTRY_VERIFY(receiver.done);QVERIFY(!receiver.rejected);QCOMPARE(receiver.type,xcb_atom_t(XCB_ATOM_ATOM));QCOMPARE(receiver.format,quint8(32));
  QList<xcb_atom_t> targets;for(qsizetype i=0;i+4<=receiver.data.size();i+=4){xcb_atom_t atom;memcpy(&atom,receiver.data.constData()+i,4);targets.append(atom);}
  for(const auto* mime:{"TARGETS","TIMESTAMP","UTF8_STRING","text/plain;charset=utf-8","text/plain"})QVERIFY(targets.contains(receiver.atom(mime)));
  QVERIFY(!targets.contains(receiver.atom("MULTIPLE")));
  receiver.request("TIMESTAMP");QTRY_VERIFY(receiver.done);QCOMPARE(receiver.format,quint8(32));QCOMPARE(receiver.data.size(),4);quint32 timestamp;memcpy(&timestamp,receiver.data.constData(),4);QVERIFY(timestamp!=XCB_CURRENT_TIME);
  for(const auto* mime:{"UTF8_STRING","text/plain;charset=utf-8","text/plain"}){receiver.request(mime,true);QTRY_VERIFY(receiver.done);QVERIFY(!receiver.rejected);QCOMPARE(receiver.data,payload.text.toUtf8());}
  receiver.request("MULTIPLE");QTRY_VERIFY(receiver.done);QVERIFY(receiver.rejected);
  receiver.request("UTF8_STRING",false,false,timestamp-1);QTRY_VERIFY(receiver.done);QVERIFY(receiver.rejected);
 }
 void filesAndImageMime(){
  X11Clipboard owner;CopyPayload files;files.kind="files";files.files=QStringList{"/tmp/synthetic space.txt","/tmp/synthetic-unicode-é.png"};QVERIFY(!copy(owner,files));
  QByteArray expected;for(const auto& path:files.files)expected+=QUrl::fromLocalFile(path).toEncoded()+"\r\n";
  Receiver receiver;receiver.request("text/uri-list");QTRY_VERIFY(receiver.done);QCOMPARE(receiver.data,expected);
  CopyPayload image;image.kind="image";QImage pixels(23,17,QImage::Format_ARGB32);pixels.fill(Qt::cyan);QBuffer buffer(&image.encodedImage);QVERIFY(buffer.open(QIODevice::WriteOnly));QVERIFY(pixels.save(&buffer,"PNG"));QVERIFY(!copy(owner,image));
  receiver.request("image/png");QTRY_VERIFY(receiver.done);QVERIFY(!receiver.rejected);QCOMPARE(receiver.data,image.encodedImage);QCOMPARE(QImage::fromData(receiver.data),pixels);
  receiver.request("UTF8_STRING");QTRY_VERIFY(receiver.done);QVERIFY(receiver.rejected);
 }
 void independentIncrementalLeasesSurviveReplacementAndImportCancel(){
  X11Clipboard owner;CopyPayload old;old.kind="text";old.text=QString(256*1024,'a');QVERIFY(!copy(owner,old));
  Receiver first,second;first.request("UTF8_STRING",false,true);second.request("text/plain",false,true);QTRY_VERIFY(first.incremental&&second.incremental);QVERIFY(!first.done&&!second.done);
  CopyPayload fresh;fresh.kind="text";fresh.text="new owner content";QVERIFY(!copy(owner,fresh));owner.cancel();
  Receiver newest;newest.request("UTF8_STRING");QTRY_VERIFY(newest.done);QCOMPARE(newest.data,fresh.text.toUtf8());
  first.resume();second.resume();QTRY_VERIFY(first.done&&second.done);QCOMPARE(first.data,old.text.toUtf8());QCOMPARE(second.data,old.text.toUtf8());
  // The native import queue remains usable while this owner serves output.
  X11Clipboard importer;bool done=false;PayloadResult imported;importer.acquire({},[&](PayloadResult value){imported=std::move(value);done=true;});QTRY_VERIFY(done);QVERIFY(!imported.error);QCOMPARE(imported.payload.bytes,fresh.text.toUtf8());
 }
 void selectionClearPreservesExistingTransfer(){
  X11Clipboard owner;CopyPayload payload;payload.kind="text";payload.text=QString(192*1024,'b');QVERIFY(!copy(owner,payload));
  Receiver receiver;receiver.request("UTF8_STRING",false,true);QTRY_VERIFY(receiver.incremental);
  const auto previous=owner.revision();Receiver competing;competing.steal();QTRY_VERIFY(owner.revision()>previous);
  receiver.resume();QTRY_VERIFY(receiver.done);QCOMPARE(receiver.data,payload.text.toUtf8());
  CopyPayload after;after.kind="text";after.text="regained";QVERIFY(!copy(owner,after));Receiver fresh;fresh.request("UTF8_STRING");QTRY_VERIFY(fresh.done);QCOMPARE(fresh.data,after.text.toUtf8());
 }
 void boundedRequestorsAndDestroyedPeer(){
  X11Clipboard owner;CopyPayload payload;payload.kind="text";payload.text=QString(96*1024,'c');QVERIFY(!copy(owner,payload));
  std::vector<std::unique_ptr<Receiver>> peers;for(int i=0;i<16;++i){auto peer=std::make_unique<Receiver>();peer->request("UTF8_STRING",false,true);peers.push_back(std::move(peer));}
  QTRY_VERIFY(std::all_of(peers.begin(),peers.end(),[](const auto& peer){return peer->incremental;}));
  Receiver excess;excess.request("UTF8_STRING");QTRY_VERIFY(excess.done);QVERIFY(excess.rejected);
  peers.pop_back();QTest::qWait(30);excess.request("UTF8_STRING");QTRY_VERIFY(excess.done);QVERIFY(!excess.rejected);QCOMPARE(excess.data,payload.text.toUtf8());
  for(auto& peer:peers) peer->resume();
  QTRY_VERIFY(std::all_of(peers.begin(),peers.end(),[](const auto& peer){return peer->done;}));
 }
 void abandonedTransferExpires(){
  X11Clipboard owner;CopyPayload old;old.kind="text";old.text=QString(96*1024,'d');QVERIFY(!copy(owner,old));Receiver receiver;receiver.request("UTF8_STRING",false,true);QTRY_VERIFY(receiver.incremental);
  CopyPayload fresh;fresh.kind="text";fresh.text="replacement";QVERIFY(!copy(owner,fresh));
  QTest::qWait(16500);receiver.resume();QTest::qWait(100);QVERIFY(receiver.data.isEmpty());QVERIFY(!receiver.done);
  Receiver active;active.request("UTF8_STRING");QTRY_VERIFY(active.done);QCOMPARE(active.data,fresh.text.toUtf8());
 }
 void sharedEncodedImageLeasesReleaseOnReplacementAndClear(){
  CopyPayload original;original.kind="image";
  QImage pixels(512,256,QImage::Format_RGB32);quint32 seed=0x31415926;
  for(int y=0;y<pixels.height();++y)for(int x=0;x<pixels.width();++x){seed=1664525*seed+1013904223;pixels.setPixel(x,y,0xff000000|(seed&0xffffff));}
  QBuffer buffer(&original.encodedImage);QVERIFY(buffer.open(QIODevice::WriteOnly));QVERIFY(pixels.save(&buffer,"PNG"));buffer.close();
  QVERIFY(original.encodedImage.size()>64*1024);QVERIFY(original.encodedImage.isDetached());
  {
   X11Clipboard owner;QVERIFY(!copy(owner,original));
   // The owner shares encoded input rather than duplicating the whole image.
   QVERIFY(!original.encodedImage.isDetached());
   Receiver receiver;receiver.request("image/png",false,true);QTRY_VERIFY(receiver.incremental);
   CopyPayload fresh;fresh.kind="text";fresh.text="current small result";QVERIFY(!copy(owner,fresh));
   QVERIFY(!original.encodedImage.isDetached()); // only its accepted INCR lease remains
   receiver.resume();QTRY_VERIFY(receiver.done);QCOMPARE(receiver.data,original.encodedImage);
   QTRY_VERIFY(original.encodedImage.isDetached());
   QVERIFY(!copy(owner,original));QVERIFY(!original.encodedImage.isDetached());
   Receiver competing;competing.steal();QTRY_VERIFY(original.encodedImage.isDetached());
   QVERIFY(!copy(owner,original));QVERIFY(!original.encodedImage.isDetached());
  }
  QVERIFY(original.encodedImage.isDetached()); // destruction releases current ownership
 }
 void persistentAfterPayloadLifetimeAndRejectedCopy(){
  X11Clipboard owner;{CopyPayload temporary;temporary.kind="text";temporary.text="leased beyond the caller";QVERIFY(!copy(owner,temporary));}
  CopyPayload invalid;invalid.kind="image";invalid.encodedImage="not-png";QCOMPARE(copy(owner,invalid).code,QString("artifact_invalid"));
  Receiver receiver;receiver.request("UTF8_STRING");QTRY_VERIFY(receiver.done);QCOMPARE(receiver.data,QByteArray("leased beyond the caller"));
 }
};
QTEST_GUILESS_MAIN(CoreClipboardTest)
#include "test_x11_core_clipboard.moc"
