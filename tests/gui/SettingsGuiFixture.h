#pragma once

#include "ui/MainWindow.h"
#include "core/BuildInfo.h"
#include "core/ReleaseLink.h"
#include "ui/Ui.h"
#include "ui/PreferenceRows.h"
#include "ui/Fonts.h"
#include "core/ByteSize.h"
#include <QAccessible>
#include <QToolTip>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QStatusBar>
#include <QSet>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QProgressBar>
#include <QAbstractItemView>
#include <QScrollBar>
#include <QScrollArea>
#include "ui/SettingsWindow.h"
#include "ui/FileDialog.h"
#include <QFileDialog>
#include "ui/ShortcutEditor.h"
#include "ui/ShortcutKeyboard.h"
#include "ui/ViewState.h"
#include "fixtures/UiSessionFixture.h"
#include "app/DiagnosticsBundle.h"
#include "core/SystemUpdates.h"
#include "core/Log.h"
#include "core/ArtifactStore.h"
#include <QJsonDocument>
#include <QDialog>
#include <QDir>
#include "storage/s3/S3Provider.h"
#include "storage/proton/ProtonProvider.h"
#include "storage/b2/B2Provider.h"
#include "storage/http/HttpImport.h"
#include "storage/http/HttpProvider.h"
#include "ui/CustomUploaderPanel.h"
#include <QSpinBox>
#include <QStyleOptionSpinBox>
#include <QMimeData>
#include <QDropEvent>
#include <QUrl>
#include <QFile>
#include <QFileInfo>
#include <QtTest>
#include <QCheckBox>
#include <QRadioButton>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QClipboard>
#include <QJsonArray>
#include <QLabel>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextLayout>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QToolButton>
#include <QStyleOptionToolButton>
#include <QGridLayout>
#include <QMessageBox>
#include <memory>
#include <QInputDialog>
#include <QMenu>
#include <QTabBar>
#include <QWheelEvent>
#include "platform/HostOpen.h"
#include <QScopeGuard>
#include <QAction>
#include <QWidgetAction>
#include <QActionGroup>
#include <QFormLayout>
#include "fixtures/Waits.h"
using namespace putgd;using namespace putgd::test;
namespace {
// Three S3 destinations: development-s3 is the capture default, clipboard is an exception to synthetic-third.
inline QJsonObject threeDestinations(){
 auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();
 for(const auto& [id,label]:QList<QPair<QString,QString>>{{"synthetic-alternate","Synthetic alternate"},{"synthetic-third","Synthetic third"}}){auto profile=profiles.first().toObject();profile["id"]=id;profile["label"]=label;profile["credentials"]=QJsonObject{{"mode","keychain"},{"reference","putgd/"+id}};profiles.append(profile);}
 doc["profiles"]=profiles;doc["captureDefaults"]=QJsonObject{{"destination","upload"},{"profileId","development-s3"},{"publicationMode","default"}};
 auto actions=doc["actions"].toObject();for(const auto& name:Settings::actionNames()){auto action=actions[name].toObject();action["destination"]=name=="clipboard"?"upload":"default";action["profileId"]=name=="clipboard"?QJsonValue("synthetic-third"):QJsonValue(QJsonValue::Null);actions[name]=action;}doc["actions"]=actions;return doc;
}
inline QStringList profileIds(const QJsonObject& document){QStringList ids;for(const auto& p:document["profiles"].toArray())ids.append(p.toObject()["id"].toString());return ids;}
}

class SettingsGuiFixture:public QObject {
 Q_OBJECT
};
