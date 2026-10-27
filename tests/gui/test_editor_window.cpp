// Offscreen widget contract for the themed editor window. Not desktop proof.
#include "ui/EditorWindow.h"
#include "ui/FileDialog.h"
#include "ui/Theme.h"
#include "ui/BlurRadius.h"
#include "editor/EditorCommands.h"
#include "platform/HostOpen.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QImageReader>
#include <QDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QGridLayout>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QToolButton>
#include <QSpinBox>
#include <QSlider>
#include <QProgressBar>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QFontComboBox>
#include <QHash>
#include <QSignalSpy>
#include <QTest>
#include "fixtures/Waits.h"
using namespace putgd;using namespace putgd::editor;

static QByteArray writeCopy(EditorWindow& window,const QString& path,QString& reason){
 QByteArray png;bool finished=false;
 if(!window.writeCopy(path,[&](const QByteArray& bytes,const QString& error){png=bytes;reason=error;finished=true;})){reason="save not started";return {};}
 if(!putgd::test::waitFor([&]{return finished;},10000))reason="save timed out";
 return png;
}
class EditorWindowTest:public QObject {
 Q_OBJECT
 static QImage canvasImage(){QImage image(400,260,QImage::Format_RGB32);image.fill(QColor("#20242c"));return image;}
 static void add(EditDocument& doc,const QJsonObject& shape){QJsonArray created;QString code,message;QVERIFY2(Commands::apply(doc,{QJsonObject{{"op","add"},{"shape",shape}}},created,code,message),qPrintable(message));}
private slots:
 void zoomMenuAndAgentCommandsShareTheSameView(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* zoom=window.findChild<QToolButton*>("editorZoom");QVERIFY(zoom&&zoom->menu());QCOMPARE(zoom->accessibleName(),QString("Zoom"));
  const auto choices=zoom->menu()->actions();QCOMPARE(choices.size(),7);
  for(int i=1;i<4;++i){choices[i]->trigger();const auto percent=choices[i]->data().toInt();QCOMPARE(window.viewState()["zoomPercent"].toInt(),percent);QVERIFY(!window.viewState()["fitted"].toBool());QVERIFY(choices[i]->isChecked());}
  QString code,message;window.viewCommand({{"action","zoom"},{"percent",50}},code,message);QVERIFY(code.isEmpty());QVERIFY(choices[1]->isChecked());
  choices[0]->trigger();QVERIFY(window.viewState()["fitted"].toBool());QVERIFY(zoom->text().startsWith("Fit · "));QVERIFY(zoom->text().endsWith(QString::number(qRound(window.canvas().zoom()*100))+"%"));QVERIFY(choices[0]->isChecked());
  const auto fitted=window.canvas().zoom();choices[5]->trigger();QVERIFY(window.canvas().zoom()>fitted);QVERIFY(!window.viewState()["fitted"].toBool());choices[6]->trigger();QVERIFY(qAbs(window.canvas().zoom()-fitted)<0.0001);
  QVERIFY(!choices[0]->shortcut().isEmpty());QVERIFY(!choices[2]->shortcut().isEmpty());QVERIFY(!choices[5]->shortcut().isEmpty());QVERIFY(!choices[6]->shortcut().isEmpty());
  const auto buttons=window.findChild<QWidget*>("editorToolbar")->findChildren<QToolButton*>();for(auto* button:buttons){QVERIFY(!button->toolTip().startsWith("Fit to window"));QVERIFY(!button->toolTip().startsWith("Actual size"));}
 }
 void compactLargeTextKeepsCanvasShareAndStableFeedback(){
  // Install the style first, then simulate a desktop's enlarged application font.
  {EditDocument doc(canvasImage());EditorWindow window(doc);}
  const auto previous=QApplication::font();auto restore=qScopeGuard([previous]{QApplication::setFont(previous);});auto larger=previous;larger.setPixelSize(qRound(Graphite::font().pixelSize()*1.5));QApplication::setFont(larger);
  EditDocument doc(canvasImage());EditorWindow window(doc);window.setDestination("Private upload to Project archive");window.resize(900,620);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  QVERIFY(window.canvas().width()>=window.width()*0.6);QVERIFY(window.findChild<QWidget*>("editorSide")->width()<=Graphite::scaled(320));
  const auto canvas=window.canvas().geometry();
  for(const auto tool:{Tool::Blur,Tool::Crop,Tool::Text,Tool::Select}){window.selectTool(tool);QApplication::processEvents();QCOMPARE(window.canvas().geometry(),canvas);}
  window.showSaveFailed("/synthetic/denied.png",QString("Permission denied. ")+QString("Your edits are kept. ").repeated(12));QApplication::processEvents();QCOMPARE(window.canvas().geometry(),canvas);
  const auto footer=window.findChild<QWidget*>("editorFooter");auto* choose=window.findChild<QPushButton*>("chooseAnotherLocation");QVERIFY(choose->isVisible());QVERIFY(footer->rect().contains(QRect(choose->mapTo(footer,QPoint()),choose->size())));
  window.findChild<QToolButton*>("editorSaveDetails")->click();auto* error=window.findChild<Graphite::ElidedLabel*>("editorSaveError");QVERIFY(error->fullTextPopup());QVERIFY(error->fullTextPopup()->isVisible());QCOMPARE(window.canvas().geometry(),canvas);error->fullTextPopup()->close();
  window.dismissSavedCopy();QApplication::processEvents();QCOMPARE(window.canvas().geometry(),canvas);
 }
 void cropFieldsPrecedeThePrimaryAction(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1100,1000);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));window.selectTool(Tool::Crop);QApplication::processEvents();
  auto* fields=window.findChild<QWidget*>("editorGeometry");auto* apply=window.findChild<QPushButton*>("applyCrop");auto* cancel=window.findChild<QPushButton*>("cancelCrop");QVERIFY(fields&&apply&&cancel);QCOMPARE(apply->property("visualRole").toString(),QString("primary"));QVERIFY(fields->mapTo(&window,fields->rect().bottomLeft()).y()<apply->mapTo(&window,QPoint()).y());QCOMPARE(apply->mapTo(&window,QPoint()).y(),cancel->mapTo(&window,QPoint()).y());
 }
 void geometryAndBlurUseTheWholePropertiesColumn(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1100,1000);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  window.selectTool(Tool::Crop);QApplication::processEvents();
  auto* x=window.findChild<QSpinBox*>("geometryX");auto* y=window.findChild<QSpinBox*>("geometryY");auto* w=window.findChild<QSpinBox*>("geometryWidth");auto* h=window.findChild<QSpinBox*>("geometryHeight");
  QCOMPARE(x->y(),y->y());QCOMPARE(w->y(),h->y());QVERIFY(x->x()<y->x());QVERIFY(x->geometry().bottom()<w->y());
  for(auto* label:window.findChild<QWidget*>("editorGeometry")->findChildren<QLabel*>())if(label->buddy()){QVERIFY(label->geometry().bottom()<label->buddy()->y());QCOMPARE(label->x(),label->buddy()->x());}
  window.selectTool(Tool::Blur);QApplication::processEvents();auto* slider=window.findChild<QSlider*>("blurStrength");auto* value=window.findChild<QSpinBox*>("blurStrengthValue");
  auto* radius=static_cast<BlurRadius*>(slider->parentWidget());const auto spinPosition=value->mapTo(radius,QPoint());
  QCOMPARE(slider->geometry().center().y(),value->mapTo(radius,value->rect().center()).y());QVERIFY(slider->x()>spinPosition.x()+value->width());QVERIFY(slider->width()>0);QVERIFY(radius->label()->geometry().bottom()<spinPosition.y());QVERIFY(window.findChild<QPushButton*>("removeMark")->property("graphiteFlush").toBool());QVERIFY(window.findChild<QToolButton*>("editorKeyboardHelp")->property("graphiteFlush").toBool());
 }
 void footerFeedbackReplacesDestinationUntilDismissed(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.setDestination("Upload to Synthetic");window.resize(900,620);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* destination=window.findChild<QLabel*>("editorDestination");auto* feedback=window.findChild<QWidget*>("editorSaveFeedback");auto* footer=window.findChild<QWidget*>("editorFooter");
  const auto canvas=window.canvas().geometry();window.showSaved("/synthetic/copy.png");QApplication::processEvents();QCOMPARE(window.canvas().geometry(),canvas);QVERIFY(!destination->isVisible());QVERIFY(feedback->isVisible());QVERIFY(footer->isAncestorOf(feedback));
  window.findChild<QToolButton*>("dismissEditorSavedCopy")->click();QVERIFY(destination->isVisible());QVERIFY(!window.viewState()["saveFeedbackVisible"].toBool());QCOMPARE(window.viewState()["lastSave"].toObject()["status"].toString(),QString("saved"));
  window.showSaveFailed("/synthetic/copy.png",QString("Disk full. ")+QString("Free enough space, then try again. ").repeated(5));QApplication::processEvents();
  auto* error=window.findChild<QLabel*>("editorSaveError");QVERIFY(error->isVisible());QVERIFY(!destination->isVisible());QVERIFY(error->text().startsWith("Couldn't save: Disk full."));QVERIFY(!error->wordWrap());QCOMPARE(window.canvas().geometry(),canvas);
  QString code,message;window.viewCommand({{"action","dismissSavedCopy"}},code,message);QVERIFY2(code.isEmpty(),qPrintable(message));QVERIFY(destination->isVisible());QVERIFY(!feedback->isVisible());
 }
 void keyboardHelpIsBelowPropertiesAndAgentControllable(){
  editor::EditDocument doc(QImage(400,260,QImage::Format_RGB32));EditorWindow window(doc);window.show();
  QString code,message;auto state=window.viewCommand({{"action","keyboardHelp"}},code,message);
  QVERIFY2(code.isEmpty(),qPrintable(message));QVERIFY(state["keyboardHelp"].toObject()["open"].toBool());
  auto* help=window.findChild<QToolButton*>("editorKeyboardHelp");auto* text=window.findChild<QWidget*>("editorKeyboardHelpText");
  QVERIFY(help&&text&&text->isVisible());QVERIFY(text->property("text").toString().contains("Shift+arrows"));auto* grid=qobject_cast<QGridLayout*>(text->layout());QVERIFY(grid);QCOMPARE(grid->columnCount(),2);QCOMPARE(grid->verticalSpacing(),4);QCOMPARE(grid->rowCount(),10);
  for(int row=0;row<grid->rowCount();++row){auto* key=qobject_cast<QLabel*>(grid->itemAtPosition(row,0)->widget());auto* meaning=qobject_cast<QLabel*>(grid->itemAtPosition(row,1)->widget());QVERIFY(key&&meaning);QCOMPARE(key->font().family(),Graphite::monoFamily());QCOMPARE(meaning->font().family(),Graphite::uiFamily());}QCOMPARE(help->accessibleDescription(),QString("Expanded"));
  QApplication::processEvents();auto* scroll=window.findChild<QScrollArea*>("editorPropertiesScroll");
  if(text->height()<=scroll->viewport()->height())QVERIFY(scroll->viewport()->rect().contains(QRect(text->mapTo(scroll->viewport(),QPoint()),text->size())));
  else for(auto* label:text->findChildren<QLabel*>()){scroll->ensureWidgetVisible(label);QApplication::processEvents();QVERIFY(scroll->viewport()->rect().contains(QRect(label->mapTo(scroll->viewport(),QPoint()),label->size())));}
  scroll->ensureWidgetVisible(help);QApplication::processEvents();
  help->setFocus();QTest::keyClick(help,Qt::Key_Space);QVERIFY(!window.viewState()["keyboardHelp"].toObject()["open"].toBool());QCOMPARE(help->accessibleDescription(),QString("Collapsed"));
  window.allowClose();
 }

 void textEntryStaysInsideCanvasWithoutMovingTheAnnotation(){
  EditDocument doc(canvasImage());EditorCanvas canvas(doc);canvas.resize(440,300);canvas.show();QVERIFY(putgd::test::waitForWindowExposed(&canvas));canvas.setTool(Tool::Text);
  for(const QPoint anchor:{QPoint(390,80),QPoint(150,250),QPoint(390,250)}){
   canvas.fit();const QPoint origin((canvas.width()-400)/2,(canvas.height()-260)/2);
   QTest::mouseClick(&canvas,Qt::LeftButton,{},origin+anchor);
   auto* field=canvas.findChild<QLineEdit*>("annotationText");QVERIFY(field&&field->isVisible());
   const QString text(180,'W');field->setText(text);QVERIFY(canvas.rect().contains(field->geometry()));
   canvas.resize(320,220);QApplication::processEvents();QVERIFY(canvas.rect().contains(field->geometry()));
   field->setCursorPosition(text.size());QTest::keyClick(field,Qt::Key_End);QTest::keyClick(field,Qt::Key_Return);
   QCOMPARE(doc.shapes().last().rect.topLeft(),anchor);QCOMPARE(doc.shapes().last().text,text);doc.remove(doc.shapes().last().id);canvas.documentChanged();
   QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);canvas.resize(440,300);QApplication::processEvents();
  }
 }
 void blurValueAndHandleFollowTheActualSlider(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));window.selectTool(Tool::Blur);
  auto* slider=window.findChild<QSlider*>("blurStrength");auto* value=window.findChild<QSpinBox*>("blurStrengthValue");QVERIFY(slider&&value);
  QCOMPARE(value->accessibleDescription(),slider->accessibleDescription());
  for(const int strength:{slider->minimum(),18,slider->maximum()}){slider->setValue(strength);QCOMPARE(value->value(),strength);QCOMPARE(window.canvas().style().strength,strength);}
  value->setValue(22);QCOMPARE(slider->value(),22);QCOMPARE(window.canvas().style().strength,22);
  add(doc,{{"kind","blur"},{"rect",QJsonArray{10,10,50,20}},{"strength",24}});window.documentEdited();window.canvas().select(doc.shapes().last().id);QCOMPARE(value->value(),24);
  class Slider final:public QSlider {public:using QSlider::QSlider;QStyleOptionSlider option()const{QStyleOptionSlider value;initStyleOption(&value);return value;}};
  for(const auto orientation:{Qt::Horizontal,Qt::Vertical})for(const bool inverted:{false,true})for(const auto direction:{Qt::LeftToRight,Qt::RightToLeft}){
   Slider control(orientation);control.setLayoutDirection(direction);control.setRange(0,100);control.setValue(50);control.setInvertedAppearance(inverted);control.resize(orientation==Qt::Horizontal?QSize(260,32):QSize(32,260));control.show();QVERIFY(putgd::test::waitForWindowExposed(&control));
   const auto option=control.option();
   const auto handle=control.style()->subControlRect(QStyle::CC_Slider,&option,QStyle::SC_SliderHandle,&control);
   QVERIFY(handle.width()>=32&&handle.height()>=32);QVERIFY2(control.rect().contains(handle),qPrintable(QString("handle %1,%2 %3x%4 in %5x%6").arg(handle.x()).arg(handle.y()).arg(handle.width()).arg(handle.height()).arg(control.width()).arg(control.height())));
   QVERIFY((orientation==Qt::Horizontal?control.sizeHint().height():control.sizeHint().width())>=32);
   const QPoint press=handle.center()+(orientation==Qt::Horizontal?QPoint(0,14):QPoint(14,0));
   QCOMPARE(control.style()->hitTestComplexControl(QStyle::CC_Slider,&option,press,&control),QStyle::SC_SliderHandle);
   QTest::mousePress(&control,Qt::LeftButton,{},press);QCOMPARE(control.value(),50);QVERIFY(control.isSliderDown());
   const QPoint end=press+(orientation==Qt::Horizontal?QPoint(28,0):QPoint(0,28));
   QMouseEvent move(QEvent::MouseMove,QPointF(end),control.mapToGlobal(QPointF(end)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&control,&move);
   QVERIFY(control.value()!=50);
   const QPoint edge=orientation==Qt::Horizontal?QPoint(control.width()-1,press.y()):QPoint(press.x(),control.height()-1);
   QMouseEvent toEdge(QEvent::MouseMove,QPointF(edge),control.mapToGlobal(QPointF(edge)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&control,&toEdge);
   QCOMPARE(control.value(),option.upsideDown?control.minimum():control.maximum());QTest::mouseRelease(&control,Qt::LeftButton,{},edge);
   for(const int position:{control.minimum(),control.maximum()}){control.setValue(position);const auto atEnd=control.option();QVERIFY(control.rect().contains(control.style()->subControlRect(QStyle::CC_Slider,&atEnd,QStyle::SC_SliderHandle,&control)));}
  }
 }
 void indeterminateProgressPaintsAndAnimatesInBothSchemes(){
  const auto restore=qScopeGuard([previous=Theme::scheme()]{Theme::setScheme(previous);});
  for(const auto scheme:{Theme::Scheme::Dark,Theme::Scheme::Light}){
   Theme::setScheme(scheme);QProgressBar bar;bar.setTextVisible(false);bar.resize(240,20);bar.setRange(0,100);bar.setValue(0);bar.show();QVERIFY(putgd::test::waitForWindowExposed(&bar));
   const auto zero=bar.grab().toImage();bar.setRange(0,0);const auto busy=bar.grab().toImage();QVERIFY(busy!=zero);
   int fill=0;for(int y=0;y<busy.height();++y)for(int x=0;x<busy.width();++x)if(busy.pixelColor(x,y)==Theme::current().text)++fill;QVERIFY(fill>100);
   auto* timer=bar.findChild<QTimer*>("graphiteBusyTimer");QVERIFY(timer);QVERIFY(timer->isActive());
   PUTGD_TRY_VERIFY(bar.property("graphiteBusyPhase").toDouble()>0.08);QVERIFY(bar.grab().toImage()!=busy);
   bar.setRange(0,100);bar.setValue(0);PUTGD_TRY_VERIFY(!timer->isActive());QCOMPARE(bar.grab().toImage(),zero);
  }
 }
 void disabledStyleAnimationKeepsAVisibleBusySegment(){
  class QuietStyle final:public QProxyStyle {
  public:
   QuietStyle():QProxyStyle(new Graphite::Style){}
   int styleHint(StyleHint hint,const QStyleOption* option=nullptr,const QWidget* widget=nullptr,QStyleHintReturn* data=nullptr)const override{return hint==SH_Widget_Animate?0:QProxyStyle::styleHint(hint,option,widget,data);}
  } style;
  QProgressBar bar;bar.setStyle(&style);bar.setTextVisible(false);bar.resize(240,20);bar.setRange(0,0);bar.show();QVERIFY(putgd::test::waitForWindowExposed(&bar));
  const auto still=bar.grab().toImage();auto* timer=bar.findChild<QTimer*>("graphiteBusyTimer");QVERIFY(timer&&!timer->isActive());
  QCOMPARE(still.pixelColor(still.width()/2,still.height()/2),Theme::current().text);QCOMPARE(bar.grab().toImage(),still);
 }
 void liveSchemeAlsoUpdatesTheOpenFallbackChooser(){
  const auto restore=qScopeGuard([previous=Theme::scheme()]{Theme::setScheme(previous);qunsetenv("PUTGD_FILE_DIALOG");});qputenv("PUTGD_FILE_DIALOG","fallback");
  Theme::setScheme(Theme::Scheme::Dark);EditDocument doc(canvasImage());EditorWindow window(doc);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));QVERIFY(window.saveCopy());
  auto* chooser=window.fileDialog().fallbackDialog();QVERIFY(chooser&&chooser->isVisible());
  for(const auto scheme:{Theme::Scheme::Light,Theme::Scheme::Dark}){
   Theme::setScheme(scheme);QApplication::processEvents();QCOMPARE(window.viewState()["scheme"].toString(),Theme::name(scheme));QCOMPARE(chooser->property("themeScheme").toString(),Theme::name(scheme));
   QCOMPARE(chooser->palette().color(QPalette::Window),Theme::current().bg);QCOMPARE(window.palette().color(QPalette::Window),Theme::current().bg);
  }
  window.fileDialog().cancel();
 }
 void keyboardContractNeverUploadsOnPlainEnter(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  QSignalSpy uploads(&window,&EditorWindow::uploadRequested);
  // No two shortcuts share a key; ambiguous shortcuts would silently never fire.
  QList<QKeySequence> keys;for(auto* shortcut:window.findChildren<QShortcut*>()){QVERIFY2(!keys.contains(shortcut->key()),qPrintable(shortcut->key().toString()));keys.append(shortcut->key());}
  QVERIFY(keys.contains(QKeySequence("Ctrl+Return")));QVERIFY(!keys.contains(QKeySequence(Qt::Key_Return)));
  window.activateWindow();window.canvas().setFocus();
  QTest::keyClick(&window.canvas(),Qt::Key_C);QCOMPARE(window.canvas().tool(),Tool::Crop);
  QTest::keyClick(&window.canvas(),Qt::Key_Return);QCOMPARE(uploads.size(),0);QCOMPARE(window.canvas().tool(),Tool::Select);
  QTest::keyClick(&window.canvas(),Qt::Key_T);QCOMPARE(window.canvas().tool(),Tool::Text);
  QTest::mouseClick(&window.canvas(),Qt::LeftButton,{},window.canvas().rect().center());
  auto* text=window.canvas().findChild<QLineEdit*>("annotationText");QVERIFY(text);QVERIFY(text->isVisible());
  // Letters typed into a text annotation are text, not tool shortcuts.
  QTest::keyClicks(text,"Ready");QCOMPARE(window.canvas().tool(),Tool::Text);
  QTest::keyClick(text,Qt::Key_Return);QCOMPARE(uploads.size(),0);QCOMPARE(doc.shapes().size(),1);QCOMPARE(doc.shapes().first().text,QString("Ready"));
  QTest::keyClick(&window,Qt::Key_Z,Qt::ControlModifier);QVERIFY(doc.shapes().isEmpty());
  QTest::keyClick(&window,Qt::Key_Z,Qt::ControlModifier|Qt::ShiftModifier);QCOMPARE(doc.shapes().size(),1);
  QTest::keyClick(&window,Qt::Key_Z,Qt::ControlModifier);QTest::keyClick(&window,Qt::Key_Y,Qt::ControlModifier);QCOMPARE(doc.shapes().size(),1);
  QTest::keyClick(&window,Qt::Key_Return,Qt::ControlModifier);QCOMPARE(uploads.size(),1);
 }
 void switchingToolsNeverRestylesTheSelection(){
  // A selected red rectangle turned yellow when the Highlight tool was chosen.
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto& canvas=window.canvas();window.selectTool(Tool::Rectangle);
  const QPoint start=canvas.rect().center()-QPoint(60,30),end=canvas.rect().center()+QPoint(60,30);
  QTest::mousePress(&canvas,Qt::LeftButton,{},start);
  QMouseEvent move(QEvent::MouseMove,QPointF(end),canvas.mapToGlobal(QPointF(end)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&move);
  QTest::mouseRelease(&canvas,Qt::LeftButton,{},end);
  QCOMPARE(doc.shapes().size(),1);const auto id=doc.shapes().first().id;QCOMPARE(canvas.selected(),id);
  // A style control edits the selection.
  QToolButton* green=nullptr;for(auto* button:window.findChildren<QToolButton*>())if(button->accessibleName()=="Color Green")green=button;
  QVERIFY(green);green->click();QCOMPARE(doc.find(id)->color,QColor("#22c55e"));
  const auto shape=*doc.find(id);
  for(auto tool:{Tool::Highlight,Tool::Arrow,Tool::Text,Tool::Blur,Tool::Redact,Tool::Rectangle,Tool::Select}){window.selectTool(tool);QCOMPARE(*doc.find(id),shape);}
  QCOMPARE(canvas.selected(),quint64(0));
 }
 void builtInSwatchesHaveColorNamesAndCustomSwatchesKeepHexNames(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.setRecentColors({"#123456"});
  QStringList names;
  for(auto* swatch:window.findChildren<QToolButton*>()){
   if(!swatch->property("swatchColor").isValid())continue;
   const auto color=swatch->property("swatchColor").value<QColor>();
   if(color==QColor("#123456")){QCOMPARE(swatch->accessibleName(),QString("Color #123456"));continue;}
   names.append(swatch->accessibleName());QCOMPARE(swatch->accessibleDescription(),color.name());QVERIFY(swatch->toolTip().contains(color.name()));
  }
  names.sort();QStringList expected{"Color Red","Color Orange","Color Yellow","Color Green","Color Blue","Color White","Color Black"};expected.sort();QCOMPARE(names,expected);
 }
 void recentColorsAndTextControlsEditTheSelection(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  // Malformed, duplicate and excess entries are dropped; at most six remain.
  window.setRecentColors({"#123456","#123456","NOTACOLOR","#A1B2C3","#000001","#000002","#000003","#000004","#000005","#000006"});
  QCOMPARE(window.recentColors(),QStringList({"#123456","#000001","#000002","#000003","#000004","#000005"}));
  add(doc,{{"kind","text"},{"at",QJsonArray{20,20}},{"text","Label"}});window.documentEdited();
  auto& canvas=window.canvas();window.selectTool(Tool::Select);
  const auto id=doc.shapes().first().id;const auto at=doc.shapes().first().rect.center();
  // Select the label by clicking it (the 400x260 image is centered at 100%).
  const QPoint origin((canvas.width()-400)/2,(canvas.height()-260)/2);QTest::mouseClick(&canvas,Qt::LeftButton,{},origin+at);QCOMPARE(canvas.selected(),id);
  QToolButton* recent=nullptr;for(auto* button:window.findChildren<QToolButton*>())if(button->accessibleName()=="Color #123456")recent=button;
  QVERIFY(recent);recent->click();QCOMPARE(doc.find(id)->color,QColor("#123456"));QVERIFY(recent->isChecked());
  auto* size=window.findChild<QSpinBox*>("textSize");QVERIFY(size);size->setValue(64);QCOMPARE(doc.find(id)->fontPixels,64);
  auto* font=window.findChild<QFontComboBox*>("textFont");QVERIFY(font);QVERIFY(font->count()>0);
  font->setCurrentIndex(font->count()-1);QCOMPARE(doc.find(id)->fontFamily,font->currentFont().family());
  QVERIFY(window.findChild<QToolButton*>("customColor"));
 }
 void textSizeControlFollowsACornerDrag(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  add(doc,{{"kind","text"},{"at",QJsonArray{40,40}},{"text","Label"},{"fontPixels",30}});window.documentEdited();
  auto& canvas=window.canvas();window.selectTool(Tool::Select);const auto id=doc.shapes().first().id;const auto box=doc.shapes().first().rect;
  const QPoint origin((canvas.width()-400)/2,(canvas.height()-260)/2);
  QTest::mouseClick(&canvas,Qt::LeftButton,{},origin+box.center());QCOMPARE(canvas.selected(),id);
  auto* size=window.findChild<QSpinBox*>("textSize");QVERIFY(size);QCOMPARE(size->value(),30);
  const QPoint corner(box.x()+box.width(),box.y()+box.height()),target(box.x()+box.width()*3/2,box.y()+box.height()*3/2);
  QTest::mousePress(&canvas,Qt::LeftButton,{},origin+corner);
  QMouseEvent move(QEvent::MouseMove,QPointF(origin+target),canvas.mapToGlobal(QPointF(origin+target)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&move);
  // The control shows the new size while the drag is still in progress.
  QCOMPARE(size->value(),doc.find(id)->fontPixels);QVERIFY(size->value()>40);
  QTest::mouseRelease(&canvas,Qt::LeftButton,{},origin+target);QCOMPARE(size->value(),doc.find(id)->fontPixels);
 }
 void initialSizeFitsSmallScreens(){
  // Fedora's assertion-enabled libstdc++ trapped an inverted clamp on screens under 1000 px.
  QCOMPARE(EditorWindow::initialSize({640,320},{800,600}),QSize(720,540));
  QCOMPARE(EditorWindow::initialSize({6000,4000},{1920,1080}),QSize(1728,972));
  QCOMPARE(EditorWindow::initialSize({100,80},{2560,1440}),QSize(900,620));
  QCOMPARE(EditorWindow::initialSize({100,80},{200,100}),QSize(320,240));
 }
 void canvasGeometryIsStableAcrossToolsAndSelection(){
  // Cinnamon acceptance found the options row collapsing mid-drag, refitting the image under the pointer.
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  const auto geometry=window.canvas().geometry();
  for(auto tool:{Tool::Select,Tool::Redact,Tool::Blur,Tool::Crop,Tool::Arrow,Tool::Rectangle,Tool::Highlight,Tool::Text,Tool::Redact}){window.selectTool(tool);QApplication::processEvents();QCOMPARE(window.canvas().geometry(),geometry);}
  auto& canvas=window.canvas();const QPoint start=canvas.rect().center()-QPoint(60,30),end=canvas.rect().center()+QPoint(60,30);
  QTest::mousePress(&canvas,Qt::LeftButton,{},start);QApplication::processEvents();QCOMPARE(canvas.geometry(),geometry);
  QMouseEvent move(QEvent::MouseMove,QPointF(end),canvas.mapToGlobal(QPointF(end)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&move);
  QTest::mouseRelease(&canvas,Qt::LeftButton,{},end);QApplication::processEvents();QCOMPARE(canvas.geometry(),geometry);
  QCOMPARE(doc.shapes().size(),1);const auto rect=doc.shapes().first().rect;
  // 400x260 image fitted at 100% and centered: the drag maps to a 120x60 box around the image center.
  QVERIFY2(qAbs(rect.left()-140)<=1&&qAbs(rect.top()-100)<=1&&qAbs(rect.right()+1-260)<=1&&qAbs(rect.bottom()+1-160)<=1,qPrintable(QString("%1,%2 %3x%4").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height())));
 }
 void agentEditsRefreshTheWindowAndBusyStateBlocksActions(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  add(doc,{{"kind","redact"},{"rect",QJsonArray{10,10,50,20}}});window.documentEdited();
  QVERIFY(doc.canUndo());QSignalSpy uploads(&window,&EditorWindow::uploadRequested),cancels(&window,&EditorWindow::cancelRequested);
  window.setBusy("Preparing the edited image…");
  QPushButton* upload=nullptr;for(auto* button:window.findChildren<QPushButton*>())if(button->text()=="Upload")upload=button;
  QVERIFY(upload);QVERIFY(!upload->isEnabled());QTest::keyClick(&window,Qt::Key_Return,Qt::ControlModifier);QTest::keyClick(&window,Qt::Key_Z,Qt::ControlModifier);
  QCOMPARE(uploads.size(),0);QCOMPARE(doc.shapes().size(),1);window.requestCancel();QCOMPARE(cancels.size(),0);
  window.allowClose();QVERIFY(window.close());
 }
 // Finalizing disables each control that acts, not whole containers: the
 // header, the Document details and the progress message keep full contrast.
 void busyStateDisablesActionsButKeepsStatusReadable(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.setDestination("Upload to synthetic destination");window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  window.setBusy("Preparing the edited image…");
  auto* header=window.findChild<QWidget*>("windowHeader");QVERIFY(header);
  for(auto* label:window.findChildren<QLabel*>())QVERIFY2(label->isEnabled(),qPrintable(label->objectName()+": "+label->text()));
  for(auto* button:window.findChildren<QAbstractButton*>())QVERIFY2(button->isEnabled()==header->isAncestorOf(button),qPrintable(button->objectName()+" "+button->text()+" "+button->accessibleName()));
  for(auto* field:window.findChildren<QAbstractSpinBox*>())QVERIFY(!field->isEnabled());
  for(auto* field:window.findChildren<QComboBox*>())QVERIFY(!field->isEnabled());
  QVERIFY(!window.canvas().isEnabled());
  QLabel* message=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->text()=="Preparing the edited image…")message=label;
  QVERIFY(message);QVERIFY(message->isVisible());
  window.allowClose();
 }
 // The toolbar shows only the zoom; the size and edited state live in the
 // Document details, which stay outside the scrolling properties.
 void toolbarShowsZoomAndDocumentKeepsTheSize(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,1100);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* zoom=window.findChild<QToolButton*>("editorZoom");QVERIFY(zoom);
  QVERIFY(zoom->text().startsWith("Fit · ")||QRegularExpression("^\\d+%$").match(zoom->text()).hasMatch());QCOMPARE(zoom->accessibleName(),QString("Zoom"));
  auto* info=window.findChild<QLabel*>("editorDocumentInfo");QVERIFY(info);QVERIFY(info->text().contains("400 × 260 px"));
  for(auto* label:window.findChildren<QLabel*>())if(label!=info)QVERIFY2(!label->text().contains("400 × 260"),qPrintable(label->objectName()));
  QVERIFY(!window.findChild<QScrollArea*>("editorPropertiesScroll")->isAncestorOf(info));
  // Every colour target is at least 32 px; the painted swatch stays inside it.
  window.selectTool(Tool::Arrow);int swatches=0;
  for(auto* button:window.findChildren<QToolButton*>())if(button->accessibleName().startsWith("Color ")||button->objectName()=="customColor"){++swatches;QVERIFY(button->width()>=Graphite::scaled(32)&&button->height()>=Graphite::scaled(32));}
  QCOMPARE(swatches,8);
  for(auto* button:window.findChildren<QToolButton*>())if(button->accessibleName()=="Color Orange"){QTest::mouseClick(button,Qt::LeftButton,{},QPoint(1,1));QCOMPARE(window.canvas().style().color,QColor(245,158,11));}
 }
 // An unchosen swatch's painted edge holds 3:1 against the panel in both
 // schemes, white on the light panel and near-black on the dark one included.
 void swatchEdgesStandOutFromThePanel(){
  const auto restore=qScopeGuard([previous=Theme::scheme()]{Theme::setScheme(previous);});
  for(const auto scheme:{Theme::Scheme::Dark,Theme::Scheme::Light}){
   Theme::setScheme(scheme);
   EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
   window.selectTool(Tool::Arrow);QApplication::processEvents();
   const auto image=window.grab().toImage();int checked=0;
   for(auto* swatch:window.findChildren<QToolButton*>()){
    if(!swatch->accessibleName().startsWith("Color ")||swatch->isChecked())continue;++checked;
    // The top edge's middle: the 1 px outline row, and the panel above the square.
    const auto edge=image.pixelColor(swatch->mapTo(&window,QPoint(swatch->width()/2,Graphite::scaled(4))));
    const auto panel=image.pixelColor(swatch->mapTo(&window,QPoint(swatch->width()/2,1)));
    const double ratio=Theme::contrast(edge,panel);
    QVERIFY2(ratio>=3.0,qPrintable(QString("%1 %2: %3:1").arg(Theme::name(scheme),swatch->accessibleName()).arg(ratio,0,'f',2)));
   }
   QCOMPARE(checked,6);
  }
 }
 // The palette and the custom picker sit in even rows, and the rule under the
 // tool's help shows only when options follow it.
 void propertiesPanelHasNoOrphans(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  window.selectTool(Tool::Arrow);QApplication::processEvents();
  QHash<int,int> rows;for(auto* button:window.findChildren<QToolButton*>())if(button->accessibleName().startsWith("Color ")||button->objectName()=="customColor")++rows[button->mapTo(&window,QPoint()).y()];
  QCOMPARE(rows.size(),2);for(const int count:rows)QCOMPARE(count,4);
  auto* rule=window.findChild<QWidget*>("editorOptionsRule");QVERIFY(rule);QVERIFY(rule->isVisible());
  window.selectTool(Tool::Select);QApplication::processEvents();QVERIFY(!rule->isVisible());
  window.selectTool(Tool::Redact);QApplication::processEvents();QVERIFY(!rule->isVisible());
  window.selectTool(Tool::Crop);QApplication::processEvents();QVERIFY(rule->isVisible());
 }
 void saveFeedbackNamesTheCopyAndOffersRecovery(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  add(doc,{{"kind","redact"},{"rect",QJsonArray{10,10,50,20}}});window.documentEdited();
  QList<QUrl> opened;host::setTestOpener([&](const QUrl& url){opened.append(url);});auto restore=qScopeGuard([]{host::setTestOpener({});});
  QTemporaryDir dir;const auto path=dir.filePath("synthetic copy.png");{QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("synthetic");}
  window.showSaved(path);
  auto* saved=window.findChild<QLabel*>("editorSaved");QVERIFY(saved);QVERIFY(saved->isVisible());
  QVERIFY(saved->text().contains("synthetic copy.png"));QCOMPARE(saved->accessibleDescription(),QFileInfo(path).absoluteFilePath());QCOMPARE(saved->toolTip(),QFileInfo(path).absoluteFilePath());
  auto* show=window.findChild<QPushButton*>("showSavedCopy");auto* another=window.findChild<QPushButton*>("chooseAnotherLocation");QVERIFY(show&&another);
  QVERIFY(show->isVisible());QVERIFY(!another->isVisible());
  show->click();PUTGD_TRY_COMPARE(opened.size(),1);QCOMPARE(opened.first(),QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
  auto state=window.viewState();QCOMPARE(state["lastSave"].toObject()["status"].toString(),QString("saved"));QCOMPARE(state["lastShowInFolder"].toObject()["opened"].toBool(),true);
  // A failure names the reason, keeps the draft and offers another location.
  window.showSaveFailed(dir.filePath("denied.png"),"Permission denied");
  auto* error=window.findChild<QLabel*>("editorSaveError");QVERIFY(error);QVERIFY(error->isVisible());QVERIFY(!error->wordWrap());QVERIFY(!saved->isVisible());
  QVERIFY(error->text().contains("Permission denied",Qt::CaseInsensitive));QVERIFY(error->accessibleDescription().contains(QFileInfo(dir.filePath("denied.png")).absoluteFilePath()));
  auto* target=window.findChild<Graphite::ElidedLabel*>("editorSaveTarget");QVERIFY(target&&target->isVisible());QVERIFY(target->text().contains("denied.png"));QVERIFY(!target->revealable());
  auto* details=window.findChild<QToolButton*>("editorSaveDetails");QVERIFY(details&&details->isVisible());details->click();auto* popup=window.findChild<Graphite::ElidedLabel*>("editorSaveError")->fullTextPopup();QVERIFY(popup&&popup->isVisible());
  auto* full=popup->findChild<QLabel*>("elidedLabelFullTextValue");QVERIFY(full);QVERIFY(full->text().contains(QFileInfo(dir.filePath("denied.png")).absoluteFilePath()));QVERIFY(full->text().contains("Permission denied"));QVERIFY(full->textInteractionFlags()&Qt::TextSelectableByKeyboard);QCOMPARE(popup->layout()->contentsMargins(),QMargins(8,8,8,8));popup->close();
  QString detailsCode,detailsMessage;auto detailsState=window.viewCommand({{"action","saveDetails"}},detailsCode,detailsMessage);QVERIFY2(detailsCode.isEmpty(),qPrintable(detailsMessage));QVERIFY(detailsState["saveDetailsOpen"].toBool());
  detailsState=window.viewCommand({{"action","saveDetails"}},detailsCode,detailsMessage);QVERIFY(detailsCode.isEmpty());QVERIFY(!detailsState["saveDetailsOpen"].toBool());
  QVERIFY(another->isVisible());QVERIFY(!show->isVisible());QCOMPARE(doc.shapes().size(),1);
  QCOMPARE(window.viewState()["lastSave"].toObject()["reason"].toString(),QString("Permission denied"));
  QString code,message;window.viewCommand({{"action","showSavedCopy"}},code,message);QCOMPARE(code,QString("editor_no_saved_copy"));
 }
 // Agents drive the window's own view and selection controls (editor.view).
 void viewCommandsDriveTheWindowControls(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  QString code,message;auto view=[&](const QJsonObject& params){code.clear();return window.viewCommand(params,code,message);};
  auto state=view({{"action","tool"},{"tool","rectangle"}});QVERIFY2(code.isEmpty(),qPrintable(message));QCOMPARE(state["tool"].toString(),QString("rectangle"));QCOMPARE(window.canvas().tool(),Tool::Rectangle);
  state=view({{"action","place"}});QCOMPARE(doc.shapes().size(),1);const auto id=doc.shapes().first().id;
  QCOMPARE(quint64(state["selectedShapeId"].toDouble()),id);QCOMPARE(state["geometry"].toArray(),QJsonArray({100,65,200,130}));
  // The geometry fields show the selection and edit it as one undo step.
  auto* x=window.findChild<QSpinBox*>("geometryX");auto* width=window.findChild<QSpinBox*>("geometryWidth");QVERIFY(x&&width);QVERIFY(x->isVisible());
  QCOMPARE(x->value(),100);QCOMPARE(width->value(),200);
  x->setValue(12);QCOMPARE(doc.find(id)->rect,QRect(12,65,200,130));QVERIFY(doc.undo());window.documentEdited();QCOMPARE(doc.find(id)->rect,QRect(100,65,200,130));QCOMPARE(x->value(),100);
  state=view({{"action","geometry"},{"rect",QJsonArray{5,6,70,80}}});QCOMPARE(doc.find(id)->rect,QRect(5,6,70,80));QCOMPARE(width->value(),70);
  view({{"action","select"},{"shapeId",999}});QCOMPARE(code,QString("edit_shape_missing"));
  state=view({{"action","select"},{"shapeId",QJsonValue()}});QVERIFY(state["selectedShapeId"].isNull());QVERIFY(!x->isVisible());
  view({{"action","geometry"},{"rect",QJsonArray{1,1,5,5}}});QCOMPARE(code,QString("editor_no_selection"));
  state=view({{"action","selectNext"}});QCOMPARE(quint64(state["selectedShapeId"].toDouble()),id);
  QVERIFY(state["selection"].toString().startsWith("Rectangle 1 of 1 selected"));QCOMPARE(window.canvas().accessibleDescription(),state["selection"].toString());
  // Zoom and pan move the view; fit restores it.
  state=view({{"action","zoom"},{"percent",400}});QCOMPARE(state["zoomPercent"].toDouble(),400.0);QVERIFY(!state["fitted"].toBool());
  const auto shown=state["visibleRect"].toArray();state=view({{"action","pan"},{"delta",QJsonArray{-200,0}}});QVERIFY(state["visibleRect"].toArray()!=shown);
  QCOMPARE(window.findChild<QToolButton*>("editorZoom")->text(),QString("400%"));
  state=view({{"action","fit"}});QVERIFY(state["fitted"].toBool());
  // Crop through the same draft as the keyboard and fields, applied once.
  state=view({{"action","tool"},{"tool","crop"}});QVERIFY(state["cropPending"].toBool());
  view({{"action","geometry"},{"rect",QJsonArray{20,30,200,100}}});QCOMPARE(doc.crop(),doc.imageRect());
  state=view({{"action","applyCrop"}});QCOMPARE(doc.crop(),QRect(20,30,200,100));QCOMPARE(state["tool"].toString(),QString("select"));
  view({{"action","explode"}});QCOMPARE(code,QString("invalid_request"));
  view({{"action","zoom"},{"percent",1}});QCOMPARE(code,QString("invalid_request"));
  view({{"action","tool"},{"tool","text"}});view({{"action","place"}});QCOMPARE(code,QString("invalid_request"));
  window.setBusy("Preparing the edited image…");
  view({{"action","tool"},{"tool","blur"}});QCOMPARE(code,QString("editor_finalizing"));
  state=view({{"action","inspect"}});QVERIFY(code.isEmpty());QVERIFY(state["busy"].toBool());QCOMPARE(state["message"].toObject()["text"].toString(),QString("Preparing the edited image…"));
  window.allowClose();
 }
 void tabLeavesCanvasWithSelectionAndPropertiesIntact(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  add(doc,{{"kind","redact"},{"rect",QJsonArray{10,10,50,20}}});add(doc,{{"kind","blur"},{"rect",QJsonArray{100,100,50,20}}});window.documentEdited();
  window.selectTool(Tool::Select);auto& canvas=window.canvas();window.activateWindow();canvas.setFocus();
  QTest::keyClick(&canvas,Qt::Key_BracketRight);const auto id=doc.shapes().at(0).id;QCOMPARE(canvas.selected(),id);
  auto* x=window.findChild<QSpinBox*>("geometryX");QVERIFY(x&&x->isVisible());
  QTest::keyClick(&canvas,Qt::Key_Tab);QCOMPARE(QApplication::focusWidget(),x);QCOMPARE(canvas.selected(),id);QVERIFY(x->isVisible());
  QTest::keyClick(x,Qt::Key_Backtab);QCOMPARE(QApplication::focusWidget(),&canvas);QCOMPARE(canvas.selected(),id);
  QTest::keyClick(&canvas,Qt::Key_BracketLeft);QCOMPARE(canvas.selected(),doc.shapes().at(1).id);
  QTest::keyClick(&canvas,Qt::Key_Escape);QCOMPARE(canvas.selected(),quint64(0));QVERIFY(!x->isVisible());
  // The Place button is the pointer-free route for each drawing tool.
  window.selectTool(Tool::Highlight);auto* place=window.findChild<QPushButton*>("placeMark");QVERIFY(place&&place->isVisible());QCOMPARE(place->text(),QString("Place highlight"));
  place->click();QCOMPARE(doc.shapes().size(),3);QCOMPARE(doc.shapes().last().kind,Shape::Kind::Highlight);
  window.selectTool(Tool::Select);QVERIFY(!place->isVisible());
 }
 // Save a copy… and Upload export the same thing: a pending crop is applied
 // before the chooser opens, and again before writing.
 void copyChooserNormalizesPngAndConfirmsTheFinalTarget(){
  qputenv("PUTGD_FILE_DIALOG","fallback");auto restore=qScopeGuard([]{qunsetenv("PUTGD_FILE_DIALOG");});
  EditDocument doc(canvasImage());EditorWindow window(doc);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));QTemporaryDir dir;
  QVERIFY(window.saveCopy());auto* chooser=window.fileDialog().fallbackDialog();QVERIFY(chooser);QCOMPARE(chooser->defaultSuffix(),QString("png"));
  const auto path=dir.filePath("without-extension");chooser->selectFile(path);QMetaObject::invokeMethod(chooser,"accept");
  PUTGD_TRY_COMPARE(window.viewState()["lastSave"].toObject()["status"].toString(),QString("saved"));QVERIFY(QFileInfo::exists(path+".png"));QVERIFY(!QFileInfo::exists(path));
  const auto final=dir.filePath("existing.jpg.png");QFile file(final);QVERIFY(file.open(QIODevice::WriteOnly));file.write("keep existing");file.close();
  QVERIFY(window.saveCopy());chooser=window.fileDialog().fallbackDialog();QVERIFY(chooser);chooser->selectFile(dir.filePath("existing.jpg"));QMetaObject::invokeMethod(chooser,"accept");
  PUTGD_TRY_VERIFY(window.fileDialog().isOpen());auto* confirm=window.fileDialog().fallbackDialog();QVERIFY(confirm&&confirm!=chooser);QCOMPARE(confirm->selectedFiles().first(),final);
  QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("keep existing"));file.close();
  window.fileDialog().cancel();QVERIFY(!window.savingCopy());
 }
 void copyWorkerKeepsTheWindowResponsiveAndSnapshotsTheDocument(){
  QImage image(2400,1800,QImage::Format_ARGB32);image.fill(Qt::white);
  EditDocument doc(image);EditorWindow window(doc);window.setDestination("Private upload to synthetic archive");window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  QTemporaryDir dir;const auto path=dir.filePath("copy.jpg");bool done=false,tick=false;QByteArray saved;
  QVERIFY(window.writeCopy(path,[&](const QByteArray& png,const QString& error){QVERIFY(error.isEmpty());saved=png;done=true;}));
  QVERIFY(window.viewState()["savingCopy"].toBool());QVERIFY(!window.findChild<QLabel*>("editorDestination")->isVisible());QCOMPARE(window.viewState()["message"].toObject()["text"].toString(),QString("Saving a copy…"));
  QVERIFY(!window.writeCopy(dir.filePath("overlap.png")));QVERIFY(!window.saveCopy());
  QTimer::singleShot(0,&window,[&]{tick=window.savingCopy();});
  add(doc,{{"kind","redact"},{"rect",QJsonArray{0,0,200,200}}});window.documentEdited();
  PUTGD_TRY_VERIFY(tick);PUTGD_TRY_VERIFY(done);
  QVERIFY(!window.viewState()["savingCopy"].toBool());QVERIFY(!window.findChild<QLabel*>("editorDestination")->isVisible());QVERIFY(!window.viewState()["lastSave"].toObject()["current"].toBool());
  QCOMPARE(window.viewState()["lastSave"].toObject()["path"].toString(),path+".png");
  QCOMPARE(QImage::fromData(saved).pixelColor(10,10),QColor(Qt::white));QVERIFY(QFileInfo::exists(path+".png"));QVERIFY(!QFileInfo::exists(path));
 }
 void saveCopyFinishesThePendingCropLikeUpload(){
  qputenv("PUTGD_FILE_DIALOG","fallback");auto restore=qScopeGuard([]{qunsetenv("PUTGD_FILE_DIALOG");});
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  QString code,message;window.viewCommand({{"action","tool"},{"tool","crop"}},code,message);
  window.viewCommand({{"action","geometry"},{"rect",QJsonArray{20,30,200,100}}},code,message);QVERIFY2(code.isEmpty(),qPrintable(message));
  QVERIFY(window.canvas().cropPending());QCOMPARE(doc.crop(),doc.imageRect());
  QVERIFY(window.saveCopy());QCOMPARE(doc.crop(),QRect(20,30,200,100));QVERIFY(!window.canvas().cropPending());
  QTemporaryDir dir;const auto path=dir.filePath("copy.png");
  QFileDialog* chooser=nullptr;PUTGD_TRY_VERIFY((chooser=window.fileDialog().fallbackDialog())&&chooser->isVisible());
  chooser->selectFile(path);QMetaObject::invokeMethod(chooser,"accept");
  PUTGD_TRY_VERIFY(QFileInfo::exists(path));PUTGD_TRY_VERIFY(!window.savingCopy());QCOMPARE(QImageReader(path).size(),QSize(200,100));
  // An agent's crop draft made while the chooser was open is finished too.
  window.viewCommand({{"action","tool"},{"tool","crop"}},code,message);window.viewCommand({{"action","geometry"},{"rect",QJsonArray{20,30,50,40}}},code,message);
  QString reason;const auto png=writeCopy(window,dir.filePath("second.png"),reason);QVERIFY2(!png.isEmpty(),qPrintable(reason));
  QCOMPARE(QImage::fromData(png,"PNG").size(),QSize(50,40));QCOMPARE(png,doc.exportPng());
  // Upload runs the same finalizer before it asks for the export.
  window.viewCommand({{"action","tool"},{"tool","crop"}},code,message);window.viewCommand({{"action","geometry"},{"rect",QJsonArray{25,35,30,20}}},code,message);
  QSignalSpy uploads(&window,&EditorWindow::uploadRequested);window.findChild<QPushButton*>("editorUpload")->click();
  QCOMPARE(uploads.size(),1);QCOMPARE(doc.crop(),QRect(25,35,30,20));
 }
 // The saved copy is tied to the draft it holds: another edit makes it an
 // earlier copy, and the header stops claiming one.
 void savedCopyGoesStaleAfterAnotherEditAndCopiesItsPath(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  QTemporaryDir dir;const auto path=dir.filePath("synthetic copy.png");
  QString reason;QVERIFY2(!writeCopy(window,path,reason).isEmpty(),qPrintable(reason));
  auto* state=window.findChild<QLabel*>("documentState");auto* saved=window.findChild<QLabel*>("editorSaved");QVERIFY(state&&saved);
  QCOMPARE(state->text(),QString("Not uploaded · Copy saved"));QCOMPARE(saved->text(),QString("Saved synthetic copy.png"));
  QVERIFY(window.viewState()["lastSave"].toObject()["current"].toBool());
  // Copy path sits beside Show in folder and takes keyboard focus.
  auto* copy=window.findChild<QPushButton*>("copySavedPath");QVERIFY(copy&&copy->isVisible());QVERIFY(copy->focusPolicy()&Qt::TabFocus);
  copy->click();QCOMPARE(QGuiApplication::clipboard()->text(),QFileInfo(path).absoluteFilePath());
  QCOMPARE(window.viewState()["lastCopiedPath"].toString(),QFileInfo(path).absoluteFilePath());
  add(doc,{{"kind","redact"},{"rect",QJsonArray{10,10,50,20}}});window.documentEdited();
  QCOMPARE(state->text(),QString("Not uploaded"));QCOMPARE(saved->text(),QString("Earlier copy saved: synthetic copy.png"));
  QVERIFY(!window.viewState()["lastSave"].toObject()["current"].toBool());QVERIFY(copy->isVisible());
  QGuiApplication::clipboard()->clear();QString code,message;window.viewCommand({{"action","copySavedPath"}},code,message);QVERIFY(code.isEmpty());
  QCOMPARE(QGuiApplication::clipboard()->text(),QFileInfo(path).absoluteFilePath());
  // Saving again makes the copy current.
  QVERIFY(!writeCopy(window,path,reason).isEmpty());QCOMPARE(state->text(),QString("Not uploaded · Copy saved"));
  // A chooser that failed (no file named yet) is shown as a failure, not a cancel.
  window.showSaveFailed({},"The chosen location is not a local file.");
  auto* error=window.findChild<QLabel*>("editorSaveError");QVERIFY(error->isVisible());QCOMPARE(error->text(),QString("Couldn't save: The chosen location is not a local file."));
  QVERIFY(window.findChild<QPushButton*>("chooseAnotherLocation")->isVisible());QVERIFY(!copy->isVisible());
  QCOMPARE(window.viewState()["lastSave"].toObject()["stage"].toString(),QString("chooser"));
 }
 // Cancel reads Discard… once there are edits; its confirmation is opened,
 // not exec'd, so editor.view can read and answer it.
 void discardConfirmationIsInspectableAndAnswerable(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* cancel=window.findChild<QPushButton*>("editorCancel");QVERIFY(cancel);QCOMPARE(cancel->text(),QString("Cancel"));
  add(doc,{{"kind","redact"},{"rect",QJsonArray{10,10,50,20}}});window.documentEdited();QCOMPARE(cancel->text(),QString("Discard…"));
  QSignalSpy cancels(&window,&EditorWindow::cancelRequested);QString code,message;auto view=[&](const QJsonObject& params){code.clear();return window.viewCommand(params,code,message);};
  auto state=view({{"action","discard"}});QVERIFY2(code.isEmpty(),qPrintable(message));
  QVERIFY(state["discardConfirmation"].toObject()["open"].toBool());QVERIFY(state["discardConfirmation"].toObject()["text"].toString().contains("Discard your edits"));
  QCOMPARE(state["cancelLabel"].toString(),QString("Discard…"));auto* box=window.findChild<QDialog*>("confirmAction");QVERIFY(box);
  auto* keep=box->findChild<QPushButton*>("keepEditingConfirm");auto* discard=box->findChild<QPushButton*>("confirmDiscard");QVERIFY(keep&&discard);
  QApplication::processEvents();QVERIFY(keep->mapTo(box,QPoint()).x()<discard->mapTo(box,QPoint()).x());QVERIFY(keep->isDefault());QVERIFY(!discard->isDefault());PUTGD_TRY_VERIFY(keep->hasFocus());
  QTest::keyClick(keep,Qt::Key_Escape);QVERIFY(!window.viewState()["discardConfirmation"].toObject()["open"].toBool());QCOMPARE(cancels.size(),0);
  view({{"action","discard"}});
  view({{"action","tool"},{"tool","blur"}});QCOMPARE(code,QString("editor_confirming"));
  state=view({{"action","keepEditing"}});QVERIFY(code.isEmpty());QVERIFY(!state["discardConfirmation"].toObject()["open"].toBool());QCOMPARE(cancels.size(),0);
  view({{"action","confirmDiscard"}});QCOMPARE(code,QString("invalid_request"));
  // The close button asks the same question.
  QVERIFY(!window.close());state=view({{"action","inspect"}});QVERIFY(state["discardConfirmation"].toObject()["open"].toBool());
  view({{"action","confirmDiscard"}});QVERIFY(code.isEmpty());QCOMPARE(cancels.size(),1);
  view({{"action","discard"}});box=nullptr;for(auto* candidate:window.findChildren<QDialog*>("confirmAction"))if(candidate->isVisible())box=candidate;QVERIFY(box);
  keep=box->findChild<QPushButton*>("keepEditingConfirm");discard=box->findChild<QPushButton*>("confirmDiscard");PUTGD_TRY_VERIFY(keep->hasFocus());
  QTest::keyClick(keep,Qt::Key_Tab);QCOMPARE(QApplication::focusWidget(),discard);QTest::keyClick(discard,Qt::Key_Return);QCOMPARE(cancels.size(),2);
  // Submitting (agents can) closes an open question without discarding.
  view({{"action","discard"}});QVERIFY(window.viewState()["discardConfirmation"].toObject()["open"].toBool());
  window.setBusy("Preparing the edited image…");QVERIFY(!window.viewState()["discardConfirmation"].toObject()["open"].toBool());QCOMPARE(cancels.size(),2);
  window.allowClose();
 }
 // An encoding or size failure gives the draft back with every control and
 // shows why, with Keep editing and Save a copy… beside it.
 void preparationFailureReturnsToEditing(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.setDestination("Upload to synthetic destination");window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  add(doc,{{"kind","redact"},{"rect",QJsonArray{10,10,50,20}}});window.documentEdited();
  window.setBusy("Preparing the edited image…");
  window.showPreparationFailed("The edited image is 41 MB, over the 25 MB limit for this output.");
  auto* upload=window.findChild<QPushButton*>("editorUpload");
  QVERIFY(upload->isEnabled());QVERIFY(window.canvas().isEnabled());QVERIFY(window.findChild<QPushButton*>("editorCancel")->isEnabled());
  auto* reason=window.findChild<QLabel*>("editorPreparationReason");QVERIFY(reason&&reason->isVisible());QVERIFY(reason->wordWrap());
  auto* keep=window.findChild<QPushButton*>("keepEditing");auto* save=window.findChild<QPushButton*>("saveCopyInstead");QVERIFY(keep&&keep->isVisible()&&save&&save->isVisible());
  // One Save a copy… on screen: the strip's stands in for the footer's.
  QVERIFY(!window.findChild<QPushButton*>("editorSaveCopy")->isVisible());
  QVERIFY(window.findChild<QLabel*>("editorDestination")->isVisible());
  auto state=window.viewState();QVERIFY(!state["busy"].toBool());QCOMPARE(state["preparationFailure"].toObject()["text"].toString(),reason->text());
  QString code,message;state=window.viewCommand({{"action","keepEditing"}},code,message);QVERIFY(code.isEmpty());QVERIFY(state["preparationFailure"].isNull());QVERIFY(!reason->isVisible());
  QVERIFY(window.findChild<QPushButton*>("editorSaveCopy")->isVisible());
  QCOMPARE(doc.shapes().size(),1);
 }
 // Summary and actions share the 56 px band until the summary needs its own row.
 void footerOnlyWrapsWhenTheDestinationCannotFit(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.setDestination("Upload to Project archive");window.resize(1100,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* destination=window.findChild<Graphite::ElidedLabel*>("editorDestination");auto* footer=window.findChild<QWidget*>("editorFooter");const auto upload=window.findChild<QPushButton*>("editorUpload");
  QVERIFY(destination);QVERIFY(destination->focusPolicy()&Qt::TabFocus);QApplication::processEvents();
  QVERIFY(!window.viewState()["destination"].toObject()["above"].toBool());
  QCOMPARE(destination->geometry().center().y(),upload->geometry().center().y());QVERIFY(destination->geometry().right()<window.findChild<QPushButton*>("editorSaveCopy")->x());
  QCOMPARE(footer->height(),Graphite::scaled(36)+20);
  const QString longName="Upload to Production screenshots for customer support, Europe, Middle East and Asia-Pacific teams, release readiness and accessibility reviews";
  window.setDestination(longName);window.resize(760,560);QApplication::processEvents();
  QVERIFY(window.viewState()["destination"].toObject()["above"].toBool());QVERIFY(destination->isVisible());
  QVERIFY(destination->geometry().bottom()<upload->geometry().top());QCOMPARE(destination->text(),longName);QCOMPARE(destination->toolTip(),longName);QVERIFY(destination->elided());
  destination->setFocus(Qt::TabFocusReason);QTest::keyClick(destination,Qt::Key_Return);QVERIFY(destination->fullTextPopup());QVERIFY(destination->fullTextPopup()->isVisible());destination->fullTextPopup()->close();
  window.setDestination("Upload to Project archive");window.resize(1400,700);QApplication::processEvents();QVERIFY(!window.viewState()["destination"].toObject()["above"].toBool());
  QCOMPARE(footer->height(),Graphite::scaled(36)+20);
 }
 void blurWarningUsesTheExistingRedactToolWithoutChangingMarks(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));window.selectTool(Tool::Blur);
  add(doc,{{"kind","blur"},{"rect",QJsonArray{10,10,50,20}},{"strength",24}});window.documentEdited();window.canvas().select(doc.shapes().last().id);const auto original=doc.shapes().last();
  auto* warning=window.findChild<QLabel*>("editorToolWarning");auto* redact=window.findChild<QToolButton*>("useRedact");QVERIFY(warning&&warning->isVisible());QVERIFY(redact&&redact->isVisible());
  redact->click();QCOMPARE(window.canvas().tool(),Tool::Redact);QCOMPARE(doc.shapes().last(),original);QVERIFY(!warning->isVisible());
  QString code,message;window.viewCommand({{"action","tool"},{"tool","blur"}},code,message);QVERIFY(code.isEmpty());QVERIFY(warning->isVisible());
  window.viewCommand({{"action","tool"},{"tool","redact"}},code,message);QVERIFY(code.isEmpty());QCOMPARE(window.canvas().tool(),Tool::Redact);QCOMPARE(doc.shapes().last(),original);QVERIFY(!warning->isVisible());
  window.selectTool(Tool::Blur);window.setBusy("Preparing…");QVERIFY(!redact->isEnabled());
 }
 void removeMarkIsVisibleUndoableAndAgentControllable(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1100,900);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  add(doc,{{"kind","rectangle"},{"rect",QJsonArray{10,10,50,20}}});window.documentEdited();const auto original=doc.shapes().first();
  auto* remove=window.findChild<QPushButton*>("removeMark");QVERIFY(remove);QVERIFY(!remove->isVisible());
  window.selectTool(Tool::Select);window.canvas().select(original.id);QVERIFY(remove->isVisible());QVERIFY(!remove->icon().isNull());QVERIFY(remove->toolTip().contains("Delete"));
  remove->click();QVERIFY(doc.shapes().isEmpty());QCOMPARE(window.canvas().selected(),quint64(0));QVERIFY(!remove->isVisible());
  QVERIFY(doc.undo());window.documentEdited();QCOMPARE(doc.shapes().first(),original);
  QString code,message;window.viewCommand({{"action","select"},{"shapeId",double(original.id)}},code,message);
  window.viewCommand({{"action","removeMark"},{"extra",true}},code,message);QCOMPARE(code,QString("invalid_request"));QCOMPARE(doc.shapes().size(),1);
  code.clear();auto state=window.viewCommand({{"action","removeMark"}},code,message);QVERIFY2(code.isEmpty(),qPrintable(message));QVERIFY(state["selectedShapeId"].isNull());QVERIFY(doc.shapes().isEmpty());
  QVERIFY(doc.undo());window.documentEdited();QCOMPARE(doc.shapes().first(),original);
  window.canvas().select(original.id);QTest::keyClick(&window.canvas(),Qt::Key_Delete);QVERIFY(doc.shapes().isEmpty());
  code.clear();window.viewCommand({{"action","removeMark"}},code,message);QCOMPARE(code,QString("editor_no_selection"));
  QVERIFY(doc.undo());window.documentEdited();window.canvas().select(original.id);window.setBusy("Preparing…");QVERIFY(!remove->isEnabled());
  code.clear();window.viewCommand({{"action","removeMark"}},code,message);QCOMPARE(code,QString("editor_finalizing"));QCOMPARE(doc.shapes().size(),1);
 }
 void shortBodyScrollsDocumentAndKeepsExportAndRecoveryVisible(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.setDestination("Private upload to Production screenshots for customer support (EU)");window.resize(1000,1100);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* scroll=window.findChild<QScrollArea*>("editorPropertiesScroll");auto* document=window.findChild<QWidget*>("editorDocument");QVERIFY(scroll&&document);
  PUTGD_TRY_VERIFY(!scroll->isAncestorOf(document));
  window.showSaveFailed("/synthetic/copy.png","Permission denied");window.resize(900,620);
  const QString reason="The edited image is 41.3 MB, over the 25 MB limit for this output. Your edits are kept: crop it and try again, or save a copy.";
  window.showPreparationFailed(reason);PUTGD_TRY_VERIFY(scroll->isAncestorOf(document));
  // Document reparenting finishes before its viewport can be measured.
  PUTGD_TRY_VERIFY(scroll->viewport()->height()>=Graphite::scaled(180));
  auto* label=window.findChild<QLabel*>("editorPreparationReason");QCOMPARE(label->text(),reason);QVERIFY(label->height()>=label->heightForWidth(label->width()));
  const auto footer=window.findChild<QWidget*>("editorFooter");
  for(const auto name:{"keepEditing","saveCopyInstead","editorUpload","editorCancel"}){
   auto* button=window.findChild<QPushButton*>(name);QVERIFY(button&&button->isVisible());QVERIFY(footer->rect().contains(QRect(button->mapTo(footer,QPoint()),button->size())));
  }
  auto* choose=window.findChild<QPushButton*>("chooseAnotherLocation");QApplication::processEvents();
  QVERIFY(footer->rect().contains(QRect(choose->mapTo(footer,QPoint()),choose->size())));
  window.resize(1000,1100);PUTGD_TRY_VERIFY(!scroll->isAncestorOf(document));QVERIFY(choose->isVisible());
 }
 void failedFolderOpenShowsCopyPathRecoveryAndClearsForANewSave(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(900,620);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  QTemporaryDir dir;const auto path=dir.filePath("missing.png");window.showSaved(path);
  QString code,message;auto state=window.viewCommand({{"action","showSavedCopy"}},code,message);QVERIFY2(code.isEmpty(),qPrintable(message));QVERIFY(!state["lastShowInFolder"].toObject()["opened"].toBool(true));
  auto* error=window.findChild<QLabel*>("editorFolderError");QVERIFY(error&&error->isVisible());QVERIFY(error->text().contains("Use Copy path instead."));
  auto* copy=window.findChild<QPushButton*>("copySavedPath");QVERIFY(copy->isVisible());
  auto* scroll=window.findChild<QScrollArea*>("editorPropertiesScroll");
  if(scroll->isAncestorOf(error)){
   PUTGD_TRY_VERIFY(scroll->viewport()->rect().contains(QRect(error->mapTo(scroll->viewport(),QPoint()),error->size())));
   QVERIFY(scroll->viewport()->rect().contains(QRect(copy->mapTo(scroll->viewport(),QPoint()),copy->size())));
  }
  copy->click();QCOMPARE(QGuiApplication::clipboard()->text(),path);
  QString reason;QVERIFY(!writeCopy(window,path,reason).isEmpty());QVERIFY(!error->isVisible());QVERIFY(window.viewState()["lastShowInFolder"].isNull());
  host::setTestOpener([](const QUrl&){});auto restore=qScopeGuard([]{host::setTestOpener({});});
  QVERIFY(window.showSavedCopy());QVERIFY(window.viewState()["lastShowInFolder"].toObject()["opened"].toBool());QVERIFY(!error->isVisible());
  window.showSaveFailed({},"The chooser failed");QVERIFY(!error->isVisible());QVERIFY(!copy->isVisible());
 }
 void unmodifiedCancelNeedsNoConfirmation(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  QSignalSpy cancels(&window,&EditorWindow::cancelRequested);QVERIFY(!window.close());QCOMPARE(cancels.size(),1);QVERIFY(window.isVisible());
 }
};
QTEST_MAIN(EditorWindowTest)
#include "test_editor_window.moc"
