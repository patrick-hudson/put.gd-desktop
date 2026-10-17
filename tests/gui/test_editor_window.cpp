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
