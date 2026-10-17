#include "platform/kde/KdeSourceAdapter.h"
#include "platform/kde/KdeClipboardFormats.h"
#include "platform/kde/KdeCaptureCompose.h"
#include "core/ImageFormat.h"
#include <QtTest>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusMetaType>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <xkbcommon/xkbcommon.h>
#include <cstdlib>
#include <QGuiApplication>
#include <QSocketNotifier>
#include <QMap>
#include <QSet>
#include <wayland-server.h>
#include "xdg-output-unstable-v1-server-protocol.h"
#include "ext-data-control-v1-server-protocol.h"
#include "wlr-data-control-unstable-v1-server-protocol.h"
#include <sys/stat.h>
#include <list>
#include <memory>
#include <unistd.h>
using namespace putgd;
namespace {
const QString frozenId="{00000000-0000-0000-0000-000000000001}";
const QString otherId="{00000000-0000-0000-0000-000000000002}";
}
// A private in-process compositor fixture proves only native protocol state.
// It does not prove Plasma integration, real output topology, or pixel capture.
class NativeOutputFixture final:public QObject {
 struct SavedEnvironment {
  QMap<QByteArray,QByteArray> values;QSet<QByteArray> absent;
  void set(const QByteArray& key,const QByteArray& value){if(!values.contains(key)&&!absent.contains(key)){if(qEnvironmentVariableIsSet(key.constData()))values.insert(key,qgetenv(key.constData()));else absent.insert(key);}qputenv(key.constData(),value);}
  ~SavedEnvironment(){for(auto it=values.cbegin();it!=values.cend();++it)qputenv(it.key().constData(),it.value());for(const auto& key:absent)qunsetenv(key.constData());}
 } environment_;
 struct Logical;
 struct Output {
  NativeOutputFixture* fixture;wl_global* global=nullptr;QList<wl_resource*> native;QList<Logical*> logical;
  QString name="DP-1";QRect rectangle=QRect(0,0,2560,1440);QSize mode=QSize(3840,2160);int scale=2,transform=0;bool sendMode=true;
 } output_{this,nullptr,{},{}};
 struct Logical {Output* output;wl_resource* resource;wl_resource* native;};
 QTemporaryDir directory_{"/tmp/pgo-XXXXXX"};wl_display* display_=nullptr;QSocketNotifier* notifier_=nullptr;int version_;QString error_;wl_global* seatGlobal_=nullptr;QList<wl_resource*> keyboards_,seats_;QByteArray keyboardText_;
 static void destroyRequest(wl_client*,wl_resource* resource){wl_resource_destroy(resource);}
 static void outputDestroyed(wl_resource* resource){auto* output=static_cast<Output*>(wl_resource_get_user_data(resource));output->native.removeOne(resource);}
 static void logicalDestroyed(wl_resource* resource){auto* logical=static_cast<Logical*>(wl_resource_get_user_data(resource));logical->output->logical.removeOne(logical);delete logical;}
 static void sendGeometry(Output& output,wl_resource* resource){wl_output_send_geometry(resource,0,0,0,0,WL_OUTPUT_SUBPIXEL_UNKNOWN,"fixture","fixture",output.transform);}
 static void sendLogical(Logical* logical){const auto rect=logical->output->rectangle;zxdg_output_v1_send_logical_position(logical->resource,rect.x(),rect.y());zxdg_output_v1_send_logical_size(logical->resource,rect.width(),rect.height());}
 static void keyboardDestroyed(wl_resource* resource){auto* self=static_cast<NativeOutputFixture*>(wl_resource_get_user_data(resource));self->keyboards_.removeOne(resource);}
 void sendKeymap(wl_resource* keyboard){QTemporaryFile file;if(!file.open()||file.write(keyboardText_)!=keyboardText_.size()||!file.flush())return;wl_keyboard_send_keymap(keyboard,WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,file.handle(),uint32_t(keyboardText_.size()));}
 static void getKeyboard(wl_client* client,wl_resource* seat,uint32_t id){auto* self=static_cast<NativeOutputFixture*>(wl_resource_get_user_data(seat));auto* keyboard=wl_resource_create(client,&wl_keyboard_interface,wl_resource_get_version(seat),id);static const struct wl_keyboard_interface implementation={destroyRequest};wl_resource_set_implementation(keyboard,&implementation,self,keyboardDestroyed);self->keyboards_.append(keyboard);self->sendKeymap(keyboard);}
 static void seatDestroyed(wl_resource* resource){auto* self=static_cast<NativeOutputFixture*>(wl_resource_get_user_data(resource));self->seats_.removeOne(resource);}
 static void bindSeat(wl_client* client,void* data,uint32_t version,uint32_t id){auto* self=static_cast<NativeOutputFixture*>(data);auto* seat=wl_resource_create(client,&wl_seat_interface,int(version),id);static const struct wl_seat_interface implementation={nullptr,getKeyboard,nullptr,destroyRequest};wl_resource_set_implementation(seat,&implementation,data,seatDestroyed);self->seats_.append(seat);wl_seat_send_capabilities(seat,WL_SEAT_CAPABILITY_KEYBOARD);}
 static void bindOutput(wl_client* client,void* data,uint32_t version,uint32_t id){
  auto* output=static_cast<Output*>(data);auto* resource=wl_resource_create(client,&wl_output_interface,int(version),id);
  static const struct wl_output_interface implementation={destroyRequest};wl_resource_set_implementation(resource,&implementation,output,outputDestroyed);output->native.append(resource);
  sendGeometry(*output,resource);if(output->sendMode)wl_output_send_mode(resource,WL_OUTPUT_MODE_CURRENT,output->mode.width(),output->mode.height(),60000);
  if(version>=2)wl_output_send_scale(resource,output->scale);
  if(version>=4)wl_output_send_name(resource,output->name.toUtf8().constData());
  if(version>=2)wl_output_send_done(resource);
 }
 static void getLogical(wl_client* client,wl_resource* manager,uint32_t id,wl_resource* native){
  auto* output=static_cast<Output*>(wl_resource_get_user_data(native));const auto version=wl_resource_get_version(manager);
  auto* resource=wl_resource_create(client,&zxdg_output_v1_interface,version,id);auto* logical=new Logical{output,resource,native};
  static const struct zxdg_output_v1_interface implementation={destroyRequest};wl_resource_set_implementation(resource,&implementation,logical,logicalDestroyed);output->logical.append(logical);
  sendLogical(logical);if(version>=2)zxdg_output_v1_send_name(resource,output->name.toUtf8().constData());
  if(version>=3)wl_output_send_done(native);else zxdg_output_v1_send_done(resource);
 }
 static void bindManager(wl_client* client,void* data,uint32_t version,uint32_t id){
  auto* resource=wl_resource_create(client,&zxdg_output_manager_v1_interface,int(version),id);
  static const struct zxdg_output_manager_v1_interface implementation={destroyRequest,getLogical};wl_resource_set_implementation(resource,&implementation,data,nullptr);
 }
 // Data control: ext and wlr share one message layout, so one implementation
 // serves both; only the resources' interface descriptions differ.
 struct DataControl {NativeOutputFixture* fixture;bool wlr;};
 struct Source {DataControl* control;QStringList mimes;};
 std::list<DataControl> dataControls_;QList<wl_resource*> dataDevices_;wl_resource* selectionSource_=nullptr;
 static const wl_interface* dataInterface(bool wlr,const char* kind){
  if(!qstrcmp(kind,"manager"))return wlr?&zwlr_data_control_manager_v1_interface:&ext_data_control_manager_v1_interface;
  if(!qstrcmp(kind,"device"))return wlr?&zwlr_data_control_device_v1_interface:&ext_data_control_device_v1_interface;
  if(!qstrcmp(kind,"source"))return wlr?&zwlr_data_control_source_v1_interface:&ext_data_control_source_v1_interface;
  return wlr?&zwlr_data_control_offer_v1_interface:&ext_data_control_offer_v1_interface;
 }
 static void sourceDestroyed(wl_resource* resource){auto* source=static_cast<Source*>(wl_resource_get_user_data(resource));auto* self=source->control->fixture;if(self->selectionSource_==resource)self->selectionSource_=nullptr;delete source;}
 static void deviceDestroyed(wl_resource* resource){auto* control=static_cast<DataControl*>(wl_resource_get_user_data(resource));control->fixture->dataDevices_.removeOne(resource);}
 static void createSource(wl_client* client,wl_resource* manager,uint32_t id){
  auto* control=static_cast<DataControl*>(wl_resource_get_user_data(manager));auto* resource=wl_resource_create(client,dataInterface(control->wlr,"source"),wl_resource_get_version(manager),id);
  static const struct ext_data_control_source_v1_interface implementation={[](wl_client*,wl_resource* resource,const char* mime){static_cast<Source*>(wl_resource_get_user_data(resource))->mimes.append(QString::fromUtf8(mime));},destroyRequest};
  wl_resource_set_implementation(resource,&implementation,new Source{control,{}},sourceDestroyed);
 }
 static void setSelection(wl_client*,wl_resource* device,wl_resource* source){
  auto* self=static_cast<DataControl*>(wl_resource_get_user_data(device))->fixture;
  if(self->selectionSource_&&self->selectionSource_!=source)ext_data_control_source_v1_send_cancelled(self->selectionSource_);
  self->selectionSource_=source;const auto mimes=source?static_cast<Source*>(wl_resource_get_user_data(source))->mimes:QStringList{};
  for(auto* target:self->dataDevices_){
   wl_resource* offer=nullptr;
   if(source){
    auto* control=static_cast<DataControl*>(wl_resource_get_user_data(target));offer=wl_resource_create(wl_resource_get_client(target),dataInterface(control->wlr,"offer"),wl_resource_get_version(target),0);
    static const struct ext_data_control_offer_v1_interface implementation={[](wl_client*,wl_resource*,const char*,int32_t fd){close(fd);},destroyRequest};
    wl_resource_set_implementation(offer,&implementation,nullptr,nullptr);ext_data_control_device_v1_send_data_offer(target,offer);for(const auto& mime:mimes)ext_data_control_offer_v1_send_offer(offer,mime.toUtf8().constData());
   }
   ext_data_control_device_v1_send_selection(target,offer);
  }
 }
 static void getDevice(wl_client* client,wl_resource* manager,uint32_t id,wl_resource*){
  auto* control=static_cast<DataControl*>(wl_resource_get_user_data(manager));auto* resource=wl_resource_create(client,dataInterface(control->wlr,"device"),wl_resource_get_version(manager),id);
  static const struct ext_data_control_device_v1_interface implementation={setSelection,destroyRequest,[](wl_client*,wl_resource*,wl_resource*){}};
  wl_resource_set_implementation(resource,&implementation,control,deviceDestroyed);control->fixture->dataDevices_.append(resource);ext_data_control_device_v1_send_selection(resource,nullptr);
 }
 static void bindDataControl(wl_client* client,void* data,uint32_t version,uint32_t id){
  auto* control=static_cast<DataControl*>(data);auto* resource=wl_resource_create(client,dataInterface(control->wlr,"manager"),int(version),id);
  static const struct ext_data_control_manager_v1_interface implementation={createSource,getDevice,destroyRequest};wl_resource_set_implementation(resource,&implementation,control,nullptr);
 }
 void flush(){if(display_)wl_display_flush_clients(display_);}
public:
 explicit NativeOutputFixture(int version=3,bool sendMode=true,int nativeVersion=4,bool keyboard=false,int seatVersion=5):version_(version){
  struct stat metadata{};
  if(!directory_.isValid()||lstat(directory_.path().toUtf8().constData(),&metadata)<0||!S_ISDIR(metadata.st_mode)||metadata.st_uid!=geteuid()||(metadata.st_mode&077)!=0){error_="Fixture runtime directory is not private and owned";return;}
  const auto socketPath=directory_.filePath("wayland-test").toUtf8();
  if(socketPath.size()>107){error_="Fixture Unix socket exceeds Linux byte limit";return;}
  environment_.set("XDG_RUNTIME_DIR",directory_.path().toUtf8());environment_.set("WAYLAND_DISPLAY","wayland-test");environment_.set("XDG_SESSION_TYPE","wayland");environment_.set("XDG_CURRENT_DESKTOP","KDE");environment_.set("QT_QPA_PLATFORM","wayland");
  display_=wl_display_create();if(!display_||wl_display_add_socket(display_,"wayland-test")<0){error_="Cannot bind private native-output fixture socket";return;}
  if(keyboard){keyboardLayout("us");seatGlobal_=wl_global_create(display_,&wl_seat_interface,seatVersion,this,bindSeat);}
  output_.sendMode=sendMode;output_.global=wl_global_create(display_,&wl_output_interface,nativeVersion,&output_,bindOutput);
  wl_global_create(display_,&zxdg_output_manager_v1_interface,version_,this,bindManager);
  auto* loop=wl_display_get_event_loop(display_);notifier_=new QSocketNotifier(wl_event_loop_get_fd(loop),QSocketNotifier::Read,this);
  connect(notifier_,&QSocketNotifier::activated,this,[this,loop]{wl_event_loop_dispatch(loop,0);flush();});
 }
 ~NativeOutputFixture() override{if(notifier_){notifier_->setEnabled(false);delete notifier_;}if(display_){wl_display_destroy_clients(display_);wl_display_destroy(display_);}}
 // Advertise wlr (version 2) and/or ext data control; wlr is announced first so
 // the client's ext preference does not depend on registry order.
 void dataControl(bool ext,bool wlr){if(wlr){dataControls_.push_back({this,true});wl_global_create(display_,&zwlr_data_control_manager_v1_interface,2,&dataControls_.back(),bindDataControl);}if(ext){dataControls_.push_back({this,false});wl_global_create(display_,&ext_data_control_manager_v1_interface,1,&dataControls_.back(),bindDataControl);}}
 QStringList selectionMimes()const{return selectionSource_?static_cast<Source*>(wl_resource_get_user_data(selectionSource_))->mimes:QStringList{};}
 int keyboardCount()const{return keyboards_.size();}
 void keyboardCapability(bool present){for(auto* seat:seats_)wl_seat_send_capabilities(seat,present?WL_SEAT_CAPABILITY_KEYBOARD:0);flush();}
 void keyboardLayout(const char* layouts){auto* context=xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES);xkb_rule_names names{};names.layout=layouts;auto* map=xkb_keymap_new_from_names(context,&names,XKB_KEYMAP_COMPILE_NO_FLAGS);if(map){char* text=xkb_keymap_get_as_string(map,XKB_KEYMAP_FORMAT_TEXT_V1);keyboardText_=QByteArray(text);keyboardText_.append(char(0));free(text);xkb_keymap_unref(map);}xkb_context_unref(context);for(auto* keyboard:keyboards_)sendKeymap(keyboard);flush();}
 void malformedKeyboardMap(){keyboardText_=QByteArray("bad\0",4);for(auto* keyboard:keyboards_)sendKeymap(keyboard);flush();}
 void removeKeyboardSeat(){if(seatGlobal_){wl_global_destroy(seatGlobal_);seatGlobal_=nullptr;}flush();}
 bool ready()const{return display_&&error_.isEmpty();}QString error()const{return error_;}
 bool discovered()const{return !output_.native.isEmpty()&&!output_.logical.isEmpty();}
 void logical(QRect rect,bool complete){output_.rectangle=rect;for(auto* logical:output_.logical){sendLogical(logical);if(complete){if(version_>=3)wl_output_send_done(logical->native);else zxdg_output_v1_send_done(logical->resource);}}flush();}
 void deprecatedDone(){for(auto* logical:output_.logical)zxdg_output_v1_send_done(logical->resource);flush();}
 void nativeMode(QSize size,int scale){output_.mode=size;output_.scale=scale;for(auto* native:output_.native){wl_output_send_mode(native,WL_OUTPUT_MODE_CURRENT,size.width(),size.height(),60000);wl_output_send_scale(native,scale);}flush();}
 void nativeDone(){for(auto* native:output_.native)wl_output_send_done(native);flush();}
 void transformAwayAndBack(){for(auto* native:output_.native){output_.transform=1;sendGeometry(output_,native);output_.transform=0;sendGeometry(output_,native);wl_output_send_done(native);}flush();}
 void remove(){wl_global_destroy(output_.global);output_.global=nullptr;flush();}
 void disconnectClients(){wl_display_destroy_clients(display_);}
 void socketAvailable(bool available){environment_.set("WAYLAND_DISPLAY",available?"wayland-test":"wayland-missing");}
};

