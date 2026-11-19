#include "SettingsGuiFixture.h"
#include <QThread>
#include <QRandomGenerator>
// Destination edits, credentials and routes stay with the destination they belong to.
class SettingsGui:public SettingsGuiFixture {
 Q_OBJECT
private slots:
 // Moving a destination keeps its unstored credential input and every action's route.
 void reorderingDestinationsKeepsSelectionEditsAndRouting(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  auto routing=[&]{auto doc=service.document();doc.remove("profiles");return doc;};const auto routes=routing();
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* list=window.findChild<QListWidget*>("destinationList");auto* down=window.findChild<QAction*>("moveDestinationDown");QVERIFY(list&&down);
  list->setCurrentRow(1);auto* access=window.findChild<QLineEdit*>("credentialAccessKey");access->setText("SYNTHETICACCESSKEY");
  down->trigger();
  QCOMPARE(list->currentRow(),2);QCOMPARE(window.findChild<QLineEdit*>("destinationId")->text(),QString("synthetic-alternate"));QCOMPARE(access->text(),QString("SYNTHETICACCESSKEY"));
  PUTGD_TRY_COMPARE(profileIds(service.document()),QStringList({"development-s3","synthetic-third","synthetic-alternate"}));
  QCOMPARE(routing(),routes);QCOMPARE(service.snapshot().actions["clipboard"].profileId,QString("synthetic-third"));
 }
 // #1675: Test upload and link refused for a saved Region replaces its progress line with the
 // refusal, also while the footer was showing that Region's message; it never stays stuck.
 void refusedTestReplacesItsProgressLine(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);
  auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();auto fields=profile["settings"].toObject();fields["region"]="fips-us-east-1";profile["settings"]=fields;profiles[0]=profile;
  auto valid=profiles[0].toObject();valid["id"]="valid-destination";valid["label"]="Valid destination";auto validFields=valid["settings"].toObject();validFields["region"]="us-east-2";valid["settings"]=validFields;profiles.append(valid);doc["profiles"]=profiles;
  QFile file(dir.filePath("config.json"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(doc).toJson());file.close();
  SettingsService service(file.fileName(),[&](auto,auto value){return provider.validateSettings(value);},nullptr,{},[&](auto,auto value){return provider.validateStoredSettings(value);});
  UiSessionFixture fixture(service,secrets,[&](const QString&,const QJsonObject& profile,auto reply){reply(provider.validateSettings(profile["settings"].toObject()));});
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* status=window.findChild<QLabel*>("destinationStatus");QVERIFY(status);
  PUTGD_TRY_COMPARE(status->property("errorField").toString(),QString("region"));
  window.findChild<QPushButton*>("testDestination")->click();
  PUTGD_TRY_VERIFY(!status->text().contains("Testing upload")&&status->text().contains("fips"));
  // Showing a valid destination clears that refusal from both footers.
  auto* footer=window.findChild<QLabel*>("settingsStatus");QVERIFY(footer);QVERIFY(footer->text().contains("fips"));
  window.findChild<QListWidget*>("destinationList")->setCurrentRow(1);
  PUTGD_TRY_VERIFY(!status->text().contains("fips")&&!footer->text().contains("fips"));
 }
 // #1676: a destination saved with a Region and an Endpoint that today's rules both refuse.
 // Fixing Region, then Endpoint, saves both: neither fix is refused because of the other's
 // saved value and then left looking saved while uploads keep using the old pair.
 void regionAndEndpointSaveAsOnePair(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);
  auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();auto fields=profile["settings"].toObject();
  fields["endpoint"]="https://storage.example.net/my-bucket";fields["region"]="Western North America (WNAM)";profile["settings"]=fields;profiles[0]=profile;doc["profiles"]=profiles;
  QFile file(dir.filePath("config.json"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(doc).toJson());file.close();
  SettingsService service(file.fileName(),[&](auto,auto value){return provider.validateSettings(value);},nullptr,{},[&](auto,auto value){return provider.validateStoredSettings(value);});
  const auto saved=[&]{return service.document()["profiles"].toArray()[0].toObject()["settings"].toObject();};QCOMPARE(saved()["region"].toString(),QString("Western North America (WNAM)"));
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* region=window.findChild<QLineEdit*>("s3Region");auto* endpoint=window.findChild<QLineEdit*>("s3Endpoint");QVERIFY(region&&endpoint);
  region->setText("us-east-1");QMetaObject::invokeMethod(region,"editingFinished");
  // The Region save is tried and refused on the saved Endpoint path before Endpoint changes.
  PUTGD_VERIFY_STEADY(saved()["region"]=="Western North America (WNAM)",1500);
  endpoint->setText("https://storage.example.net");QMetaObject::invokeMethod(endpoint,"editingFinished");
  PUTGD_TRY_VERIFY(saved()["region"]=="us-east-1"&&saved()["endpoint"]=="https://storage.example.net");
 }
 // #1676: a Region edit on a destination that isn't saved yet saves with the whole new
 // destination, never as a pair on its own that would drop it.
 void newDestinationThenRegionSavesBoth(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  QAction* add=nullptr;for(auto* action:window.findChildren<QAction*>())if(action->text()=="Amazon S3 / S3-compatible")add=action;QVERIFY(add);
  // Add destination asks for a name first.
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget))dialog->accept();});add->trigger();
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");auto* region=window.findChild<QLineEdit*>("s3Region");QVERIFY(bucket&&region);
  bucket->setText("new-bucket");QMetaObject::invokeMethod(bucket,"editingFinished");
  region->setText("eu-west-1");QMetaObject::invokeMethod(region,"editingFinished");
  PUTGD_TRY_VERIFY(service.document()["profiles"].toArray().size()==2&&service.document()["profiles"].toArray()[1].toObject()["settings"].toObject()["bucket"]=="new-bucket"&&service.document()["profiles"].toArray()[1].toObject()["settings"].toObject()["region"]=="eu-west-1");
 }
 // #1676: a Region edit right after a provider change saves with the whole changed destination.
 void providerChangeThenRegionSavesBoth(){
  QTemporaryDir dir;CredentialStore secrets;ProviderRegistry registry;registry.add("s3",std::make_shared<S3Provider>(secrets));registry.add("custom-http",std::make_shared<HttpUploaderProvider>(secrets));
  SettingsService service(dir.filePath("config.json"),[&](auto id,auto values){return registry.validate(id,values);},nullptr,[&](auto id,auto reference){return registry.validateCredentials(id,reference);});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  UiSessionFixture fixture(service,secrets);fixture.setup.setNamedCredentials(http::namedCredentials);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  const auto saved=[&]{return service.document()["profiles"].toArray()[0].toObject();};
  auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("custom-http"));
  PUTGD_TRY_COMPARE(saved()["provider"].toString(),QString("custom-http"));
  provider->setCurrentIndex(provider->findData("s3"));
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");auto* region=window.findChild<QLineEdit*>("s3Region");QVERIFY(bucket&&region);
  bucket->setText("back-bucket");QMetaObject::invokeMethod(bucket,"editingFinished");
  region->setText("eu-west-1");QMetaObject::invokeMethod(region,"editingFinished");
  PUTGD_TRY_VERIFY(saved()["provider"]=="s3"&&saved()["settings"].toObject()["bucket"]=="back-bucket"&&saved()["settings"].toObject()["region"]=="eu-west-1");
 }
 // #1676: a Region edit finished while a Bucket save is still in flight keeps that Bucket: the
 // pair carries only Region and Endpoint and is applied onto the latest saved settings.
 void regionAfterBucketInFlightKeepsTheBucket(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);
  // Saving the new Bucket takes a while, so Region is finished while it is in flight.
  SettingsService service(dir.filePath("config.json"),[&](auto,auto value){if(value["bucket"]=="new-bucket"&&value["region"]!="eu-west-1")QThread::msleep(700);return provider.validateSettings(value);});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  const auto saved=[&]{return service.document()["profiles"].toArray()[0].toObject()["settings"].toObject();};
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");auto* region=window.findChild<QLineEdit*>("s3Region");QVERIFY(bucket&&region);
  bucket->setText("new-bucket");QMetaObject::invokeMethod(bucket,"editingFinished");
  putgd::test::settle(600);
  region->setText("eu-west-1");QMetaObject::invokeMethod(region,"editingFinished");
  PUTGD_TRY_VERIFY(saved()["bucket"]=="new-bucket"&&saved()["region"]=="eu-west-1");
  PUTGD_VERIFY_STEADY(saved()["bucket"]=="new-bucket"&&saved()["region"]=="eu-west-1",1500);
 }
 void removingDestinationBeforeDebounceNeverAppliesItsEditToNextDestination(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto second=profiles[0].toObject();second["id"]="second-destination";second["label"]="Second destination";auto fields=second["settings"].toObject();fields["bucket"]="second-bucket";second["settings"]=fields;profiles.append(second);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();window.findChild<QLineEdit*>("s3Bucket")->setText("edited-first-bucket");QMetaObject::invokeMethod(window.findChild<QLineEdit*>("s3Bucket"),"editingFinished");QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QMessageBox*>(widget))dialog->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();PUTGD_TRY_COMPARE(service.document()["profiles"].toArray().size(),1);QCOMPARE(service.document()["profiles"].toArray()[0].toObject(),second);
 }
 // #1704: random label, bucket and refused-bucket edits, adds, removes, moves and a toggle, with
 // each save the resident sees released at a random point. Whatever the timing, the accepted
 // document ends as the last value given to each destination, in the order shown.
 void saveQueueEndsWithLastIntentPerDestination_data(){QTest::addColumn<int>("seed");const int first=qEnvironmentVariableIsSet("PUTGD_SAVE_QUEUE_SEED")?qEnvironmentVariableIntValue("PUTGD_SAVE_QUEUE_SEED"):1704;for(int seed=first;seed<first+4;++seed)QTest::newRow(qPrintable(QString::number(seed)))<<seed;}
 void saveQueueEndsWithLastIntentPerDestination(){
  QFETCH(int,seed);
  QRandomGenerator random{quint32(seed)};
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);
  SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* list=window.findChild<QListWidget*>("destinationList");auto* label=window.findChild<QLineEdit*>("destinationLabel");auto* bucket=window.findChild<QLineEdit*>("s3Bucket");
  auto* notifications=window.settings()->findChild<QCheckBox*>("notifications");QAction* add=nullptr;for(auto* action:window.findChildren<QAction*>())if(action->text()=="Amazon S3 / S3-compatible")add=action;
  QVERIFY(list&&label&&bucket&&notifications&&add);
  struct Intent{QString id,label,bucket;bool refused=false;};QList<Intent> model;
  for(const auto& value:service.document()["profiles"].toArray()){const auto p=value.toObject();model.append({p["id"].toString(),p["label"].toString(),p["settings"].toObject()["bucket"].toString()});}
  bool notify=notifications->isChecked();int added=0;
  const auto type=[](QLineEdit* field,const QString& text){field->setText(text);QMetaObject::invokeMethod(field,"editingFinished");};
  for(int step=0;step<80;++step){
   const int row=random.bounded(int(model.size()));list->setCurrentRow(row);
   switch(random.bounded(8)){
   case 0:model[row].label=QString("Label %1").arg(step);type(label,model[row].label);break;
   case 1:model[row].bucket=QString("bucket-%1").arg(step);model[row].refused=false;type(bucket,model[row].bucket);break;
   // Too short: the resident refuses it, naming no field, and a later valid bucket replaces it.
   // The label given with it still saves.
   // Both are sent before anything else replaces them.
   case 2:model[row].refused=true;type(bucket,"ab");model[row].label=QString("Kept %1").arg(step);type(label,model[row].label);for(int i=0;i<8;++i){fixture.client.release();putgd::test::settle(5);}break;
   case 3:if(model.size()<12){const auto name=QString("Added %1").arg(++added);QTimer::singleShot(0,[name]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue(name);dialog->accept();}});add->trigger();model.append({QString("added-%1").arg(added),name,{}});}break;
   case 4:if(model.size()>1){QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();model.removeAt(row);}break;
   case 5:case 6:{const int delta=random.bounded(2)?1:-1;QJsonObject result;if(row+delta>=0&&row+delta<model.size()&&!window.settings()->moveSelectedDestination(delta,model[row].id,result))model.move(row,row+delta);break;}
   default:notifications->click();notify=!notify;
   }
   if(random.bounded(4)==0)fixture.client.release();
   putgd::test::settle(int(random.bounded(25)));
  }
  // A destination still without a saved bucket gets one: nothing in the model is refused at the end.
  for(int row=0;row<model.size();++row)if(model[row].bucket.isEmpty()||model[row].refused){list->setCurrentRow(row);model[row].bucket=QString("final-%1").arg(row);type(bucket,model[row].bucket);}
  PUTGD_TRY_VERIFY((fixture.client.release(),fixture.client.held.isEmpty()&&window.settings()->flushAutosave()));
  const auto profiles=service.document()["profiles"].toArray();QStringList ids;for(const auto& intent:std::as_const(model))ids.append(intent.id);
  QVERIFY2(profileIds(service.document())==ids,qPrintable(QString("seed %1: %2 != %3").arg(seed).arg(profileIds(service.document()).join(','),ids.join(','))));
  for(int row=0;row<model.size();++row){const auto p=profiles[row].toObject();QVERIFY2(p["label"]==model[row].label&&p["settings"].toObject()["bucket"]==model[row].bucket,qPrintable(QString("seed %1, %2: %3/%4").arg(seed).arg(model[row].id,p["label"].toString(),p["settings"].toObject()["bucket"].toString())));}
  QCOMPARE(service.document()["preferences"].toObject()["notifications"].toBool(),notify);
 }
 // #1701: a Region that can't be saved on one destination never stops another from saving. The
 // new destination saves, the first keeps its accepted Region, and the footer names the first.
 void unsavableRegionNeverBlocksAnotherDestination(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  const auto first=service.document()["profiles"].toArray()[0].toObject();
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* region=window.findChild<QLineEdit*>("s3Region");QVERIFY(region);region->setText("Western North America (WNAM)");QMetaObject::invokeMethod(region,"editingFinished");
  QAction* add=nullptr;for(auto* action:window.findChildren<QAction*>())if(action->text()=="Amazon S3 / S3-compatible")add=action;QVERIFY(add);
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("Second");dialog->accept();}});add->trigger();
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");bucket->setText("second-bucket");QMetaObject::invokeMethod(bucket,"editingFinished");
  const auto profiles=[&]{return service.document()["profiles"].toArray();};
  PUTGD_TRY_VERIFY(profiles().size()==2&&profiles()[1].toObject()["settings"].toObject()["bucket"]=="second-bucket");
  QCOMPARE(profiles()[0].toObject()["settings"].toObject()["region"],first["settings"].toObject()["region"]);
  auto* footer=window.findChild<QLabel*>("settingsStatus");QVERIFY(footer);
  PUTGD_TRY_VERIFY2(footer->text().startsWith(first["label"].toString()+" wasn't saved:"),qPrintable(footer->text()));
 }
 // #1704: a reply that arrives while another field is being typed in changes nothing there: its
 // text, its cursor and its message stay as they are.
 void replyWhileTypingLeavesTheField(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();window.activateWindow();QVERIFY(putgd::test::waitForWindowActive(&window));
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");auto* region=window.findChild<QLineEdit*>("s3Region");auto* message=window.findChild<QLabel*>("s3RegionChoicesError");QVERIFY(bucket&&region&&message);
  // Typed Bucket is sent once typing pauses; the resident hasn't answered yet.
  bucket->setFocus();bucket->selectAll();QTest::keyClicks(bucket,"typed-bucket");
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  region->setFocus();region->selectAll();QTest::keyClicks(region,"Western North America (WNAM)");
  PUTGD_TRY_VERIFY(message->isVisible()&&!message->text().isEmpty());
  region->setCursorPosition(7);const auto text=region->text(),shown=message->text();
  QVERIFY(fixture.client.release());
  PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["bucket"].toString(),QString("typed-bucket"));
  PUTGD_VERIFY_STEADY(region->text()==text&&region->cursorPosition()==7&&message->isVisible()&&message->text()==shown&&bucket->text()=="typed-bucket",800);
 }
 // #1704: an edit made while a connection test runs waits for it, then saves when it ends.
 void editDuringConnectionTestSavesWhenItEnds(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  std::function<void(Error)> finish;UiSessionFixture fixture(service,secrets,[&](const QString&,const QJsonObject&,auto reply){finish=std::move(reply);});
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  window.findChild<QPushButton*>("testDestination")->click();PUTGD_TRY_VERIFY(bool(finish));
  auto* notifications=window.settings()->findChild<QCheckBox*>("notifications");QVERIFY(notifications);
  const auto saved=[&]{return service.document()["preferences"].toObject()["notifications"].toBool();};const bool before=saved();
  notifications->click();
  PUTGD_VERIFY_STEADY(saved()==before,800);
  finish({});
  PUTGD_TRY_COMPARE(saved(),!before);
 }
 // #1704: a save refused because the file changed elsewhere stops the queue and keeps it. Retry
 // saving applies the kept edit to the file as it is now: the other change stays, and the form
 // shows it.
 void conflictKeepsTheQueueForRetry(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  // A Region that can't be saved stays as typed through all of it.
  auto* region=window.findChild<QLineEdit*>("s3Region");const auto savedRegion=service.document()["profiles"].toArray()[0].toObject()["settings"].toObject().value("region");
  region->setText("Western North America (WNAM)");QMetaObject::invokeMethod(region,"editingFinished");
  const auto original=service.document()["profiles"].toArray()[0].toObject().value("label");
  auto* label=window.findChild<QLineEdit*>("destinationLabel");label->setText("Renamed first");QMetaObject::invokeMethod(label,"editingFinished");
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  auto elsewhere=service.document();auto profiles=elsewhere["profiles"].toArray();auto second=profiles[1].toObject();auto fields=second["settings"].toObject();fields["bucket"]="changed-elsewhere";second["settings"]=fields;profiles[1]=second;elsewhere["profiles"]=profiles;QVERIFY(service.save(elsewhere,service.revision()));
  QVERIFY(fixture.client.release());
  auto* banner=window.findChild<QWidget*>("destinationConflictBanner");auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");QVERIFY(banner&&retry);
  PUTGD_TRY_VERIFY(banner->isVisible()&&retry->isVisible());
  const auto saved=[&](int row){return service.document()["profiles"].toArray()[row].toObject();};
  QCOMPARE(saved(0)["label"],original);
  retry->click();
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));QVERIFY(fixture.client.release());
  PUTGD_TRY_VERIFY(saved(0)["label"]=="Renamed first"&&saved(1)["settings"].toObject()["bucket"]=="changed-elsewhere");
  PUTGD_TRY_VERIFY(!banner->isVisible());
  QCOMPARE(saved(0)["settings"].toObject()["region"],savedRegion);QCOMPARE(region->text(),QString("Western North America (WNAM)"));
  auto* status=window.findChild<QLabel*>("destinationStatus");PUTGD_TRY_VERIFY(status->isVisible()&&status->property("errorField").toString()=="region");
  window.findChild<QListWidget*>("destinationList")->setCurrentRow(1);QCOMPARE(window.findChild<QLineEdit*>("s3Bucket")->text(),QString("changed-elsewhere"));
 }
 void customUploaderImportKeepsItsTokenOutOfSettingsAndCheckpoints(){
  const QByteArray token="PUTGD-SYNTHETIC-GUI-TOKEN-0123456789abcdef";
  QTemporaryDir dir;CredentialStore secrets;ProviderRegistry registry;
  registry.add("s3",std::make_shared<S3Provider>(secrets));registry.add("custom-http",std::make_shared<HttpUploaderProvider>(secrets));
  SettingsService service(dir.filePath("config.json"),[&](auto id,auto values){return registry.validate(id,values);},nullptr,[&](auto id,auto reference){return registry.validateCredentials(id,reference);});
  QVERIFY(service.save(Settings::developmentTemplate(),{}));
  UiSessionFixture fixture(service,secrets);fixture.setup.setNamedCredentials(http::namedCredentials);
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* provider=window.findChild<QComboBox*>("storageProvider");
  QVERIFY(provider->findData("custom-http")>=0);
  provider->setCurrentIndex(provider->findData("custom-http"));
  const auto saved=[&]{return service.document()["profiles"].toArray()[0].toObject();};
  PUTGD_TRY_COMPARE(saved()["provider"].toString(),QString("custom-http"));
  // A new one is saved with its defaults and says what it still needs.
  QCOMPARE(saved()["settings"].toObject(),http::defaults());
  auto* panel=window.findChild<CustomUploaderPanel*>("customUploaderEditor");QVERIFY(panel&&panel->isVisible());
  QVERIFY(!window.findChild<QLineEdit*>("credentialAccessKey")->isVisible());
  auto* mode=window.findChild<QComboBox*>("credentialMode");mode->setCurrentIndex(mode->findData("session"));emit mode->activated(mode->currentIndex());
  PUTGD_TRY_COMPARE(saved()["credentials"].toObject()["mode"].toString(),QString("session"));
  // The file repeats its token in the deletion URL, as XBackBone's does.
  const QByteArray file="{\"Version\":\"16.1.0\",\"Name\":\"Lab media\",\"DestinationType\":\"ImageUploader, FileUploader\",\"RequestMethod\":\"POST\",\"RequestURL\":\"https://media.example.test/upload\",\"Body\":\"MultipartFormData\",\"Arguments\":{\"file\":\"{filename}\",\"token\":\""+token+"\"},\"FileFormName\":\"upload\",\"URL\":\"{json:url}\",\"DeletionURL\":\"{json:url}/delete/"+token+"\"}";
  QVERIFY(!panel->importBytes(file));
  // The notice says what happened and never repeats the file's Name (#1614).
  auto* notice=window.findChild<QLabel*>("uploaderNotice");QVERIFY(notice);
  QVERIFY2(notice->text().startsWith("Imported uploader.")&&!notice->text().contains("Lab media"),qPrintable(notice->text()));
  PUTGD_TRY_COMPARE(saved()["settings"].toObject()["requestUrl"].toString(),QString("https://media.example.test/upload"));
  const auto settings=saved()["settings"].toObject();
  QCOMPARE(settings["arguments"].toArray(),QJsonArray({QJsonObject{{"name","file"},{"secret",false},{"value","{filename}"}},QJsonObject{{"name","token"},{"secret",true}}}));
  QCOMPARE(settings["deletionUrl"].toString(),QString("{json:url}/delete/{secret:arguments/token}"));
  QCOMPARE(settings["accepts"].toArray(),QJsonArray({"image","file"}));
  // Held in the window, masked, and nowhere else.
  QCOMPARE(panel->typedSecrets().value("arguments/token"),QString::fromLatin1(token));
  auto* value=window.findChild<QLineEdit*>("uploaderFieldValue1");QVERIFY(value);
  QCOMPARE(value->echoMode(),QLineEdit::Password);
  QVERIFY(window.settings()->hasCredentialInputs());
  QFile config(dir.filePath("config.json"));QVERIFY(config.open(QIODevice::ReadOnly));QVERIFY(!config.readAll().contains(token));config.close();
  const auto checkpoint=window.settings()->saveViewState();
  QVERIFY2(!checkpoint.contains("stateError"),qPrintable(checkpoint["stateError"].toString()));
  QVERIFY(!QJsonDocument(checkpoint).toJson().contains(token));
  QVERIFY(!QJsonDocument(panel->state()).toJson().contains(token));
  QCOMPARE(panel->missingSecrets(),QStringList());
  // Stored for the session: the field empties and shows the value as stored.
  auto* store=window.findChild<QPushButton*>("storeCredentials");PUTGD_TRY_VERIFY(store->isEnabled());store->click();
  PUTGD_TRY_VERIFY(panel->typedSecrets().isEmpty());
  QVERIFY(value->text().isEmpty());
  const auto cached=secrets.cached("development-s3",saved()["credentials"].toObject());
  QVERIFY(cached);QCOMPARE(cached->values.value("arguments/token"),QString::fromLatin1(token));
  QVERIFY(!window.settings()->hasCredentialInputs());
  QVERIFY(config.open(QIODevice::ReadOnly));QVERIFY(!config.readAll().contains(token));config.close();
  QVERIFY(!QJsonDocument(window.settings()->saveViewState()).toJson().contains(token));
  // Plain HTTP beyond loopback is refused beside the address until the destination allows it.
  auto* url=window.findChild<QLineEdit*>("uploaderUrl");url->setText("http://media.example.test/upload");emit url->editingFinished();
  auto* problem=window.findChild<QLabel*>("uploaderUrlError");QVERIFY(problem);
  PUTGD_TRY_VERIFY(problem->isVisible()&&problem->text().contains("plain HTTP"));
  QCOMPARE(saved()["settings"].toObject()["requestUrl"].toString(),QString("https://media.example.test/upload"));
  auto* allow=window.findChild<QCheckBox*>("uploaderAllowHttp");QVERIFY(allow->isVisible());allow->click();
  PUTGD_TRY_COMPARE(saved()["settings"].toObject()["requestUrl"].toString(),QString("http://media.example.test/upload"));
  QCOMPARE(saved()["settings"].toObject()["allowInsecureHttp"].toBool(),true);
  // The deletion URL names this value, so it cannot stop being a secret: the change is
  // refused beside the form and the saved row stays as it was.
  auto* secret=window.findChild<QCheckBox*>("uploaderFieldSecret1");secret->click();
  auto* refused=window.findChild<QLabel*>("uploaderSecretsError");QVERIFY(refused);
  PUTGD_TRY_VERIFY(refused->isVisible()&&refused->text().contains("does not store"));
  QCOMPARE(saved()["settings"].toObject()["arguments"].toArray()[1].toObject(),QJsonObject({{"name","token"},{"secret",true}}));
 }
};
QTEST_MAIN(SettingsGui)
#include "test_settings_destinations.moc"
