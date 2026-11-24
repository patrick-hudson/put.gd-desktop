#include "platform/wayland/DataControlSourceAdapter.h"
#include "fixtures/WaylandTestServer.h"
#include <QtTest>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <wayland-server.h>
#include "ext-data-control-v1-server-protocol.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fixtures/Waits.h"
using namespace putgd;
// A private in-process compositor: a seat, and data-control only when asked for.
// It proves the adapter's protocol behaviour, not any real desktop; COSMIC and
// Budgie need their own graphical acceptance.
class Compositor final:public test::WaylandTestServer {
 struct Offer {Compositor* owner;QByteArray bytes;};
 struct Source {Compositor* owner;QList<QByteArray> mimes;};
 QList<wl_resource*> devices_;wl_resource* selection_=nullptr;bool initialSelection_;
 static void deviceDestroyed(wl_resource* resource){static_cast<Compositor*>(wl_resource_get_user_data(resource))->devices_.removeOne(resource);}
 static void sourceDestroyed(wl_resource* resource){auto* source=static_cast<Source*>(wl_resource_get_user_data(resource));if(source->owner->selection_==resource)source->owner->selection_=nullptr;delete source;}
 static void offerDestroyed(wl_resource* resource){delete static_cast<Offer*>(wl_resource_get_user_data(resource));}
 static void receive(wl_client*,wl_resource* resource,const char*,int32_t fd){const auto bytes=static_cast<Offer*>(wl_resource_get_user_data(resource))->bytes;if(::write(fd,bytes.constData(),size_t(bytes.size()))<0)qWarning("fixture write failed");::close(fd);}
 void announce(const QList<QByteArray>& mimes,const QByteArray& bytes){
  for(auto* device:devices_){
   auto* offer=wl_resource_create(wl_resource_get_client(device),&ext_data_control_offer_v1_interface,wl_resource_get_version(device),0);
   static const struct ext_data_control_offer_v1_interface implementation={receive,destroyRequest};
   wl_resource_set_implementation(offer,&implementation,new Offer{this,bytes},offerDestroyed);
   ext_data_control_device_v1_send_data_offer(device,offer);
   for(const auto& mime:mimes)ext_data_control_offer_v1_send_offer(offer,mime.constData());
   ext_data_control_device_v1_send_selection(device,offer);
  }
 }
 static void setSelection(wl_client*,wl_resource* device,wl_resource* source){
  auto* self=static_cast<Compositor*>(wl_resource_get_user_data(device));
  if(self->selection_&&self->selection_!=source)ext_data_control_source_v1_send_cancelled(self->selection_);
  self->selection_=source;self->announce(source?static_cast<Source*>(wl_resource_get_user_data(source))->mimes:QList<QByteArray>{},{});
 }
 static void createSource(wl_client* client,wl_resource* manager,uint32_t id){
  auto* resource=wl_resource_create(client,&ext_data_control_source_v1_interface,wl_resource_get_version(manager),id);
  static const struct ext_data_control_source_v1_interface implementation={[](wl_client*,wl_resource* resource,const char* mime){static_cast<Source*>(wl_resource_get_user_data(resource))->mimes.append(mime);},destroyRequest};
  wl_resource_set_implementation(resource,&implementation,new Source{static_cast<Compositor*>(wl_resource_get_user_data(manager)),{}},sourceDestroyed);
 }
 static void getDevice(wl_client* client,wl_resource* manager,uint32_t id,wl_resource*){
  auto* self=static_cast<Compositor*>(wl_resource_get_user_data(manager));
  auto* device=wl_resource_create(client,&ext_data_control_device_v1_interface,wl_resource_get_version(manager),id);
  static const struct ext_data_control_device_v1_interface implementation={setSelection,destroyRequest,[](wl_client*,wl_resource*,wl_resource*){}};
  wl_resource_set_implementation(device,&implementation,self,deviceDestroyed);self->devices_.append(device);
  if(self->initialSelection_)ext_data_control_device_v1_send_selection(device,nullptr);
 }
 static void bindManager(wl_client* client,void* data,uint32_t version,uint32_t id){
  auto* manager=wl_resource_create(client,&ext_data_control_manager_v1_interface,int(version),id);
  static const struct ext_data_control_manager_v1_interface implementation={createSource,getDevice,destroyRequest};
  wl_resource_set_implementation(manager,&implementation,data,nullptr);
 }
public:
 explicit Compositor(bool dataControl,bool initialSelection=true):WaylandTestServer("/tmp/pgw-XXXXXX"),initialSelection_(initialSelection){
  if(!error.isEmpty())return;
  if(dataControl)wl_global_create(display_,&ext_data_control_manager_v1_interface,1,this,bindManager);
 }
 ~Compositor() override{stop();}
 bool bound()const{return !devices_.isEmpty();}
 // Another program takes the clipboard.
 void otherOwner(const QList<QByteArray>& mimes,const QByteArray& bytes){announce(mimes,bytes);flush();}
 void firstSelection(){for(auto* device:devices_)ext_data_control_device_v1_send_selection(device,nullptr);flush();}
 QList<QByteArray> offeredMimes()const{return selection_?static_cast<Source*>(wl_resource_get_user_data(selection_))->mimes:QList<QByteArray>{};}
 // What a paste would get from the current owner.
 QByteArray paste(const QByteArray& mime){
  if(!selection_)return {};
  int pipes[2];if(pipe2(pipes,O_NONBLOCK|O_CLOEXEC)<0)return {};
  ext_data_control_source_v1_send_send(selection_,mime.constData(),pipes[1]);::close(pipes[1]);flush();
  QByteArray bytes;bool done=false;
  const bool finished=test::waitFor([&]{char chunk[4096];const auto n=::read(pipes[0],chunk,sizeof chunk);if(n>0)bytes.append(chunk,n);else if(n==0)done=true;return done;});
  ::close(pipes[0]);return finished?bytes:QByteArray{};
 }
};
class DataControlSourceTest final:public QObject {
 Q_OBJECT
 static Error copied(DataControlSourceAdapter& adapter,const CopyPayload& payload){
  Error result{"unfinished","Copy did not finish",{}};bool done=false;
  adapter.copy(payload,[&](Error error){result=std::move(error);done=true;});
  if(!test::waitFor([&]{return done;}))qWarning("Copy did not finish in time");
  return result;
 }
 static CopyPayload text(const QString& value){CopyPayload payload;payload.kind="text";payload.text=value;return payload;}
 static bool ready(DataControlSourceAdapter& adapter){return test::waitFor([&]{return adapter.capabilities()["clipboard"].toObject()["available"].toBool();});}
private slots:
 void initTestCase(){
  // The guiless route reaches the same private connection as the graphical
  // resident, whose Qt Wayland platform this fixture cannot host.
  qputenv("XDG_SESSION_TYPE","wayland");qputenv("XDG_CURRENT_DESKTOP","KDE");qunsetenv("QT_QPA_PLATFORM");
 }
 void linkReachesTheClipboard(){
  Compositor compositor(true);QVERIFY2(compositor.error.isEmpty(),qPrintable(compositor.error));
  DataControlSourceAdapter adapter;QVERIFY(ready(adapter));
  const auto link=QString("https://put.gd/acceptance/fixture.png");
  const auto error=copied(adapter,text(link));QVERIFY2(!error,qPrintable(error.code));
  QVERIFY(compositor.offeredMimes().contains("text/plain;charset=utf-8"));
  QCOMPARE(compositor.paste("text/plain;charset=utf-8"),link.toUtf8());
  QCOMPARE(compositor.paste("text/plain"),link.toUtf8());
 }
 void imageReachesTheClipboard(){
  Compositor compositor(true);QVERIFY2(compositor.error.isEmpty(),qPrintable(compositor.error));
  DataControlSourceAdapter adapter;QVERIFY(ready(adapter));
  CopyPayload payload;payload.kind="image";payload.encodedImage=QByteArray("\x89PNG synthetic",13);
  const auto error=copied(adapter,payload);QVERIFY2(!error,qPrintable(error.code));
  QCOMPARE(compositor.paste("image/png"),payload.encodedImage);
 }
 void clipboardCanBeRead(){
  Compositor compositor(true);QVERIFY2(compositor.error.isEmpty(),qPrintable(compositor.error));
  DataControlSourceAdapter adapter;QVERIFY(ready(adapter));
  compositor.otherOwner({"text/plain;charset=utf-8"},"copied elsewhere");
  QVERIFY(test::waitFor([&]{return adapter.clipboardRevision()==1;}));
  auto prepared=adapter.prepareAcquisition("clipboard");
  PayloadResult result;bool done=false;
  prepared({},[&](PayloadResult value){result=std::move(value);done=true;});
  QVERIFY(test::waitFor([&]{return done;}));QVERIFY2(!result.error,qPrintable(result.error.code));
  QCOMPARE(result.payload.kind,QString("text"));QCOMPARE(result.payload.bytes,QByteArray("copied elsewhere"));
 }
 // Invoked before the compositor answered; read after the connection finished.
 void clipboardReadPreparedBeforeConnecting(){
  Compositor compositor(true,false);QVERIFY2(compositor.error.isEmpty(),qPrintable(compositor.error));
  DataControlSourceAdapter adapter;
  auto prepared=adapter.prepareAcquisition("clipboard");
  QVERIFY(test::waitFor([&]{return compositor.bound();}));
  compositor.otherOwner({"text/plain;charset=utf-8"},"copied elsewhere");QVERIFY(ready(adapter));
  PayloadResult result;bool done=false;
  prepared({},[&](PayloadResult value){result=std::move(value);done=true;});
  QVERIFY(test::waitFor([&]{return done;}));QVERIFY2(!result.error,qPrintable(result.error.code));
  QCOMPARE(result.payload.bytes,QByteArray("copied elsewhere"));
 }
 void withoutDataControlTheFailureStaysHonest(){
  Compositor compositor(false);QVERIFY2(compositor.error.isEmpty(),qPrintable(compositor.error));
  DataControlSourceAdapter adapter;
  QVERIFY(test::waitFor([&]{return adapter.capabilities()["clipboard"].toObject()["error"].toString().contains("lacks");}));
  QVERIFY(!adapter.capabilities()["clipboard"].toObject()["available"].toBool());
  const auto error=copied(adapter,text("https://put.gd/never"));
  QCOMPARE(error.code,QString("source_unsupported"));QVERIFY(error.message.contains("lacks"));
  PayloadResult result;adapter.acquire("clipboard",{},[&](PayloadResult value){result=std::move(value);});
  QCOMPARE(result.error.code,QString("source_unsupported"));
 }
 void captureStaysUnavailable(){
  Compositor compositor(true);QVERIFY2(compositor.error.isEmpty(),qPrintable(compositor.error));
  DataControlSourceAdapter adapter;QVERIFY(ready(adapter));
  const auto capabilities=adapter.capabilities();QCOMPARE(capabilities["adapter"].toString(),QString("wayland-data-control"));
  QVERIFY(!capabilities["shortcuts"].toObject()["available"].toBool());
  for(const auto& action:{QString("region"),QString("window"),QString("desktop")}){
   QVERIFY(!capabilities["capture"].toObject()[action].toObject()["available"].toBool());
   PayloadResult result;adapter.acquire(action,{},[&](PayloadResult value){result=std::move(value);});
   QCOMPARE(result.error.code,QString("source_unsupported"));
  }
 }
 // A cold-started upload may read the revision before the compositor's first
 // selection event. That event must not read as the user copying something, or
 // the link is dropped with "the clipboard changed during the upload".
 void connectingDoesNotLookLikeACopy(){
  Compositor compositor(true,false);QVERIFY2(compositor.error.isEmpty(),qPrintable(compositor.error));
  DataControlSourceAdapter adapter;
  QCOMPARE(adapter.clipboardRevision(),quint64(0));
  QVERIFY(test::waitFor([&]{return compositor.bound();}));
  QCOMPARE(adapter.clipboardRevision(),quint64(0));
  compositor.firstSelection();QVERIFY(ready(adapter));
  QCOMPARE(adapter.clipboardRevision(),quint64(0));
  compositor.otherOwner({"text/plain"},"copied elsewhere");
  QVERIFY(test::waitFor([&]{return adapter.clipboardRevision()==1;}));
  const auto error=copied(adapter,text("https://put.gd/after"));QVERIFY2(!error,qPrintable(error.code));
  QVERIFY(adapter.lastCopyRevision()==adapter.clipboardRevision());
 }
 // Mir announces no initial selection, so readiness comes from the sync (#1275). The first real
 // copy after that must still count as a change, or an import prepared while empty takes it.
 void aFirstCopyAfterAnAssumedEmptyClipboardIsAChange(){
  Compositor compositor(true,false);QVERIFY2(compositor.error.isEmpty(),qPrintable(compositor.error));
  DataControlSourceAdapter adapter;QVERIFY(test::waitFor([&]{return compositor.bound();}));QVERIFY(ready(adapter));
  const auto before=adapter.clipboardRevision();
  compositor.otherOwner({"text/plain"},"copied after connecting");
  QVERIFY(test::waitFor([&]{return adapter.clipboardRevision()==before+1;}));
 }
};
QTEST_GUILESS_MAIN(DataControlSourceTest)
#include "test_wayland_data_control_source.moc"
