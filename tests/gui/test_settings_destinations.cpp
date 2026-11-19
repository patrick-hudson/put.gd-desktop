#include "SettingsGuiFixture.h"
#include "storage/s3/R2Provider.h"
#include "storage/s3/R2Settings.h"
#include <QClipboard>
#include <QThread>
#include <QRandomGenerator>
#include <QContextMenuEvent>
#include <QMenu>
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
 // #1704: random label, bucket and refused-bucket edits, adds, removes, moves, route changes and a
 // toggle, with each save the resident sees released at a random point, transport failures and
 // conflicts answered with Retry saving, and edits made elsewhere before the Retry: a destination
 // removed, the default route and a prefix changed, and a new destination's ID taken. Whatever the
 // timing, one request is in flight at most, every legal move succeeds, and the accepted document
 // ends as the last value given to each destination, in the order shown, with what changed
 // elsewhere and wasn't touched here kept.
 // Two seeds per run keep the suite inside its CI time; PUTGD_SAVE_QUEUE_SEED and
 // PUTGD_SAVE_QUEUE_SEEDS (a count) run more on a builder.
 void saveQueueEndsWithLastIntentPerDestination_data(){QTest::addColumn<int>("seed");const int first=qEnvironmentVariableIsSet("PUTGD_SAVE_QUEUE_SEED")?qEnvironmentVariableIntValue("PUTGD_SAVE_QUEUE_SEED"):1704;const int count=qEnvironmentVariableIsSet("PUTGD_SAVE_QUEUE_SEEDS")?qMax(1,qEnvironmentVariableIntValue("PUTGD_SAVE_QUEUE_SEEDS")):2;for(int seed=first;seed<first+count;++seed)QTest::newRow(qPrintable(QString::number(seed)))<<seed;}
 void saveQueueEndsWithLastIntentPerDestination(){
  QFETCH(int,seed);
  QRandomGenerator random{quint32(seed)};
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);
  SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* list=window.findChild<QListWidget*>("destinationList");auto* label=window.findChild<QLineEdit*>("destinationLabel");auto* bucket=window.findChild<QLineEdit*>("s3Bucket");
  auto* notifications=window.settings()->findChild<QCheckBox*>("notifications");auto* route=window.settings()->findChild<QComboBox*>("defaultCaptureDestination");auto* shown=window.settings()->findChild<QComboBox*>("destinationProfile");
  auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");auto* banner=window.findChild<QWidget*>("destinationConflictBanner");
  QAction* add=nullptr;for(auto* action:window.findChildren<QAction*>())if(action->text()=="Amazon S3 / S3-compatible")add=action;
  QVERIFY(list&&label&&bucket&&notifications&&route&&shown&&retry&&banner&&add);
  struct Intent{QString id,label,bucket,prefix;bool refused=false;};QList<Intent> model;
  const auto intentOf=[](const QJsonObject& p){const auto s=p["settings"].toObject();return Intent{p["id"].toString(),p["label"].toString(),s["bucket"].toString(),s["objectPrefix"].toString()};};
  for(const auto& value:service.document()["profiles"].toArray())model.append(intentOf(value.toObject()));
  QString routed=service.document()["captureDefaults"].toObject()["profileId"].toString();

  bool notify=notifications->isChecked();int added=0,elsewhere=0;
  // The order before the first move not saved yet: a conflict puts the list back to it.
  QStringList orderBaseline;
  const auto type=[](QLineEdit* field,const QString& text){field->setText(text);QMetaObject::invokeMethod(field,"editingFinished");};
  const auto shownIds=[&]{QStringList ids;for(int i=0;i<shown->count();++i)ids.append(shown->itemData(i).toString());return ids;};
  const auto where=[&](const QString& id){for(int i=0;i<model.size();++i)if(model[i].id==id)return i;return -1;};
  // A rename of some destination, sent and held, so the next answer can be made to fail.
  const auto oneInFlight=[&](int step){const int r=random.bounded(int(model.size()));list->setCurrentRow(r);model[r].label=QString("Held %1").arg(step);type(label,model[r].label);return putgd::test::waitFor([&]{return !fixture.client.held.isEmpty();},2000);};
  for(int step=0;step<80;++step){
   QVERIFY2(fixture.client.held.size()<=1,qPrintable(QString("seed %1: %2 requests in flight").arg(seed).arg(fixture.client.held.size())));
   const int row=random.bounded(int(model.size()));list->setCurrentRow(row);
   switch(random.bounded(11)){
   case 0:model[row].label=QString("Label %1").arg(step);type(label,model[row].label);break;
   case 1:model[row].bucket=QString("bucket-%1").arg(step);model[row].refused=false;type(bucket,model[row].bucket);break;
   // Too short: the resident refuses it, naming no field, and a later valid bucket replaces it.
   // The label given with it still saves. Both are sent before anything else replaces them.
   case 2:model[row].refused=true;type(bucket,"ab");model[row].label=QString("Kept %1").arg(step);type(label,model[row].label);for(int i=0;i<8;++i){fixture.client.release();putgd::test::settle(5);}break;
   case 3:if(model.size()<12){const auto name=QString("Added %1").arg(++added);QTimer::singleShot(0,[name]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue(name);dialog->accept();}});add->trigger();model.append({QString("added-%1").arg(added),name,{},{}});}break;
   // Removing the destination the default route uses leaves no destination there.
   case 4:if(model.size()>1){if(routed==model[row].id){
     // A route to an add the resident refused was never saved: the file's route stays.
     // Removing it cancels any route to it still queued: the route is what the file has, or no
     // destination if the file routes there too.
     // Nothing in flight either: a save already sent may still be on its way to the resident.
     PUTGD_TRY_VERIFY((fixture.client.release(),fixture.client.held.isEmpty()&&window.settings()->flushAutosave()));
     const auto filed=service.document()["captureDefaults"].toObject()["profileId"].toString();routed=filed==model[row].id?QString():filed;}QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();model.removeAt(row);}break;
   case 5:case 6:{const int delta=random.bounded(2)?1:-1;if(row+delta<0||row+delta>=model.size())break;QJsonObject result;if(orderBaseline.isEmpty())for(const auto& intent:std::as_const(model))orderBaseline.append(intent.id);const auto error=window.settings()->moveSelectedDestination(delta,model[row].id,result);QVERIFY2(!error,qPrintable(QString("seed %1: %2").arg(seed).arg(error.message)));model.move(row,row+delta);break;}
   case 7:{const int pick=random.bounded(int(model.size())+1);const auto id=pick<model.size()?model[pick].id:QString();const int index=route->findData(id);if(index>=0){route->setCurrentIndex(index);routed=id;}break;}
   case 8:{
    // A transport failure stops the queue; Retry saving sends it again.
    if(!oneInFlight(step))break;
    fixture.client.failOnce.insert("settings.save",Error{"synthetic_transport","Synthetic transport failure",{}});QVERIFY(fixture.client.release());
    QVERIFY(putgd::test::waitFor([&]{return retry->isVisible();},3000));retry->click();break;}
   case 9:{
    // A conflict: the file changes elsewhere while a save is held. What was in flight lands first,
    // so no move waits to be saved; a conflicting move is put back, which a test of its own covers.
    PUTGD_TRY_VERIFY((retry->isVisible()?retry->click():void(),fixture.client.release(),fixture.client.held.isEmpty()&&!retry->isVisible()&&window.settings()->flushAutosave()));orderBaseline.clear();
    if(!oneInFlight(step))break;
    auto doc=service.document();auto profiles=doc["profiles"].toArray();const auto ids=profileIds(doc);
    // Removed elsewhere: any accepted destination this window keeps, the routed one included.
    QString gone;{QStringList candidates;for(const auto& id:ids)if(where(id)>=0)candidates.append(id);if(candidates.size()>1&&ids.size()>2)gone=candidates[random.bounded(int(candidates.size()))];}
    if(!gone.isEmpty()){for(int i=0;i<profiles.size();++i)if(profiles[i].toObject()["id"]==gone){profiles.removeAt(i);break;}
     auto actions=doc["actions"].toObject();for(auto it=actions.begin();it!=actions.end();++it){auto action=it.value().toObject();if(action.value("profileId")==QJsonValue(gone)){action["destination"]="local";action["profileId"]=QJsonValue::Null;it.value()=action;}}doc["actions"]=actions;
     if(auto capture=doc.value("captureDefaults").toObject();capture.value("profileId")==QJsonValue(gone)){capture["destination"]="local";capture["profileId"]=QJsonValue::Null;doc["captureDefaults"]=capture;}
     // A route this window chose to it can't be saved: the file's route stays, no destination if
     // the file routed there too.
     if(routed==gone)routed=doc.value("captureDefaults").toObject().value("profileId").toString();
     model.removeAt(where(gone));}
    // A prefix changed elsewhere on a destination this window keeps.
    // There is always this one, so the file does change.
    {QList<int> kept;for(int i=0;i<profiles.size();++i)if(where(profiles[i].toObject()["id"].toString())>=0)kept.append(i);
     if(!kept.isEmpty()){const int i=kept[random.bounded(int(kept.size()))];auto p=profiles[i].toObject();auto settings=p["settings"].toObject();settings["objectPrefix"]=QString("elsewhere-%1/").arg(++elsewhere);p["settings"]=settings;profiles[i]=p;model[where(p["id"].toString())].prefix=settings["objectPrefix"].toString();}
     else{auto preferences=doc["preferences"].toObject();preferences["captureDelaySeconds"]=QList<int>{3,5,10}[++elsewhere%3];doc["preferences"]=preferences;}}
    // The default route changed elsewhere, while this window's own route is saved: it stays.
    {QStringList others;for(const auto& value:std::as_const(profiles))if(where(value.toObject()["id"].toString())>=0)others.append(value.toObject()["id"].toString());const auto capture=doc.value("captureDefaults").toObject();const bool mineSaved=capture.value("profileId")==(routed.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(routed));
     if(mineSaved&&!others.isEmpty()&&random.bounded(2)){const auto to=others[random.bounded(int(others.size()))];auto changed=capture;changed["destination"]="upload";changed["profileId"]=to;doc["captureDefaults"]=changed;routed=to;}}
    // A new destination's ID taken elsewhere before it was saved.
    QString taken;for(const auto& intent:std::as_const(model))if(!ids.contains(intent.id)&&intent.id!=gone){taken=intent.id;break;}
    if(!taken.isEmpty()){auto p=Settings::developmentTemplate()["profiles"].toArray().first().toObject();p["id"]=taken;p["label"]=QString("Elsewhere %1").arg(++elsewhere);auto settings=p["settings"].toObject();settings["bucket"]="elsewhere-bucket";settings["objectPrefix"]="";p["settings"]=settings;p["credentials"]=QJsonObject{{"mode","keychain"},{"reference","putgd/"+taken}};profiles.append(p);}
    doc["profiles"]=profiles;{const auto saved=service.save(doc,service.revision());QVERIFY2(saved,qPrintable(saved.error.message+" "+profileIds(doc).join(",")+" "+QJsonDocument(saved.error.details).toJson(QJsonDocument::Compact)));}
    // The saves held now meet the changed file; one that is invalid anyway is refused first.
    QVERIFY2(putgd::test::waitFor([&]{fixture.client.release();return banner->isVisible()&&retry->isVisible();},3000),qPrintable(QString("seed %1: no conflict").arg(seed)));
    retry->click();
    QVERIFY(putgd::test::waitFor([&]{return !banner->isVisible();},3000));
    // The form is the file as it is now with this window's edits on top: an ID taken elsewhere
    // went to the other destination, and this window's one has a fresh ID.
    QString renamed;
    if(!taken.isEmpty()){const auto now=shownIds();QString fresh;for(const auto& id:now)if(where(id)<0&&id!=taken)fresh=id;QVERIFY2(!fresh.isEmpty(),qPrintable(QString("seed %1: %2 not renamed").arg(seed).arg(taken)));model[where(taken)].id=fresh;renamed=fresh;if(routed==taken)routed=fresh;
     // The destination added elsewhere comes last in the file, after every one this window keeps.
     model.append(intentOf(profiles.last().toObject()));}
    // The order is the one this window had, less what went, with what came elsewhere at the end;
    // or, when moves weren't saved yet, the order before them, which a conflict puts back, with
    // destinations added since after it.
    {QStringList kept;for(const auto& intent:std::as_const(model))kept.append(intent.id);
     QStringList reverted;for(auto id:std::as_const(orderBaseline)){if(id==taken&&!renamed.isEmpty())id=renamed;if(kept.contains(id)&&!reverted.contains(id))reverted.append(id);}for(const auto& id:std::as_const(kept))if(!reverted.contains(id))reverted.append(id);
     const auto now=shownIds();QVERIFY2(now==kept,qPrintable(QString("seed %1: shown %2, expected %3 or %4").arg(seed).arg(now.join(','),kept.join(','),reverted.join(','))));
     QList<Intent> ordered;for(const auto& id:now)ordered.append(model[where(id)]);model=ordered;orderBaseline.clear();}
    break;}
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
  for(int row=0;row<model.size();++row){const auto p=profiles[row].toObject();const auto s=p["settings"].toObject();QVERIFY2(p["label"]==model[row].label&&s["bucket"]==model[row].bucket&&s["objectPrefix"]==model[row].prefix,qPrintable(QString("seed %1, %2: %3/%4/%5, expected %6/%7/%8").arg(seed).arg(model[row].id,p["label"].toString(),s["bucket"].toString(),s["objectPrefix"].toString(),model[row].label,model[row].bucket,model[row].prefix)));}
  QCOMPARE(service.document()["captureDefaults"].toObject()["profileId"],routed.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(routed));
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
 // #1712: aws-global on an S3 destination whose Endpoint isn't Amazon S3 fails the check the form
 // shows, so it is never sent: the label typed with it saves, and the saved Region stays.
 void awsGlobalOffAmazonIsNeverSent(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});
  auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();auto fields=profile["settings"].toObject();fields["endpoint"]="https://minio.example.com";fields["region"]="us-east-1";profile["settings"]=fields;profiles[0]=profile;doc["profiles"]=profiles;
  {const auto saved=service.save(doc,{});QVERIFY2(saved,qPrintable(saved.error.message));}
  UiSessionFixture fixture(service,secrets);int sentGlobal=0;
  fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject& params){if(method=="settings.save"&&params["document"].toObject()["profiles"].toArray()[0].toObject()["settings"].toObject()["region"]==QJsonValue(QStringLiteral("aws-global")))++sentGlobal;};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* region=window.findChild<QLineEdit*>("s3Region");auto* label=window.findChild<QLineEdit*>("destinationLabel");auto* message=window.findChild<QLabel*>("s3RegionChoicesError");QVERIFY(region&&label&&message);
  region->setText("aws-global");QMetaObject::invokeMethod(region,"editingFinished");
  PUTGD_TRY_VERIFY(message->isVisible()&&message->text().contains("aws-global is only for Amazon S3"));
  label->setText("Renamed");QMetaObject::invokeMethod(label,"editingFinished");
  const auto saved=[&]{return service.document()["profiles"].toArray()[0].toObject();};
  PUTGD_TRY_COMPARE(saved()["label"].toString(),QString("Renamed"));
  PUTGD_TRY_VERIFY(window.settings()->flushAutosave());
  QCOMPARE(sentGlobal,0);QCOMPARE(saved()["settings"].toObject()["region"].toString(),QString("us-east-1"));
 }
 // #1712: a restored draft holding that aws-global, replayed by Retry saving, sends the label
 // typed with it but not the Region, which stays as typed in the form.
 void restoredAwsGlobalOffAmazonIsNeverSent(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});
  auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();auto fields=profile["settings"].toObject();fields["endpoint"]="https://minio.example.com";fields["region"]="us-east-1";profile["settings"]=fields;profiles[0]=profile;doc["profiles"]=profiles;
  {const auto saved=service.save(doc,{});QVERIFY2(saved,qPrintable(saved.error.message));}
  UiSessionFixture fixture(service,secrets);fixture.client.failOnce.insert("settings.save",Error{"synthetic_transport","Synthetic transport failure",{}});
  int sentGlobal=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject& params){if(method=="settings.save"&&params["document"].toObject()["profiles"].toArray()[0].toObject()["settings"].toObject()["region"]==QJsonValue(QStringLiteral("aws-global")))++sentGlobal;};
  QJsonObject checkpoint;
  {MainWindow first(fixture.session);first.navigate("destinations");first.show();
   auto* label=first.findChild<QLineEdit*>("destinationLabel");label->setText("Renamed here");QMetaObject::invokeMethod(label,"editingFinished");
   PUTGD_TRY_VERIFY(first.findChild<QPushButton*>("retryDestinationAutosave")->isVisible());
   auto* region=first.findChild<QLineEdit*>("s3Region");region->setText("aws-global");QMetaObject::invokeMethod(region,"editingFinished");
   checkpoint=first.settings()->saveViewState();QVERIFY(!checkpoint.contains("stateError"));}
  MainWindow second(fixture.session);QVERIFY(second.settings()->restoreViewState(checkpoint));second.navigate("destinations");second.show();
  auto* retry=second.findChild<QPushButton*>("retryDestinationAutosave");PUTGD_TRY_VERIFY(retry->isVisible());retry->click();
  const auto saved=[&]{return service.document()["profiles"].toArray()[0].toObject();};
  PUTGD_TRY_COMPARE(saved()["label"].toString(),QString("Renamed here"));
  PUTGD_TRY_VERIFY(second.settings()->flushAutosave());
  QCOMPARE(sentGlobal,0);QCOMPARE(saved()["settings"].toObject()["region"].toString(),QString("us-east-1"));
  QCOMPARE(second.findChild<QLineEdit*>("s3Region")->text(),QString("aws-global"));
 }
 // #1713: a move saved while a later one waits is the order the list goes back to when the later
 // move meets a conflict or is refused, so the list shows the file's order, not the one from before
 // both moves.
 void conflictingMoveGoesBackToTheSavedMove_data(){QTest::addColumn<bool>("refused");QTest::newRow("conflict")<<false;QTest::newRow("refused")<<true;}
 void conflictingMoveGoesBackToTheSavedMove(){
  QFETCH(bool,refused);
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* shown=window.settings()->findChild<QComboBox*>("destinationProfile");QVERIFY(shown);
  const auto shownIds=[&]{QStringList ids;for(int i=0;i<shown->count();++i)ids.append(shown->itemData(i).toString());return ids;};
  const auto before=profileIds(service.document());
  QJsonObject result;QVERIFY(!window.settings()->moveSelectedDestination(1,before[0],result));
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  QVERIFY(!window.settings()->moveSelectedDestination(1,before[0],result));
  const QStringList saved{before[1],before[0],before[2]};
  QVERIFY(fixture.client.release());PUTGD_TRY_COMPARE(profileIds(service.document()),saved);
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  if(refused)fixture.client.failOnce.insert("settings.save",Error{"config_invalid","Synthetic refusal",{}});
  else{auto elsewhere=service.document();auto preferences=elsewhere["preferences"].toObject();preferences["captureDelaySeconds"]=5;elsewhere["preferences"]=preferences;QVERIFY(service.save(elsewhere,service.revision()));}
  QVERIFY(fixture.client.release());
  if(refused)PUTGD_TRY_VERIFY(fixture.client.held.isEmpty()&&!window.settings()->saveViewState()["stateError"].toString().contains("awaiting"));
  else{auto* banner=window.findChild<QWidget*>("destinationConflictBanner");PUTGD_TRY_VERIFY(banner->isVisible());}
  PUTGD_TRY_COMPARE(shownIds(),saved);QCOMPARE(profileIds(service.document()),saved);
 }
 // #1713: a destination added while a move is in flight, and moved up before that move saves, is
 // saved beside its neighbour. When the later order, from that and one more move, is refused, the list goes back to the file's
 // order, the added one in its saved place, not to the first move's order with it at the end.
 void refusedMoveAfterAnAddShowsTheFilesOrder(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* shown=window.settings()->findChild<QComboBox*>("destinationProfile");QVERIFY(shown);
  const auto shownIds=[&]{QStringList ids;for(int i=0;i<shown->count();++i)ids.append(shown->itemData(i).toString());return ids;};
  // A B C: A moves down (B A C) and that save is held; D is added (B A C D) and moved up (B A D C);
  // B moves down (A B D C). The add saves after B, its neighbour (B D A C).
  const auto ids=profileIds(service.document());
  QJsonObject result;QVERIFY(!window.settings()->moveSelectedDestination(1,ids[0],result));
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  QAction* add=nullptr;for(auto* action:window.findChildren<QAction*>())if(action->text()=="Amazon S3 / S3-compatible")add=action;QVERIFY(add);
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("Later");dialog->accept();}});add->trigger();
  QVERIFY(!window.settings()->moveSelectedDestination(-1,"later",result));
  QVERIFY(!window.settings()->moveSelectedDestination(1,ids[1],result));
  // The first move, then the add, save; the later order is refused.
  QVERIFY(fixture.client.release());PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  QVERIFY(fixture.client.release());PUTGD_TRY_VERIFY(profileIds(service.document()).contains("later")&&fixture.client.held.size()==1);
  const auto saved=profileIds(service.document());QVERIFY2(saved.last()!="later",qPrintable(saved.join(',')));
  fixture.client.failOnce.insert("settings.save",Error{"config_invalid","Synthetic refusal",{}});QVERIFY(fixture.client.release());
  PUTGD_TRY_COMPARE(shownIds(),saved);QCOMPARE(profileIds(service.document()),saved);
 }
 // #1713: a move refused while a newer one waits leaves nothing unsaved once the newer one saves.
 void refusedMoveIsForgottenWhenANewerOneSaves(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  const auto first=profileIds(service.document())[0];
  QJsonObject result;QVERIFY(!window.settings()->moveSelectedDestination(1,first,result));
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  QVERIFY(!window.settings()->moveSelectedDestination(1,first,result));
  fixture.client.failOnce.insert("settings.save",Error{"config_invalid","Synthetic refusal",{}});QVERIFY(fixture.client.release());
  PUTGD_TRY_VERIFY((fixture.client.release(),fixture.client.held.isEmpty()&&window.settings()->flushAutosave()&&profileIds(service.document()).last()==first));
  PUTGD_TRY_VERIFY(!window.settings()->saveViewState()["dirty"].toBool());
 }
 // #1713: after a move's save fails, another writer reorders the file and the window finds the
 // change itself. The move not saved goes back, as when a save's answer is the conflict, so Retry
 // saving keeps the other writer's order.
 void watchedConflictPutsBackTheMove(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* shown=window.settings()->findChild<QComboBox*>("destinationProfile");QVERIFY(shown);
  const auto shownIds=[&]{QStringList ids;for(int i=0;i<shown->count();++i)ids.append(shown->itemData(i).toString());return ids;};
  const auto before=profileIds(service.document());
  QJsonObject result;QVERIFY(!window.settings()->moveSelectedDestination(1,before[0],result));
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  fixture.client.failOnce.insert("settings.save",Error{"synthetic_transport","Synthetic transport failure",{}});QVERIFY(fixture.client.release());
  auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");PUTGD_TRY_VERIFY(retry->isVisible());
  auto elsewhere=service.document();QJsonArray reordered;const auto profiles=elsewhere["profiles"].toArray();for(int i=profiles.size()-1;i>=0;--i)reordered.append(profiles[i]);elsewhere["profiles"]=reordered;QVERIFY(service.save(elsewhere,service.revision()));
  const auto theirs=profileIds(service.document());
  auto* banner=window.findChild<QWidget*>("destinationConflictBanner");PUTGD_TRY_VERIFY(banner->isVisible());
  PUTGD_TRY_COMPARE(shownIds(),theirs);
  retry->click();
  PUTGD_TRY_VERIFY((fixture.client.release(),!banner->isVisible()&&fixture.client.held.isEmpty()&&window.settings()->flushAutosave()));
  QCOMPARE(profileIds(service.document()),theirs);QCOMPARE(shownIds(),theirs);
 }
 // #1713: the list going back after a conflict the window finds itself moves only the rows: the
 // name being typed in keeps its text, caret and undo history.
 void conflictRollbackLeavesTheFieldBeingTyped(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();window.activateWindow();QVERIFY(putgd::test::waitForWindowActive(&window));
  auto* shown=window.settings()->findChild<QComboBox*>("destinationProfile");auto* label=window.findChild<QLineEdit*>("destinationLabel");QVERIFY(shown&&label);
  const auto shownIds=[&]{QStringList ids;for(int i=0;i<shown->count();++i)ids.append(shown->itemData(i).toString());return ids;};
  const auto before=profileIds(service.document());
  QJsonObject result;QVERIFY(!window.settings()->moveSelectedDestination(1,before[0],result));
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  fixture.client.failOnce.insert("settings.save",Error{"synthetic_transport","Synthetic transport failure",{}});QVERIFY(fixture.client.release());
  auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");PUTGD_TRY_VERIFY(retry->isVisible());
  label->setFocus();PUTGD_TRY_VERIFY(label->hasFocus());label->end(false);QTest::keyClicks(label," edited");label->setCursorPosition(3);
  const auto text=label->text();QVERIFY(label->isUndoAvailable());
  auto elsewhere=service.document();auto preferences=elsewhere["preferences"].toObject();preferences["captureDelaySeconds"]=5;elsewhere["preferences"]=preferences;QVERIFY(service.save(elsewhere,service.revision()));
  auto* banner=window.findChild<QWidget*>("destinationConflictBanner");PUTGD_TRY_VERIFY(banner->isVisible());
  PUTGD_TRY_COMPARE(shownIds(),before);
  QCOMPARE(label->text(),text);QCOMPARE(label->cursorPosition(),3);QVERIFY(label->isUndoAvailable());
 }
 // #1713: a move answered with a conflict because another client reordered meanwhile: once the
 // window has the file again, the list shows that client's order, not the one before the move.
 void conflictAnsweredMoveShowsTheOtherWritersOrder(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* shown=window.settings()->findChild<QComboBox*>("destinationProfile");QVERIFY(shown);
  const auto shownIds=[&]{QStringList ids;for(int i=0;i<shown->count();++i)ids.append(shown->itemData(i).toString());return ids;};
  const auto before=profileIds(service.document());
  QJsonObject result;QVERIFY(!window.settings()->moveSelectedDestination(1,before[0],result));
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  auto elsewhere=service.document();QJsonArray reversed;const auto profiles=elsewhere["profiles"].toArray();for(int i=profiles.size()-1;i>=0;--i)reversed.append(profiles[i]);elsewhere["profiles"]=reversed;QVERIFY(service.save(elsewhere,service.revision()));
  const auto theirs=profileIds(service.document());
  QVERIFY(fixture.client.release());
  auto* banner=window.findChild<QWidget*>("destinationConflictBanner");PUTGD_TRY_VERIFY(banner->isVisible());
  PUTGD_TRY_COMPARE(fixture.session.revision(),service.revision());
  PUTGD_TRY_COMPARE(shownIds(),theirs);
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
 // #1704: after a conflict with a configuration file the resident refuses, Retry saving keeps
 // saving stopped and the file as it is: that file's revision never lets the queued edit replace it.
 void retryNeverOverwritesARefusedFile(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);const auto path=dir.filePath("config.json");
  SettingsService service(path,[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* label=window.findChild<QLineEdit*>("destinationLabel");label->setText("Renamed here");QMetaObject::invokeMethod(label,"editingFinished");
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  // Edited elsewhere into a file the resident refuses.
  auto refused=service.document();auto profiles=refused["profiles"].toArray();auto profile=profiles[0].toObject();auto fields=profile["settings"].toObject();fields["bucket"]="x";profile["settings"]=fields;profile["label"]="Edited elsewhere";profiles[0]=profile;refused["profiles"]=profiles;
  {QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(QJsonDocument(refused).toJson());}
  QFile read(path);QVERIFY(read.open(QIODevice::ReadOnly));const auto written=read.readAll();read.close();
  QVERIFY(fixture.client.release());
  auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");auto* banner=window.findChild<QWidget*>("destinationConflictBanner");auto* fileLine=window.findChild<QLabel*>("destinationConfigFileError");QVERIFY(retry&&banner&&fileLine);
  PUTGD_TRY_VERIFY(banner->isVisible()&&retry->isVisible());
  retry->click();
  PUTGD_TRY_VERIFY(fileLine->isVisible()&&fileLine->text().startsWith("The configuration file wasn't loaded"));
  PUTGD_VERIFY_STEADY(banner->isVisible()&&fixture.client.held.isEmpty(),800);
  QVERIFY(read.open(QIODevice::ReadOnly));QCOMPARE(read.readAll(),written);
 }
 // #1704: removing the destination the default route uses falls back only the routes the file still
 // has on it when the removal is sent: a route changed elsewhere to another destination meanwhile
 // stays after Retry saving.
 void removalFallbackKeepsARouteChangedElsewhere(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  QCOMPARE(service.document()["captureDefaults"].toObject()["profileId"].toString(),QString("development-s3"));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  // A rename of another destination is in flight, so the removal waits behind it.
  window.findChild<QListWidget*>("destinationList")->setCurrentRow(1);auto* label=window.findChild<QLineEdit*>("destinationLabel");label->setText("Renamed alternate");QMetaObject::invokeMethod(label,"editingFinished");
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  window.findChild<QListWidget*>("destinationList")->setCurrentRow(0);
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();
  // Elsewhere the default route moves to the third destination.
  auto elsewhere=service.document();auto capture=elsewhere["captureDefaults"].toObject();capture["profileId"]="synthetic-third";elsewhere["captureDefaults"]=capture;QVERIFY(service.save(elsewhere,service.revision()));
  QVERIFY(fixture.client.release());
  auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");PUTGD_TRY_VERIFY(retry->isVisible()&&window.findChild<QWidget*>("destinationConflictBanner")->isVisible());
  retry->click();
  PUTGD_TRY_VERIFY((fixture.client.release(),!profileIds(service.document()).contains("development-s3")&&service.document()["profiles"].toArray()[0].toObject()["label"]=="Renamed alternate"));
  PUTGD_VERIFY_STEADY((fixture.client.release(),service.document()["captureDefaults"].toObject()["profileId"]=="synthetic-third"),800);
 }
 // #1704: a destination removed while a save is in flight and one added straight after under the same
 // name: the new one gets an ID the file doesn't have, so it is saved as an add, not dropped as an
 // edit of the one going away.
 void removeThenAddTheSameNameSavesTheNewOne(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* list=window.findChild<QListWidget*>("destinationList");auto* label=window.findChild<QLineEdit*>("destinationLabel");
  list->setCurrentRow(2);label->setText("Third renamed");QMetaObject::invokeMethod(label,"editingFinished");
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  list->setCurrentRow(1);QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();
  QAction* add=nullptr;for(auto* action:window.findChildren<QAction*>())if(action->text()=="Amazon S3 / S3-compatible")add=action;QVERIFY(add);
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("Synthetic alternate");dialog->accept();}});add->trigger();
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");bucket->setText("replacement-bucket");QMetaObject::invokeMethod(bucket,"editingFinished");
  PUTGD_TRY_VERIFY((fixture.client.release(),profileIds(service.document())==QStringList({"development-s3","synthetic-third","synthetic-alternate-2"})));
  const auto added=service.document()["profiles"].toArray()[2].toObject();
  QCOMPARE(added["label"].toString(),QString("Synthetic alternate"));QCOMPARE(added["settings"].toObject()["bucket"].toString(),QString("replacement-bucket"));
 }
 // #1704: an edit of a destination whose saved Region today's rules refuse is refused however often
 // it is sent. It is sent once, said so, and the rest of the queue saves.
 void editRefusedForAnOldRegionIsNotResent(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);
  auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();auto fields=profile["settings"].toObject();fields["region"]="fips-us-east-1";profile["settings"]=fields;profiles[0]=profile;doc["profiles"]=profiles;
  QFile file(dir.filePath("config.json"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(doc).toJson());file.close();
  SettingsService service(file.fileName(),[&](auto,auto value){return provider.validateSettings(value);},nullptr,{},[&](auto,auto value){return provider.validateStoredSettings(value);});
  // Saves of the Prefix edit, told apart by the value they carry.
  UiSessionFixture fixture(service,secrets);int prefixSaves=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject& params){if(method=="settings.save"&&params["document"].toObject()["profiles"].toArray()[0].toObject()["settings"].toObject()["objectPrefix"]=="screens/")++prefixSaves;};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* prefix=window.findChild<QLineEdit*>("s3ObjectPrefix");QVERIFY(prefix);prefix->setText("screens/");QMetaObject::invokeMethod(prefix,"editingFinished");
  auto* notifications=window.settings()->findChild<QCheckBox*>("notifications");const bool before=service.document()["preferences"].toObject()["notifications"].toBool();notifications->click();
  PUTGD_TRY_COMPARE(service.document()["preferences"].toObject()["notifications"].toBool(),!before);
  PUTGD_VERIFY_STEADY(prefixSaves==1,1000);
  QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["objectPrefix"].toString(),QString());
  auto* status=window.findChild<QLabel*>("settingsStatus");PUTGD_TRY_VERIFY(status->text().contains("fips"));
 }
 // #1704: a destination added and removed before its add was sent leaves nothing to remove, so an
 // ID someone else uses meanwhile is never removed by Retry saving.
 void cancelledUnsentAddNeverRemovesAnotherDestination(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* list=window.findChild<QListWidget*>("destinationList");auto* label=window.findChild<QLineEdit*>("destinationLabel");
  list->setCurrentRow(0);label->setText("First renamed");QMetaObject::invokeMethod(label,"editingFinished");
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  QAction* add=nullptr;for(auto* action:window.findChildren<QAction*>())if(action->text()=="Amazon S3 / S3-compatible")add=action;QVERIFY(add);
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("Later");dialog->accept();}});add->trigger();
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();
  // Elsewhere, a destination with the ID the cancelled one had.
  auto elsewhere=service.document();auto profiles=elsewhere["profiles"].toArray();auto theirs=profiles[0].toObject();theirs["id"]="later";theirs["label"]="Theirs";theirs["credentials"]=QJsonObject{{"mode","keychain"},{"reference","putgd/later"}};profiles.append(theirs);elsewhere["profiles"]=profiles;QVERIFY(service.save(elsewhere,service.revision()));
  QVERIFY(fixture.client.release());
  auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");PUTGD_TRY_VERIFY(retry->isVisible());retry->click();
  PUTGD_TRY_VERIFY((fixture.client.release(),fixture.client.held.isEmpty()&&service.document()["profiles"].toArray()[0].toObject()["label"]=="First renamed"));
  PUTGD_VERIFY_STEADY((fixture.client.release(),profileIds(service.document()).contains("later")),800);
 }
 // A move whose save meets a file changed elsewhere isn't kept: the list goes back to the saved
 // order and says so, and Retry saving doesn't send the move again.
 void conflictingMovePutsTheSavedOrderBack(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  const auto saved=profileIds(service.document());
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  QJsonObject result;QVERIFY(!window.settings()->moveSelectedDestination(1,saved[0],result));
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  auto elsewhere=service.document();auto preferences=elsewhere["preferences"].toObject();preferences["captureDelaySeconds"]=5;elsewhere["preferences"]=preferences;QVERIFY(service.save(elsewhere,service.revision()));
  QVERIFY(fixture.client.release());
  auto* status=window.findChild<QLabel*>("destinationStatus");auto* banner=window.findChild<Ui::Banner*>("destinationConflictBanner");QVERIFY(banner);
  PUTGD_TRY_VERIFY(banner->isVisible()&&banner->textLabel()->text().contains("previous destination order is restored"));
  QStringList shown;auto* combo=window.settings()->findChild<QComboBox*>("destinationProfile");for(int i=0;i<combo->count();++i)shown.append(combo->itemData(i).toString());QCOMPARE(shown,saved);
  window.findChild<QPushButton*>("retryDestinationAutosave")->click();
  PUTGD_VERIFY_STEADY((fixture.client.release(),!banner->isVisible()&&profileIds(service.document())==saved),800);
  Q_UNUSED(status);
 }
 // #1704: a draft restored from a checkpoint, checkpointed again and restored again still replays
 // only its own changes, against the configuration it was first based on: a destination added
 // elsewhere meanwhile stays, and a destination added here whose ID was taken is saved anew.
 void twiceRestoredDraftReplaysOnlyItsOwnChanges(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.failOnce.insert("settings.save",Error{"synthetic_transport","Synthetic transport failure",{}});
  QJsonObject checkpoint;
  {MainWindow first(fixture.session);first.navigate("destinations");first.show();
   QAction* add=nullptr;for(auto* action:first.findChildren<QAction*>())if(action->text()=="Amazon S3 / S3-compatible")add=action;QVERIFY(add);
   QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("Later");dialog->accept();}});add->trigger();
   PUTGD_TRY_VERIFY(first.findChild<QPushButton*>("retryDestinationAutosave")->isVisible());
   first.findChild<QListWidget*>("destinationList")->setCurrentRow(0);auto* label=first.findChild<QLineEdit*>("destinationLabel");label->setText("Renamed here");QMetaObject::invokeMethod(label,"editingFinished");
   checkpoint=first.settings()->saveViewState();QVERIFY2(!checkpoint.contains("stateError"),qPrintable(checkpoint.value("stateError").toString()));}
  // Elsewhere: a destination added, and another one under the ID this window's new one has.
  auto elsewhere=service.document();auto profiles=elsewhere["profiles"].toArray();
  for(const auto* id:{"elsewhere-added","later"}){auto p=profiles.first().toObject();p["id"]=id;p["label"]=QString("Theirs ")+id;p["credentials"]=QJsonObject{{"mode","keychain"},{"reference",QString("putgd/")+id}};profiles.append(p);}
  elsewhere["profiles"]=profiles;QVERIFY(service.save(elsewhere,service.revision()));
  // A helper that restores a checkpoint has the current file already.
  PUTGD_TRY_COMPARE(fixture.session.revision(),service.revision());
  QJsonObject again;
  {MainWindow second(fixture.session);QVERIFY(second.settings()->restoreViewState(checkpoint));again=second.settings()->saveViewState();QVERIFY2(!again.contains("stateError"),qPrintable(again.value("stateError").toString()));}
  MainWindow third(fixture.session);QVERIFY(third.settings()->restoreViewState(again));third.navigate("destinations");third.show();
  auto* retry=third.findChild<QPushButton*>("retryDestinationAutosave");PUTGD_TRY_VERIFY(retry->isVisible());retry->click();
  const auto label=[&](const QString& id){for(const auto& value:service.document()["profiles"].toArray())if(value.toObject()["id"]==id)return value.toObject()["label"].toString();return QString();};
  PUTGD_TRY_VERIFY(label("development-s3")=="Renamed here"&&label("later-2")=="Later");
  PUTGD_VERIFY_STEADY(label("elsewhere-added")=="Theirs elsewhere-added"&&label("later")=="Theirs later",800);
 }
 // #1716: a destination order kept in a checkpoint is never restored. A move not saved yet is kept
 // with a rename, then another client reorders the file or changes something else. The restored
 // window shows the file's order at once, the kept order is never sent, and Retry saving still
 // saves the rename.
 void restoredMoveYieldsToAnotherClientsReorder_data(){QTest::addColumn<bool>("reordered");QTest::newRow("reordered elsewhere")<<true;QTest::newRow("other change elsewhere")<<false;}
 void restoredMoveYieldsToAnotherClientsReorder(){
  QFETCH(bool,reordered);
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.failOnce.insert("settings.save",Error{"synthetic_transport","Synthetic transport failure",{}});
  const auto before=profileIds(service.document());const QStringList kept{before[1],before[0],before[2]};
  int sentKept=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject& params){if(method=="settings.save"&&profileIds(params["document"].toObject())==kept)++sentKept;};
  QJsonObject checkpoint;
  {MainWindow first(fixture.session);first.navigate("destinations");first.show();
   auto* label=first.findChild<QLineEdit*>("destinationLabel");label->setText("Renamed here");QMetaObject::invokeMethod(label,"editingFinished");
   PUTGD_TRY_VERIFY(first.findChild<QPushButton*>("retryDestinationAutosave")->isVisible());
   QJsonObject result;QVERIFY(!first.settings()->moveSelectedDestination(1,before[0],result));
   checkpoint=first.settings()->saveViewState();QVERIFY(!checkpoint.contains("stateError"));QVERIFY(checkpoint.contains("baseDocument"));}
  QCOMPARE(sentKept,0);
  auto elsewhere=service.document();
  if(reordered){QJsonArray reversed;const auto profiles=elsewhere["profiles"].toArray();for(int i=profiles.size()-1;i>=0;--i)reversed.append(profiles[i]);elsewhere["profiles"]=reversed;}
  else{auto preferences=elsewhere["preferences"].toObject();preferences["captureDelaySeconds"]=5;elsewhere["preferences"]=preferences;}
  QVERIFY(service.save(elsewhere,service.revision()));
  const auto filed=profileIds(service.document());
  PUTGD_TRY_COMPARE(fixture.session.revision(),service.revision());
  MainWindow second(fixture.session);QVERIFY(second.settings()->restoreViewState(checkpoint));second.navigate("destinations");second.show();
  auto* shown=second.settings()->findChild<QComboBox*>("destinationProfile");QVERIFY(shown);
  const auto shownIds=[&]{QStringList ids;for(int i=0;i<shown->count();++i)ids.append(shown->itemData(i).toString());return ids;};
  QCOMPARE(shownIds(),filed);
  auto* retry=second.findChild<QPushButton*>("retryDestinationAutosave");PUTGD_TRY_VERIFY(retry->isVisible());retry->click();
  const auto renamed=[&]{for(const auto& value:service.document()["profiles"].toArray())if(value.toObject()["id"]==before[0])return value.toObject()["label"].toString();return QString();};
  PUTGD_TRY_COMPARE(renamed(),QString("Renamed here"));
  PUTGD_TRY_VERIFY(second.settings()->flushAutosave());
  QCOMPARE(sentKept,0);QCOMPARE(profileIds(service.document()),filed);QCOMPARE(shownIds(),filed);
 }
 // #1716: replaying a checkpoint never queues an order. Its rename fails on the first Retry, another
 // client reorders again, and the second Retry saves the rename over that client's order intact.
 void restoredReplayQueuesNoOrderAcrossRetries(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.failOnce.insert("settings.save",Error{"synthetic_transport","Synthetic transport failure",{}});
  const auto before=profileIds(service.document());
  const auto reorder=[&](const QStringList& ids){auto doc=service.document();QHash<QString,QJsonValue> byId;for(const auto& value:doc["profiles"].toArray())byId.insert(value.toObject()["id"].toString(),value);QJsonArray ordered;for(const auto& id:ids)ordered.append(byId.value(id));doc["profiles"]=ordered;return service.save(doc,service.revision());};
  QJsonObject checkpoint;
  {MainWindow first(fixture.session);first.navigate("destinations");first.show();
   auto* label=first.findChild<QLineEdit*>("destinationLabel");label->setText("Renamed here");QMetaObject::invokeMethod(label,"editingFinished");
   PUTGD_TRY_VERIFY(first.findChild<QPushButton*>("retryDestinationAutosave")->isVisible());
   QJsonObject result;QVERIFY(!first.settings()->moveSelectedDestination(1,before[0],result));
   checkpoint=first.settings()->saveViewState();QVERIFY(!checkpoint.contains("stateError"));}
  QVERIFY(reorder({before[2],before[1],before[0]}));PUTGD_TRY_COMPARE(fixture.session.revision(),service.revision());
  MainWindow second(fixture.session);QVERIFY(second.settings()->restoreViewState(checkpoint));second.navigate("destinations");second.show();
  auto* retry=second.findChild<QPushButton*>("retryDestinationAutosave");PUTGD_TRY_VERIFY(retry->isVisible());
  fixture.client.failOnce.insert("settings.save",Error{"synthetic_transport","Synthetic transport failure",{}});retry->click();
  PUTGD_TRY_VERIFY(retry->isVisible()&&retry->isEnabled()&&!fixture.client.failOnce.contains("settings.save"));
  const QStringList theirs{before[0],before[2],before[1]};QVERIFY(reorder(theirs));
  auto* banner=second.findChild<QWidget*>("destinationConflictBanner");PUTGD_TRY_VERIFY(banner->isVisible());retry->click();
  const auto renamed=[&]{for(const auto& value:service.document()["profiles"].toArray())if(value.toObject()["id"]==before[0])return value.toObject()["label"].toString();return QString();};
  PUTGD_TRY_COMPARE(renamed(),QString("Renamed here"));PUTGD_TRY_VERIFY(second.settings()->flushAutosave());
  QCOMPARE(profileIds(service.document()),theirs);
 }
 // #1716: a checkpoint that held only a move holds nothing once the move isn't restored: Settings
 // opens saved, with no Retry saving offered.
 void moveOnlyCheckpointRestoresAsSaved(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.failOnce.insert("settings.save",Error{"synthetic_transport","Synthetic transport failure",{}});
  const auto before=profileIds(service.document());
  QJsonObject checkpoint;
  {MainWindow first(fixture.session);first.navigate("destinations");first.show();
   QJsonObject result;QVERIFY(!first.settings()->moveSelectedDestination(1,before[0],result));
   PUTGD_TRY_VERIFY(first.findChild<QPushButton*>("retryDestinationAutosave")->isVisible());
   checkpoint=first.settings()->saveViewState();QVERIFY(!checkpoint.contains("stateError"));QVERIFY(checkpoint["dirty"].toBool());}
  MainWindow second(fixture.session);QVERIFY(second.settings()->restoreViewState(checkpoint));second.navigate("destinations");second.show();
  PUTGD_VERIFY_STEADY(!second.findChild<QPushButton*>("retryDestinationAutosave")->isVisible()&&!second.settings()->saveViewState()["dirty"].toBool(),500);
  QCOMPARE(profileIds(service.document()),before);
 }
 // #1704: an older checkpoint has no base, and the file has changed since: Retry saving says the
 // changes can't be merged and writes nothing; the draft stays visible.
 void legacyCheckpointIsNotMergedIntoAChangedFile(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.failOnce.insert("settings.save",Error{"synthetic_transport","Synthetic transport failure",{}});
  QJsonObject checkpoint;
  {MainWindow first(fixture.session);first.navigate("destinations");first.show();auto* label=first.findChild<QLineEdit*>("destinationLabel");label->setText("Renamed here");QMetaObject::invokeMethod(label,"editingFinished");
   PUTGD_TRY_VERIFY(first.findChild<QPushButton*>("retryDestinationAutosave")->isVisible());checkpoint=first.settings()->saveViewState();QVERIFY(!checkpoint.contains("stateError"));}
  checkpoint.remove("baseDocument");
  auto elsewhere=service.document();auto preferences=elsewhere["preferences"].toObject();preferences["captureDelaySeconds"]=5;elsewhere["preferences"]=preferences;QVERIFY(service.save(elsewhere,service.revision()));
  const auto saved=service.document();
  PUTGD_TRY_COMPARE(fixture.session.revision(),service.revision());
  MainWindow second(fixture.session);QVERIFY(second.settings()->restoreViewState(checkpoint));second.navigate("destinations");second.show();
  auto* retry=second.findChild<QPushButton*>("retryDestinationAutosave");PUTGD_TRY_VERIFY(retry->isVisible());retry->click();
  auto* status=second.findChild<QLabel*>("settingsStatus");
  PUTGD_TRY_VERIFY(status->text().contains("can't be merged automatically")||second.findChild<Ui::Banner*>("destinationConflictBanner")->textLabel()->text().contains("can't be merged automatically"));
  PUTGD_VERIFY_STEADY(service.document()==saved&&second.findChild<QLineEdit*>("destinationLabel")->text()=="Renamed here",800);
 }
 // #1704: an add removed while it was in flight, whose save then meets a conflict, is never removed
 // by its ID after Retry saving: that ID may be someone else's now.
 void cancelledInFlightAddNeverRemovesById(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  QAction* add=nullptr;for(auto* action:window.findChildren<QAction*>())if(action->text()=="Amazon S3 / S3-compatible")add=action;QVERIFY(add);
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("Later");dialog->accept();}});add->trigger();
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();
  auto elsewhere=service.document();auto profiles=elsewhere["profiles"].toArray();auto theirs=profiles[0].toObject();theirs["id"]="later";theirs["label"]="Theirs";theirs["credentials"]=QJsonObject{{"mode","keychain"},{"reference","putgd/later"}};profiles.append(theirs);elsewhere["profiles"]=profiles;QVERIFY(service.save(elsewhere,service.revision()));
  QVERIFY(fixture.client.release());
  auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");auto* banner=window.findChild<QWidget*>("destinationConflictBanner");PUTGD_TRY_VERIFY(retry->isVisible());retry->click();
  // Everything Retry saving sends has gone through.
  PUTGD_TRY_VERIFY((fixture.client.release(),!banner->isVisible()&&fixture.client.held.isEmpty()&&window.settings()->flushAutosave()));
  QVERIFY(profileIds(service.document()).contains("later"));
 }
 // #1704: with no default output in the file, setting one sends only its destination and profile;
 // another writer's sharing mode for it, set meanwhile, is kept after Retry saving.
 void newDefaultOutputKeepsSharingSetElsewhere(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  QVERIFY(!service.document().contains("captureDefaults"));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* route=window.settings()->findChild<QComboBox*>("defaultCaptureDestination");QVERIFY(route);route->setCurrentIndex(route->findData(QString()));
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  auto elsewhere=service.document();elsewhere["captureDefaults"]=QJsonObject{{"destination","upload"},{"profileId","development-s3"},{"publicationMode","share"}};QVERIFY(service.save(elsewhere,service.revision()));
  QVERIFY(fixture.client.release());
  auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");PUTGD_TRY_VERIFY(retry->isVisible());retry->click();
  PUTGD_TRY_VERIFY((fixture.client.release(),service.document()["captureDefaults"].toObject()["destination"]=="local"));
  QCOMPARE(service.document()["captureDefaults"].toObject()["profileId"],QJsonValue(QJsonValue::Null));
  QCOMPARE(service.document()["captureDefaults"].toObject()["publicationMode"].toString(),QString("share"));
 }
 // #1704: a route to an add the resident refuses outright waits for nothing, whether it was chosen
 // before the refusal (as reported) or after it. With no default output in the file, choosing one
 // also makes the actions inherit it; none of that is sent while the add can't be saved. Credential
 // setup stays available, and once the add is saved the route and the inheritance go with it.
 void routeToRefusedAddWaitsForNothing_data(){QTest::addColumn<bool>("beforeRefusal");QTest::newRow("before refusal")<<true;QTest::newRow("after refusal")<<false;}
 void routeToRefusedAddWaitsForNothing(){
  QFETCH(bool,beforeRefusal);
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(Settings::developmentTemplate(),{}));QVERIFY(!service.document().contains("captureDefaults"));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  QAction* add=nullptr;for(auto* action:window.findChildren<QAction*>())if(action->text()=="Amazon S3 / S3-compatible")add=action;QVERIFY(add);
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("Second");dialog->accept();}});add->trigger();
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  auto* route=window.settings()->findChild<QComboBox*>("defaultCaptureDestination");QVERIFY(route->findData("second")>=0);
  // Before: chosen while the add is still in flight, so queued before the refusal.
  if(beforeRefusal)route->setCurrentIndex(route->findData("second"));
  QVERIFY(fixture.client.release());
  // The refusal is answered when nothing is in flight.
  PUTGD_TRY_VERIFY(fixture.client.held.isEmpty()&&window.settings()->flushAutosave());
  if(!beforeRefusal)route->setCurrentIndex(route->findData("second"));
  // Nothing goes while the add can't be saved: anything sent now would be held.
  PUTGD_VERIFY_STEADY(fixture.client.held.isEmpty(),500);fixture.client.hold={};
  auto* status=window.findChild<QLabel*>("settingsStatus");QVERIFY2(status->text().startsWith("A route to "),qPrintable(status->text()));
  window.findChild<QListWidget*>("destinationList")->setCurrentRow(0);
  window.findChild<QLineEdit*>("credentialAccessKey")->setText("SYNTHETICACCESSKEY");window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-secret");
  PUTGD_TRY_VERIFY(window.findChild<QPushButton*>("storeCredentials")->isEnabled());
  window.findChild<QLineEdit*>("credentialAccessKey")->clear();window.findChild<QLineEdit*>("credentialSecretKey")->clear();
  window.findChild<QListWidget*>("destinationList")->setCurrentRow(1);auto* bucket=window.findChild<QLineEdit*>("s3Bucket");bucket->setText("second-bucket");QMetaObject::invokeMethod(bucket,"editingFinished");
  const auto action=[&](const QString& name){return service.document().value("actions").toObject().value(name).toObject().value("destination");};
  PUTGD_TRY_VERIFY(service.document().value("captureDefaults").toObject().value("profileId")==QJsonValue(QStringLiteral("second"))&&action("region")==QJsonValue(QStringLiteral("default")));
 }
 // #1704: Paste from a field's context menu saves at once, like a paste from the keyboard.
 void contextMenuPasteSavesAtOnce(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  UiSessionFixture fixture(service,secrets);QList<QJsonObject> held;fixture.client.hold=[&held](const QString& method,const QJsonObject& params){if(method!="settings.save")return false;held.append(params);return true;};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();window.activateWindow();QVERIFY(putgd::test::waitForWindowActive(&window));
  // The paste replaces the selected text, so no edit of its own comes before it.
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");QVERIFY(bucket&&!bucket->text().isEmpty());bucket->setFocus();bucket->selectAll();PUTGD_TRY_VERIFY(bucket->hasFocus());
  QGuiApplication::clipboard()->setText("pasted-bucket");
  QTimer::singleShot(0,[]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;for(auto* action:menu->actions())if(action->text().section('\t',0,0)==QCoreApplication::translate("QLineEdit","&Paste"))action->trigger();menu->close();});
  QContextMenuEvent event(QContextMenuEvent::Mouse,QPoint(4,4),bucket->mapToGlobal(QPoint(4,4)));QApplication::sendEvent(bucket,&event);
  QCOMPARE(bucket->text(),QString("pasted-bucket"));
  // Sent at once, not after the typing pause: the save in flight is the paste's.
  QCOMPARE(window.settings()->saveViewState()["stateError"].toString(),QString("request_awaiting_acceptance"));
  PUTGD_TRY_COMPARE(held.size(),qsizetype(1));
  QCOMPARE(held.first()["document"].toObject()["profiles"].toArray()[0].toObject()["settings"].toObject()["bucket"].toString(),QString("pasted-bucket"));
 }
 // #1704 (Sol T6): removing the destination an explicit recording route uses leaves no route to it
 // behind: a later route edit doesn't queue one that would wait forever, and nothing stays unsaved.
 void removingRecordingDestinationStrandsNoRoute(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto doc=threeDestinations();auto actions=doc["actions"].toObject();auto record=actions["record"].toObject();record["destination"]="upload";record["profileId"]="synthetic-third";actions["record"]=record;doc["actions"]=actions;
  {const auto saved=service.save(doc,{});QVERIFY2(saved,qPrintable(saved.error.message));}
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  window.findChild<QListWidget*>("destinationList")->setCurrentRow(2);
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();
  PUTGD_TRY_VERIFY(!profileIds(service.document()).contains("synthetic-third"));
  auto* route=window.settings()->findChild<QComboBox*>("defaultCaptureDestination");route->setCurrentIndex(route->findData("synthetic-alternate"));
  PUTGD_TRY_COMPARE(service.document()["captureDefaults"].toObject()["profileId"].toString(),QString("synthetic-alternate"));
  PUTGD_TRY_VERIFY(window.settings()->flushAutosave()&&!window.settings()->actionSaveError());
  QCOMPARE(service.document()["actions"].toObject()["record"].toObject()["destination"].toString(),QString("local"));
 }
 // #1704 (Sol T5): a restored draft that switched the default output from one destination to
 // another replays the destination with the profile, so it isn't left local by a writer elsewhere.
 void restoredRouteSwitchKeepsItsDestination(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.failOnce.insert("settings.save",Error{"synthetic_transport","Synthetic transport failure",{}});
  QJsonObject checkpoint;
  {MainWindow first(fixture.session);first.navigate("destinations");first.show();auto* route=first.settings()->findChild<QComboBox*>("defaultCaptureDestination");route->setCurrentIndex(route->findData("synthetic-alternate"));
   PUTGD_TRY_VERIFY(first.findChild<QPushButton*>("retryDestinationAutosave")->isVisible());checkpoint=first.settings()->saveViewState();QVERIFY(!checkpoint.contains("stateError"));}
  auto elsewhere=service.document();elsewhere["captureDefaults"]=QJsonObject{{"destination","local"},{"profileId",QJsonValue::Null},{"publicationMode","default"}};QVERIFY(service.save(elsewhere,service.revision()));
  PUTGD_TRY_COMPARE(fixture.session.revision(),service.revision());
  MainWindow second(fixture.session);QVERIFY(second.settings()->restoreViewState(checkpoint));second.navigate("destinations");second.show();
  auto* retry=second.findChild<QPushButton*>("retryDestinationAutosave");PUTGD_TRY_VERIFY(retry->isVisible());retry->click();
  PUTGD_TRY_VERIFY(service.document()["captureDefaults"].toObject()["profileId"]=="synthetic-alternate");
  QCOMPARE(service.document()["captureDefaults"].toObject()["destination"].toString(),QString("upload"));
 }
 // #1704 (Sol T7): a route to an add the resident refused, then a conflict and Retry saving: once
 // the add is fixed and saved, the route goes too.
 void routeToRefusedAddSurvivesRetry(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  QAction* add=nullptr;for(auto* action:window.findChildren<QAction*>())if(action->text()=="Amazon S3 / S3-compatible")add=action;QVERIFY(add);
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("Second");dialog->accept();}});add->trigger();
  auto* status=window.findChild<QLabel*>("destinationStatus");PUTGD_TRY_VERIFY(status->text().contains("bucket"));
  auto* route=window.settings()->findChild<QComboBox*>("defaultCaptureDestination");route->setCurrentIndex(route->findData("second"));
  auto elsewhere=service.document();auto preferences=elsewhere["preferences"].toObject();preferences["captureDelaySeconds"]=5;elsewhere["preferences"]=preferences;QVERIFY(service.save(elsewhere,service.revision()));
  auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");PUTGD_TRY_VERIFY(retry->isVisible());retry->click();
  PUTGD_TRY_VERIFY(!window.findChild<QWidget*>("destinationConflictBanner")->isVisible());
  window.findChild<QListWidget*>("destinationList")->setCurrentRow(1);auto* bucket=window.findChild<QLineEdit*>("s3Bucket");bucket->setText("second-bucket");QMetaObject::invokeMethod(bucket,"editingFinished");
  PUTGD_TRY_COMPARE(service.document()["captureDefaults"].toObject()["profileId"].toString(),QString("second"));
 }
 // #1704 (Sol T9): a restored draft that makes an action inherit a new default output replays the
 // default output first, so the inheritance isn't refused and dropped.
 void restoredInheritanceReplaysDefaultFirst(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  QVERIFY(!service.document().contains("captureDefaults"));
  UiSessionFixture fixture(service,secrets);fixture.client.failOnce.insert("settings.save",Error{"synthetic_transport","Synthetic transport failure",{}});
  QJsonObject checkpoint;
  {MainWindow first(fixture.session);first.navigate("settings");first.show();auto* inherit=first.settings()->findChild<QCheckBox*>("useDefaultRegion");QVERIFY(inherit);if(inherit->isChecked())inherit->click();inherit->click();
   PUTGD_TRY_VERIFY(first.settings()->findChild<QPushButton*>("retryAutosave")->isVisible());checkpoint=first.settings()->saveViewState();QVERIFY(!checkpoint.contains("stateError"));
   QCOMPARE(checkpoint["draft"].toObject()["actions"].toObject()["region"].toObject()["destination"].toString(),QString("default"));}
  MainWindow second(fixture.session);QVERIFY(second.settings()->restoreViewState(checkpoint));second.navigate("settings");second.show();
  auto* retry=second.settings()->findChild<QPushButton*>("retryAutosave");PUTGD_TRY_VERIFY(retry->isVisible());retry->click();
  PUTGD_TRY_VERIFY(service.document()["actions"].toObject()["region"].toObject()["destination"]=="default"&&service.document().contains("captureDefaults"));
  PUTGD_TRY_VERIFY(second.settings()->flushAutosave()&&!second.settings()->actionSaveError());
 }
 // #1704 (CodeRabbit): removing the destination Clipboard uploads to, while another save is held,
 // then another writer moving Clipboard to a third destination: after Retry saving it stays there.
 void removedDestinationsRouteMovedElsewhereStays(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  QCOMPARE(service.document()["actions"].toObject()["clipboard"].toObject()["profileId"].toString(),QString("synthetic-third"));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* list=window.findChild<QListWidget*>("destinationList");auto* label=window.findChild<QLineEdit*>("destinationLabel");
  list->setCurrentRow(0);label->setText("First renamed");QMetaObject::invokeMethod(label,"editingFinished");
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  list->setCurrentRow(2);QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();
  auto elsewhere=service.document();auto actions=elsewhere["actions"].toObject();auto clipboard=actions["clipboard"].toObject();clipboard["profileId"]="synthetic-alternate";actions["clipboard"]=clipboard;elsewhere["actions"]=actions;QVERIFY(service.save(elsewhere,service.revision()));
  QVERIFY(fixture.client.release());
  auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");auto* banner=window.findChild<QWidget*>("destinationConflictBanner");PUTGD_TRY_VERIFY(retry->isVisible());retry->click();
  PUTGD_TRY_VERIFY((fixture.client.release(),!banner->isVisible()&&fixture.client.held.isEmpty()&&window.settings()->flushAutosave()&&!profileIds(service.document()).contains("synthetic-third")));
  QCOMPARE(service.document()["actions"].toObject()["clipboard"].toObject()["profileId"].toString(),QString("synthetic-alternate"));
 }
 // #1704 (CodeRabbit): an add queued behind another save, the default output routed to it, and the
 // add removed before it is sent: nothing of either goes. Another writer then routes the default
 // output to a third destination, and after Retry saving it stays there.
 void unsentAddsRouteMovedElsewhereStays(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* label=window.findChild<QLineEdit*>("destinationLabel");auto* route=window.settings()->findChild<QComboBox*>("defaultCaptureDestination");QVERIFY(label&&route);
  label->setText("First renamed");QMetaObject::invokeMethod(label,"editingFinished");
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  QAction* add=nullptr;for(auto* action:window.findChildren<QAction*>())if(action->text()=="Amazon S3 / S3-compatible")add=action;QVERIFY(add);
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("Later");dialog->accept();}});add->trigger();
  QVERIFY(route->findData("later")>=0);route->setCurrentIndex(route->findData("later"));
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();
  QVERIFY(route->findData("later")<0);
  auto elsewhere=service.document();auto capture=elsewhere["captureDefaults"].toObject();capture["profileId"]="synthetic-third";elsewhere["captureDefaults"]=capture;QVERIFY(service.save(elsewhere,service.revision()));
  QVERIFY(fixture.client.release());
  auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");auto* banner=window.findChild<QWidget*>("destinationConflictBanner");PUTGD_TRY_VERIFY(retry->isVisible());retry->click();
  PUTGD_TRY_VERIFY((fixture.client.release(),!banner->isVisible()&&fixture.client.held.isEmpty()&&window.settings()->flushAutosave()&&service.document()["profiles"].toArray()[0].toObject()["label"]=="First renamed"));
  QCOMPARE(service.document().value("captureDefaults").toObject().value("profileId"),QJsonValue(QStringLiteral("synthetic-third")));
  QVERIFY(!profileIds(service.document()).contains("later"));
 }
 // #1704 (CodeRabbit): an add removed while in flight that the resident saved, though its answer
 // was lost. The removal waits until the window has read the file again, so Retry saving clicked
 // before that read answers, or after it fails, never drops it; once the file shows the add as it
 // was sent, it is removed.
 void lostAddAnswerRemovalWaitsForTheFile_data(){QTest::addColumn<bool>("readFails");QTest::newRow("read held")<<false;QTest::newRow("read fails")<<true;}
 void lostAddAnswerRemovalWaitsForTheFile(){
  QFETCH(bool,readFails);
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);
  // The held requests' methods, in the order the fake resident holds them.
  QStringList methods;QJsonObject sent;bool holdReads=false;
  fixture.client.hold=[&](const QString& method,const QJsonObject& params){const bool hold=method=="settings.save"||(holdReads&&method=="ui.snapshot");if(hold){methods.append(method);if(method=="settings.save"&&sent.isEmpty())sent=params;}return hold;};
  const auto releaseFirst=[&](const QString& method){const int i=methods.indexOf(method);if(i<0)return false;methods.removeAt(i);return fixture.client.release(i);};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  QAction* add=nullptr;for(auto* action:window.findChildren<QAction*>())if(action->text()=="Amazon S3 / S3-compatible")add=action;QVERIFY(add);
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("Later");dialog->accept();}});add->trigger();
  PUTGD_TRY_COMPARE(methods.count("settings.save"),qsizetype(1));
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();
  // The resident saves the add, but its answer is lost, and the window's reads of the file wait.
  holdReads=true;
  {const auto saved=service.save(sent["document"].toObject(),service.revision());QVERIFY2(saved,qPrintable(saved.error.message));}QVERIFY(profileIds(service.document()).contains("later"));
  fixture.client.failOnce.insert("settings.save",Error{"client_unavailable","Synthetic lost answer",{}});QVERIFY(releaseFirst("settings.save"));
  auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");PUTGD_TRY_VERIFY(retry->isVisible()&&methods.contains("ui.snapshot"));
  retry->click();
  if(readFails){
   fixture.client.failOnce.insert("ui.snapshot",Error{"client_unavailable","Synthetic read failure",{}});QVERIFY(releaseFirst("ui.snapshot"));
   // The removal stays pending: nothing is removed, and Retry saving is offered again.
   PUTGD_TRY_VERIFY(retry->isVisible()&&retry->isEnabled());
   PUTGD_VERIFY_STEADY(profileIds(service.document()).contains("later")&&!methods.contains("settings.save"),500);
   retry->click();
  }else PUTGD_VERIFY_STEADY(profileIds(service.document()).contains("later")&&!methods.contains("settings.save"),500);
  holdReads=false;
  PUTGD_TRY_VERIFY((releaseFirst("ui.snapshot")||releaseFirst("settings.save"),!profileIds(service.document()).contains("later")));
  PUTGD_TRY_VERIFY((releaseFirst("ui.snapshot")||releaseFirst("settings.save"),fixture.client.held.isEmpty()&&!retry->isVisible()&&window.settings()->flushAutosave()));
 }
 // #1704: the default route goes to a destination another writer then removes and routes past.
 // After Retry saving the form shows the file's route, not an empty choice, so picking "No
 // destination" is a change and saves.
 void routeToDestinationRemovedElsewhereShowsTheFilesRoute(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* route=window.settings()->findChild<QComboBox*>("defaultCaptureDestination");auto* label=window.findChild<QLineEdit*>("destinationLabel");QVERIFY(route&&label);
  const auto filed=[&]{return service.document().value("captureDefaults").toObject().value("profileId");};
  route->setCurrentIndex(route->findData("synthetic-alternate"));
  PUTGD_TRY_VERIFY((fixture.client.release(),filed()==QJsonValue(QStringLiteral("synthetic-alternate"))));
  label->setText("First renamed");QMetaObject::invokeMethod(label,"editingFinished");
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  auto elsewhere=service.document();auto profiles=elsewhere["profiles"].toArray();for(int i=0;i<profiles.size();++i)if(profiles[i].toObject()["id"]=="synthetic-alternate"){profiles.removeAt(i);break;}elsewhere["profiles"]=profiles;
  auto capture=elsewhere["captureDefaults"].toObject();capture["profileId"]="synthetic-third";elsewhere["captureDefaults"]=capture;QVERIFY(service.save(elsewhere,service.revision()));
  QVERIFY(fixture.client.release());
  auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");auto* banner=window.findChild<QWidget*>("destinationConflictBanner");PUTGD_TRY_VERIFY(retry->isVisible());retry->click();
  PUTGD_TRY_VERIFY((fixture.client.release(),!banner->isVisible()&&fixture.client.held.isEmpty()&&window.settings()->flushAutosave()));
  QCOMPARE(route->currentData().toString(),QString("synthetic-third"));
  route->setCurrentIndex(route->findData(QString()));
  PUTGD_TRY_VERIFY((fixture.client.release(),filed().isNull()));
 }
 // #1704 (CodeRabbit): with no default output in the file, the sharing mode shown is the Region
 // action's. Choosing only a destination doesn't send it, so a default output another writer
 // creates meanwhile keeps its own sharing mode after Retry saving.
 void legacySharingShownIsNoEdit(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto doc=Settings::developmentTemplate();auto actions=doc["actions"].toObject();auto region=actions["region"].toObject();region["publicationMode"]="share";actions["region"]=region;doc["actions"]=actions;
  {const auto saved=service.save(doc,{});QVERIFY2(saved,qPrintable(saved.error.message));}QVERIFY(!service.document().contains("captureDefaults"));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* route=window.settings()->findChild<QComboBox*>("defaultCaptureDestination");route->setCurrentIndex(route->findData(QString()));
  PUTGD_TRY_COMPARE(fixture.client.held.size(),qsizetype(1));
  auto elsewhere=service.document();elsewhere["captureDefaults"]=QJsonObject{{"destination","upload"},{"profileId","development-s3"},{"publicationMode","private"}};{const auto saved=service.save(elsewhere,service.revision());QVERIFY2(saved,qPrintable(saved.error.message));}
  QVERIFY(fixture.client.release());
  auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");PUTGD_TRY_VERIFY(retry->isVisible());retry->click();
  PUTGD_TRY_VERIFY((fixture.client.release(),service.document()["captureDefaults"].toObject()["destination"]=="local"));
  QCOMPARE(service.document()["captureDefaults"].toObject()["publicationMode"].toString(),QString("private"));
 }
 // #1678: the dashboard's S3 API address fills Account ID, Jurisdiction and Bucket, saved as one
 // edit. A refused fill saves none of it and leaves what was pasted showing, so the saved bucket
 // never pairs with an account it was not pasted with; nothing is saved just because it was shown.
 void cloudflareR2PasteSavesAccountJurisdictionAndBucketTogether(){
  QTemporaryDir dir;CredentialStore secrets;auto s3=std::make_shared<S3Provider>(secrets);R2Provider r2(s3);
  SettingsService service(dir.filePath("config.json"),[&](const QString& id,const QJsonObject& value){
   if(id==r2::providerId&&value["bucket"]=="refused-bucket")return Error{"s3_settings_invalid","Synthetic refusal of this bucket",{{"field","bucket"}}};
   return id==r2::providerId?r2.validateSettings(value):s3->validateSettings(value);});
  const QString oldAccount="0123456789abcdef0123456789abcdef",pastedAccount="fedcba9876543210fedcba9876543210",nextAccount="00112233445566778899aabbccddeeff";
  auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto settings=r2::defaults();settings["accountId"]=oldAccount;settings["bucket"]="old-bucket";
  profiles.append(QJsonObject{{"id","r2-lab"},{"label","R2 lab"},{"provider",r2::providerId},{"settings",settings},{"credentials",QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}}}});
  doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));
  const auto saved=[&]{for(const auto& profile:service.document()["profiles"].toArray())if(profile.toObject()["id"]=="r2-lab")return profile.toObject()["settings"].toObject();return QJsonObject{};};
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  window.findChild<QListWidget*>("destinationList")->setCurrentRow(1);
  auto* account=window.findChild<QLineEdit*>("r2AccountId");auto* bucket=window.findChild<QLineEdit*>("s3Bucket");auto* jurisdiction=window.findChild<QComboBox*>("r2Jurisdiction");
  auto* unitLine=window.findChild<QLabel*>("r2UnitStatus");QVERIFY(unitLine);
  PUTGD_TRY_COMPARE(account->text(),oldAccount);
  const auto shown=service.revision();PUTGD_COMPARE_STEADY(service.revision(),shown,300);
  // Ctrl+V over a selection fills all three at once; the save is refused, so none of them is saved.
  QGuiApplication::clipboard()->setText("https://"+pastedAccount.toUpper()+".eu.r2.cloudflarestorage.com/refused-bucket");
  account->setFocus();account->selectAll();QTest::keyClick(account,Qt::Key_V,Qt::ControlModifier);
  QCOMPARE(account->text(),pastedAccount);QCOMPARE(jurisdiction->currentData().toString(),QString("eu"));QCOMPARE(bucket->text(),QString("refused-bucket"));
  PUTGD_TRY_VERIFY(window.settings()->flushAutosave()&&unitLine->isVisible()&&unitLine->text().contains("Synthetic refusal"));
  QCOMPARE(saved()["accountId"].toString(),oldAccount);QCOMPARE(saved()["jurisdiction"].toString(),QString("default"));QCOMPARE(saved()["bucket"].toString(),QString("old-bucket"));
  QCOMPARE(account->text(),pastedAccount);QCOMPARE(bucket->text(),QString("refused-bucket"));
  // The answer writes no field message: the values are valid, and only the unit line says why they aren't saved.
  QVERIFY(!window.findChild<QLabel*>("s3BucketError")->isVisible());
  // Leaving the unchanged field does nothing.
  emit account->editingFinished();
  PUTGD_COMPARE_STEADY(saved()["bucket"].toString(),QString("old-bucket"),300);QVERIFY(unitLine->isVisible());
  // The context menu's Paste over a selection: a valid address saves all three together.
  QGuiApplication::clipboard()->setText("https://"+nextAccount+".r2.cloudflarestorage.com/good-bucket");
  account->selectAll();
  QTimer::singleShot(0,[]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;for(auto* action:menu->actions())if(action->text().section('\t',0,0)==QCoreApplication::translate("QLineEdit","&Paste"))action->trigger();menu->close();});
  QContextMenuEvent event(QContextMenuEvent::Mouse,QPoint(4,4),account->mapToGlobal(QPoint(4,4)));QApplication::sendEvent(account,&event);
  PUTGD_TRY_COMPARE(saved()["bucket"].toString(),QString("good-bucket"));
  QCOMPARE(saved()["accountId"].toString(),nextAccount);QCOMPARE(saved()["jurisdiction"].toString(),QString("default"));
  PUTGD_TRY_VERIFY(!unitLine->isVisible());
  // #1699 Q5: an address with no bucket, for another account, empties Bucket: the bucket shown is the
  // old account's, and nothing is saved until one is entered for this account.
  QGuiApplication::clipboard()->setText("https://"+oldAccount+".r2.cloudflarestorage.com");
  account->setFocus();account->selectAll();QTest::keyClick(account,Qt::Key_V,Qt::ControlModifier);
  QCOMPARE(account->text(),oldAccount);QVERIFY(bucket->text().isEmpty());
  PUTGD_COMPARE_STEADY(saved()["accountId"].toString(),nextAccount,300);QCOMPARE(saved()["bucket"].toString(),QString("good-bucket"));
  PUTGD_TRY_VERIFY(unitLine->isVisible());
  QVERIFY(service.revision()!=shown);
 }
 // #1699: a Cloudflare R2 destination saves as one profile, latest state winning. A revert typed
 // while a save is in flight is sent once that save is answered, so what is saved ends as what is
 // shown; a second R2 destination saves on its own while the first's draft can't be saved.
 void cloudflareR2SavesLatestStateAndAddsBesideAnIncompleteDraft(){
  QTemporaryDir dir;CredentialStore secrets;auto s3=std::make_shared<S3Provider>(secrets);R2Provider r2(s3);
  SettingsService service(dir.filePath("config.json"),[&](const QString& id,const QJsonObject& value){return id==r2::providerId?r2.validateSettings(value):s3->validateSettings(value);});
  auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto settings=r2::defaults();settings["accountId"]="0123456789abcdef0123456789abcdef";settings["bucket"]="old-bucket";
  profiles.append(QJsonObject{{"id","r2-lab"},{"label","R2 lab"},{"provider",r2::providerId},{"settings",settings},{"credentials",QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}}}});
  doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));
  const auto saved=[&](const QString& id){for(const auto& profile:service.document()["profiles"].toArray())if(profile.toObject()["id"]==id)return profile.toObject();return QJsonObject{};};
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  window.findChild<QListWidget*>("destinationList")->setCurrentRow(1);
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");PUTGD_TRY_COMPARE(bucket->text(),QString("old-bucket"));
  const auto type=[&](const QString& text){bucket->setFocus();bucket->setText(text);bucket->setModified(true);emit bucket->editingFinished();};
  fixture.client.replyDelayMs["settings.save"]=400;const auto before=service.revision();
  type("new-bucket");type("old-bucket");
  PUTGD_TRY_VERIFY(service.revision()!=before);PUTGD_TRY_VERIFY(window.settings()->flushAutosave());
  QCOMPARE(saved("r2-lab")["settings"].toObject()["bucket"].toString(),bucket->text());
  fixture.client.replyDelayMs.remove("settings.save");
  // An incomplete draft is not sent and says why; the new destination doesn't wait for it or carry it.
  type("");auto* unitLine=window.findChild<QLabel*>("r2UnitStatus");PUTGD_TRY_VERIFY(unitLine->isVisible());
  // #1699 Q4: an R2 destination isn't added until Account ID, Jurisdiction and Bucket are valid, so the
  // resident is never sent one it must refuse.
  QStringList sent;fixture.client.hold=[&](const QString& method,const QJsonObject& params){if(method=="settings.save")sent.append(QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact)));return false;};
  QJsonObject added;window.settings()->r2Command({{"action","add"},{"label","Second R2"}},[&](Error error,QJsonObject state){QVERIFY2(!error,qPrintable(error.message));added=state;});
  const auto second=added["profileId"].toString();QVERIFY(!second.isEmpty());
  PUTGD_COMPARE_STEADY(std::any_of(sent.cbegin(),sent.cend(),[&](const QString& doc){return doc.contains("\""+second+"\"");}),false,300);
  window.settings()->r2Command({{"action","paste"},{"text","https://00112233445566778899aabbccddeeff.eu.r2.cloudflarestorage.com/second-bucket"}},[](Error error,QJsonObject){QVERIFY2(!error,qPrintable(error.message));});
  PUTGD_TRY_COMPARE(saved(second)["settings"].toObject()["bucket"].toString(),QString("second-bucket"));
  QCOMPARE(saved(second)["provider"].toString(),r2::providerId);
  QCOMPARE(saved("r2-lab")["settings"].toObject()["bucket"].toString(),QString("old-bucket"));
 }
 // #1699 Q1: a destination another writer converted to Cloudflare R2 while this window showed it as
 // S3 is never converted back from here: an edit made in the stale form sends that edit, not the
 // whole old destination, even after the conflict and Retry saving.
 void conversionElsewhereIsNeverUndoneFromAStaleForm(){
  QTemporaryDir dir;CredentialStore secrets;auto s3=std::make_shared<S3Provider>(secrets);R2Provider r2(s3);
  SettingsService service(dir.filePath("config.json"),[&](const QString& id,const QJsonObject& value){return id==r2::providerId?r2.validateSettings(value):s3->validateSettings(value);});
  QVERIFY(service.save(Settings::developmentTemplate(),{}));
  UiSessionFixture fixture(service,secrets);fixture.client.hold=[](const QString& method,const QJsonObject&){return method=="settings.save";};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* label=window.findChild<QLineEdit*>("destinationLabel");auto* retry=window.findChild<QPushButton*>("retryDestinationAutosave");QVERIFY(label&&retry);
  label->setFocus();label->setText("Renamed once");emit label->editingFinished();
  PUTGD_TRY_VERIFY(!fixture.client.held.isEmpty());
  // Elsewhere: the destination becomes Cloudflare R2 while this window's rename is in flight.
  auto doc=service.document();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();auto settings=r2::defaults();
  settings["accountId"]="0123456789abcdef0123456789abcdef";settings["bucket"]="elsewhere-bucket";profile["provider"]=r2::providerId;profile["settings"]=settings;profiles[0]=profile;doc["profiles"]=profiles;
  QVERIFY(service.save(doc,service.revision()));
  // The window learns of it as the resident's notice makes it: the accepted document is R2 now, while
  // the form still shows the S3 destination with this window's rename on top.
  fixture.client.hold={};QVERIFY(fixture.client.release());
  bool refreshed=false;fixture.session.refreshSnapshot([&](Error){refreshed=true;});PUTGD_TRY_VERIFY(refreshed);
  QCOMPARE(window.findChild<QComboBox*>("storageProvider")->currentData().toString(),QString("s3"));
  label->setFocus();label->setText("Renamed twice");emit label->editingFinished();
  PUTGD_TRY_VERIFY((retry->isVisible()?retry->click():void(),!retry->isVisible()&&window.settings()->flushAutosave()));
  const auto saved=service.document()["profiles"].toArray()[0].toObject();
  QCOMPARE(saved["provider"].toString(),r2::providerId);QCOMPARE(saved["settings"].toObject()["bucket"].toString(),QString("elsewhere-bucket"));
 }
 // #1678: an S3 destination on Cloudflare R2 becomes a Cloudflare R2 destination only when Move to
 // Cloudflare R2 is pressed, keeping its ID, routes and credentials; showing it saves nothing.
 void moveToCloudflareR2KeepsIdRoutesAndCredentials(){
  QTemporaryDir dir;CredentialStore secrets;auto s3=std::make_shared<S3Provider>(secrets);R2Provider r2(s3);
  SettingsService service(dir.filePath("config.json"),[&](const QString& id,const QJsonObject& value){return id==r2::providerId?r2.validateSettings(value):s3->validateSettings(value);});
  auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();auto settings=profile["settings"].toObject();
  settings["endpoint"]="https://0123456789abcdef0123456789abcdef.eu.r2.cloudflarestorage.com";settings["region"]="Western North America (WNAM)";settings["bucket"]="lab-bucket";settings["aclMode"]="omit";settings["storageClass"]="STANDARD";
  // A Link domain S3 accepts (plain HTTP on loopback) and R2 refuses.
  settings["linkMode"]="public";settings["publicUrlBase"]="http://127.0.0.1:9000/pub";
  profile["settings"]=settings;profiles[0]=profile;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));
  auto before=service.document();
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* move=window.findChild<QPushButton*>("moveToCloudflareR2");QVERIFY(move);PUTGD_TRY_VERIFY(move->isVisibleTo(&window));
  PUTGD_COMPARE_STEADY(service.document(),before,300);
  // #1699 Q2: what R2 would refuse is said first and nothing moves, so the move never saves without it.
  move->click();
  PUTGD_COMPARE_STEADY(service.document(),before,300);
  QVERIFY2(window.findChild<QLabel*>("destinationStatus")->text().startsWith("Before moving to Cloudflare R2:"),qPrintable(window.findChild<QLabel*>("destinationStatus")->text()));
  auto* link=window.findChild<QLineEdit*>("s3PublicUrlBase");link->setFocus();link->setText("https://pub-0123456789abcdef.r2.dev");emit link->editingFinished();
  PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["publicUrlBase"].toString(),QString("https://pub-0123456789abcdef.r2.dev"));
  settings["publicUrlBase"]="https://pub-0123456789abcdef.r2.dev";before=service.document();
  move->click();
  const auto moved=[&]{return service.document()["profiles"].toArray()[0].toObject();};
  PUTGD_TRY_COMPARE(moved()["provider"].toString(),r2::providerId);
  QCOMPARE(moved()["id"],profile["id"]);QCOMPARE(moved()["label"],profile["label"]);QCOMPARE(moved()["credentials"],profile["credentials"]);
  QCOMPARE(service.document()["actions"],before["actions"]);
  QCOMPARE(moved()["settings"].toObject()["accountId"].toString(),QString("0123456789abcdef0123456789abcdef"));
  QCOMPARE(r2::s3Settings(moved()["settings"].toObject())["endpoint"],settings["endpoint"]);
  QVERIFY(!move->isVisibleTo(&window));
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
  QVERIFY2(!checkpoint.contains("stateError"),qPrintable(checkpoint.value("stateError").toString()));
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
