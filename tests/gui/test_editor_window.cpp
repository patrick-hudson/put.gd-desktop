// Offscreen widget contract for the themed editor window. Not desktop proof.
#include "ui/EditorWindow.h"
#include "editor/EditorCommands.h"
#include <QApplication>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSignalSpy>
#include <QTest>
using namespace putgd;using namespace putgd::editor;

class EditorWindowTest:public QObject {
 Q_OBJECT
 static QImage canvasImage(){QImage image(400,260,QImage::Format_RGB32);image.fill(QColor("#20242c"));return image;}
 static void add(EditDocument& doc,const QJsonObject& shape){QJsonArray created;QString code,message;QVERIFY2(Commands::apply(doc,{QJsonObject{{"op","add"},{"shape",shape}}},created,code,message),qPrintable(message));}
private slots:
 void keyboardContractNeverUploadsOnPlainEnter(){
  EditDocument doc(canvasImage());EditorWindow window(doc,"Upload to Synthetic destination");window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
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
 void initialSizeFitsSmallScreens(){
  // Fedora's assertion-enabled libstdc++ trapped an inverted clamp on screens under 1000 px.
  QCOMPARE(EditorWindow::initialSize({640,320},{800,600}),QSize(720,540));
  QCOMPARE(EditorWindow::initialSize({6000,4000},{1920,1080}),QSize(1728,972));
  QCOMPARE(EditorWindow::initialSize({100,80},{2560,1440}),QSize(900,620));
  QCOMPARE(EditorWindow::initialSize({100,80},{200,100}),QSize(320,240));
 }
 void canvasGeometryIsStableAcrossToolsAndSelection(){
  // Cinnamon acceptance found the options row collapsing mid-drag, refitting the image under the pointer.
  EditDocument doc(canvasImage());EditorWindow window(doc,"Upload");window.resize(1000,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
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
  EditDocument doc(canvasImage());EditorWindow window(doc,"Save to the local archive");window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
  add(doc,{{"kind","redact"},{"rect",QJsonArray{10,10,50,20}}});window.documentEdited();
  QVERIFY(doc.canUndo());QSignalSpy uploads(&window,&EditorWindow::uploadRequested),cancels(&window,&EditorWindow::cancelRequested);
  window.setBusy("Preparing the edited image…");
  QPushButton* upload=nullptr;for(auto* button:window.findChildren<QPushButton*>())if(button->text()=="Upload")upload=button;
  QVERIFY(upload);QVERIFY(!upload->isEnabled());QTest::keyClick(&window,Qt::Key_Return,Qt::ControlModifier);QTest::keyClick(&window,Qt::Key_Z,Qt::ControlModifier);
  QCOMPARE(uploads.size(),0);QCOMPARE(doc.shapes().size(),1);window.requestCancel();QCOMPARE(cancels.size(),0);
  window.allowClose();QVERIFY(window.close());
 }
 void unmodifiedCancelNeedsNoConfirmation(){
  EditDocument doc(canvasImage());EditorWindow window(doc,"Upload");window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
  QSignalSpy cancels(&window,&EditorWindow::cancelRequested);QVERIFY(!window.close());QCOMPARE(cancels.size(),1);QVERIFY(window.isVisible());
 }
};
QTEST_MAIN(EditorWindowTest)
#include "test_editor_window.moc"
