#include "platform/kde/DataControlClipboard.h"
#include "../fixtures/ImagePluginMappings.h"
#include <QtTest>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <wayland-server.h>
#include "ext-data-control-v1-server-protocol.h"
#include <sys/stat.h>
#include <unistd.h>
using namespace putgd;
using test::loadedImagePlugins;
// A private in-process compositor that owns one clipboard selection. It proves
// only the data-control import path, not Plasma integration.
class DataControlFixture final:public QObject {
 QTemporaryDir directory_{"/tmp/pgk-XXXXXX"};wl_display* display_=nullptr;QSocketNotifier* notifier_=nullptr;
 QList<QPair<QByteArray,QByteArray>> formats_;
 static DataControlFixture* self(wl_resource* resource){return static_cast<DataControlFixture*>(wl_resource_get_user_data(resource));}
 static void destroyRequest(wl_client*,wl_resource* resource){wl_resource_destroy(resource);}
 static void receive(wl_client*,wl_resource* offer,const char* mime,int32_t fd){
  auto* fixture=self(offer);fixture->requested.append(mime);
  for(const auto& [type,bytes]:fixture->formats_)if(type==mime){for(qsizetype offset=0;offset<bytes.size();){const auto n=::write(fd,bytes.constData()+offset,size_t(bytes.size()-offset));if(n<=0)break;offset+=n;}break;}
  ::close(fd);
 }
 static void getDevice(wl_client* client,wl_resource* manager,uint32_t id,wl_resource*){
  auto* fixture=self(manager);const auto version=wl_resource_get_version(manager);
  auto* device=wl_resource_create(client,&ext_data_control_device_v1_interface,version,id);
  static const struct ext_data_control_device_v1_interface deviceImplementation={[](wl_client*,wl_resource*,wl_resource*){},destroyRequest,[](wl_client*,wl_resource*,wl_resource*){}};
  wl_resource_set_implementation(device,&deviceImplementation,fixture,nullptr);
  auto* offer=wl_resource_create(client,&ext_data_control_offer_v1_interface,version,0);
  static const struct ext_data_control_offer_v1_interface offerImplementation={receive,destroyRequest};
  wl_resource_set_implementation(offer,&offerImplementation,fixture,nullptr);
  ext_data_control_device_v1_send_data_offer(device,offer);
  for(const auto& format:fixture->formats_)ext_data_control_offer_v1_send_offer(offer,format.first.constData());
  ext_data_control_device_v1_send_selection(device,offer);
 }
 static void createSource(wl_client* client,wl_resource* manager,uint32_t id){
  auto* source=wl_resource_create(client,&ext_data_control_source_v1_interface,wl_resource_get_version(manager),id);
  static const struct ext_data_control_source_v1_interface implementation={[](wl_client*,wl_resource*,const char*){},destroyRequest};
  wl_resource_set_implementation(source,&implementation,nullptr,nullptr);
 }
 static void bindManager(wl_client* client,void* data,uint32_t version,uint32_t id){
  auto* manager=wl_resource_create(client,&ext_data_control_manager_v1_interface,int(version),id);
  static const struct ext_data_control_manager_v1_interface implementation={createSource,getDevice,destroyRequest};
  wl_resource_set_implementation(manager,&implementation,data,nullptr);
 }
 static void bindSeat(wl_client* client,void*,uint32_t version,uint32_t id){
  auto* seat=wl_resource_create(client,&wl_seat_interface,int(version),id);
  static const struct wl_seat_interface implementation={nullptr,nullptr,nullptr,destroyRequest};
  wl_resource_set_implementation(seat,&implementation,nullptr,nullptr);wl_seat_send_capabilities(seat,0);
 }
public:
 QString error;QList<QByteArray> requested;
 explicit DataControlFixture(QList<QPair<QByteArray,QByteArray>> formats):formats_(std::move(formats)){
  struct stat metadata{};
  if(!directory_.isValid()||lstat(directory_.path().toUtf8().constData(),&metadata)<0||!S_ISDIR(metadata.st_mode)||metadata.st_uid!=geteuid()||(metadata.st_mode&077)!=0){error="Fixture runtime directory is not private and owned";return;}
  if(directory_.filePath("wayland-test").toUtf8().size()>107){error="Fixture Unix socket exceeds Linux byte limit";return;}
  qputenv("XDG_RUNTIME_DIR",directory_.path().toUtf8());qputenv("WAYLAND_DISPLAY","wayland-test");
  display_=wl_display_create();if(!display_||wl_display_add_socket(display_,"wayland-test")<0){error="Cannot bind private data-control fixture socket";return;}
  wl_global_create(display_,&wl_seat_interface,5,this,bindSeat);
  wl_global_create(display_,&ext_data_control_manager_v1_interface,1,this,bindManager);
  auto* loop=wl_display_get_event_loop(display_);notifier_=new QSocketNotifier(wl_event_loop_get_fd(loop),QSocketNotifier::Read,this);
  connect(notifier_,&QSocketNotifier::activated,this,[this,loop]{wl_event_loop_dispatch(loop,0);wl_display_flush_clients(display_);});
 }
 ~DataControlFixture() override{if(notifier_){notifier_->setEnabled(false);delete notifier_;}if(display_){wl_display_destroy_clients(display_);wl_display_destroy(display_);}}
};
// The KDE Core resident decodes PNG offers in process with format detection
// off; other images use the transient image job. Qt 6.4.2 and 6.8.2 load
// every installed image plugin for a "png"-named reader unless detection is off.
class KdeClipboardImportTest final:public QObject {
 Q_OBJECT
 static bool importFrom(DataControlFixture& owner,PayloadResult& result){
  if(!owner.error.isEmpty())return false;
  DataControlClipboard importer;if(!QTest::qWaitFor([&]{return importer.available();},5000))return false;
  bool done=false;importer.acquire({},[&](PayloadResult value){result=std::move(value);done=true;},importer.revision());
  return QTest::qWaitFor([&]{return done;},20000);
 }
private slots:
 void initTestCase(){
  // A synthetic plugin is always installed, so an in-process plugin load is
  // observable even where the system ships no image plugins.
  QCoreApplication::addLibraryPath(QStringLiteral(PUTGD_IMAGE_PLUGIN_FIXTURE_DIR));
  qputenv("XDG_SESSION_TYPE","wayland");qputenv("XDG_CURRENT_DESKTOP","KDE");qunsetenv("QT_QPA_PLATFORM");
  QCOMPARE(loadedImagePlugins(),QStringList{});
 }
 void cleanup(){QCOMPARE(loadedImagePlugins(),QStringList{});}
 void pngDecodesInProcessWithoutPlugins(){
  DataControlFixture owner({{"image/bmp",test::syntheticBmp},{"image/png",test::syntheticPng},{"text/plain","synthetic caption"}});
  PayloadResult result;QVERIFY2(importFrom(owner,result),qPrintable(owner.error));QVERIFY2(!result.error,qPrintable(result.error.code));
  QCOMPARE(result.payload.kind,QString("image"));QVERIFY(test::syntheticPixels(result.payload.image));
  QCOMPARE(owner.requested,QList<QByteArray>{"image/png"});
 }
 void claimedPngDecodesInImageJob(){
  // The image job child always uses Qt's offscreen platform, never the fixture.
  DataControlFixture owner({{"image/png",test::syntheticBmp}});
  PayloadResult result;QVERIFY2(importFrom(owner,result),qPrintable(owner.error));QVERIFY2(!result.error,qPrintable(result.error.code));
  QVERIFY(test::syntheticPixels(result.payload.image));QCOMPARE(owner.requested,QList<QByteArray>{"image/png"});
 }
};
QTEST_GUILESS_MAIN(KdeClipboardImportTest)
#include "test_kde_clipboard_import.moc"
