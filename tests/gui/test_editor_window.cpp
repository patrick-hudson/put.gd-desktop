// The editor window never uploads without an explicit action, and uploads what the user cropped to.
#include "ui/EditorWindow.h"
#include "ui/FileDialog.h"
#include "ui/Theme.h"
#include "ui/BlurRadius.h"
#include "editor/EditorCommands.h"
#include "platform/HostOpen.h"
#include <QApplication>
#include <QPainter>
#include <QAccessible>
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#endif
#include <algorithm>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QFileDialog>
#include <QImageReader>
#include <QDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QToolButton>
#include <QSpinBox>
#include <QSplitter>
#include <QSlider>
#include <QProgressBar>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStyleOptionSlider>
#include <QStyleOptionComboBox>
#include <QTimer>
#include <QFontComboBox>
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
private slots:
 void keyboardContractNeverUploadsOnPlainEnter(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  QSignalSpy uploads(&window,&EditorWindow::uploadRequested);
  // Plain Enter is never an upload shortcut.
  for(auto* shortcut:window.findChildren<QShortcut*>())QVERIFY(shortcut->key()!=QKeySequence(Qt::Key_Return));
  window.activateWindow();window.canvas().setFocus();
  QTest::keyClick(&window.canvas(),Qt::Key_C);QCOMPARE(window.canvas().tool(),Tool::Crop);
  QTest::keyClick(&window.canvas(),Qt::Key_Return);QCOMPARE(uploads.size(),0);QCOMPARE(window.canvas().tool(),Tool::Select);
  QTest::keyClick(&window.canvas(),Qt::Key_T);QCOMPARE(window.canvas().tool(),Tool::Text);
  QTest::mouseClick(&window.canvas(),Qt::LeftButton,{},window.canvas().rect().center());
  auto* text=window.canvas().findChild<QLineEdit*>("annotationText");QVERIFY(text);QVERIFY(text->isVisible());
  QTest::keyClicks(text,"Ready");
  QTest::keyClick(text,Qt::Key_Return);QCOMPARE(uploads.size(),0);QCOMPARE(doc.shapes().size(),1);QCOMPARE(doc.shapes().first().text,QString("Ready"));
  QTest::keyClick(&window,Qt::Key_Return,Qt::ControlModifier);QCOMPARE(uploads.size(),1);
 }
 // Round 2 (U1): switching tools while a mark is being drawn abandons the drag, and its document gesture with it.
 // Left open, the gesture would hold every later edit (Quick redact's marks among them) out of the undo history.
 void switchingToolsDuringADragEndsTheDocumentGesture(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  QString code,message;window.viewCommand({{"action","tool"},{"tool","redact"}},code,message);QVERIFY2(code.isEmpty(),qPrintable(message));
  const auto center=window.canvas().rect().center();
  QTest::mousePress(&window.canvas(),Qt::LeftButton,{},center);QTest::mouseMove(&window.canvas(),center+QPoint(60,30));
  QVERIFY(doc.inGesture()); // the half-drawn mark is a gesture of the document
  window.viewCommand({{"action","tool"},{"tool","select"}},code,message);QVERIFY2(code.isEmpty(),qPrintable(message));
  QVERIFY(!doc.inGesture());QVERIFY(doc.shapes().isEmpty()); // abandoned, not kept
  QTest::mouseRelease(&window.canvas(),Qt::LeftButton,{},center+QPoint(60,30));
  QVERIFY(!doc.inGesture());QVERIFY(doc.shapes().isEmpty());
 }
 // Round 3 (U1): a middle-button press during a left-button drag turns it into a pan; the document gesture ends with the drag.
 void middleButtonPanDuringADragEndsTheDocumentGesture(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  QString code,message;window.viewCommand({{"action","tool"},{"tool","redact"}},code,message);QVERIFY2(code.isEmpty(),qPrintable(message));
  const auto center=window.canvas().rect().center();
  QTest::mousePress(&window.canvas(),Qt::LeftButton,{},center);QTest::mouseMove(&window.canvas(),center+QPoint(60,30));
  QVERIFY(doc.inGesture());
  QTest::mousePress(&window.canvas(),Qt::MiddleButton,{},center+QPoint(60,30));
  QVERIFY(!doc.inGesture());QVERIFY(doc.shapes().isEmpty());
  QTest::mouseRelease(&window.canvas(),Qt::MiddleButton,{},center+QPoint(60,30));
  QTest::mouseRelease(&window.canvas(),Qt::LeftButton,{},center+QPoint(60,30));
  QVERIFY(!doc.inGesture());
  // The document edits normally afterwards: one undo step per change.
  QVERIFY(doc.add(Shape{})>0);QVERIFY(doc.undo());
 }
 // Round 3 (U13): while a mark is being dragged, every change an agent asks for answers editor_busy, and the drag
 // still completes normally. Without it, abandoning the drag would roll back an edit the agent was told succeeded.
 void agentChangesDuringADragAnswerBusyAndTheDragCompletes(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  QString code,message;window.viewCommand({{"action","tool"},{"tool","redact"}},code,message);QVERIFY2(code.isEmpty(),qPrintable(message));
  const auto center=window.canvas().rect().center();
  QTest::mousePress(&window.canvas(),Qt::LeftButton,{},center);QTest::mouseMove(&window.canvas(),center+QPoint(60,30));
  QVERIFY(doc.inGesture());
  const auto refused=[&](const QJsonObject& command){QString c,m;const auto result=window.applyCommands(QJsonArray{command},c,m);return c==QString("editor_busy")&&result.isEmpty();};
  const QJsonObject rect{{"kind","redact"},{"rect",QJsonArray{5,5,20,20}}};
  QVERIFY(refused({{"op","add"},{"shape",rect}}));
  QVERIFY(refused({{"op","update"},{"shapeId",1},{"shape",QJsonObject{{"rect",QJsonArray{6,6,20,20}}}}}));
  QVERIFY(refused({{"op","remove"},{"shapeId",1}}));
  QVERIFY(refused({{"op","crop"},{"rect",QJsonArray{0,0,100,100}}}));
  QVERIFY(refused({{"op","resetCrop"}}));
  QVERIFY(refused({{"op","undo"}}));
  for(const auto* action:{"removeMark","removeSuggestions","place","geometry","applyCrop","blurStyle","stepNumber"}){
   code.clear();window.viewCommand({{"action",action}},code,message);QCOMPARE(code,QString("editor_busy"));
  }
  Error asked{"pending","",{}};window.quickRedact({"email"},[&](Error e,QJsonObject){asked=e;});QCOMPARE(asked.code,QString("editor_busy"));
  // Views that change nothing still answer.
  code.clear();window.viewCommand({{"action","zoom"},{"percent",150}},code,message);QVERIFY2(code.isEmpty(),qPrintable(code+": "+message));
  // The drag completes as if nothing had been asked: one new mark, and one undo removes it.
  QTest::mouseRelease(&window.canvas(),Qt::LeftButton,{},center+QPoint(60,30));
  QVERIFY(!doc.inGesture());QCOMPARE(doc.shapes().size(),1);
  QString c,m;QVERIFY(!window.applyCommands(QJsonArray{QJsonObject{{"op","add"},{"shape",rect}}},c,m).isEmpty());QCOMPARE(doc.shapes().size(),2);
  QVERIFY(doc.undo());QVERIFY(doc.undo());QVERIFY(doc.shapes().isEmpty());
 }
 // Round 4 (U13): an agent cannot change the selection in the middle of a drag. Selecting another shape while a pen
 // stroke was being drawn made the next pointer move read that shape's (empty) point list.
 void selectionViewsDuringAPenDragAnswerBusyAndTheStrokeCompletes(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  Shape box;box.kind=Shape::Kind::Rectangle;box.rect=QRect(5,5,40,30);const auto boxId=doc.add(box);
  QString code,message;window.viewCommand({{"action","tool"},{"tool","pen"}},code,message);QVERIFY2(code.isEmpty(),qPrintable(message));
  const auto start=window.canvas().rect().center();
  QTest::mousePress(&window.canvas(),Qt::LeftButton,{},start);QTest::mouseMove(&window.canvas(),start+QPoint(20,10));
  QVERIFY(doc.inGesture());
  const auto before=window.canvas().selected();QVERIFY(before!=boxId);
  for(const auto& params:{QJsonObject{{"action","select"},{"shapeId",double(boxId)}},QJsonObject{{"action","select"},{"shapeId",QJsonValue()}},QJsonObject{{"action","selectNext"}},QJsonObject{{"action","selectPrevious"}},QJsonObject{{"action","selectAffectedText"}}}){
   code.clear();window.viewCommand(params,code,message);QCOMPARE(code,QString("editor_busy"));
  }
  QCOMPARE(window.canvas().selected(),before); // nothing changed
  QTest::mouseMove(&window.canvas(),start+QPoint(40,25));QTest::mouseMove(&window.canvas(),start+QPoint(60,20));
  QTest::mouseRelease(&window.canvas(),Qt::LeftButton,{},start+QPoint(60,20));
  QVERIFY(!doc.inGesture());QCOMPARE(doc.shapes().size(),2);
  const auto& pen=doc.shapes().last();QCOMPARE(pen.kind,Shape::Kind::Pen);QVERIFY(pen.points.size()>=3);
  QVERIFY(doc.find(boxId)->points.isEmpty()); // the rectangle was never treated as the stroke
  QVERIFY(doc.undo());QCOMPARE(doc.shapes().size(),1); // one stroke, one undo step
 }
 // CodeRabbit 4613753548: keys that edit or change the selection wait for a pointer drag, so a nudge cannot join the drag's
 // undo step and be rolled back with it when the drag is abandoned.
 void keysDuringAPointerDragWaitForIt(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  Shape box;box.kind=Shape::Kind::Rectangle;box.rect=QRect(5,5,40,30);const auto boxId=doc.add(box);
  QString code,message;window.viewCommand({{"action","tool"},{"tool","arrow"}},code,message);QVERIFY2(code.isEmpty(),qPrintable(message));
  window.canvas().setFocus();
  const auto start=window.canvas().rect().center();
  QTest::mousePress(&window.canvas(),Qt::LeftButton,{},start);QTest::mouseMove(&window.canvas(),start+QPoint(60,30));
  QVERIFY(doc.inGesture());const auto drawing=window.canvas().selected();
  QTest::keyClick(&window.canvas(),Qt::Key_BracketLeft);QTest::keyClick(&window.canvas(),Qt::Key_Right);QTest::keyClick(&window.canvas(),Qt::Key_Delete);
  QCOMPARE(window.canvas().selected(),drawing);QCOMPARE(doc.find(boxId)->rect,QRect(5,5,40,30)); // not selected, not moved, not deleted
  // A middle-button pan then drops only the unfinished arrow.
  QTest::mousePress(&window.canvas(),Qt::MiddleButton,{},start+QPoint(60,30));
  QVERIFY(!doc.inGesture());QCOMPARE(doc.shapes().size(),1);QCOMPARE(doc.shapes().first().id,boxId);QCOMPARE(doc.shapes().first().rect,QRect(5,5,40,30));
  QTest::mouseRelease(&window.canvas(),Qt::MiddleButton,{},start+QPoint(60,30));QTest::mouseRelease(&window.canvas(),Qt::LeftButton,{},start+QPoint(60,30));
  // Keys work again once nothing is being dragged.
  window.canvas().select(boxId);QTest::keyClick(&window.canvas(),Qt::Key_Right);QCOMPARE(doc.find(boxId)->rect,QRect(6,5,40,30));
 }
 // A crop drawn but not yet applied is applied before Upload exports the image.
 void uploadFinishesThePendingCrop(){
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  QString code,message;window.viewCommand({{"action","tool"},{"tool","crop"}},code,message);
  window.viewCommand({{"action","geometry"},{"rect",QJsonArray{25,35,30,20}}},code,message);QVERIFY2(code.isEmpty(),qPrintable(message));
  QVERIFY(window.canvas().cropPending());QCOMPARE(doc.crop(),doc.imageRect());
  QSignalSpy uploads(&window,&EditorWindow::uploadRequested);window.findChild<QPushButton*>("editorUpload")->click();
  QCOMPARE(uploads.size(),1);QCOMPARE(doc.crop(),QRect(25,35,30,20));
 }
 // Save Copy, from the window or an agent, writes the crop that was drawn but not applied.
 void saveCopyFinishesThePendingCropLikeUpload(){
  qputenv("PUTGD_FILE_DIALOG","fallback");auto restore=qScopeGuard([]{qunsetenv("PUTGD_FILE_DIALOG");});
  EditDocument doc(canvasImage());EditorWindow window(doc);window.resize(1000,700);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  QString code,message;window.viewCommand({{"action","tool"},{"tool","crop"}},code,message);
  window.viewCommand({{"action","geometry"},{"rect",QJsonArray{20,30,200,100}}},code,message);QVERIFY2(code.isEmpty(),qPrintable(message));
  QVERIFY(window.canvas().cropPending());QVERIFY(window.saveCopy());
  QTemporaryDir dir;const auto path=dir.filePath("copy.png");
  QFileDialog* chooser=nullptr;PUTGD_TRY_VERIFY((chooser=window.fileDialog().fallbackDialog())&&chooser->isVisible());
  chooser->selectFile(path);QMetaObject::invokeMethod(chooser,"accept");
  PUTGD_TRY_VERIFY(QFileInfo::exists(path));PUTGD_TRY_VERIFY(!window.savingCopy());QCOMPARE(QImageReader(path).size(),QSize(200,100));
  window.viewCommand({{"action","tool"},{"tool","crop"}},code,message);window.viewCommand({{"action","geometry"},{"rect",QJsonArray{20,30,50,40}}},code,message);
  QString reason;const auto png=writeCopy(window,dir.filePath("second.png"),reason);QVERIFY2(!png.isEmpty(),qPrintable(reason));
  QCOMPARE(QImage::fromData(png,"PNG").size(),QSize(50,40));
 }
};
QTEST_MAIN(EditorWindowTest)
#include "test_editor_window.moc"
