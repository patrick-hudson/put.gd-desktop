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