// Disposable-bus fixtures prove transport, ownership, and frozen identity. They
// cannot establish KWin desktop acceptance or actual capture permission.
class ScreenshotFixture final:public QObject,protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface","org.kde.KWin.ScreenShot2")
 Q_PROPERTY(uint Version READ version)
public:
 uint version()const{return 4;}
 QString windowId=frozenId;int captures=0;
public slots:
 QVariantMap CaptureActiveWindow(const QVariantMap&,const QDBusUnixFileDescriptor& descriptor){
  ++captures;const auto id=windowId;const int fd=dup(descriptor.fileDescriptor());const auto request=message();const auto bus=connection();setDelayedReply(true);
  QTimer::singleShot(30,this,[fd,request,bus,id]{
   QImage image(2,2,QImage::Format_ARGB32);image.fill(Qt::cyan);
   const auto written=write(fd,image.constBits(),image.sizeInBytes());Q_UNUSED(written);close(fd);
   const QVariantMap metadata{{"type","raw"},{"format",uint(image.format())},{"width",uint(2)},{"height",uint(2)},{"stride",uint(image.bytesPerLine())},{"scale",1.0},{"windowId",id}};
   bus.send(request.createReply(QVariant::fromValue(metadata)));
  });return {};
 }
};
class WindowInfoFixture final:public QObject,protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface","org.kde.KWin")
public:
 QStringList queries;QVariantMap info{{"uuid",frozenId},{"resourceClass","external-fixture"},{"desktopFile","external-fixture"}};
 bool fail=false;int delayMs=0;
