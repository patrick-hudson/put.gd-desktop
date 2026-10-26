// Offscreen widget contract for the themed editor window. Not desktop proof.
#include "ui/EditorWindow.h"
#include "ui/FileDialog.h"
#include "editor/EditorCommands.h"
#include "platform/HostOpen.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QImageReader>
#include <QMessageBox>
#include <QFileInfo>
#include <QJsonArray>
#include <QLabel>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QToolButton>
#include <QSpinBox>
#include <QFontComboBox>
#include <QSignalSpy>
#include <QTest>
#include "fixtures/Waits.h"
using namespace putgd;using namespace putgd::editor;

class EditorWindowTest:public QObject {
 Q_OBJECT
 static QImage canvasImage(){QImage image(400,260,QImage::Format_RGB32);image.fill(QColor("#20242c"));return image;}
 static void add(EditDocument& doc,const QJsonObject& shape){QJsonArray created;QString code,message;QVERIFY2(Commands::apply(doc,{QJsonObject{{"op","add"},{"shape",shape}}},created,code,message),qPrintable(message));}
private slots:
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
  QToolButton* green=nullptr;for(auto* button:window.findChildren<QToolButton*>())if(button->accessibleName()=="Color #22c55e")green=button;
  QVERIFY(green);green->click();QCOMPARE(doc.find(id)->color,QColor("#22c55e"));
  const auto shape=*doc.find(id);
  for(auto tool:{Tool::Highlight,Tool::Arrow,Tool::Text,Tool::Blur,Tool::Redact,Tool::Rectangle,Tool::Select}){window.selectTool(tool);QCOMPARE(*doc.find(id),shape);}
  QCOMPARE(canvas.selected(),quint64(0));
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
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* zoom=window.findChild<QLabel*>("editorZoom");QVERIFY(zoom);
  QVERIFY2(QRegularExpression("^\\d+%$").match(zoom->text()).hasMatch(),qPrintable(zoom->text()));QVERIFY(zoom->accessibleName().startsWith("Zoom "));
  auto* info=window.findChild<QLabel*>("editorDocumentInfo");QVERIFY(info);QVERIFY(info->text().contains("400 × 260 px"));
  for(auto* label:window.findChildren<QLabel*>())if(label!=info)QVERIFY2(!label->text().contains("400 × 260"),qPrintable(label->objectName()));
  QVERIFY(!window.findChild<QScrollArea*>("editorPropertiesScroll")->isAncestorOf(info));
  // Every colour target is at least 32 px; the painted swatch stays inside it.
  window.selectTool(Tool::Arrow);int swatches=0;
  for(auto* button:window.findChildren<QToolButton*>())if(button->accessibleName().startsWith("Color ")||button->objectName()=="customColor"){++swatches;QVERIFY(button->width()>=32&&button->height()>=32);}
  QCOMPARE(swatches,8);
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
  auto* error=window.findChild<QLabel*>("editorSaveError");QVERIFY(error);QVERIFY(error->isVisible());QVERIFY(error->wordWrap());QVERIFY(!saved->isVisible());
  QVERIFY(error->text().contains("Permission denied",Qt::CaseInsensitive));QVERIFY(error->accessibleDescription().contains(QFileInfo(dir.filePath("denied.png")).absoluteFilePath()));
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
  QCOMPARE(window.findChild<QLabel*>("editorZoom")->text(),QString("400%"));
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
 // Tab walks the marks and then leaves the canvas, so the keyboard is never trapped.
 void tabWalksMarksThenLeavesTheCanvas(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  add(doc,{{"kind","redact"},{"rect",QJsonArray{10,10,50,20}}});add(doc,{{"kind","blur"},{"rect",QJsonArray{100,100,50,20}}});window.documentEdited();
  window.selectTool(Tool::Select);auto& canvas=window.canvas();
  QTest::keyClick(&canvas,Qt::Key_Tab);QCOMPARE(canvas.selected(),doc.shapes().at(0).id);
  QTest::keyClick(&canvas,Qt::Key_Tab);QCOMPARE(canvas.selected(),doc.shapes().at(1).id);
  QTest::keyClick(&canvas,Qt::Key_Backtab);QCOMPARE(canvas.selected(),doc.shapes().at(0).id);
  QTest::keyClick(&canvas,Qt::Key_Tab);QTest::keyClick(&canvas,Qt::Key_Tab);QCOMPARE(canvas.selected(),quint64(0));
  // The Place button is the pointer-free route for each drawing tool.
  window.selectTool(Tool::Highlight);auto* place=window.findChild<QPushButton*>("placeMark");QVERIFY(place&&place->isVisible());QCOMPARE(place->text(),QString("Place highlight"));
  place->click();QCOMPARE(doc.shapes().size(),3);QCOMPARE(doc.shapes().last().kind,Shape::Kind::Highlight);
  window.selectTool(Tool::Select);QVERIFY(!place->isVisible());
 }
 // Save a copy… and Upload export the same thing: a pending crop is applied
 // before the chooser opens, and again before writing.
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
  PUTGD_TRY_VERIFY(QFileInfo::exists(path));QCOMPARE(QImageReader(path).size(),QSize(200,100));
  // An agent's crop draft made while the chooser was open is finished too.
  window.viewCommand({{"action","tool"},{"tool","crop"}},code,message);window.viewCommand({{"action","geometry"},{"rect",QJsonArray{20,30,50,40}}},code,message);
  QString reason;const auto png=window.writeCopy(dir.filePath("second.png"),reason);QVERIFY2(!png.isEmpty(),qPrintable(reason));
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
  QString reason;QVERIFY2(!window.writeCopy(path,reason).isEmpty(),qPrintable(reason));
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
  QVERIFY(!window.writeCopy(path,reason).isEmpty());QCOMPARE(state->text(),QString("Not uploaded · Copy saved"));
  // A chooser that failed (no file named yet) is shown as a failure, not a cancel.
  window.showSaveFailed({},"The chosen location is not a local file.");
  auto* error=window.findChild<QLabel*>("editorSaveError");QVERIFY(error->isVisible());QCOMPARE(error->text(),QString("Couldn't save the copy: the chosen location is not a local file."));
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
  QCOMPARE(state["cancelLabel"].toString(),QString("Discard…"));QVERIFY(window.findChild<QMessageBox*>("confirmAction"));
  view({{"action","tool"},{"tool","blur"}});QCOMPARE(code,QString("editor_confirming"));
  state=view({{"action","keepEditing"}});QVERIFY(code.isEmpty());QVERIFY(!state["discardConfirmation"].toObject()["open"].toBool());QCOMPARE(cancels.size(),0);
  view({{"action","confirmDiscard"}});QCOMPARE(code,QString("invalid_request"));
  // The close button asks the same question.
  QVERIFY(!window.close());state=view({{"action","inspect"}});QVERIFY(state["discardConfirmation"].toObject()["open"].toBool());
  view({{"action","confirmDiscard"}});QVERIFY(code.isEmpty());QCOMPARE(cancels.size(),1);
  // Submitting (agents can) closes an open question without discarding.
  view({{"action","discard"}});QVERIFY(window.viewState()["discardConfirmation"].toObject()["open"].toBool());
  window.setBusy("Preparing the edited image…");QVERIFY(!window.viewState()["discardConfirmation"].toObject()["open"].toBool());QCOMPARE(cancels.size(),1);
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
 // The footer never elides the destination: it fits beside the actions or
 // wraps on its own line above them, and it takes keyboard focus.
 void footerKeepsTheWholeDestinationReadable(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.setDestination("Upload to Project archive");window.resize(900,620);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* destination=window.findChild<QLabel*>("editorDestination");QVERIFY(destination);QVERIFY(destination->focusPolicy()&Qt::TabFocus);
  QApplication::processEvents();
  QVERIFY(!window.viewState()["destination"].toObject()["above"].toBool());
  QVERIFY2(destination->width()>=destination->fontMetrics().horizontalAdvance(destination->text()),qPrintable(QString::number(destination->width())));
  const auto upload=window.findChild<QPushButton*>("editorUpload");
  QCOMPARE(destination->geometry().center().y()/4,upload->geometry().center().y()/4);
  const QString longName="Upload to Production screenshots for customer support, Europe, Middle East and Asia-Pacific teams";
  window.setDestination(longName);window.resize(760,560);QApplication::processEvents();
  QVERIFY2(destination->fontMetrics().horizontalAdvance(longName)>window.width()-400,qPrintable(QString::number(window.width())));
  QVERIFY(window.viewState()["destination"].toObject()["above"].toBool());QVERIFY(destination->wordWrap());QVERIFY(destination->isVisible());
  QVERIFY(destination->geometry().bottom()<upload->geometry().top());QCOMPARE(destination->text(),longName);
  QVERIFY(destination->height()>=destination->heightForWidth(destination->width()));
  window.resize(1400,700);QApplication::processEvents();QVERIFY(!window.viewState()["destination"].toObject()["above"].toBool());
 }
 void unmodifiedCancelNeedsNoConfirmation(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  QSignalSpy cancels(&window,&EditorWindow::cancelRequested);QVERIFY(!window.close());QCOMPARE(cancels.size(),1);QVERIFY(window.isVisible());
 }
};
QTEST_MAIN(EditorWindowTest)
#include "test_editor_window.moc"
