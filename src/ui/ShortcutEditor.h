#pragma once
#include <QWidget>
#include <QHash>
#include <QJsonObject>
#include <QKeySequence>
#include <QSet>
#include "core/Error.h"
#include <functional>
class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QPushButton;
namespace putgd {
class ShortcutEditor final:public QWidget {
public:
 explicit ShortcutEditor(QWidget* parent=nullptr);
 ~ShortcutEditor() override;
 using RecordingControl=std::function<void(bool,std::function<void(Error)>)>;
 void setRecordingControl(RecordingControl control){recordingControl_=std::move(control);}
 void cancelRecording();
 void setActions(const QJsonObject&);
 QJsonObject actions(const QJsonObject&) const;
 void setStatus(const QJsonObject&);
 const QHash<QString,QLineEdit*>& editors() const{return editors_;}
protected:
 bool eventFilter(QObject*,QEvent*) override;
private:
 void startRecording(const QString& action);
 void finishRecording(const QString& message);
 QHash<QString,QLineEdit*> editors_;QHash<QString,QLabel*> bindingStates_;QLabel* status_;QPlainTextEdit* details_;
 QHash<QString,QPushButton*> recordButtons_;
 QLabel* recordingStatus_;
 RecordingControl recordingControl_;
 QString recordingAction_;
 QKeySequence chord_;
 QSet<quint64> pressed_;
 quint64 recordingGeneration_=0;
 bool recordingReady_=false;
};
}