public slots:
 QVariantMap getWindowInfo(const QString& id){
  queries.append(id);if(fail){sendErrorReply("org.kde.KWin.SyntheticFailure","Synthetic failure");return {};}
  if(delayMs){const auto request=message();const auto bus=connection();const auto frozen=info;setDelayedReply(true);QTimer::singleShot(delayMs,this,[request,bus,frozen]{bus.send(request.createReply(QVariant::fromValue(frozen)));});return {};}
  return info;
 }
};
class ShortcutFixture final:public QObject,protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface","org.kde.KGlobalAccel")
public:
 QHash<QString,QList<KdeKeySequence>> bindings;QSet<QString> active;
 int inactiveDelay=0,inactiveCalls=0,setterDelay=0,setterCalls=0;bool failInactive=false,rejectKeys=false;
public slots:
 void doRegister(const QStringList& id){active.insert(id[1]);}
 QList<putgd::KdeKeySequence> setShortcutKeys(const QStringList& id,const QList<putgd::KdeKeySequence>& keys,uint){
  ++setterCalls;if(rejectKeys){bindings[id[1]]={};active.remove(id[1]);return {};}bindings[id[1]]=keys;active.insert(id[1]);
  if(setterDelay){const auto request=message();const auto bus=connection();setDelayedReply(true);QTimer::singleShot(setterDelay,this,[request,bus,keys]{bus.send(request.createReply(QVariant::fromValue(keys)));});return {};}
  return keys;
 }
 void setInactive(const QStringList& id){
  ++inactiveCalls;
  if(failInactive){sendErrorReply("org.putgd.SyntheticFailure","Synthetic inactive failure");return;}
  if(!inactiveDelay){active.remove(id[1]);return;}
  const auto request=message();const auto bus=connection();const auto action=id[1];setDelayedReply(true);
  QTimer::singleShot(inactiveDelay,this,[this,request,bus,action]{active.remove(action);bus.send(request.createReply());});
 }
 QDBusObjectPath getComponent(const QString&){return QDBusObjectPath("/component/putgd");}
};
class KdeOwnershipTest final:public QObject {
 Q_OBJECT
 QTemporaryDir data_;
 ScreenshotFixture screenshot_;WindowInfoFixture info_;ShortcutFixture shortcuts_;
 static bool keyboardMap(KdeShortcuts& source,const char* layout="us"){
  auto* context=xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES);if(!context)return false;
  xkb_rule_names names{};names.layout=layout;auto* map=xkb_keymap_new_from_names(context,&names,XKB_KEYMAP_COMPILE_NO_FLAGS);
  if(map){source.setKeyboardMap(map);xkb_keymap_unref(map);}xkb_context_unref(context);return map!=nullptr;
 }
