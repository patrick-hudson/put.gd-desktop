#include "SettingsGuiFixture.h"
class SettingsGui:public SettingsGuiFixture {
 Q_OBJECT
private slots:
 void idleSettingsLayoutSettles_data(){
  QTest::addColumn<int>("page");QTest::addColumn<QSize>("size");
  for(int page=0;page<4;++page)for(const auto size:{QSize(1440,960),QSize(1060,680)})QTest::newRow(qPrintable(QString("page%1-%2").arg(page).arg(size.width())))<<page<<size;
 }
 void idleSettingsLayoutSettles(){
  QFETCH(int,page);QFETCH(QSize,size);
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(size);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(page);
  class Events final:public QObject {
  public:
   explicit Events(QWidget* root):root_(root){quiet.start();qApp->installEventFilter(this);}
   ~Events()override{qApp->removeEventFilter(this);}
   int layouts=0,paints=0;QElapsedTimer quiet;QHash<QString,int> sources;
  protected:
   bool eventFilter(QObject* object,QEvent* event)override{
    if(event->type()!=QEvent::LayoutRequest&&event->type()!=QEvent::Paint)return false;
    auto* widget=qobject_cast<QWidget*>(object);if(!widget||(widget!=root_&&!root_->isAncestorOf(widget)))return false;
    if(event->type()==QEvent::Paint)++paints;else{++layouts;quiet.restart();++sources[widget->objectName()+":"+widget->metaObject()->className()];}return false;
   }
  private:QWidget* root_;
  } events(&window);
  window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  // Include retained, hidden pages: they must settle too. A layout feedback
  // loop never reaches this quiet interval, regardless of machine speed.
  const bool settled=putgd::test::waitFor([&]{return events.quiet.elapsed()>=100;});
  if(!settled)qWarning()<<"Unsettled Settings layouts:"<<events.layouts<<events.sources;
  QVERIFY(settled);events.layouts=events.paints=0;
  PUTGD_VERIFY_STEADY(events.layouts<64&&events.paints<64,1000);
  qInfo()<<"Idle Settings layout/paint events:"<<events.layouts<<events.paints;
 }
 void stackedLabelsStayCloseToTheirNextWidget_data(){QTest::addColumn<int>("pixels");QTest::newRow("100")<<14;QTest::newRow("125")<<18;QTest::newRow("150")<<21;}
 void stackedLabelsStayCloseToTheirNextWidget(){
  QFETCH(int,pixels);Fonts::load();Ui::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,900);window.show();int checked=0;
  // Queued responsive layout can finish after a fixed sample delay under load.
  const auto check=[&](QWidget* page){putgd::test::settle(40);for(auto* value:page->findChildren<QWidget*>())if((value->property("preferenceInlineValue").toBool()||value->property("preferenceChoiceValue").toBool())&&value->isVisible()&&value->property("preferenceChoiceSizer").isValid()&&!value->property("preferenceShortcutValue").toBool()){auto* row=value->parentWidget();auto* label=qobject_cast<QWidget*>(row->property("preferenceLabel").value<QObject*>());QVERIFY(label);auto* captions=label->parentWidget();bool stacked=false;int gap=0;const auto fits=[&]{stacked=row->property("stacked").toBool();gap=stacked?value->mapTo(row,QPoint()).y()-captions->mapTo(row,QPoint(0,captions->height())).y():value->mapTo(row,QPoint()).x()-captions->mapTo(row,QPoint(captions->width(),0)).x();return gap>=0&&gap<=Ui::space(stacked?Ui::Space::s:Ui::Space::l)+1;};PUTGD_TRY_VERIFY2(fits(),qPrintable(QString("%1 caption gap %2, stacked %3").arg(value->objectName()).arg(gap).arg(stacked)));++checked;}};
  window.navigate("settings");auto* tabs=window.findChild<QTabWidget*>("settingsTabs");for(int page=0;page<tabs->count();++page){tabs->setCurrentIndex(page);for(auto* toggle:tabs->currentWidget()->findChildren<QToolButton*>())if(toggle->property("uiDisclosure").toBool())toggle->setChecked(true);check(tabs->currentWidget());}window.navigate("destinations");window.findChild<QToolButton*>("advancedSettings")->setChecked(true);auto* provider=window.findChild<QComboBox*>("storageProvider");for(const auto* kind:{"s3","backblaze-b2","proton-drive"}){provider->setCurrentIndex(provider->findData(kind));check(window.findChild<QScrollArea*>("destinationScroll")->widget());}QVERIFY(checked>0);
 }
 void railPageHeadersShareTheirTopAndCentre(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1440,960);window.navigate("settings");window.show();putgd::test::settle(30);
  auto* tabs=window.findChild<QTabWidget*>("settingsTabs");int centre=-1;
  for(int page=0;page<tabs->count();++page){tabs->setCurrentIndex(page);putgd::test::settle(30);auto* heading=window.findChild<QLabel*>("settingsPageHeading"+tabs->tabText(page));auto* scroll=qobject_cast<QScrollArea*>(tabs->currentWidget());PUTGD_TRY_COMPARE(heading->mapTo(scroll->viewport(),QPoint()).y(),qMin(Ui::pageHeaderTop(),Ui::space(Ui::Space::l)));QCOMPARE(heading->height(),Ui::scaled(Ui::Metric::control));const int y=heading->mapTo(&window,heading->rect().center()).y();if(centre<0)centre=y;QCOMPARE(y,centre);}
  window.navigate("destinations");auto* heading=window.findChild<QLineEdit*>("destinationLabel");auto* more=window.findChild<QPushButton*>("destinationActions");PUTGD_TRY_VERIFY2(qAbs(heading->mapTo(&window,heading->rect().center()).y()-centre)<=1,qPrintable(QString("Destination centre %1, Settings centre %2; title height %3").arg(heading->mapTo(&window,heading->rect().center()).y()).arg(centre).arg(heading->height())));PUTGD_TRY_VERIFY(qAbs(more->mapTo(&window,more->rect().center()).y()-centre)<=1);
 }
 void formsShareGutterAndAlignToFirstControlLine_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<21;}
 void formsShareGutterAndAlignToFirstControlLine(){
  QFETCH(int,pixels);Fonts::load();Ui::apply(*qApp);const auto oldFont=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(oldFont);});auto font=oldFont;font.setPixelSize(pixels);qApp->setFont(font);
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");
  auto* notifications=window.findChild<QCheckBox*>("notifications");putgd::test::settle(30);auto* preferences=notifications->parentWidget();while(preferences&&!preferences->findChild<QFormLayout*>())preferences=preferences->parentWidget();QVERIFY(preferences);auto* preferencesForm=preferences->findChild<QFormLayout*>();QVERIFY(preferencesForm);QVERIFY(!preferencesForm->labelForField(notifications->parentWidget()));
  int gutter=0;
  const auto check=[&](QWidget* page){for(auto* form:page->findChildren<QFormLayout*>())for(int row=0;row<form->rowCount();++row){auto* item=form->itemAt(row,QFormLayout::LabelRole);if(!item||!item->widget())continue;auto* label=item->widget();PUTGD_TRY_VERIFY(label->minimumWidth()>0);if(!gutter)gutter=label->minimumWidth();QCOMPARE(label->minimumWidth(),gutter);QVERIFY(gutter<=Ui::scaled(240));QVERIFY(label->height()>=Ui::scaled(Ui::Metric::control));QVERIFY(label->minimumWidth()+16<=Ui::Metric::labelGutter);QVERIFY(form->labelAlignment().testFlag(Qt::AlignBaseline));}};
  for(int page=0;page<tabs->count();++page){tabs->setCurrentIndex(page);putgd::test::settle(30);check(tabs->currentWidget());}
  window.navigate("destinations");auto* bucket=window.findChild<QLineEdit*>("s3Bucket");auto* error=window.findChild<QLabel*>("s3BucketError");QVERIFY(bucket&&error);error->setText("Synthetic validation error that wraps under the control.");error->show();putgd::test::settle(30);gutter=0;check(window.findChild<QScrollArea*>("destinationScroll")->widget());
  QLabel* caption=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->buddy()==bucket)caption=label;QVERIFY(caption);QVERIFY(!QRect(caption->mapTo(&window,QPoint()),caption->size()).intersects(QRect(bucket->mapTo(&window,QPoint()),bucket->size())));QCOMPARE(bucket->width(),window.findChild<QComboBox*>("s3RegionChoices")->width());QVERIFY(error->mapTo(&window,QPoint()).y()>=bucket->mapTo(&window,QPoint(0,bucket->height())).y());
  auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));putgd::test::settle(30);gutter=0;check(window.findChild<QScrollArea*>("destinationScroll")->widget());
 }
 void preferenceTextAndFirstControlGeometry_data(){
  QTest::addColumn<double>("scale");QTest::addColumn<QSize>("size");
  for(double scale:{1.0,1.25,1.5})for(const auto size:{QSize(1440,960),QSize(1060,680)})QTest::newRow(qPrintable(QString("%1-%2").arg(scale).arg(size.width())))<<scale<<size;
 }
 void preferenceTextAndFirstControlGeometry(){
  QFETCH(double,scale);QFETCH(QSize,size);Fonts::load();Ui::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPointSizeF(14*scale*72/96);qApp->setFont(font);
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(size);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");
  const auto check=[&](QWidget* page){
   int labels=0;
   for(auto* form:page->findChildren<QFormLayout*>()){
    auto* card=form->parentWidget();if(!card->isVisibleTo(page)||!card->property("preferenceCard").toBool())continue;
    for(int row=0;row<form->rowCount();++row){
     auto* item=form->itemAt(row,QFormLayout::FieldRole);auto* field=item?item->widget():nullptr;if(!field||!form->isRowVisible(row))continue;
     if(auto* holder=qobject_cast<QWidget*>(field->property("preferenceLabel").value<QObject*>())){
      auto* caption=qobject_cast<QLabel*>(holder);if(!caption)caption=holder->findChild<QLabel*>();if(!caption||caption->isHidden())continue;++labels;
      QCOMPARE(caption->mapTo(card,QPoint()).x(),Ui::space(Ui::Space::l));
      auto* control=caption->buddy();if(!control||qobject_cast<QLabel*>(control)||field->property("preferenceStacked").toBool()||field->property("stacked").toBool())continue;
      if(qobject_cast<QComboBox*>(control->parentWidget())||qobject_cast<QPushButton*>(control->parentWidget()))control=control->parentWidget();
      const int delta=caption->mapTo(card,caption->rect().center()).y()-control->mapTo(card,control->rect().center()).y();
      QVERIFY2(qAbs(delta)<=2,qPrintable(QString("%1: label/control centre delta %2").arg(control->objectName()).arg(delta)));
     }
     if(auto* toggle=qobject_cast<QToolButton*>(field);toggle&&(toggle->property("preferenceDisclosure").toBool()||toggle->property("preferenceAddRow").toBool())){
      const int inset=Ui::space(Ui::Space::l);QCOMPARE(toggle->mapTo(card,QPoint(inset,0)).x(),inset);
      // Antialiased glyph edges need not contain the exact foreground colour.
      // Measure visible ink against the backdrop, in device pixels, away from
      // focus outlines, trailing glyphs and card borders.
      const QImage actual=toggle->grab().toImage();const qreal dpr=actual.devicePixelRatio();const QColor background=actual.pixelColor(qRound(2*dpr),actual.height()/2);
      QImage expected(actual.size(),QImage::Format_ARGB32_Premultiplied);expected.setDevicePixelRatio(dpr);expected.fill(background);const QColor ink=toggle->property("preferenceAddRow").toBool()?Theme::current().textSecondary:Theme::current().text;
      const bool addRow=toggle->property("preferenceAddRow").toBool();const int leading=addRow?toggle->iconSize().width()+Ui::space(Ui::Space::s):0,trailing=addRow?0:Ui::scaled(24);
      QPainter painter(&expected);if(addRow)toggle->icon().paint(&painter,QRect(inset,(toggle->height()-toggle->iconSize().height())/2,toggle->iconSize().width(),toggle->iconSize().height()));Ui::paintText(painter,QRect(inset+leading,0,toggle->width()-2*inset-leading-trailing,toggle->height()),toggle->text(),ink,Ui::font());painter.end();
      const auto contrast=[&](const QColor& c){return qAbs(c.red()-background.red())+qAbs(c.green()-background.green())+qAbs(c.blue()-background.blue());};
      const auto first=[&](const QImage& image,int from=0){for(int x=std::max(from,qCeil(2*dpr));x<image.width()/2;++x)for(int y=qCeil(2*dpr);y<image.height()-qCeil(2*dpr);++y)if(contrast(image.pixelColor(x,y))>=contrast(ink)/4)return x;return -1;};
      const int actualStart=first(actual),expectedStart=first(expected);QVERIFY(actualStart>=0);QVERIFY(expectedStart>=0);
      QVERIFY2(qAbs(actualStart-expectedStart)/dpr<=1,qPrintable(QString("%1 (%2) actual %3 expected %4 DPR %5 font %6").arg(toggle->objectName(),toggle->text()).arg(actualStart/dpr).arg(expectedStart/dpr).arg(dpr).arg(toggle->font().toString())));
      if(addRow){
       // The leading glyph matched above; the text must still follow it at the glyph gap.
       QImage text(actual.size(),QImage::Format_ARGB32_Premultiplied);text.setDevicePixelRatio(dpr);text.fill(background);QPainter textPainter(&text);Ui::paintText(textPainter,QRect(inset+leading,0,toggle->width()-2*inset-leading,toggle->height()),toggle->text(),ink,Ui::font());textPainter.end();
       const int from=qCeil((inset+toggle->iconSize().width()+1)*dpr),actualText=first(actual,from),expectedText=first(text,from);QVERIFY(expectedText>=0);
       QVERIFY2(actualText>=0&&qAbs(actualText-expectedText)/dpr<=1,qPrintable(QString("%1 text actual %2 expected %3 DPR %4").arg(toggle->objectName()).arg(actualText/dpr).arg(expectedText/dpr).arg(dpr)));
      }
     }
    }
   }
   QVERIFY(labels>0);
  };
  for(int page=0;page<tabs->count();++page){tabs->setCurrentIndex(page);for(auto* toggle:tabs->currentWidget()->findChildren<QToolButton*>())if(toggle->property("uiDisclosure").toBool())toggle->setChecked(true);putgd::test::settle(40);check(tabs->currentWidget());}
  window.navigate("destinations");auto* scroll=window.findChild<QScrollArea*>("destinationScroll");auto* provider=window.findChild<QComboBox*>("storageProvider");window.findChild<QToolButton*>("advancedSettings")->setChecked(true);
  for(const auto* kind:{"s3","backblaze-b2","proton-drive"}){provider->setCurrentIndex(provider->findData(kind));putgd::test::settle(40);check(scroll->widget());}
  provider->setCurrentIndex(provider->findData("s3"));auto* mode=window.findChild<QComboBox*>("credentialMode");mode->setCurrentIndex(mode->currentData()=="session"?mode->findData("keychain"):mode->findData("session"));putgd::test::settle(40);check(scroll->widget());
 }
 void formHintsAndTrailingActionsKeepTheirGeometry_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<21;}
 void formHintsAndTrailingActionsKeepTheirGeometry(){
  QFETCH(int,pixels);Fonts::load();Ui::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1440,960);window.show();window.navigate("settings");auto* tabs=window.findChild<QTabWidget*>("settingsTabs");
  const auto check=[&](QWidget* page){for(auto* hint:page->findChildren<QLabel*>()){if(!hint->objectName().endsWith("Hint")||!hint->isVisibleTo(page))continue;auto* holder=hint->parentWidget();if(holder->property("visualRole").toString()!="settingRow")continue;auto* layout=qobject_cast<QVBoxLayout*>(holder->layout());QVERIFY(layout);auto* line=layout->itemAt(0)->layout();QVERIFY(line);int bottom=0;for(int i=0;i<line->count();++i)if(auto* field=line->itemAt(i)->widget();field&&field->isVisibleTo(holder))bottom=qMax(bottom,field->mapTo(holder,QPoint(0,field->height())).y());QVERIFY2(hint->y()-bottom>=Ui::space(Ui::Space::xs),qPrintable(QString("%1 gap %2; hint y/h %3/%4, holder h %5, line y/h %6/%7, minimum h %8, hfw %9").arg(hint->objectName()).arg(hint->y()-bottom).arg(hint->y()).arg(hint->height()).arg(holder->height()).arg(line->geometry().y()).arg(line->geometry().height()).arg(layout->minimumSize().height()).arg(layout->heightForWidth(holder->width()))));}};
  for(int i=0;i<tabs->count();++i){tabs->setCurrentIndex(i);putgd::test::settle(30);check(tabs->currentWidget());}
  window.navigate("destinations");putgd::test::settle(30);check(window.findChild<QScrollArea*>("destinationScroll")->widget());auto* acl=window.findChild<QComboBox*>("s3AclMode");auto* aclHint=window.findChild<QLabel*>("s3AclModeHint");QVERIFY(acl&&aclHint);auto* scroll=window.findChild<QScrollArea*>("destinationScroll");scroll->ensureWidgetVisible(aclHint,0,40);PUTGD_VERIFY_STEADY(aclHint->isVisible(),100);QVERIFY(!QRect(aclHint->mapTo(&window,QPoint()),aclHint->size()).intersects(QRect(acl->mapTo(&window,QPoint()),acl->size())));QVERIFY(window.findChild<QLineEdit*>("s3PublicUrlBase")->mapTo(&window,QPoint()).y()-aclHint->mapTo(&window,QPoint(0,aclHint->height())).y()>=Ui::space(Ui::Space::m));auto* secret=window.findChild<QLineEdit*>("credentialSecretKey");auto* show=window.findChild<QAction*>("showCredentialSecretAction");QVERIFY(show);QVERIFY(secret->actions().contains(show));QVERIFY(!show->icon().isNull());QVERIFY(window.findChild<QPushButton*>("showCredentialSecret")->isHidden());
  auto* example=window.findChild<QLabel*>("linkExample");QLabel* caption=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->text()=="Example:")caption=label;QVERIFY(caption);const int labelBaseline=caption->mapTo(&window,QPoint()).y()+(caption->height()-caption->fontMetrics().height())/2+caption->fontMetrics().ascent();const int valueBaseline=example->mapTo(&window,QPoint()).y()+(example->height()-example->fontMetrics().height())/2+example->fontMetrics().ascent();QVERIFY(qAbs(labelBaseline-valueBaseline)<=1);
  window.findChild<QToolButton*>("advancedSettings")->setChecked(true);putgd::test::settle(30);auto* custom=window.findChild<QCheckBox*>("customEndpoint");auto* bucket=window.findChild<QLineEdit*>("s3Bucket");QLabel* bucketCaption=nullptr;for(auto* text:window.findChildren<QLabel*>())if(text->buddy()==bucket)bucketCaption=text;QVERIFY(bucketCaption);QCOMPARE(custom->mapTo(&window,QPoint()).x(),bucketCaption->mapTo(&window,QPoint()).x());
  auto* list=window.findChild<QListWidget*>("destinationList");list->setCurrentRow(2);auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));putgd::test::settle(30);auto* links=window.findChild<QRadioButton*>("protonCreateShareLink");QVERIFY(links);QCOMPARE(links->mapTo(&window,QPoint()).x(),window.findChild<QLabel*>("protonAccountStatus")->mapTo(&window,QPoint()).x());QVERIFY(!window.findChild<QPushButton*>("protonCopyingNote"));check(window.findChild<QScrollArea*>("destinationScroll")->widget());list->setCurrentRow(0);putgd::test::settle(50);scroll->ensureWidgetVisible(aclHint,0,40);PUTGD_VERIFY_STEADY(aclHint->isVisible(),100);QVERIFY(!QRect(aclHint->mapTo(&window,QPoint()),aclHint->size()).intersects(QRect(acl->mapTo(&window,QPoint()),acl->size())));
 }
 void topLevelPagesShareOneRailEdge(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();
  for(const auto& page:{"history","settings","destinations"}){window.navigate(page);putgd::test::settle(20);for(const auto& name:{"historySidebar","settingsRail","destinationRail"})if(auto* rail=window.findChild<QWidget*>(name))if(rail->objectName()=="destinationRail")QVERIFY(rail->width()>=Ui::railWidth()&&rail->width()<=Ui::railWidth()+2*Ui::space(Ui::Space::xl));else QCOMPARE(rail->width(),Ui::railWidth());}
 }
 void technicalPlaceholdersKeepTheValueTypeface(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings()->showLocalCopySettings();window.show();auto* input=window.findChild<QLineEdit*>("folderImages");QVERIFY(input);QCOMPARE(input->placeholderText(),QString("Same as Save to"));QCOMPARE(input->font().family(),Ui::monoFamily());input->setText("synthetic");QCOMPARE(input->font().family(),Ui::monoFont(Ui::Type::technical).family());input->clear();QCOMPARE(input->font().family(),Ui::monoFamily());
 }
 void inlineIdentifiersKeepTheInterfaceTypeface(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  for(const auto& name:{"s3Bucket","s3ObjectPrefix","s3Region"}){auto* edit=window.findChild<QLineEdit*>(name);QVERIFY(edit);edit->clear();QCOMPARE(edit->font().family(),Ui::uiFamily());edit->setText("synthetic");QCOMPARE(edit->font().family(),Ui::uiFamily());QCOMPARE(edit->font().pixelSize(),Ui::scaled(Ui::Type::body));}
 }
 void linkExampleFollowsDomainAndDisclosureCaptionsAreClickable(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* field=window.findChild<QLineEdit*>("s3PublicUrlBase");auto* example=window.findChild<QLabel*>("linkExample");auto* row=window.findChild<QWidget*>("advancedConnectionRow");auto* toggle=window.findChild<QToolButton*>("advancedSettings");auto* summary=window.findChild<QLabel*>("connectionSummary");QVERIFY(row&&toggle&&summary&&example);QVERIFY(row->isAncestorOf(toggle));QVERIFY(row->isAncestorOf(summary));QVERIFY(!example->wordWrap());QCOMPARE(example->font().family(),Ui::monoFamily());QCOMPARE(example->font().pixelSize(),Ui::scaled(13));QCOMPARE(example->palette().color(QPalette::WindowText),Theme::current().textSecondary);QVERIFY(example->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));field->setFocus();field->setText("https://synthetic.example.invalid");PUTGD_TRY_VERIFY(example->text().startsWith(field->text()));QCOMPARE(example->toolTip(),example->text());QTest::mouseClick(summary,Qt::LeftButton);QVERIFY(toggle->isChecked());QVERIFY(summary->isHidden());QTest::keyClick(toggle,Qt::Key_Space);QVERIFY(!toggle->isChecked());QVERIFY(!summary->isHidden());
 }
 void checkboxAccessibilityKeepsActionAndCategory(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();
  for(const auto& [name,category]:QList<QPair<QString,QString>>{{"launchAtLogin","Startup"},{"keepUploadCopies","Each capture"},{"notifications","Notifications"}}){auto* check=window.findChild<QCheckBox*>(name);QCOMPARE(check->accessibleName(),check->text());auto* hint=window.findChild<QLabel*>(name+"Hint");QCOMPARE(check->accessibleDescription(),category+(hint?". "+hint->text():QString{}));}
  QWidget host;QCheckBox check("Do the action",&host);Ui::settingRow("Category","Help text",&check,&host);QCOMPARE(check.accessibleName(),check.text());QCOMPARE(check.accessibleDescription(),QString("Category. Help text"));
 }
 void statusBandScalesItsRegularSummaryAtNarrowAndLargeText(){
  Fonts::load();Ui::apply(*qApp);
  const auto original=QApplication::font();auto restore=qScopeGuard([&]{QApplication::setFont(original);});
  for(const int pixels:{14,21}){auto font=original;font.setPixelSize(pixels);QApplication::setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.resize(1060,680);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
   QCOMPARE(window.findChild<QLabel*>("appVersion")->font().pixelSize(),Ui::scaled(13));QCOMPARE(window.findChild<QPushButton*>("appVersionLink")->font().pixelSize(),Ui::scaled(13));auto* summary=window.findChild<QToolButton*>("captureOutputSummary");QCOMPARE(summary->font().pixelSize(),Ui::scaled(13));QCOMPARE(summary->font().weight(),QFont::Normal);if(pixels>14)QVERIFY(summary->font().pixelSize()>13);auto* band=window.findChild<QStatusBar*>("statusBand");QVERIFY(band);QVERIFY(summary->height()>=summary->fontMetrics().height());QVERIFY(band->rect().contains(QRect(summary->mapTo(band,QPoint()),summary->size())));QCOMPARE(window.width(),1060);
   QVERIFY(window.findChild<QAction*>("changeCaptureOutput"));QVERIFY(summary->height()>=32);QVERIFY(band->height()>=summary->height());
   window.settings()->showCaptureSettings();window.findChild<QToolButton*>("captureExceptionsToggle")->setChecked(true);
   const auto scheme=Theme::scheme();auto restoreScheme=qScopeGuard([&]{Theme::setScheme(scheme);});Theme::setScheme(Theme::Scheme::Light);
   for(const auto& action:Settings::invocableActionNames()){
    auto name=action.left(1).toUpper()+action.mid(1);auto* override=window.findChild<QCheckBox*>("override"+name);if(!override->isChecked())override->click();
    for(const auto& prefix:QStringList{"profile","publication"}){auto* control=window.findChild<QComboBox*>(prefix+name);control->ensurePolished();QCOMPARE(control->font().pixelSize(),Ui::font(Ui::Type::body).pixelSize());PUTGD_TRY_COMPARE(control->height(),window.findChild<QComboBox*>("defaultCaptureDestination")->height());QVERIFY2(putgd::test::waitFor([&]{return control->parentWidget()->rect().contains(control->geometry());}),qPrintable(QString("%1: control %2,%3 %4x%5, parent %6x%7, visible %8").arg(control->objectName()).arg(control->x()).arg(control->y()).arg(control->width()).arg(control->height()).arg(control->parentWidget()->width()).arg(control->parentWidget()->height()).arg(control->isVisible())));Ui::setLocked(control,true);Ui::setLocked(control,false);QCOMPARE(control->font().pixelSize(),Ui::font(Ui::Type::body).pixelSize());QCOMPARE(control->sizeHint().height(),window.findChild<QComboBox*>("defaultCaptureDestination")->sizeHint().height());PUTGD_TRY_COMPARE(control->height(),window.findChild<QComboBox*>("defaultCaptureDestination")->height());}
   }
   Theme::setScheme(scheme);
   auto* check=window.findChild<QCheckBox*>("notifications");QVERIFY(check->sizeHint().height()>=32);QCOMPARE(check->style()->pixelMetric(QStyle::PM_IndicatorWidth),Ui::scaled(17));
  }
 }
 void fileDialogsOpenTheSharedChooser(){
  // #259: Choose folder…, Browse… and Edit an image file… open the shared chooser,
  // and ui.fileDialog drives them. The fallback is forced: no portal is asked.
  qputenv("PUTGD_FILE_DIALOG","fallback");auto restore=qScopeGuard([]{qunsetenv("PUTGD_FILE_DIALOG");});
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto archive=doc["archive"].toObject();archive["root"]=dir.filePath("archive");doc["archive"]=archive;QVERIFY(service.save(doc,{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  MainWindow window(fixture.session);window.show();fixture.session.refreshSnapshot();
  fixture.server.setUiCommand([&](const QString& name,const QJsonObject& params,JsonReply reply){QCOMPARE(name,QString("fileDialog"));
   if(params["dialog"]=="editImage"){if(params["action"]=="open")window.chooseImageToEdit();else if(params["action"]=="cancel")window.editImageDialog().cancel();reply({},window.editImageDialog().state());return;}
   window.settings()->fileDialogCommand(params["dialog"].toString(),params["action"].toString(),params["field"].toString(),std::move(reply));});
  auto call=[&](const QJsonObject& params)->std::pair<Error,QJsonObject>{bool done=false;Error error{"test_timeout","No answer",{}};QJsonObject result;
   fixture.server.dispatch({{"schemaVersion",1},{"requestId","ui"},{"session",fixture.server.socketPath()},{"method","ui.fileDialog"},{"params",params}},[&](Error e,QJsonObject r){error=e;result=r;done=true;});
   if(!putgd::test::waitFor([&]{return done;},5000))qWarning("ui.fileDialog timed out");return {error,result};};
  QCOMPARE(call({{"dialog","editImage"},{"action","open"},{"field","root"}}).first.code,QString("invalid_request"));
  QCOMPARE(call({{"dialog","outputFolder"},{"action","open"},{"field","music"}}).first.code,QString("invalid_request"));
  QCOMPARE(call({{"dialog","shell"},{"action","open"}}).first.code,QString("invalid_request"));
  QCOMPARE(call({{"dialog","diagnosticsFolder"},{"action","open"}}).first.code,QString("diagnostics_dialog_closed"));
  // Browse… for Videos: the chosen folder lands in its field and autosaves.
  QTemporaryDir videos;QVERIFY(videos.isValid());
  auto [error,state]=call({{"dialog","outputFolder"},{"action","open"},{"field","videos"}});QVERIFY2(!error,qPrintable(error.message));
  QVERIFY(state["open"].toBool());QCOMPARE(state["backend"].toString(),QString("fallback"));QCOMPARE(state["mode"].toString(),QString("folder"));QCOMPARE(state["title"].toString(),QString("Choose output folder"));
  QCOMPARE(call({{"dialog","outputFolder"},{"action","open"},{"field","images"}}).first.code,QString("file_dialog_open"));
  QFileDialog* chooser=nullptr;PUTGD_TRY_VERIFY((chooser=window.findChild<QFileDialog*>("putgdFileDialog"))&&chooser->isVisible());
  QCOMPARE(chooser->fileMode(),QFileDialog::Directory);
  chooser->setDirectory(videos.path());chooser->selectFile(videos.path());QMetaObject::invokeMethod(chooser,"accept");
  PUTGD_TRY_COMPARE(window.findChild<QLineEdit*>("folderVideos")->text(),videos.path());
  PUTGD_TRY_COMPARE(service.snapshot().archive.folders.value("videos"),videos.path());
  QCOMPARE(call({{"dialog","outputFolder"},{"action","inspect"}}).second["last"].toObject()["status"].toString(),QString("chosen"));
  // Edit an image file…: opened, then cancelled through the command.
  auto opened=call({{"dialog","editImage"},{"action","open"}});QVERIFY(opened.second["open"].toBool());QCOMPARE(opened.second["mode"].toString(),QString("open"));
  PUTGD_TRY_VERIFY((chooser=window.findChild<QFileDialog*>("putgdFileDialog"))&&chooser->isVisible());QCOMPARE(chooser->nameFilters(),QStringList{"Images (*.png *.jpg *.jpeg)"});
  auto cancelled=call({{"dialog","editImage"},{"action","cancel"}});QVERIFY(!cancelled.second["open"].toBool());QCOMPARE(cancelled.second["last"].toObject()["status"].toString(),QString("cancelled"));
  QVERIFY(window.findChild<QWidget*>("editImageChooserFailure")->isHidden());
 }
 // A chooser that fails is not a cancel: its reason shows beside the action that
 // opened it, what was there stays, and Try again opens the chooser again. A
 // cancel stays silent.
 void chooserFailuresShowBesideTheirActionWithTryAgain(){
  qputenv("PUTGD_FILE_DIALOG","fallback");auto restore=qScopeGuard([]{qunsetenv("PUTGD_FILE_DIALOG");});
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto archive=doc["archive"].toObject();archive["root"]=dir.filePath("archive");doc["archive"]=archive;QVERIFY(service.save(doc,{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);window.show();fixture.session.refreshSnapshot();
  const FileDialog::Result failed{FileDialog::Result::Status::Failed,{},"The chosen location is not a local file."},cancelled{};
  // Local copies → Images.
  auto* images=window.findChild<QLineEdit*>("folderImages");images->setText(dir.filePath("kept"));auto* failure=window.findChild<QWidget*>("folderImagesChooserFailure");QVERIFY(failure&&failure->isHidden());
  window.settings()->folderChooserFinished("outputFolder","images",failed);QVERIFY(failure->isVisibleTo(window.settings()));
  auto* message=window.findChild<QLabel*>("folderImagesChooserError");QVERIFY2(message->text().contains("not a local file")&&message->text().contains("unchanged"),qPrintable(message->text()));QCOMPARE(images->text(),dir.filePath("kept"));
  QVERIFY(window.findChild<QWidget*>("archiveRootChooserFailure")->isHidden());
  window.findChild<QPushButton*>("folderImagesChooserRetry")->click();QVERIFY(window.settings()->findChild<QFileDialog*>("putgdFileDialog"));
  window.settings()->fileDialogCommand("outputFolder","cancel",{},[](Error,QJsonObject){});PUTGD_TRY_VERIFY(failure->isHidden());QCOMPARE(images->text(),dir.filePath("kept"));
  window.settings()->folderChooserFinished("outputFolder","images",cancelled);QVERIFY(failure->isHidden());
  // Edit an image file…: under the command band, nothing opened.
  QSignalSpy edits(&window,&MainWindow::editRequested);auto* banner=window.findChild<QWidget*>("editImageChooserFailure");QVERIFY(banner->isHidden());
  window.imageChooserFinished(cancelled);QVERIFY(banner->isHidden());
  window.imageChooserFinished(failed);QVERIFY(banner->isVisible());QVERIFY(window.findChild<QLabel*>("editImageChooserError")->text().contains("not a local file"));QCOMPARE(edits.count(),0);
  auto* band=window.findChild<QWidget*>("commandBand");PUTGD_TRY_COMPARE(banner->geometry().top(),band->geometry().bottom()+1);
  window.findChild<QPushButton*>("retryEditImageChooser")->click();QVERIFY(window.editImageDialog().isOpen());window.editImageDialog().cancel();PUTGD_TRY_VERIFY(banner->isHidden());QCOMPARE(edits.count(),0);
  window.imageChooserFinished(failed);window.findChild<QToolButton*>("dismissEditImageChooserFailure")->click();QVERIFY(banner->isHidden());
  // The package dialog's Choose folder…
  DiagnosticsBundles bundles(fixture.operations(),dir.filePath("state"));fixture.server.setDiagnosticsBundles(&bundles);
  bool opened=false;window.settings()->diagnosticsCommand({{"action","openPackage"}},[&](Error,QJsonObject){opened=true;});PUTGD_TRY_VERIFY(opened);auto* dialog=window.settings()->findChild<QDialog*>("diagnosticsPackageDialog");
  auto* packageFailure=dialog->findChild<QWidget*>("packageFolderChooserFailure");QVERIFY(packageFailure->isHidden());const auto folder=dialog->findChild<QLabel*>("packageFolder")->text();
  window.settings()->folderChooserFinished("diagnosticsFolder",{},failed);QVERIFY(packageFailure->isVisible());QCOMPARE(dialog->findChild<QLabel*>("packageFolder")->text(),folder);QVERIFY(dialog->findChild<QLabel*>("packageFolderChooserError")->text().contains("not a local file"));
  window.settings()->folderChooserFinished("diagnosticsFolder",{},cancelled);QVERIFY(packageFailure->isHidden());
  dialog->reject();
 }
 void conflictingReorderRestoresThePreviousOrder(){
  QTemporaryDir dir;const auto path=dir.filePath("config.json");SettingsService service(path,[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* list=window.findChild<QListWidget*>("destinationList");list->setCurrentRow(0);
  // Another writer changes the file just before the reorder is saved.
  QByteArray external;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method!="settings.save"||!external.isEmpty())return;auto doc=service.document();auto preferences=doc["preferences"].toObject();preferences["notifications"]=false;doc["preferences"]=preferences;external=QJsonDocument(doc).toJson();QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write(external);};
  window.findChild<QAction*>("moveDestinationDown")->trigger();
  QCOMPARE(rowLabels(list),QStringList({"Synthetic alternate","put.gd development destination","Synthetic third"}));
  QLabel* status=window.findChild<QLabel*>("settingsStatus");QVERIFY(status);
  PUTGD_TRY_VERIFY2(statusCode(window)=="config_conflict",qPrintable(status->text()));QVERIFY(status->text().contains("previous destination order is restored"));QVERIFY(!status->text().contains("config_conflict"));
  QCOMPARE(rowLabels(list),QStringList({"put.gd development destination","Synthetic alternate","Synthetic third"}));QCOMPARE(list->currentRow(),0);
  QCOMPARE(window.findChild<QLineEdit*>("destinationId")->text(),QString("development-s3"));
  QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),external);
  QVERIFY(window.settings()->autosaveText().contains("changed elsewhere"));
  // Further moves wait for a reload instead of saving over the other writer.
  QJsonObject result;QCOMPARE(window.settings()->moveSelectedDestination(1,{},result).code,QString("config_conflict"));
  acceptNextConfirmation();window.findChild<QPushButton*>("reloadSettings")->click();PUTGD_TRY_VERIFY(!window.settings()->autosaveText().contains("changed elsewhere"));
  QVERIFY(!window.settings()->moveSelectedDestination(1,"development-s3",result));
  PUTGD_TRY_COMPARE(profileIds(service.document()),QStringList({"synthetic-alternate","development-s3","synthetic-third"}));QVERIFY(!service.snapshot().notifications);
 }
 // A conflict offers its recovery where it is reported, on Settings and on
 // Destinations, and the confirmation names the edits a reload discards, not
 // what the other writer changed.
 void sidebarsFollowTheirLabelsAtLargeText(){
  auto font=QApplication::font();const auto original=font;auto restore=qScopeGuard([&]{QApplication::setFont(original);});
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  int baseRow=0;{MainWindow window(fixture.session);window.navigate("settings");window.resize(1060,680);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
   QCOMPARE(window.findChild<QWidget*>("settingsRail")->width(),Ui::railWidth());auto* nav=window.findChild<QListWidget*>("settingsNavigation");baseRow=nav->visualItemRect(nav->item(0)).height();}
  font.setPixelSize(21);QApplication::setFont(font);
  MainWindow window(fixture.session);window.navigate("settings");window.resize(1060,680);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* nav=window.findChild<QListWidget*>("settingsNavigation");QVERIFY(nav->visualItemRect(nav->item(0)).height()>baseRow);QVERIFY(Ui::railWidth()<280);
  for(const char* name:{"settingsRail","destinationRail"}){auto* rail=window.findChild<QWidget*>(name);if(rail->objectName()=="destinationRail")QVERIFY(rail->width()>=Ui::railWidth()&&rail->width()<=Ui::railWidth()+2*Ui::space(Ui::Space::xl));else QCOMPARE(rail->width(),Ui::railWidth());}
  QVERIFY(window.findChild<QWidget*>("destinationRail")->width()>=window.findChild<QWidget*>("settingsRail")->width());
  // Category labels fit inside the fixed rail.
  const QFontMetrics strong(Ui::font(Ui::Type::body,true));for(int i=0;i<nav->count();++i)QVERIFY(strong.horizontalAdvance(nav->item(i)->text())+74<=window.findChild<QWidget*>("settingsRail")->width());
  // The Add destination… button keeps its whole label.
  window.navigate("destinations");auto* add=window.findChild<QPushButton*>("addS3Destination");PUTGD_TRY_VERIFY(add->isVisible());QVERIFY2(add->width()>=add->sizeHint().width(),qPrintable(QString("%1 < %2").arg(add->width()).arg(add->sizeHint().width())));
 }
 // Shortcuts is named by the rail and its page heading only.
 void shortDropdownShowsEveryOptionWithoutScrolling_data(){
  QTest::addColumn<int>("count");QTest::addColumn<bool>("editable");
  for(const auto count:{1,2,3,8})for(const auto editable:{false,true})QTest::newRow(qPrintable(QString("%1-options-%2").arg(count).arg(editable?"editable":"fixed")))<<count<<editable;
 }
 void shortDropdownShowsEveryOptionWithoutScrolling(){
  QFETCH(int,count);QFETCH(bool,editable);Ui::apply(*qApp);
  QWidget root;auto* layout=new QVBoxLayout(&root);auto* combo=Ui::choiceBox(&root);
  combo->setEditable(editable);for(int i=0;i<count;++i)combo->addItem(QString("Option %1").arg(i+1));
  layout->addWidget(combo);layout->addStretch();Ui::styleWindow(root);root.resize(600,400);root.show();
  for(const int startingIndex:QList<int>{0,count-1}){
   combo->setCurrentIndex(startingIndex);combo->showPopup();auto* view=combo->view();PUTGD_TRY_VERIFY(view->isVisible());
   const auto viewport=view->viewport()->rect();
   QVERIFY2(viewport.contains(view->visualRect(combo->model()->index(0,0))),"First option is clipped");
   QVERIFY2(viewport.contains(view->visualRect(combo->model()->index(count-1,0))),"Last option is clipped");
   QCOMPARE(view->verticalScrollBar()->maximum(),0);
   const int target=startingIndex==0?count-1:0;
   QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->visualRect(combo->model()->index(target,0)).center());
   PUTGD_TRY_COMPARE(combo->currentIndex(),target);PUTGD_TRY_VERIFY(!view->isVisible());
   combo->showPopup();QTest::keyClick(view,Qt::Key_Home);QTest::keyClick(view,Qt::Key_Return);
   PUTGD_TRY_COMPARE(combo->currentIndex(),0);PUTGD_TRY_VERIFY(!view->isVisible());
  }
 }
 void radioCaptionsStayInsideTheirRows_data(){QTest::addColumn<int>("pixels");QTest::newRow("text100")<<14;QTest::newRow("text150")<<21;}
 void radioCaptionsStayInsideTheirRows(){
  QFETCH(int,pixels);Ui::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.findChild<QComboBox*>("storageProvider")->setCurrentIndex(window.findChild<QComboBox*>("storageProvider")->findData("proton-drive"));window.show();for(const int width:{900,1060,1280,1440}){window.resize(width,900);putgd::test::settle(50);for(const auto& name:{"protonCreateShareLink","protonKeepPrivate"}){auto* radio=window.findChild<QRadioButton*>(name);auto* row=radio->parentWidget();QVERIFY(row->property("inlineHelpRow").toBool());for(auto* caption:row->findChildren<QLabel*>()){QVERIFY2(row->rect().contains(QRect(caption->mapTo(row,QPoint()),caption->size())),qPrintable(QString("%1 at %2: caption %3 row %4").arg(name).arg(width).arg(caption->height()).arg(row->height())));QVERIFY(caption->height()>=caption->heightForWidth(caption->width()));}}}
 }
 void wheelOverSettingsTabsDoesNotChangePageButKeyboardStillWorks(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");auto* bar=tabs->tabBar();tabs->setCurrentIndex(1);const QPointF position=bar->tabRect(1).center();for(const auto delta:{120,-120}){QWheelEvent event(position,bar->mapToGlobal(position.toPoint()),QPoint{},QPoint{0,delta},Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(bar,&event);QCOMPARE(tabs->currentIndex(),1);}bar->setFocus();QTest::keyClick(bar,Qt::Key_Right);QCOMPARE(tabs->currentIndex(),2);
 }
 void displayUnitsPreserveStoredBytesPixelsAndMilliseconds(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto settings){return s3.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();QCOMPARE(window.findChild<QLineEdit*>("maxFileBytes")->text(),QString("Auto"));QCOMPARE(window.findChild<QLineEdit*>("imagePixels")->text(),QString("100"));QCOMPARE(qobject_cast<QDoubleSpinBox*>(window.findChild<QLineEdit*>("s3ConnectTimeoutMs")->parentWidget())->cleanText(),QString("5"));window.findChild<QLineEdit*>("maxFileBytes")->setText("2.000000000931322574615478515625");window.findChild<QLineEdit*>("imagePixels")->setText("100.000001");window.findChild<QLineEdit*>("s3ConnectTimeoutMs")->setText("5.123");PUTGD_TRY_COMPARE(service.document()["limits"].toObject()["maxFileBytes"].toInteger(),2147483649LL);QCOMPARE(service.document()["limits"].toObject()["imagePixels"].toInteger(),100000001LL);QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["connectTimeoutMs"].toInt(),5123);QCOMPARE(window.findChild<QLineEdit*>("maxFileBytes")->text(),QString("2.000000000931322574615478515625"));
 }
 void applicationMenuDoesNotRepeatTheWindow(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);
  auto* menu=window.findChild<QMenu*>("applicationMenuPopup");QVERIFY(menu);
  QStringList entries;for(auto* action:menu->actions())if(!action->isSeparator()){QVERIFY(action->icon().isNull());entries<<action->text().replace("&&","&");}
  QCOMPARE(entries,QStringList({"Capture actions","Check for updates","About put.gd","Quit put.gd"}));
  QStringList band;for(auto* button:window.findChildren<QToolButton*>("captureActionButton")){band<<button->accessibleName();if(button->property("captureAction")!="edit")QVERIFY2(!button->menu(),qPrintable(button->accessibleName()));}
  for(const auto& entry:entries)QVERIFY2(!band.contains(entry)&&!QStringList({"History","Destinations","Settings"}).contains(entry),qPrintable(entry));
  QVERIFY(!window.findChild<QMenu*>("captureRegionMenu"));
 }
 // #239: the clipboard action says upload, like the tray; ids and keys stay.
 void clipboardActionSaysUpload(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();
  QToolButton* clipboard=nullptr;for(auto* button:window.findChildren<QToolButton*>("captureActionButton"))if(button->property("captureAction")=="clipboard")clipboard=button;
  QVERIFY(clipboard);QCOMPARE(clipboard->accessibleName(),QString("Upload clipboard"));QVERIFY(clipboard->accessibleDescription().startsWith("Upload copied local files"));QVERIFY(clipboard->accessibleDescription().contains("uploaded as text"));
  auto* helpAction=window.findChild<QAction*>("captureActionsHelp");QVERIFY(helpAction);helpAction->trigger();auto* helpDialog=window.findChild<QMessageBox*>("settingHelpDialog");QVERIFY(helpDialog);QVERIFY(helpDialog->informativeText().contains("Upload clipboard: upload copied local files",Qt::CaseInsensitive));QVERIFY(!helpDialog->informativeText().contains("mport"));helpDialog->close();
  auto* fileTemplate=window.findChild<QLineEdit*>("fileTemplate");QCOMPARE(fileTemplate->accessibleName(),QString("Copied & uploaded files"));QVERIFY(fileTemplate->toolTip().startsWith("Names copied and uploaded files when Custom template is selected"));
  QCOMPARE(fileTemplate->property("configPath").toString(),QString("/naming/fileTemplate"));
  // The size limits' help sits on their Auto / No limit choice.
  auto help=[&](const QString& key){auto* choice=window.findChild<QWidget*>(key+"Choice");return (choice?choice:window.findChild<QWidget*>(key))->toolTip();};
  for(const auto* name:{"maxFiles","maxFileBytes","pendingArtifactBytes","clipboardTextBytes"}){const auto text=help(name);QVERIFY2(!text.isEmpty(),name);QVERIFY2(!text.contains("mport"),qPrintable(text));}
  QVERIFY(help("maxFileBytes").startsWith("The largest individual uploaded file"));QVERIFY(help("clipboardTextBytes").contains("rejected before uploading"));
  QVERIFY(window.findChild<QWidget*>("useDefaultClipboard"));QCOMPARE(window.findChild<QCheckBox*>("useDefaultClipboard")->accessibleName(),QString("Upload clipboard: use default output"));
 }
 void sharedSizeLabelsKeepBinaryArithmeticAndExactInput(){
  QCOMPARE(ByteSize::summary(2*1024*1024),QString("2 MB"));QCOMPARE(ByteSize::summary(3LL*1024*1024*1024),QString("3 GB"));QCOMPARE(ByteSize::fileSummary(35840),QString("35 KB"));QCOMPARE(ByteSize::fileSummary(123),QString("123 B"));QCOMPARE(ByteSize::localized(3LL*1024*1024*1024),QString("3.0 GB"));const qint64 exact=2147483649LL;QCOMPARE(ByteSize::fromDecimal(ByteSize::exact(exact,ByteSize::GiB),ByteSize::GiB).toInteger(),exact);
 }
 // Final read: the 1 B/s default read "0.000977 KB/s". Rates are whole KB/s, the
 // lowest is "Any progress", and storage keeps bytes per second.
 void detailedSettingHelpIsReachableFromKeyboard(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();auto* help=window.findChild<QToolButton*>("historyRetentionChoiceHelp");QVERIFY(help);QVERIFY(help->isVisible());help->setFocus();QTest::keyClick(help,Qt::Key_Space);auto* note=window.findChild<QLabel*>("historyRetentionChoiceHelpText");QVERIFY(note);QVERIFY(note->isVisible());QVERIFY(note->wordWrap());QVERIFY(note->text().contains("metadata"));QVERIFY(note->mapTo(&window,QPoint()).y()>window.findChild<QComboBox*>("historyRetentionChoice")->mapTo(&window,QPoint()).y());QTest::keyClick(help,Qt::Key_Space);QVERIFY(note->isHidden());QVERIFY(!window.findChild<QMessageBox*>("settingHelpDialog"));
 }
 void deepLinksRevealAndFocusTheirControl(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");auto* tabs=window.findChild<QTabWidget*>("settingsTabs");tabs->setCurrentIndex(1);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* scroll=qobject_cast<QScrollArea*>(tabs->widget(1));auto* bar=scroll->verticalScrollBar();PUTGD_TRY_VERIFY(bar->maximum()>0);bar->setValue(bar->maximum());
  // Change output… from History: Capture from its top, Destination focused.
  window.navigate("history");window.findChild<QAction*>("changeCaptureOutput")->trigger();
  auto* destination=window.findChild<QComboBox*>("profileClipboard");PUTGD_TRY_COMPARE(window.focusWidget(),static_cast<QWidget*>(destination));
  QCOMPARE(tabs->currentIndex(),1);QVERIFY(scroll->viewport()->rect().contains(QRect(destination->mapTo(scroll->viewport(),QPoint()),destination->size())));
  window.activateWindow();window.navigate("history");window.showLocalCopySettings();auto* keep=window.findChild<QCheckBox*>("keepUploadCopies");
  PUTGD_TRY_VERIFY(keep->hasFocus());QVERIFY(Ui::keyboardFocus(keep));QCOMPARE(tabs->currentIndex(),1);
  const auto shot=keep->grab().toImage();const auto accent=Theme::current().accent;int focusPixels=0;for(int y=0;y<shot.height();++y)for(int x=0;x<shot.width();++x){const auto c=shot.pixelColor(x,y);if(qAbs(c.red()-accent.red())<16&&qAbs(c.green()-accent.green())<16&&qAbs(c.blue()-accent.blue())<16)++focusPixels;}QVERIFY2(focusPixels>=4,"The revealed checkbox must have a visible keyboard-focus cue");
  QVERIFY(scroll->viewport()->rect().contains(QRect(keep->mapTo(scroll->viewport(),QPoint()),keep->size())));
  // Updates: without an update action or a usable Check now, the status itself takes focus.
  window.settings()->showUpdates();auto* status=window.findChild<QLabel*>("updateStatus");PUTGD_TRY_COMPARE(window.focusWidget(),static_cast<QWidget*>(status));QCOMPARE(tabs->currentIndex(),0);
 }
 void formStackingUsesMeasuredFieldsAndMargins(){
  QScrollArea scroll;auto* page=new QWidget;scroll.setWidget(page);scroll.setWidgetResizable(true);auto* outer=new QVBoxLayout(page);outer->setContentsMargins(20,0,20,0);QList<QFormLayout*> forms;
  for(int i=0;i<2;++i){auto* group=new QWidget;auto* layout=new QVBoxLayout(group);layout->setContentsMargins(8,0,8,0);auto* form=new QFormLayout;form->setContentsMargins(4,0,4,0);form->setHorizontalSpacing(12);auto* field=new QLineEdit;field->setMinimumWidth(i?220:80);form->addRow(new QLabel("Value"),field);layout->addLayout(form);outer->addWidget(group);forms.append(form);}
  Ui::alignForms(page);scroll.resize(520,300);scroll.show();PUTGD_TRY_COMPARE(forms[0]->rowWrapPolicy(),QFormLayout::DontWrapRows);QCOMPARE(forms[1]->rowWrapPolicy(),QFormLayout::DontWrapRows);
  scroll.resize(300,300);PUTGD_TRY_COMPARE(forms[0]->rowWrapPolicy(),QFormLayout::WrapAllRows);QCOMPARE(forms[1]->rowWrapPolicy(),QFormLayout::WrapAllRows);
  scroll.resize(520,300);PUTGD_TRY_COMPARE(forms[0]->rowWrapPolicy(),QFormLayout::DontWrapRows);QCOMPARE(forms[1]->rowWrapPolicy(),QFormLayout::DontWrapRows);
 }
 void formsUseTheirValueTypesAndTargets(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");auto* tabs=window.findChild<QTabWidget*>("settingsTabs");tabs->setCurrentIndex(3);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  // Editable filename templates use the stacked technical field role.
  for(const auto* key:{"fileTemplate","captureTemplate","imageTemplate","textTemplate"}){auto* field=window.findChild<QLineEdit*>(key);QCOMPARE(field->font().family(),Ui::monoFamily());QCOMPARE(field->font().pixelSize(),Ui::scaled(Ui::Type::technical));}
  // The ACL choice says what the upload requests; stored values are unchanged.
  auto* acl=window.findChild<QComboBox*>("s3AclMode");QCOMPARE(acl->itemText(acl->findData("omit")),QString("Use bucket policy"));QCOMPARE(acl->itemText(acl->findData("public-read")),QString("Request public read"));QCOMPARE(acl->accessibleName(),QString("Upload access request"));
  // Help keeps its 14 px glyph; radio help follows the title line height.
  int helps=0;for(auto* help:window.findChildren<QToolButton*>())if(help->property("settingHelpFor").isValid()){++helps;QCOMPARE(help->size(),QSize(Ui::scaled(24),Ui::scaled(window.findChild<QRadioButton*>(help->property("settingHelpFor").toString())?Ui::Type::bodyLine:24)));QCOMPARE(help->iconSize(),QSize(Ui::scaled(14),Ui::scaled(14)));QVERIFY(help->text().isEmpty());QVERIFY(!help->accessibleDescription().isEmpty());}QVERIFY(helps>=3);
  // Numeric inputs stay compact; choice controls share the family width. Only changed values show Reset.
  window.findChild<QToolButton*>("resourceLimitsToggle")->setChecked(true);putgd::test::settle(20);
  QSet<int> widths;for(const auto* key:{"concurrentUploads","maxFiles","imageBytes","pendingImageBytes","recentActivityLimit"}){auto* edit=window.findChild<QLineEdit*>(key);QWidget* box=qobject_cast<QComboBox*>(edit->parentWidget())?edit->parentWidget():edit;widths.insert(box->width());}
  QCOMPARE(widths.size(),1);QVERIFY(!window.findChild<QLabel*>("failedArtifactDaysUnit"));QVERIFY(window.findChild<QLabel*>("historyRetentionDaysUnit"));
  QPushButton* reset=nullptr;for(auto* button:window.findChildren<QPushButton*>())if(button->accessibleName()=="Reset Uploads at once to default")reset=button;QVERIFY(reset&&!reset->isVisible());
  for(auto* label:tabs->widget(3)->findChildren<QLabel*>())QVERIFY2(!(label->isVisible()&&label->text()=="Default"),"No plain Default labels beside values");
  window.findChild<QLineEdit*>("concurrentUploads")->setText("5");QVERIFY(reset->isVisible());
 }
 void spinValuesFitAtEveryTextScale_data(){QTest::addColumn<int>("pixels");QTest::newRow("100")<<14;QTest::newRow("125")<<18;QTest::newRow("150")<<21;}
 void spinValuesFitAtEveryTextScale(){
  QFETCH(int,pixels);Ui::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,900);window.navigate("destinations");window.show();QSet<QAbstractSpinBox*> checked;
  auto* provider=window.findChild<QComboBox*>("storageProvider");for(const auto& kind:{"s3","backblaze-b2","proton-drive"}){provider->setCurrentIndex(provider->findData(kind));window.findChild<QToolButton*>("advancedSettings")->setChecked(true);if(QString(kind)=="backblaze-b2"){auto* mode=window.findChild<QComboBox*>("b2LinkMode");mode->setCurrentIndex(mode->findData("authorized"));QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,mode->currentIndex()));}putgd::test::settle(80);
   for(auto* spin:window.findChildren<QAbstractSpinBox*>())if(spin->isVisibleTo(&window)){checked.insert(spin);if(auto* number=qobject_cast<QSpinBox*>(spin))number->setValue(number->maximum());if(auto* number=qobject_cast<QDoubleSpinBox*>(spin))number->setValue(number->maximum());putgd::test::settle(20);auto* edit=spin->findChild<QLineEdit*>();QVERIFY(edit);QStyleOptionSpinBox option;option.initFrom(spin);option.frame=spin->hasFrame();option.buttonSymbols=spin->buttonSymbols();const auto room=spin->style()->subControlRect(QStyle::CC_SpinBox,&option,QStyle::SC_SpinBoxEditField,spin);QVERIFY2(room.contains(edit->geometry()),qPrintable(spin->objectName()));QVERIFY2(edit->contentsRect().width()>=edit->fontMetrics().horizontalAdvance(edit->displayText())+2,qPrintable(spin->objectName()));QVERIFY(edit->minimumWidth()<spin->width());}
  }QSet<QAbstractSpinBox*> expected;for(auto* spin:window.settings()->findChildren<QAbstractSpinBox*>())expected.insert(spin);for(auto* spin:window.findChild<QWidget*>("destinationPreferenceColumn")->findChildren<QAbstractSpinBox*>())expected.insert(spin);QCOMPARE(checked,expected);auto* hint=window.findChild<QLabel*>("b2LinkDaysHint");auto* value=hint->parentWidget();QVERIFY(value->width()>=Ui::scaled(280));
 }
 void readOnlyValuesFollowTheirLabelWhenStacked(){
  Ui::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(21);qApp->setFont(font);
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();window.findChild<QToolButton*>("advancedSettings")->setChecked(true);
  auto* endpoint=window.findChild<QLabel*>("s3EndpointValue");QVERIFY(endpoint);window.resize(1800,800);putgd::test::settle(80);QWidget* field=endpoint;while(field&&!field->property("stacked").isValid())field=field->parentWidget();QVERIFY(field);
  QSet<bool> seen;for(const int width:{1800,1060,1800}){window.resize(width,800);putgd::test::settle(80);const bool stacked=field->property("stacked").toBool();seen.insert(stacked);QCOMPARE(endpoint->alignment()&Qt::AlignHorizontal_Mask,stacked?Qt::AlignLeft:Qt::AlignRight);}
  QCOMPARE(seen.size(),2);
 }
 void numericUnitsAppearOnlyBesideTheValueAndDeadlinesTrimZeros(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);for(auto* label:window.settings()->findChildren<QLabel*>())for(const auto& unit:{" (days)"," (GB)"," (MB)"})QVERIFY2(!label->text().endsWith(unit),qPrintable(label->text()));for(const auto& key:{"maxFileBytes","imageBytes","historyRetentionDays"}){auto* unit=window.findChild<QLabel*>(QString(key)+"Unit");QVERIFY(unit);QVERIFY(!unit->text().isEmpty());}auto* deadline=window.findChild<QDoubleSpinBox*>("protonRequestTimeout");deadline->setValue(120);QCOMPARE(deadline->text(),QString("120 seconds"));deadline->setValue(120.123);QCOMPARE(deadline->text(),QString("120.123 seconds"));QCOMPARE(deadline->decimals(),3);
 }
 void absoluteFolderGuidanceFollowsInlineValidation(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.showCaptureSettings();window.show();auto* root=window.findChild<QLineEdit*>("archiveRoot");auto* hint=window.findChild<QLabel*>("archiveRootHint");auto* error=window.findChild<QLabel*>("archiveRootError");QVERIFY(root&&hint&&error);QVERIFY(hint->isHidden());
  root->clear();QVERIFY(!hint->isHidden());root->setText("relative/path");QVERIFY(!hint->isHidden());PUTGD_TRY_VERIFY(!error->isHidden());QVERIFY(hint->isHidden());root->setText(dir.filePath("valid"));QVERIFY(hint->isHidden());PUTGD_TRY_VERIFY(error->isHidden());
 }
 void compactSettingsKeepSharedInsetsWithoutAConflict(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");auto* page=qobject_cast<QScrollArea*>(tabs->currentWidget());auto* heading=window.findChild<QLabel*>("settingsPageHeadingGeneral");PUTGD_TRY_VERIFY(heading->property("compactHeading").toBool());QCOMPARE(page->widget()->layout()->contentsMargins(),QMargins(Ui::space(Ui::Space::xxl),qMin(Ui::pageHeaderTop(),Ui::space(Ui::Space::l)),Ui::space(Ui::Space::xxl),Ui::space(Ui::Space::xl)));QCOMPARE(page->widget()->layout()->spacing(),Ui::space(Ui::Space::xxl));QVERIFY(window.findChild<QLabel*>("settingsPageDescriptionGeneral")->isHidden());for(auto* card:page->widget()->findChildren<QWidget*>())if(card->property("visualRole")=="card")QCOMPARE(card->layout()->contentsMargins(),QMargins(0,Ui::space(Ui::Space::xl),0,0));
 }
 void settingLabelsUseBuddiesAndOnlyUsefulHelpButtons(){
  QWidget host;QLineEdit field(&host);field.setObjectName("syntheticValue");auto* label=Ui::settingLabel("Value","A longer explanation",&field,&host);QVERIFY(label->findChild<QLabel*>());QCOMPARE(label->findChild<QLabel*>()->buddy(),&field);QCOMPARE(label->findChildren<QToolButton*>().size(),1);auto* help=label->findChild<QToolButton*>();QVERIFY(help->text().isEmpty());QCOMPARE(help->size(),QSize(Ui::scaled(24),Ui::scaled(24)));QCOMPARE(label->findChild<QLabel*>()->focusPolicy(),Qt::NoFocus);
  auto* plain=Ui::settingLabel("Value","Value",&field,&host);QVERIFY(plain->findChildren<QToolButton*>().isEmpty());auto* empty=Ui::settingLabel("Value",{},&field,&host);QVERIFY(empty->findChildren<QToolButton*>().isEmpty());
 }
 void settingsKeepHelpForDetailsBeyondLabelsAndHints(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();
  for(const auto& key:QStringList{"launchAtLogin","notifications","keepUploadCopies","archiveRoot","folderImages","folderVideos","folderText","folderOther","s3ObjectPrefix","b2LinkDays","protonFolder","storedCredentialHint","updateCommand","s3Service","s3Bucket","s3RegionChoices","credentialAccessKey","credentialSecretKey","credentialSessionToken","credentialMode"}){
   auto* control=window.findChild<QWidget*>(key);QVERIFY2(control,qPrintable(key));QVERIFY2(!window.findChild<QToolButton*>(key+"Help"),qPrintable(key));QLabel* caption=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->buddy()==control)caption=label;if(qobject_cast<QCheckBox*>(control)){QVERIFY(caption);QCOMPARE(caption->focusPolicy(),Qt::NoFocus);}else{QVERIFY2(caption,qPrintable(key));QCOMPARE(caption->focusPolicy(),Qt::NoFocus);}
  }
  for(const auto& key:QStringList{"archiveRoot","s3ObjectPrefix","b2LinkDays","protonFolder"}){
   auto* hint=window.findChild<QLabel*>(key+"Hint");auto* control=window.findChild<QWidget*>(key);QVERIFY2(hint&&!hint->text().isEmpty(),qPrintable(key));QVERIFY(control->accessibleDescription().endsWith(hint->text()));
  }
  QVERIFY(!window.findChild<QToolButton*>("appearanceThemeHelp"));
  for(const auto& key:QStringList{"unlockCredentialsAtStartup","uploadClipboardBehavior","namingMode","collisionPolicy","customFileNamePatternToggle","s3AclMode","protonExpiration"}){
   auto* help=window.findChild<QToolButton*>(key+"Help");QVERIFY2(help,qPrintable(key));QCOMPARE(help->focusPolicy(),Qt::StrongFocus);QVERIFY(!help->accessibleDescription().isEmpty());
  }
 }
 void advancedCompactValuesEndInTheSharedColumn(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.resize(1440,1100);window.show();auto* field=window.findChild<QLineEdit*>("failedArtifactDays");auto* count=window.findChild<QLineEdit*>("recentActivityLimit");auto* choice=window.findChild<QComboBox*>("historyRetentionChoice");QVERIFY(field&&count&&choice);auto* retry=window.findChild<QComboBox*>("failedArtifactChoice");QVERIFY(retry);PUTGD_TRY_COMPARE(retry->parentWidget()->width(),choice->parentWidget()->width());PUTGD_TRY_COMPARE(count->width(),Ui::scaled(120));QCOMPARE(count->alignment(),Qt::AlignRight);PUTGD_TRY_COMPARE(count->mapTo(&window,QPoint(count->width(),0)).x(),choice->mapTo(&window,QPoint(choice->width(),0)).x());QVERIFY(!window.findChild<QLabel*>("failedArtifactDaysUnit"));PUTGD_TRY_COMPARE(retry->mapTo(&window,QPoint(retry->width(),0)).x(),choice->mapTo(&window,QPoint(choice->width(),0)).x());
 }
 void checkboxCaptionsUseTheirAvailableWidthBeforeWrapping(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.resize(1440,960);window.show();auto* check=window.findChild<QCheckBox*>("unlockCredentialsAtStartup");QLabel* caption=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->buddy()==check)caption=label;auto* help=window.findChild<QToolButton*>("unlockCredentialsAtStartupHelp");QVERIFY(caption&&help);PUTGD_TRY_VERIFY(caption->width()>=caption->fontMetrics().horizontalAdvance(caption->text()));QCOMPARE(help->mapTo(caption->parentWidget(),QPoint()).x(),caption->width()+Ui::space(Ui::Space::xs));
  auto* holder=caption->parentWidget();holder->setFixedWidth(caption->fontMetrics().horizontalAdvance("Unlock saved credentials")+help->width()+Ui::space(Ui::Space::xs));PUTGD_VERIFY_STEADY(caption->height()>caption->fontMetrics().height(),30);QVERIFY(help->y()<caption->fontMetrics().height());QVERIFY(holder->rect().contains(help->geometry()));
 }
 void choiceFamiliesKeepTheStandardWidthWhenContentsChange(){
  QWidget host;QVBoxLayout column(&host);QList<QFormLayout*> forms;QList<QComboBox*> choices;
  for(int i=0;i<3;++i){auto* form=new QFormLayout;Ui::styleForm(form);column.addLayout(form);auto* combo=Ui::choiceBox(&host);combo->addItem("Short");form->addRow("Value",combo);forms<<form;choices<<combo;}
  Ui::alignChoices(&host,{forms[0],forms[1]});Ui::alignChoices(&host,{forms[2]});host.resize(1000,400);host.show();putgd::test::settle(30);for(auto* combo:choices)QCOMPARE(combo->width(),Ui::scaled(320));
  choices[0]->addItem(QString(120,'W'));PUTGD_TRY_COMPARE(choices[0]->width(),Ui::scaled(320));QCOMPARE(choices[1]->width(),choices[0]->width());QCOMPARE(choices[2]->width(),Ui::scaled(320));
 }
 void familyLabelGuttersAreCappedAndFieldsUseTheRemainder_data(){QTest::addColumn<int>("pixels");QTest::newRow("100")<<14;QTest::newRow("125")<<18;QTest::newRow("150")<<21;}
 void familyLabelGuttersAreCappedAndFieldsUseTheRemainder(){
  QFETCH(int,pixels);Fonts::load();Ui::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1600,1100);window.showCaptureSettings();window.show();
  int familyGutter=-1;const auto check=[&](QComboBox* combo){QVERIFY(combo);auto* value=combo->parentWidget();while(value&&!value->property("preferenceChoiceValue").toBool())value=value->parentWidget();QVERIFY(value);auto* row=value->parentWidget();auto* caption=qobject_cast<QWidget*>(row->property("preferenceLabel").value<QObject*>());QVERIFY(caption);if(row->property("stacked").toBool()){QVERIFY(value->width()>=Ui::scaled(280)||value->width()==row->width()-2*Ui::space(Ui::Space::l));return;}const int gutter=value->mapTo(row,QPoint()).x()-caption->parentWidget()->mapTo(row,QPoint()).x();QVERIFY(gutter<=220+Ui::scaled(24)+Ui::space(Ui::Space::xs)+Ui::space(Ui::Space::l));if(familyGutter<0)familyGutter=gutter;QCOMPARE(gutter,familyGutter);QCOMPARE(value->mapTo(row,QPoint(value->width(),0)).x(),row->width()-Ui::space(Ui::Space::l));QVERIFY(caption->width()<=220+Ui::scaled(24)+Ui::space(Ui::Space::xs));for(auto* hint:row->findChildren<QLabel*>(Qt::FindDirectChildrenOnly))if(hint->isVisible()&&hint->objectName().endsWith("Hint")){QCOMPARE(hint->width(),row->width()-2*Ui::space(Ui::Space::l));QVERIFY(hint->height()>=hint->heightForWidth(hint->width()));}};
  // The shared family sizer runs in a queued callback; all visible rows must
  // agree before the first row's gutter becomes the family's reference.
  const auto checkFamily=[&](QWidget* page){
   QList<QComboBox*> choices;
   for(auto* combo:page->findChildren<QComboBox*>())if(combo->isVisible()&&combo->property("preferenceChoice").toBool())choices.append(combo);
   QVERIFY(!choices.isEmpty());
   const auto ready=[&]{
    int gutter=familyGutter;
    for(auto* combo:choices){
     auto* value=combo->parentWidget();while(value&&!value->property("preferenceChoiceValue").toBool())value=value->parentWidget();if(!value)return false;
     auto* row=value->parentWidget();auto* caption=qobject_cast<QWidget*>(row->property("preferenceLabel").value<QObject*>());
     if(!caption||!value->property("preferenceChoiceSizer").isValid())return false;
     if(row->property("stacked").toBool()){
      if(value->width()<Ui::scaled(280)&&value->width()!=row->width()-2*Ui::space(Ui::Space::l))return false;
      continue;
     }
     const int current=value->mapTo(row,QPoint()).x()-caption->parentWidget()->mapTo(row,QPoint()).x();
     if(current>220+Ui::scaled(24)+Ui::space(Ui::Space::xs)+Ui::space(Ui::Space::l))return false;
     if(gutter<0)gutter=current;if(current!=gutter)return false;
     if(value->mapTo(row,QPoint(value->width(),0)).x()!=row->width()-Ui::space(Ui::Space::l))return false;
     if(caption->width()>220+Ui::scaled(24)+Ui::space(Ui::Space::xs))return false;
     for(auto* hint:row->findChildren<QLabel*>(Qt::FindDirectChildrenOnly))if(hint->isVisible()&&hint->objectName().endsWith("Hint"))
      if(hint->width()!=row->width()-2*Ui::space(Ui::Space::l)||hint->height()<hint->heightForWidth(hint->width()))return false;
    }
    return true;
   };
   PUTGD_TRY_VERIFY(ready());
   for(auto* combo:choices)check(combo);
  };
  auto* tabs=window.findChild<QTabWidget*>("settingsTabs");for(int page:{0,1,3}){tabs->setCurrentIndex(page);checkFamily(tabs->currentWidget());}
  familyGutter=-1;window.navigate("destinations");window.findChild<QToolButton*>("advancedSettings")->setChecked(true);checkFamily(window.findChild<QScrollArea*>("destinationScroll"));
 }
 void formChoicesShareWidthsAndTextFieldsKeepTheirColumn(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1600,1200);window.showCaptureSettings();window.show();
  auto* choices=window.findChild<QComboBox*>("defaultCaptureDestination");QVERIFY(choices);putgd::test::settle(30);PUTGD_TRY_VERIFY(choices->width()>=Ui::scaled(240));const int width=choices->width();
  for(const auto& name:{"defaultCaptureDestination","defaultCaptureSharing","namingMode","collisionPolicy"}){auto* combo=window.findChild<QComboBox*>(name);QVERIFY(combo);QCOMPARE(combo->width(),width);}
  auto* path=window.findChild<QLineEdit*>("archiveRoot");QVERIFY(path->width()>=Ui::scaled(280));
  choices->addItem(QString(120,'W'));putgd::test::settle(30);for(const auto& name:{"defaultCaptureDestination","defaultCaptureSharing","namingMode","collisionPolicy"})QCOMPARE(window.findChild<QComboBox*>(name)->width(),width);
  window.navigate("destinations");putgd::test::settle(30);auto* serviceChoice=window.findChild<QComboBox*>("s3Service");auto* region=window.findChild<QComboBox*>("s3RegionChoices");auto* acl=window.findChild<QComboBox*>("s3AclMode");QVERIFY(serviceChoice&&region&&acl);QCOMPARE(serviceChoice->width(),region->width());QCOMPARE(serviceChoice->width(),acl->width());QCOMPARE(window.findChild<QLineEdit*>("s3Bucket")->width(),region->width());QCOMPARE(window.findChild<QLineEdit*>("s3ObjectPrefix")->width(),region->width());
  window.resize(1060,680);putgd::test::settle(30);for(auto* combo:{serviceChoice,region,acl}){const auto bounds=QRect(combo->mapTo(&window,QPoint()),combo->size());QVERIFY(bounds.left()>=0&&bounds.right()<window.width());}QCOMPARE(window.findChild<QScrollArea*>("destinationScroll")->horizontalScrollBar()->maximum(),0);
 }
 void cardsAlignAllOrdinaryFieldLeftEdges(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  for(const auto size:{QSize(1440,1000),QSize(1060,680)}){window.resize(size);putgd::test::settle(60);for(auto* card:window.findChildren<QWidget*>())if(card->property("preferenceCard").toBool()&&card->isVisibleTo(&window)){int left=-1;for(auto* value:card->findChildren<QWidget*>())if(value->property("preferenceInlineValue").toBool()||value->property("preferenceChoiceValue").toBool()){if(!value->isVisibleTo(card))continue;const int x=value->mapTo(card,QPoint()).x();if(left<0)left=x;QCOMPARE(x,left);}}}
 }
 void pageActionsAndFieldsStayWithinOneColumn(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1600,1200);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");QVERIFY(tabs);
  for(int page=0;page<tabs->count();++page){tabs->setCurrentIndex(page);putgd::test::settle(30);auto* scroll=qobject_cast<QScrollArea*>(tabs->widget(page));QVERIFY(scroll);for(auto* card:scroll->findChildren<QFrame*>())if(card->property("visualRole")=="card")QVERIFY(card->width()<=Ui::scaled(760));auto* heading=window.findChild<QLabel*>("settingsPageHeading"+tabs->tabText(page));auto* subtitle=window.findChild<QLabel*>("settingsPageDescription"+tabs->tabText(page));QVERIFY(heading&&subtitle&&subtitle->isHidden());QVERIFY(!subtitle->text().isEmpty());QVERIFY(heading->parentWidget()->width()<=Ui::scaled(760));}
  auto* reset=window.findChild<QPushButton*>("resetAdvancedDefaults");auto* heading=window.findChild<QLabel*>("settingsPageHeadingAdvanced");QVERIFY(reset&&heading);QVERIFY(window.findChild<QWidget*>("resourceLimits")->isAncestorOf(reset));QVERIFY(reset->isHidden()||!reset->isVisible());
  window.showCaptureSettings();putgd::test::settle(30);auto* path=window.findChild<QLineEdit*>("archiveRoot");QVERIFY(path);auto* pathHint=window.findChild<QLabel*>("archiveRootHint");QVERIFY(pathHint);QVERIFY(pathHint->alignment()&Qt::AlignLeft);QCOMPARE(pathHint->mapTo(&window,QPoint()).x(),path->mapTo(&window,QPoint()).x());QVERIFY(path->width()<=Ui::scaled(760)-2*Ui::space(Ui::Space::l));QPushButton* browse=nullptr;for(auto* button:path->parentWidget()->findChildren<QPushButton*>())if(button->text()=="Browse…")browse=button;QVERIFY(browse);QCOMPARE(browse->mapTo(&window,QPoint()).x()-path->mapTo(&window,QPoint(path->width(),0)).x(),Ui::space(Ui::Space::s));
  window.navigate("destinations");putgd::test::settle(30);auto* header=window.findChild<QWidget*>("destinationHeader");auto* more=window.findChild<QPushButton*>("destinationActions");QVERIFY(header&&more);QVERIFY(header->width()<=Ui::scaled(760));QCOMPARE(more->mapTo(header,QPoint(more->width(),0)).x(),header->width());QVERIFY(!window.findChild<QToolButton*>("destinationLabelHelp"));auto* region=window.findChild<QComboBox*>("s3RegionChoices");QVERIFY(region&&region->lineEdit());QCOMPARE(region->font().family(),Ui::uiFamily());QCOMPARE(region->lineEdit()->font().family(),Ui::uiFamily());
 }
 void radioChoicesLeadTheirWrappedText_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<21;}
 void radioChoicesLeadTheirWrappedText(){
  Fonts::load();Ui::apply(*qApp);QFETCH(int,pixels);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();auto proton=profiles[1].toObject();proton["provider"]="proton-drive";proton["settings"]=ProtonProvider::defaults();profiles[1]=proton;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();window.findChild<QListWidget*>("destinationList")->setCurrentRow(1);
  for(const auto size:{QSize(1440,960),QSize(1060,680)}){window.resize(size);putgd::test::settle(30);const auto radios=window.findChildren<QRadioButton*>();QCOMPARE(radios.size(),2);for(auto* radio:radios){QVERIFY(radio->isVisible());auto* row=radio->parentWidget();auto* line=qobject_cast<QHBoxLayout*>(row->layout()->itemAt(0)->layout());QVERIFY(line);QCOMPARE(line->itemAt(0)->widget(),radio);QCOMPARE(line->spacing(),Ui::space(Ui::Space::m));QCOMPARE(radio->width(),Ui::scaled(24));QLabel* title=nullptr;for(auto* label:row->findChildren<QLabel*>())if(label->buddy()==radio)title=label;QVERIFY(title);QCOMPARE(title->text(),radio->accessibleName());QVERIFY(!window.findChild<QToolButton*>(radio->objectName()+"Help"));QVERIFY(!radio->accessibleDescription().isEmpty());const auto rect=[row](QWidget* widget){return QRect(widget->mapTo(row,QPoint()),widget->size());};QVERIFY(rect(radio).right()<rect(title).left());QCOMPARE(rect(radio).top(),rect(title).top());const int offset=qMax(0,(title->fontMetrics().height()-Ui::scaled(18))/2);QCOMPARE(radio->contentsMargins().top(),offset);QVERIFY(qAbs(rect(radio).top()+offset+Ui::scaled(18)/2-(rect(title).top()+title->fontMetrics().height()/2))<=1);for(auto* label:row->findChildren<QLabel*>())if(label!=title){QVERIFY(rect(label).left()>=rect(title).left());QVERIFY(label->wordWrap());QVERIFY(label->height()>=label->heightForWidth(label->width()));if(label==line->itemAt(1)->layout()->itemAt(1)->widget())QCOMPARE(rect(label).top()-rect(title).bottom()-1,Ui::space(Ui::Space::xs));}}}
  auto* privateMode=window.findChild<QRadioButton*>("protonKeepPrivate");QTest::mouseClick(privateMode,Qt::LeftButton);QVERIFY(privateMode->isChecked());QVERIFY(!window.findChild<QCheckBox*>("protonShareDefault")->isChecked());auto* share=window.findChild<QRadioButton*>("protonCreateShareLink");share->setFocus();QTest::keyClick(share,Qt::Key_Space);QVERIFY(window.findChild<QCheckBox*>("protonShareDefault")->isChecked());
 }
 void checkboxRowActivatesItsBackgroundAndCaptionButNotHelp(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.resize(1440,960);window.show();auto* check=window.findChild<QCheckBox*>("unlockCredentialsAtStartup");auto* help=window.findChild<QToolButton*>("unlockCredentialsAtStartupHelp");QVERIFY(check&&help);QLabel* caption=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->buddy()==check)caption=label;QVERIFY(caption);auto* row=check->parentWidget();while(row&&!row->property("preferenceRow").toBool())row=row->parentWidget();QVERIFY(row);QVERIFY(putgd::test::waitForWindowExposed(&window));QSignalSpy clicked(check,&QCheckBox::clicked);
  const QPoint blank(row->width()-100,row->height()/2);const bool initial=check->isChecked();QTest::mouseClick(row,Qt::LeftButton,{},blank);QCOMPARE(check->isChecked(),!initial);QTest::mouseClick(caption,Qt::LeftButton);QCOMPARE(check->isChecked(),initial);QCOMPARE(clicked.size(),2);
  QTest::mouseRelease(caption,Qt::LeftButton);QCOMPARE(clicked.size(),2);QTest::mousePress(row,Qt::LeftButton,{},blank);QTest::mouseRelease(row,Qt::LeftButton,{},QPoint(-5,-5));QCOMPARE(clicked.size(),2);QTest::mousePress(row,Qt::LeftButton,{},blank);QTest::mouseRelease(row,Qt::LeftButton,{},help->mapTo(row,help->rect().center()));QCOMPARE(clicked.size(),2);
  QTest::mouseClick(help,Qt::LeftButton);QVERIFY(help->isChecked());QCOMPARE(check->isChecked(),initial);QCOMPARE(clicked.size(),2);help->click();check->setEnabled(false);QTest::mouseClick(row,Qt::LeftButton,{},blank);QTest::mouseClick(caption,Qt::LeftButton);QCOMPARE(clicked.size(),2);check->setEnabled(true);QTest::mousePress(row,Qt::LeftButton,{},blank);check->setEnabled(false);QTest::mouseRelease(row,Qt::LeftButton,{},blank);QCOMPARE(clicked.size(),2);
 }
 void radioTextSelectsTheChoiceWhileHelpStaysIndependent(){
  QWidget host;auto* layout=new QVBoxLayout(&host);auto* radio=new QRadioButton("Keep private");radio->setObjectName("privateChoice");radio->setAutoExclusive(false);auto* row=PreferenceRows::radio(radio,"No share link is created. Existing folder access still applies.","Upload privately without creating a link.");layout->addWidget(row);host.resize(520,200);host.show();QVERIFY(putgd::test::waitForWindowExposed(&host));QLabel* title=nullptr;QLabel* caption=nullptr;for(auto* label:row->findChildren<QLabel*>())if(label->buddy()==radio)title=label;else caption=label;auto* help=row->findChild<QToolButton*>("privateChoiceHelp");QVERIFY(title&&caption&&help);QCOMPARE(radio->accessibleName(),QString("Keep private"));QSignalSpy clicked(radio,&QRadioButton::clicked);
  for(auto* label:{title,caption}){radio->setChecked(false);QTest::mouseClick(label,Qt::LeftButton,{},label->rect().center());QVERIFY(radio->isChecked());QVERIFY(radio->hasFocus());}QCOMPARE(clicked.size(),2);
  radio->setChecked(false);QTest::mouseClick(help,Qt::LeftButton);QVERIFY(help->isChecked());QVERIFY(!radio->isChecked());QCOMPARE(clicked.size(),2);auto* explanation=row->findChild<QLabel*>("privateChoiceHelpText");QVERIFY(explanation&&explanation->isVisible());help->setFocus();QTest::keyClick(help,Qt::Key_Space);QVERIFY(!help->isChecked());QVERIFY(!radio->isChecked());
  QTest::mouseClick(title,Qt::RightButton);QVERIFY(!radio->isChecked());radio->setEnabled(false);QTest::mouseClick(caption,Qt::LeftButton);QVERIFY(!radio->isChecked());radio->setEnabled(true);radio->setFocus();QTest::keyClick(radio,Qt::Key_Space);QVERIFY(radio->isChecked());QCOMPARE(clicked.size(),3);QCOMPARE(radio->accessibleName(),QString("Keep private"));
 }
 void largeTextSettingsKeepUsefulRowsInView(){
  Fonts::load();Ui::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(21);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");
  for(int page:{0,2}){tabs->setCurrentIndex(page);putgd::test::settle(50);auto* scroll=qobject_cast<QScrollArea*>(tabs->currentWidget());scroll->verticalScrollBar()->setValue(0);int visible=0;QStringList geometry;for(auto* row:scroll->widget()->findChildren<QWidget*>())if(row->property("preferenceRow").toBool()&&row->isVisible()){const QRect rect(row->mapTo(scroll->viewport(),QPoint()),row->size());geometry<<QString("%1 y%2 h%3").arg(row->objectName()).arg(rect.y()).arg(rect.height());if(scroll->viewport()->rect().contains(rect))++visible;}QVERIFY2(visible>=(page==0?4:3),qPrintable(QString("page %1 has %2 complete rows (%3); viewport %4").arg(page).arg(visible).arg(geometry.join(", ")).arg(scroll->viewport()->height())));QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);}
  // A fixed choice whose widest option fits keeps its card inline, even when free text would need scaled(280).
  {tabs->setCurrentIndex(0);putgd::test::settle(30);auto* theme=window.findChild<QComboBox*>("appearanceTheme");QLabel* caption=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->buddy()==theme)caption=label;QVERIFY(theme&&caption);const int captionCentre=caption->mapTo(&window,caption->rect().center()).y(),themeCentre=theme->mapTo(&window,theme->rect().center()).y();QVERIFY2(qAbs(captionCentre-themeCentre)<=theme->height()/2,qPrintable(QString("caption %1, choice %2").arg(captionCentre).arg(themeCentre)));QVERIFY(theme->mapTo(&window,QPoint()).x()>caption->mapTo(&window,QPoint(caption->width(),0)).x());}
  int right=-1;for(const auto& action:Settings::presentationOrder()){auto* input=window.findChild<QLineEdit*>("shortcut"+action.left(1).toUpper()+action.mid(1));auto* field=input->parentWidget()->parentWidget();auto* row=field->parentWidget();QVERIFY(!row->property("stacked").toBool());const int edge=field->mapTo(&window,QPoint(field->width(),0)).x();if(right<0)right=edge;QCOMPARE(edge,right);QVERIFY(input->width()>=input->fontMetrics().horizontalAdvance(input->text()));}
 }
 void preferenceCardsKeepControlsAndIndicatorsAligned_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<21;}
 void preferenceCardsKeepControlsAndIndicatorsAligned(){
  Fonts::load();Ui::apply(*qApp);QFETCH(int,pixels);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1440,1200);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");
  // Resize/show queues several rounds of height-for-width and choice layout.
  // Wait for each exact geometry contract, rather than assuming 30 ms drains them.
  for(const auto& size:{QSize(1440,960),QSize(1060,680)}){window.resize(size);int columnLeft=-1;for(int page=0;page<2;++page){tabs->setCurrentIndex(page);auto* scroll=qobject_cast<QScrollArea*>(tabs->widget(page));auto* column=window.findChild<QWidget*>(page?"capturePreferenceColumn":"generalPreferenceColumn");QVERIFY(column);PUTGD_TRY_VERIFY(column->width()<=Ui::scaled(760));PUTGD_TRY_COMPARE(column->mapTo(scroll->viewport(),QPoint()).x(),Ui::space(Ui::Space::xxl));PUTGD_TRY_VERIFY(scroll->viewport()->width()-column->mapTo(scroll->viewport(),QPoint()).x()-column->width()>=Ui::space(Ui::Space::xxl)-2);if(columnLeft<0)columnLeft=column->mapTo(&window,QPoint()).x();PUTGD_TRY_COMPARE(column->mapTo(&window,QPoint()).x(),columnLeft);QWidget* previousSection=nullptr;for(auto* section:column->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))if(section->property("preferenceSection").toBool()){auto* heading=section->layout()->itemAt(0)->widget();PUTGD_TRY_COMPARE(heading->height(),heading->sizeHint().height());for(auto* label:section->findChildren<QLabel*>(Qt::FindDirectChildrenOnly))if(label->wordWrap())PUTGD_TRY_VERIFY(label->height()>=label->heightForWidth(label->width()));PUTGD_TRY_VERIFY(column->rect().contains(section->geometry()));if(previousSection)PUTGD_TRY_VERIFY2(section->y()-previousSection->geometry().bottom()-1>=Ui::space(Ui::Space::xxl),qPrintable(QString("page %1 window %2x%3; section y/h/min/max %4/%5/%6/%7; previous bottom %8; column h/min/minimum/HFW %9/%10/%11/%12; spacing %13; previous min/HFW %14/%15; current min/HFW %16/%17").arg(page).arg(window.width()).arg(window.height()).arg(section->y()).arg(section->height()).arg(section->minimumHeight()).arg(section->maximumHeight()).arg(previousSection->geometry().bottom()+1).arg(column->height()).arg(column->minimumHeight()).arg(column->layout()->totalMinimumSize().height()).arg(column->layout()->totalHeightForWidth(column->width())).arg(column->layout()->spacing()).arg(previousSection->layout()->totalMinimumSize().height()).arg(previousSection->layout()->totalHeightForWidth(previousSection->width())).arg(section->layout()->totalMinimumSize().height()).arg(section->layout()->totalHeightForWidth(section->width()))));previousSection=section;}int cards=0;for(auto* card:column->findChildren<QWidget*>())if(card->property("preferenceCard").toBool()&&card->isVisibleTo(column)){++cards;PUTGD_TRY_VERIFY(card->parentWidget()->rect().contains(card->geometry()));auto* form=card->findChild<QFormLayout*>();QVERIFY(form);for(int row=0;row<form->rowCount();++row){auto* item=form->itemAt(row,QFormLayout::FieldRole);if(!item||!item->widget()||!form->isRowVisible(row))continue;auto* field=item->widget();PUTGD_TRY_VERIFY(card->rect().contains(QRect(field->mapTo(card,QPoint()),field->size())));if(auto* text=qobject_cast<QLabel*>(field))PUTGD_TRY_VERIFY(text->height()>=text->heightForWidth(text->width()));else if(field->property("preferenceRow").toBool())PUTGD_TRY_VERIFY2(field->height()>=Ui::scaled(Ui::Metric::control)+2*Ui::space(Ui::Space::s),qPrintable(QString("field %1 height %2, floor %3, class %4, minimum %5").arg(field->objectName()).arg(field->height()).arg(Ui::scaled(Ui::Metric::control)+2*Ui::space(Ui::Space::s)).arg(field->metaObject()->className()).arg(field->minimumHeight())));QVERIFY(!form->itemAt(row,QFormLayout::LabelRole));for(auto* value:field->findChildren<QWidget*>())if(value->objectName().endsWith("PreferenceValue"))if(value->property("preferenceChoiceValue").toBool()){PUTGD_TRY_VERIFY(value->width()>0);PUTGD_TRY_COMPARE(value->mapTo(card,QPoint(value->width(),0)).x(),card->width()-Ui::space(Ui::Space::l));}}}QVERIFY(cards>=3);PUTGD_TRY_COMPARE(scroll->horizontalScrollBar()->maximum(),0);}}
  tabs->setCurrentIndex(0);auto* check=window.findChild<QCheckBox*>("notifications");QVERIFY(!check->text().isEmpty());QCOMPARE(check->accessibleName(),check->text());QLabel* label=nullptr;for(auto* candidate:tabs->widget(0)->findChildren<QLabel*>())if(candidate->buddy()==check)label=candidate;QVERIFY(label);const bool before=check->isChecked();QTest::mouseClick(label,Qt::LeftButton);QCOMPARE(check->isChecked(),!before);
 }
 void sharedFormsUseOneMinimumControlWidth(){
  QWidget host;QVBoxLayout outer(&host);QList<QWidget*> pages;QList<QFormLayout*> forms;
  for(int width:{80,220}){auto* page=new QWidget;auto* box=new QVBoxLayout(page);box->setContentsMargins(28,0,28,0);auto* form=new QFormLayout;Ui::styleForm(form);auto* field=new QLineEdit;field->setMinimumWidth(width);form->addRow(new QLabel("Value"),field);box->addLayout(form);outer.addWidget(page);pages<<page;forms<<form;}
  for(auto* page:pages)Ui::alignForms(page,forms);host.resize(520,300);host.show();PUTGD_TRY_COMPARE(forms[0]->rowWrapPolicy(),QFormLayout::DontWrapRows);QCOMPARE(forms[1]->rowWrapPolicy(),forms[0]->rowWrapPolicy());host.resize(320,300);PUTGD_TRY_COMPARE(forms[0]->rowWrapPolicy(),QFormLayout::WrapAllRows);QCOMPARE(forms[1]->rowWrapPolicy(),forms[0]->rowWrapPolicy());
 }
 void compactPageHeaderHasNoStaleLayoutItemHeight(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();
  auto state=window.settings()->saveViewState();state["conflict"]=true;QVERIFY(window.settings()->restoreViewState(state));
  auto* heading=window.findChild<QLabel*>("settingsPageHeadingGeneral");auto* header=heading->parentWidget();
  for(int i=0;i<12;++i){window.resize(1440,1200);PUTGD_TRY_VERIFY(!heading->property("compactHeading").toBool());window.resize(1060,680);PUTGD_TRY_VERIFY(heading->property("compactHeading").toBool());PUTGD_TRY_COMPARE(header->height(),header->sizeHint().height());QCOMPARE(header->height(),header->layout()->sizeHint().height());}
  // Replay Qt assigning the reported stale +7 px through the real layout item.
  auto* layout=header->parentWidget()->layout();auto* item=layout->itemAt(layout->indexOf(header));
  auto allocation=item->geometry();allocation.setHeight(header->sizeHint().height()+7);item->setGeometry(allocation);
  QCOMPARE(header->height(),header->sizeHint().height());
  layout->invalidate();layout->activate();QCOMPARE(header->height(),header->layout()->sizeHint().height());
 }
 void mainBandsUseSharedInsetsAndOneTypeSizePerSide(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();
  auto* band=window.findChild<QWidget*>("commandBand");auto* status=window.findChild<QWidget*>("statusBand");QVERIFY(band&&status);
  const auto insets=QMargins(Ui::space(Ui::Space::m),Ui::space(Ui::Space::s),Ui::space(Ui::Space::m),Ui::space(Ui::Space::s));
  for(auto size:{QSize(1440,1000),QSize(1060,680)}){window.resize(size);PUTGD_COMPARE_STEADY(band->layout()->contentsMargins(),insets,30);QCOMPARE(status->height(),Ui::scaled(40));auto* header=window.findChild<QWidget*>("windowHeader");auto* navigation=window.findChild<QTabBar*>("mainNavigation");QVERIFY(header->height()>=navigation->height()+2*Ui::space(Ui::Space::xs));if(size.height()<Ui::scaled(720))QVERIFY(header->height()<Ui::scaled(Ui::Metric::header));}
  auto* chrome=window.findChild<QWidget*>("mainChrome");QVERIFY(chrome&&chrome->isAncestorOf(band)&&chrome->isAncestorOf(window.findChild<QWidget*>("windowHeader")));
  auto* output=window.findChild<QToolButton*>("captureOutputSummary");QCOMPARE(output->font().pixelSize(),Ui::scaled(13));QCOMPARE(output->font().weight(),QFont::Normal);
  auto* version=window.findChild<QLabel*>("appVersion");auto* versionLink=window.findChild<QPushButton*>("appVersionLink");QCOMPARE(version->font().pixelSize(),Ui::scaled(13));QCOMPARE(version->font().weight(),QFont::Normal);QCOMPARE(version->font().pixelSize(),versionLink->font().pixelSize());QCOMPARE(window.findChild<QPushButton*>("updateAvailable")->font().pixelSize(),version->font().pixelSize());
  fixture.server.setUiDiagnostics([&]{auto data=fixture.operations().diagnostics();data["update"]=QJsonObject{{"supported",true},{"result",QJsonObject{{"updateAvailable",true},{"latest","26.1028.9999"}}}};return data;});fixture.session.refreshSnapshot();auto* update=window.findChild<QPushButton*>("updateAvailable");PUTGD_TRY_VERIFY(update->isVisible());
  auto* settings=window.settings();auto state=settings->saveViewState();state["conflict"]=true;QVERIFY(settings->restoreViewState(state));auto* feedback=window.findChild<QLabel*>("autosaveStatus");PUTGD_TRY_VERIFY(feedback->isVisible());QCOMPARE(feedback->font().pixelSize(),Ui::scaled(13));QVERIFY(version->isHidden());QVERIFY(window.findChild<QWidget*>("appVersionGroup")->isHidden());QVERIFY(update->isVisible());QVERIFY(update->width()>=update->sizeHint().width());QVERIFY(window.findChild<QLabel*>("autosaveExplanation")->isHidden());
 }
 void railShowsTheResidentVersionAndOpensItsGitHubPage(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();PUTGD_TRY_VERIFY(fixture.session.ready());
  auto* rail=window.findChild<QWidget*>("statusBand");auto* label=window.findChild<QLabel*>("appVersion");auto* link=window.findChild<QPushButton*>("appVersionLink");QVERIFY(rail&&label&&link);QVERIFY(rail->isAncestorOf(label)&&rail->isAncestorOf(link));
  // The real build identity: the full compiled version, as diagnostics and About report it.
  const auto build=QJsonDocument::fromJson(buildManifestJson()).object();QCOMPARE(fixture.session.build(),build);const auto current=releaseLink(build);QWidget* shown=current.url.isEmpty()?static_cast<QWidget*>(label):link;
  PUTGD_TRY_VERIFY(shown->isVisible());QCOMPARE(shown->property("text").toString().remove(QChar(0x200B)),QString::fromLatin1(buildVersion()));QCOMPARE(shown->accessibleName(),"put.gd version "+QString::fromLatin1(buildVersion()));
  QVERIFY2(shown->mapTo(&window,QPoint(shown->width(),shown->height())).x()>window.width()-shown->width()-40,"The version sits at the right of the status band");
  openedUrls_.clear();host::setTestOpener([this](const QUrl& url){recordProtonUrl(url);});auto cleanup=qScopeGuard([]{host::setTestOpener({});});
  QJsonObject snapshot;fixture.server.dispatch({{"schemaVersion",1},{"requestId","version"},{"session",fixture.server.socketPath()},{"method","ui.snapshot"},{"params",QJsonObject{}}},[&](Error e,QJsonObject data){QVERIFY(!e);snapshot=data;});PUTGD_TRY_VERIFY(!snapshot.isEmpty());
  auto apply=[&](const QString& version,bool dirty){auto injected=build;injected["version"]=version;injected["dirty"]=dirty;injected["gitCommit"]="528fe444b0cb0123456789abcdef0123456789ab";auto data=snapshot;data["build"]=injected;fixture.session.applySnapshot(data);};
  apply("0.1.1-nightly.430",false);PUTGD_TRY_VERIFY(link->isVisible());QVERIFY(!label->isVisible());QCOMPARE(link->text(),QString("0.1.1-nightly.430"));QCOMPARE(link->accessibleName(),QString("put.gd version 0.1.1-nightly.430"));QVERIFY(link->focusPolicy()&Qt::TabFocus);
  link->setFocus();QTest::keyClick(link,Qt::Key_Space);PUTGD_TRY_COMPARE(openedUrls_.size(),1);QCOMPARE(openedUrls_.first(),QUrl("https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v0.1.1-nightly.430"));
  // A development build has no GitHub release, even when its commit is pushed: plain text.
  const QString clean="0.1.1-dev.430+g528fe444b0cb";apply(clean,false);PUTGD_TRY_VERIFY(label->isVisible());QVERIFY(!link->isVisible());QCOMPARE(label->text().remove(QChar(0x200B)),clean);QVERIFY2(label->height()>=label->fontMetrics().height(),"A clean development version shows on one line");
  // Dirty development builds have no public page: the full string stays readable, unlinked, wrapping after "+" if needed.
  const QString dirty="0.1.1-dev.430+g528fe444b0cb.dirty";apply(dirty,true);PUTGD_TRY_VERIFY(label->isVisible());QVERIFY(!link->isVisible());QCOMPARE(label->text().remove(QChar(0x200B)),dirty);QCOMPARE(label->accessibleName(),"put.gd version "+dirty);
  PUTGD_TRY_VERIFY2(window.rect().contains(QRect(label->mapTo(&window,QPoint()),label->size()))&&label->width()>=label->fontMetrics().horizontalAdvance(dirty),"The longest development version shows whole in the status band");QCOMPARE(openedUrls_.size(),1);
  window.resize(1060,680);apply(QString(180,'x')+"-dev.430",false);PUTGD_TRY_VERIFY(label->isHidden());auto* summary=window.findChild<QToolButton*>("captureOutputSummary");QVERIFY(summary);PUTGD_TRY_VERIFY(summary->width()>=summary->sizeHint().width());
 }
};
QTEST_MAIN(SettingsGui)
#include "test_settings_appearance.moc"