private slots:
 void initTestCase(){
  qDBusRegisterMetaType<putgd::KdeKeySequence>();qDBusRegisterMetaType<QList<putgd::KdeKeySequence>>();
  QVERIFY(data_.isValid());qputenv("XDG_DATA_HOME",data_.path().toUtf8());QVERIFY(QDir().mkpath(data_.filePath("applications")));
  QFile entry(data_.filePath("applications/putgd.desktop"));QVERIFY(entry.open(QIODevice::WriteOnly));
  entry.write("[Desktop Entry]\nExec=\""+QCoreApplication::applicationFilePath().toUtf8()+"\"\nX-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2;\n");entry.close();
  // A distinct connection forces the native wire path instead of Qt local-call dispatch.
  auto bus=QDBusConnection::connectToBus(QDBusConnection::SessionBus,"putgd-kde-ownership-fixture");QVERIFY(bus.isConnected());
  QVERIFY(bus.registerObject("/kglobalaccel",&shortcuts_,QDBusConnection::ExportAllSlots));QVERIFY(bus.registerService("org.kde.kglobalaccel"));
  QVERIFY(bus.registerObject("/org/kde/KWin/ScreenShot2",&screenshot_,QDBusConnection::ExportAllSlots|QDBusConnection::ExportAllProperties));QVERIFY(bus.registerService("org.kde.KWin.ScreenShot2"));
  QVERIFY(bus.registerObject("/KWin",&info_,QDBusConnection::ExportAllSlots));QVERIFY(bus.registerService("org.kde.KWin"));
 }
 void cleanupTestCase(){QDBusConnection::disconnectFromBus("putgd-kde-ownership-fixture");}
 void init(){screenshot_.captures=0;screenshot_.windowId=frozenId;info_.queries.clear();info_.info={{"uuid",frozenId},{"resourceClass","external-fixture"},{"desktopFile","external-fixture"}};info_.fail=false;info_.delayMs=0;shortcuts_.inactiveDelay=0;shortcuts_.setterDelay=0;shortcuts_.failInactive=false;shortcuts_.rejectKeys=false;}
 void dataControlFallsBackToWlrAndPrefersExt_data(){
  QTest::addColumn<bool>("ext");QTest::addColumn<bool>("wlr");QTest::addColumn<QString>("protocol");QTest::addColumn<int>("version");
  QTest::newRow("wlr only (KWin 6.3)")<<false<<true<<"wlr-data-control-unstable-v1"<<2;
  QTest::newRow("ext only")<<true<<false<<"ext-data-control-v1"<<1;
  QTest::newRow("both")<<true<<true<<"ext-data-control-v1"<<1;
 }
 void dataControlFallsBackToWlrAndPrefersExt(){
  QFETCH(bool,ext);QFETCH(bool,wlr);QFETCH(QString,protocol);QFETCH(int,version);
  NativeOutputFixture compositor(3,true,4,true);QVERIFY2(compositor.ready(),qPrintable(compositor.error()));compositor.dataControl(ext,wlr);
  DataControlClipboard clipboard;QTRY_VERIFY(clipboard.available());
  QCOMPARE(clipboard.status()["import"].toString(),protocol);QCOMPARE(clipboard.status()["protocol_version"].toInt(),version);
  bool done=false;Error result{"pending","",{}};clipboard.copy({"text","synthetic result",{},{}},[&](Error error){result=std::move(error);done=true;});
  QTRY_VERIFY(done);QVERIFY2(!result,qPrintable(result.code+": "+result.message));
  QVERIFY(compositor.selectionMimes().contains("text/plain;charset=utf-8"));QCOMPARE(clipboard.lastCopyRevision(),std::optional<quint64>(clipboard.revision()));
 }
 void noDataControlReportsBothProtocols(){
  NativeOutputFixture compositor(3,true,4,true);QVERIFY(compositor.ready());DataControlClipboard clipboard;
  QTRY_VERIFY(clipboard.status()["error"].toString().contains("wlr-data-control"));QVERIFY(!clipboard.available());QCOMPARE(clipboard.status()["import"].toString(),QString("none"));
  bool done=false;Error result;clipboard.copy({"text","synthetic result",{},{}},[&](Error error){result=std::move(error);done=true;});QVERIFY(done);QCOMPARE(result.code,QString("clipboard_unavailable"));
 }
 void keyboardMapDiscoveryReplacementAndRemoval(){
  NativeOutputFixture compositor(3,true,4,true);QVERIFY2(compositor.ready(),qPrintable(compositor.error()));DataControlClipboard clipboard;QSignalSpy changed(&clipboard,&DataControlClipboard::keyboardChanged);
  QTRY_VERIFY(clipboard.keyboardMap());QCOMPARE(xkb_keymap_num_layouts(clipboard.keyboardMap()),xkb_layout_index_t(1));
  // Holding a reference must remain safe across map replacement.
  auto* previous=xkb_keymap_ref(clipboard.keyboardMap());compositor.keyboardLayout("us,fr");QTRY_COMPARE(xkb_keymap_num_layouts(clipboard.keyboardMap()),xkb_layout_index_t(2));QCOMPARE(xkb_keymap_num_layouts(previous),xkb_layout_index_t(1));xkb_keymap_unref(previous);
  const auto count=changed.count();compositor.malformedKeyboardMap();QTRY_VERIFY(!clipboard.keyboardMap());QVERIFY(changed.count()>count);
  compositor.keyboardLayout("us");QTRY_VERIFY(clipboard.keyboardMap());compositor.removeKeyboardSeat();QTRY_VERIFY(!clipboard.keyboardMap());
 }
 void keyboardCapabilityCyclesReleaseLiveServerResources(){
  NativeOutputFixture compositor(3,true,4,true);QVERIFY(compositor.ready());DataControlClipboard clipboard;QTRY_VERIFY(clipboard.keyboardMap());QTRY_COMPARE(compositor.keyboardCount(),1);
  for(int cycle=0;cycle<3;++cycle){compositor.keyboardCapability(false);QTRY_VERIFY(!clipboard.keyboardMap());QTRY_COMPARE(compositor.keyboardCount(),0);compositor.keyboardCapability(true);QTRY_VERIFY(clipboard.keyboardMap());QTRY_COMPARE(compositor.keyboardCount(),1);}
 }
 void legacySeatStillDiscoversKeyboardMap(){NativeOutputFixture compositor(3,true,4,true,2);QVERIFY(compositor.ready());DataControlClipboard clipboard;QTRY_VERIFY(clipboard.keyboardMap());QCOMPARE(xkb_keymap_num_layouts(clipboard.keyboardMap()),xkb_layout_index_t(1));}
 void keyboardMapConnectionLossAndReconnect(){
  NativeOutputFixture compositor(3,true,4,true);QVERIFY2(compositor.ready(),qPrintable(compositor.error()));DataControlClipboard clipboard;QTRY_VERIFY(clipboard.keyboardMap());
  compositor.disconnectClients();QTRY_VERIFY(!clipboard.keyboardMap());QTRY_VERIFY(clipboard.keyboardMap());
 }
 void outputConnectionLossAndReconnectNotifyStatus(){
  NativeOutputFixture compositor;QVERIFY2(compositor.ready(),qPrintable(compositor.error()));KdeOutputs outputs;
  QTRY_VERIFY(outputs.status()["available"].toBool());QSignalSpy changed(&outputs,&KdeOutputs::changed);
  compositor.socketAvailable(false);compositor.disconnectClients();
  QTRY_COMPARE(outputs.status()["error"].toString(),QString("Native output connection is unavailable"));
  const auto lostRevision=outputs.revision();const auto lostSignals=changed.count();QVERIFY(lostSignals>0);
  QTRY_COMPARE(outputs.status()["error"].toString(),QString("Cannot connect to the Wayland compositor for output discovery"));
  QVERIFY(outputs.revision()>lostRevision);QVERIFY(changed.count()>lostSignals);QVERIFY(outputs.layout().isEmpty());
  const auto failedRevision=outputs.revision();const auto failedSignals=changed.count();
  QTest::qWait(1100);QCOMPARE(outputs.revision(),failedRevision);QCOMPARE(changed.count(),failedSignals);
  compositor.socketAvailable(true);QTRY_VERIFY(outputs.status()["available"].toBool());QVERIFY(outputs.revision()>failedRevision);
  QCOMPARE(outputs.layout().front().identity,QString("DP-1"));
 }
 void lowVersionOutputReportsProtocolNotOverflow(){
  NativeOutputFixture compositor(3,true,1);QVERIFY2(compositor.ready(),qPrintable(compositor.error()));KdeOutputs outputs;
  QTRY_COMPARE(outputs.status()["error"].toString(),QString("Compositor output protocol lacks wl_output v2 batch completion"));
  QVERIFY(outputs.layout().isEmpty());const auto revision=outputs.revision();QSignalSpy changed(&outputs,&KdeOutputs::changed);
  compositor.remove();QTRY_COMPARE(outputs.status()["error"].toString(),QString("Waiting for compositor outputs"));
  QVERIFY(outputs.revision()>revision);QVERIFY(changed.count()>0);
 }
 void nativeV3WaitsForWlDone(){
  NativeOutputFixture compositor;QVERIFY2(compositor.ready(),qPrintable(compositor.error()));KdeOutputs outputs;
  QTRY_VERIFY(outputs.status()["available"].toBool());const auto revision=outputs.revision();
  compositor.logical(QRect(-2560,0,2560,1440),false);QTRY_VERIFY(!outputs.status()["available"].toBool());QVERIFY(outputs.revision()>revision);
  compositor.deprecatedDone();QTest::qWait(20);QVERIFY(!outputs.status()["available"].toBool());
  compositor.nativeDone();QTRY_VERIFY(outputs.status()["available"].toBool());
  const auto frames=outputs.layout();QCOMPARE(frames.size(),1);QCOMPARE(frames[0].logicalRect,QRectF(-2560,0,2560,1440));QCOMPARE(frames[0].density,1.5);
 }
 void nativeV2CannotCompleteDuringPendingNativeBatch(){
  NativeOutputFixture compositor(2);QVERIFY2(compositor.ready(),qPrintable(compositor.error()));KdeOutputs outputs;
  QTRY_VERIFY(outputs.status()["available"].toBool());
  compositor.nativeMode(QSize(5120,2880),2);compositor.logical(QRect(0,0,3200,1800),true);
  QTRY_VERIFY(!outputs.status()["available"].toBool());compositor.nativeDone();QTRY_VERIFY(outputs.status()["available"].toBool());
  QCOMPARE(outputs.layout().front().logicalRect,QRectF(0,0,3200,1800));QCOMPARE(outputs.layout().front().density,1.6);
 }
 void nativeV2CannotReuseOldLogicalCompletion(){
  NativeOutputFixture compositor(2);QVERIFY2(compositor.ready(),qPrintable(compositor.error()));KdeOutputs outputs;
  QTRY_VERIFY(outputs.status()["available"].toBool());const auto revision=outputs.revision();
  compositor.nativeMode(QSize(5120,2880),2);compositor.nativeDone();
  QTRY_VERIFY(!outputs.status()["available"].toBool());QVERIFY(outputs.layout().isEmpty());QVERIFY(outputs.revision()>revision);
  compositor.logical(QRect(0,0,3200,1800),true);QTRY_VERIFY(outputs.status()["available"].toBool());
  QCOMPARE(outputs.layout().front().logicalRect,QRectF(0,0,3200,1800));QCOMPARE(outputs.layout().front().density,1.6);
 }
 void nativeV2UnchangedBatchesPreserveSnapshot(){
  NativeOutputFixture compositor(2);QVERIFY2(compositor.ready(),qPrintable(compositor.error()));KdeOutputs outputs;
  QTRY_VERIFY(outputs.status()["available"].toBool());const auto revision=outputs.revision();
  compositor.nativeMode(QSize(3840,2160),2);compositor.nativeDone();compositor.logical(QRect(0,0,2560,1440),true);
  QTest::qWait(20);QVERIFY(outputs.status()["available"].toBool());QCOMPARE(outputs.revision(),revision);
  QCOMPARE(outputs.layout().front().logicalRect,QRectF(0,0,2560,1440));QCOMPARE(outputs.layout().front().density,1.5);
 }
 void nativeRemovalAndAwayBackInvalidateSnapshots(){
  NativeOutputFixture compositor;QVERIFY2(compositor.ready(),qPrintable(compositor.error()));KdeOutputs outputs;
  QTRY_VERIFY(outputs.status()["available"].toBool());const auto before=outputs.layout();auto revision=outputs.revision();
  compositor.transformAwayAndBack();QTRY_VERIFY(outputs.revision()>revision);QTRY_VERIFY(outputs.status()["available"].toBool());
  QCOMPARE(outputs.layout().front().logicalRect,before.front().logicalRect);QCOMPARE(outputs.layout().front().rotation,before.front().rotation);
  revision=outputs.revision();compositor.remove();QTRY_VERIFY(outputs.layout().isEmpty());QVERIFY(outputs.revision()>revision);QVERIFY(!outputs.status()["available"].toBool());
 }
 void nativeOutputRequiresCurrentMode(){
  NativeOutputFixture compositor(3,false);QVERIFY2(compositor.ready(),qPrintable(compositor.error()));KdeOutputs outputs;
  QTRY_VERIFY(compositor.discovered());QVERIFY(!outputs.status()["available"].toBool());
  compositor.nativeMode(QSize(3840,2160),2);compositor.nativeDone();QTRY_VERIFY(outputs.status()["available"].toBool());
 }
 void coreCapabilitiesRemainValidWhileOutputsUnavailable(){
  NativeOutputFixture compositor(3,false);QVERIFY2(compositor.ready(),qPrintable(compositor.error()));KdeSourceAdapter source;
  QTRY_VERIFY(compositor.discovered());QTRY_VERIFY(source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());
  for(int i=0;i<100;++i){
   const QJsonObject capabilities=source.capabilities();const auto capture=capabilities["capture"].toObject();
   QCOMPARE(capabilities["resident"].toString(),QString("qt-core"));QVERIFY(!capabilities["outputs"].toObject()["available"].toBool());
   QVERIFY(!capture["region"].toObject()["available"].toBool());QVERIFY(!capture["desktop"].toObject()["available"].toBool());
   QVERIFY(!capture["desktop"].toObject()["error"].toString().isEmpty());QVERIFY(capture["window"].toObject()["available"].toBool());
   const QJsonObject helper=capture["region"].toObject()["helper"].toObject();
   QVERIFY(helper["active"].isBool());QVERIFY(!helper["active"].toBool());QVERIFY(helper["processId"].isDouble());QCOMPARE(helper["processId"].toInteger(),qint64(0));
   QCOMPARE(helper,capabilities["captureHelper"].toObject());
  }
  compositor.nativeMode(QSize(3840,2160),2);compositor.nativeDone();
  QTRY_VERIFY(source.capabilities()["capture"].toObject()["desktop"].toObject()["available"].toBool());
 }
 void coreApplicationOwnsNoGraphicalConnection(){QVERIFY(!qobject_cast<QGuiApplication*>(QCoreApplication::instance()));}
 void nativeLogicalGeometryPreservesFractionalScaleAndRotation(){
  QString error;
  const KdeOutputs::Metadata fractional{"DP-1",QRect(-1920,0,2560,1440),QSize(3840,2160),0,2};
  const KdeOutputs::Metadata rotated{"DP-2",QRect(640,0,1080,1920),QSize(1920,1080),1,1};
  auto frames=KdeOutputs::framesFromMetadata({rotated,fractional},&error);
  QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(frames.size(),2);
  QCOMPARE(frames[0].identity,QString("DP-1"));QCOMPARE(frames[0].logicalRect,QRectF(fractional.logical));QCOMPARE(frames[0].density,1.5);
  QCOMPARE(frames[0].nativeRect.size(),QSize(3840,2160));QCOMPARE(frames[1].logicalRect,QRectF(rotated.logical));
  QCOMPARE(frames[1].nativeRect.size(),QSize(1080,1920));QCOMPARE(frames[1].rotation,90);
 }
 void nativeOutputMetadataFailsClosed(){
  const KdeOutputs::Metadata valid{"DP-1",QRect(0,0,1920,1080),QSize(1920,1080),0,1};
  for(int mode=0;mode<7;++mode){
   auto altered=valid;QList<KdeOutputs::Metadata> outputs;
   if(mode==0)altered.name.clear();
   if(mode==1)altered.logical.setWidth(0);
   if(mode==2)altered.transform=8;
   if(mode==3)altered.scale=0;
   if(mode==4)altered.mode.setHeight(0);
   if(mode==5)altered.logical.moveLeft(1000001);
   outputs.append(altered);if(mode==6)outputs.append(valid);
   QString error;QVERIFY(KdeOutputs::framesFromMetadata(outputs,&error).isEmpty());QVERIFY(!error.isEmpty());
  }
 }
 void fullOutputCompositionSharesImmutablePixels(){
  CaptureFrame frame;frame.identity="DP-1";frame.logicalRect=QRectF(-10.25,5.5,64,32);
  frame.nativeRect=QRect(-20,11,128,64);frame.density=2;frame.rotation=1;
  frame.pixels=QImage(128,64,QImage::Format_ARGB32);frame.pixels.fill(Qt::red);frame.pixels.setDevicePixelRatio(2);
  Error error;auto result=composeKdeFrames({frame},frame.logicalRect,128*64,error);
  QVERIFY(!error);QCOMPARE(result.constBits(),frame.pixels.constBits());
  QCOMPARE(result.devicePixelRatio(),qreal(2));QCOMPARE(result.pixelColor(30,20),QColor(Qt::red));
  frame.pixels.fill(Qt::blue);QCOMPARE(result.pixelColor(30,20),QColor(Qt::red));
  QCOMPARE(frame.pixels.pixelColor(30,20),QColor(Qt::blue));
 }
 void fullOutputOptimizationPreservesGeometryAndBudgetFailures(){
  CaptureFrame frame;frame.identity="DP-1";frame.logicalRect=QRectF(0,0,64,32);
  frame.nativeRect=QRect(0,0,64,32);frame.pixels=QImage(64,32,QImage::Format_ARGB32);frame.pixels.fill(Qt::green);
  for(int variant=0;variant<8;++variant){
   auto candidate=frame;auto selection=frame.logicalRect;auto budget=qint64(64*32);
   if(variant==0)--budget;
   if(variant==1)candidate.identity.clear();
   if(variant==2)candidate.density=0;
   if(variant==3)candidate.nativeRect.setWidth(63);
   if(variant==4)selection.setWidth(32);
   if(variant==5)selection.setWidth(70);
   if(variant==6){candidate.logicalRect.moveLeft(-10.25);selection=candidate.logicalRect;candidate.logicalRect.setWidth(42.7);selection=candidate.logicalRect;}
   if(variant==7)candidate.pixels={};
   Error expectedError,actualError;
   const auto expected=composeFrames({candidate},selection,budget,expectedError);
   const auto actual=composeKdeFrames({candidate},selection,budget,actualError);
   QCOMPARE(actualError.code,expectedError.code);QCOMPARE(actual.size(),expected.size());QCOMPARE(actual,expected);
  }
 }
 void pngSignatureHintUsesActualBytes(){
  const auto png=QByteArray::fromHex("89504e470d0a1a0a");
  QCOMPARE(imageFormatHint(png),QByteArray("png"));
  QCOMPARE(imageFormatHint(png+QByteArray("body")),QByteArray("png"));
  for(qsizetype size=0;size<png.size();++size)QVERIFY(imageFormatHint(QByteArrayView(png).first(size)).isEmpty());
  auto altered=png;altered[7]='x';QVERIFY(imageFormatHint(altered).isEmpty());
  QVERIFY(imageFormatHint(QByteArray::fromHex("ffd8ffe000104a464946")).isEmpty());
 }
 void standardTextAndUriOffersSkipImageDiscovery(){
  int discoveries=0;const auto discover=[&]{++discoveries;return QList<QByteArray>{"image/jpeg","application/x-plugin-image"};};
  const QStringList offered{"UTF8_STRING","text/plain","text/uri-list","text/plain;charset=utf-8"};
  QCOMPARE(kdeClipboardImportFormats(offered,discover),QStringList({"text/uri-list","text/plain;charset=utf-8","text/plain","UTF8_STRING"}));
  QCOMPARE(discoveries,0);
  QVERIFY(kdeClipboardImportFormats({},discover).isEmpty());QCOMPARE(discoveries,0);
 }
 void pngPrioritySkipsImageDiscoveryDespiteAlternativeOffers(){
  int discoveries=0;const auto discover=[&]{++discoveries;return QList<QByteArray>{"image/jpeg","image/png","application/x-plugin-image"};};
  const QStringList offered{"image/jpeg","text/plain","application/x-plugin-image","image/png","text/uri-list"};
  QCOMPARE(kdeClipboardImportFormats(offered,discover),QStringList({"text/uri-list","image/png","text/plain"}));
  QCOMPARE(discoveries,0);
  // A URI list without local files advances to PNG; terminal PNG errors never
  // fall back to another image, matching the production importer contract.
  QCOMPARE(kdeClipboardImportFormats({"image/png"},discover),QStringList({"image/png"}));QCOMPARE(discoveries,0);
 }
 void customImageAliasesRetainDiscoveryAndOriginalOrder(){
  int discoveries=0;const auto discover=[&]{++discoveries;return QList<QByteArray>{"image/webp","application/x-plugin-image","image/jpeg","image/png"};};
  const QStringList offered{"image/jpeg","text/plain","application/x-plugin-image","image/webp","text/uri-list"};
  QCOMPARE(kdeClipboardImportFormats(offered,discover),QStringList({"text/uri-list","image/webp","application/x-plugin-image","image/jpeg","text/plain"}));QCOMPARE(discoveries,1);
  QCOMPARE(kdeClipboardImportFormats({"application/x-plugin-image","text/plain"},discover),QStringList({"application/x-plugin-image","text/plain"}));QCOMPARE(discoveries,2);
 }
 void unknownNonImageOffersKeepDiscoveryFallback(){
  int discoveries=0;const auto discover=[&]{++discoveries;return QList<QByteArray>{"application/x-unknown"};};
  QCOMPARE(kdeClipboardImportFormats({"application/x-unknown","text/plain"},discover),QStringList({"application/x-unknown","text/plain"}));QCOMPARE(discoveries,1);
 }
 void recordingWaitsForNativeInactiveAndRestoresLatestBindings(){
  KdeShortcuts source;QVERIFY(keyboardMap(source));source.bind({{"window","Ctrl+1"}});QTRY_VERIFY(source.status()["window"].toObject()["bound"].toBool());QVERIFY(shortcuts_.active.contains("window"));
  shortcuts_.inactiveDelay=50;bool done=false;Error error;source.setRecording(true,[&](Error value){error=value;done=true;});QVERIFY(!done);QVERIFY(shortcuts_.active.contains("window"));QTRY_VERIFY(done);QVERIFY(!error);QVERIFY(!shortcuts_.active.contains("window"));
  source.bind({{"window","Ctrl+2"}});QVERIFY(keyboardMap(source,"de"));QTest::qWait(50);QVERIFY(!shortcuts_.active.contains("window"));QCOMPARE(source.status()["window"].toObject()["requested"].toString(),QString("Ctrl+2"));
  shortcuts_.inactiveDelay=0;done=false;source.setRecording(false,[&](Error value){error=value;done=true;});QTRY_VERIFY(done);QVERIFY(!error);QVERIFY(shortcuts_.active.contains("window"));
  QCOMPARE(shortcuts_.bindings["window"].first().keys.first(),QKeySequence("Ctrl+2")[0].toCombined());
 }
 void recordingRejectsNativeReleaseFailure(){
  KdeShortcuts source;QVERIFY(keyboardMap(source));source.bind({{"window","Ctrl+1"}});QTRY_VERIFY(source.status()["window"].toObject()["bound"].toBool());
  shortcuts_.failInactive=true;bool done=false;Error error;source.setRecording(true,[&](Error value){error=value;done=true;});QTRY_VERIFY(done);QCOMPARE(error.code,QString("shortcut_recording_failed"));
  shortcuts_.failInactive=false;done=false;source.setRecording(false,[&](Error value){error=value;done=true;});QTRY_VERIFY(done);QVERIFY(!error);
 }
 void recordingSurvivesShortcutServiceRestart(){
  KdeShortcuts source;QVERIFY(keyboardMap(source));source.bind({{"window","Ctrl+1"}});QTRY_VERIFY(source.available());QTRY_VERIFY(source.status()["window"].toObject()["bound"].toBool());
  bool done=false;source.setRecording(true,[&](Error error){QVERIFY(!error);done=true;});QTRY_VERIFY(done);
  auto bus=QDBusConnection("putgd-kde-ownership-fixture");QVERIFY(bus.unregisterService("org.kde.kglobalaccel"));QTRY_VERIFY(!source.available());
  // A replacement daemon starts without the previous daemon's registrations.
  shortcuts_.active.clear();shortcuts_.bindings.clear();source.bind({{"window","Ctrl+2"}});const auto inactive=shortcuts_.inactiveCalls;
  QVERIFY(bus.registerService("org.kde.kglobalaccel"));QTRY_VERIFY(shortcuts_.inactiveCalls>inactive);QVERIFY(shortcuts_.active.isEmpty());QCOMPARE(source.status()["window"].toObject()["state"].toString(),QString("recording"));
  done=false;source.setRecording(false,[&](Error error){QVERIFY(!error);done=true;});QTRY_VERIFY(done);QVERIFY(shortcuts_.active.contains("window"));QCOMPARE(shortcuts_.bindings["window"].first().keys.first(),QKeySequence("Ctrl+2")[0].toCombined());
 }
 void recordingRejectsStaleServiceReply_data(){
  QTest::addColumn<bool>("restoring");QTest::newRow("pending-pause")<<false;QTest::newRow("pending-restore")<<true;
 }
 void recordingRejectsStaleServiceReply(){
  QFETCH(bool,restoring);KdeShortcuts source;QVERIFY(keyboardMap(source));source.bind({{"window","Ctrl+1"}});QTRY_VERIFY(source.status()["window"].toObject()["bound"].toBool());
  QSignalSpy activated(&source,&KdeShortcuts::activated);
  if(restoring){bool paused=false;source.setRecording(true,[&](Error error){QVERIFY(!error);paused=true;});QTRY_VERIFY(paused);shortcuts_.setterDelay=300;}
  else shortcuts_.inactiveDelay=300;
  const auto calls=restoring?shortcuts_.setterCalls:shortcuts_.inactiveCalls;int completed=0;Error result;
  source.setRecording(!restoring,[&](Error error){result=error;++completed;});
  QTRY_VERIFY((restoring?shortcuts_.setterCalls:shortcuts_.inactiveCalls)>calls);QCOMPARE(completed,0);
  auto bus=QDBusConnection("putgd-kde-ownership-fixture");QVERIFY(bus.unregisterService("org.kde.kglobalaccel"));QTRY_VERIFY(!source.available());
  shortcuts_.active.clear();shortcuts_.bindings.clear();shortcuts_.inactiveDelay=0;shortcuts_.setterDelay=0;
  QVERIFY(bus.registerService("org.kde.kglobalaccel"));QTRY_COMPARE(completed,1);QCOMPARE(result.code,QString("shortcut_recording_interrupted"));
  if(!restoring){QVERIFY(shortcuts_.active.isEmpty());QCOMPARE(source.status()["window"].toObject()["state"].toString(),QString("recording"));bool resumed=false;source.setRecording(false,[&](Error error){QVERIFY(!error);resumed=true;});QTRY_VERIFY(resumed);}
  QTRY_VERIFY(source.status()["window"].toObject()["bound"].toBool());
  auto signal=QDBusMessage::createSignal("/component/putgd","org.kde.kglobalaccel.Component","globalShortcutPressed");signal.setArguments({QString("putgd"),QString("window"),QVariant::fromValue(qlonglong(1))});
  // Retry signal delivery only until the asynchronous component lookup reconnects.
  QTRY_VERIFY(([&]{bus.send(signal);return activated.count()>0;})());QTest::qWait(30);QCOMPARE(completed,1);
 }
 void recordingReportsConflictingRestore(){
  KdeShortcuts source;QVERIFY(keyboardMap(source));source.bind({{"window","Ctrl+1"}});QTRY_VERIFY(source.status()["window"].toObject()["bound"].toBool());
  bool done=false;source.setRecording(true,[&](Error error){QVERIFY(!error);done=true;});QTRY_VERIFY(done);
  shortcuts_.rejectKeys=true;done=false;Error result;source.setRecording(false,[&](Error error){result=error;done=true;});QTRY_VERIFY(done);shortcuts_.rejectKeys=false;
  QCOMPARE(result.code,QString("shortcut_conflict"));QVERIFY(!source.status()["window"].toObject()["bound"].toBool());QVERIFY(!shortcuts_.active.contains("window"));
 }
 void externalWindowCaptureRemainsNative(){
  KdeSourceAdapter source;QTRY_VERIFY(source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());source.setUiProcess(424242);
  auto prepared=source.prepareAcquisition("window");
  bool done=false;PayloadResult result;prepared({},[&](PayloadResult r){result=std::move(r);done=true;});QTRY_VERIFY(done);
  QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(screenshot_.captures,1);QCOMPARE(result.payload.image.pixelColor(0,0),QColor(Qt::cyan));
 }
 void helperWindowRejectedAfterHelperExit(){
  KdeSourceAdapter source;QTRY_VERIFY(source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());source.setUiProcess(424242);
  info_.info["resourceClass"]="putgd-ui";auto prepared=source.prepareAcquisition("window");source.setUiProcess(0);
  bool done=false;PayloadResult result;prepared({},[&](PayloadResult r){result=std::move(r);done=true;});QTRY_VERIFY(done);
  QCOMPARE(result.error.code,QString("target_unavailable"));QVERIFY(result.payload.image.isNull());QCOMPARE(screenshot_.captures,1);QCOMPARE(info_.queries,QStringList{frozenId});
 }
 void frozenUuidQueriedAfterFocusChanges(){
  KdeSourceAdapter source;QTRY_VERIFY(source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());source.setUiProcess(424242);
  auto prepared=source.prepareAcquisition("window");QTRY_COMPARE(screenshot_.captures,1);screenshot_.windowId=otherId;
  bool done=false;PayloadResult result;prepared({},[&](PayloadResult r){result=std::move(r);done=true;});QTRY_VERIFY(done);
  QVERIFY(!result.error);QCOMPARE(info_.queries,QStringList{frozenId});QCOMPARE(screenshot_.captures,1);
 }
 void missingWrongAndUnavailableIdentityAreRejected(){
  for(int mode=0;mode<4;++mode){
   info_.queries.clear();info_.info={{"uuid",frozenId},{"resourceClass","external-fixture"},{"desktopFile","external-fixture"}};info_.fail=mode==3;
   if(mode==0)info_.info={};
   if(mode==1)info_.info["uuid"]=otherId;
   if(mode==2)info_.info["desktopFile"]="putgd-ui";
   KdeSourceAdapter source;QTRY_VERIFY(source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());source.setUiProcess(424242);
   bool done=false;PayloadResult result;source.acquire("window",{},[&](PayloadResult r){result=std::move(r);done=true;});QTRY_VERIFY(done);
   QCOMPARE(result.error.code,QString("target_unavailable"));QVERIFY(result.payload.image.isNull());QCOMPARE(info_.queries,QStringList{frozenId});
  }
 }
 void emptyExternalAppIdIsAccepted(){
  KdeSourceAdapter source;QTRY_VERIFY(source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());source.setUiProcess(424242);
  info_.info["resourceClass"]=QString{};info_.info["desktopFile"]=QString{};
  bool done=false;PayloadResult result;source.acquire("window",{},[&](PayloadResult r){result=std::move(r);done=true;});QTRY_VERIFY(done);
  QVERIFY(!result.error);QCOMPARE(info_.queries,QStringList{frozenId});
 }
 void cancellationDuringIdentityLookupCompletesOnce(){
  KdeSourceAdapter source;QTRY_VERIFY(source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());source.setUiProcess(424242);info_.delayMs=200;
  int completed=0;PayloadResult result;source.acquire("window",{},[&](PayloadResult r){result=std::move(r);++completed;});QTRY_COMPARE(info_.queries.size(),1);QCOMPARE(completed,0);
  source.cancelAcquisition();QTRY_COMPARE(completed,1);QCOMPARE(result.error.code,QString("cancelled"));QVERIFY(result.payload.image.isNull());QTest::qWait(30);QCOMPARE(completed,1);
 }
 void noHelperDoesNotQueryWindowInfo(){
  KdeSourceAdapter source;QTRY_VERIFY(source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());source.setUiProcess(424242);source.setUiProcess(0);info_.fail=true;
  bool done=false;PayloadResult result;source.acquire("window",{},[&](PayloadResult r){result=std::move(r);done=true;});QTRY_VERIFY(done);
  QVERIFY(!result.error);QVERIFY(info_.queries.isEmpty());
 }
};
QTEST_GUILESS_MAIN(KdeOwnershipTest)
#include "test_kde_ownership.moc"
