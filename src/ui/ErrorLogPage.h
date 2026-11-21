#pragma once
#include "UiSession.h"
#include <QCache>
#include <QJsonArray>
#include <QPointer>
#include <QSet>
#include <QWidget>
#include <functional>
class QCheckBox;class QComboBox;class QGridLayout;class QHBoxLayout;class QLabel;class QLineEdit;class QMessageBox;class QPlainTextEdit;class QPushButton;class QTimer;class QToolButton;class QTreeWidget;
namespace putgd {
// Settings → Error log (#1753): every error put.gd reported, newest first, from the
// resident's error log (errors.list). A row is the time, the kind of action, the item
// with its thumbnail, the destination, a one-line message and whether it was later
// resolved; selecting it shows the whole entry and its next steps as buttons that do
// them. ui.errorLog drives the same controls (command()).
class ErrorLogPage final:public QWidget {
 Q_OBJECT
public:
 explicit ErrorLogPage(UiSession&,QWidget* parent=nullptr);
 // From the first page with filters cleared: the entry {id}, or the newest entry about
 // {historyId} or {operationId}, of {kind} when given. An empty target shows the newest.
 void showEntry(const QJsonObject& target);
 void refresh();
 void command(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply);
 QJsonObject state()const;
 static QString kindLabel(const QString& kind);
 static QString stepLabel(const QString& step,const QString& kind);
signals:
 // Navigation the page asks its window for.
 void destinationRequested(const QString& profileId);
 void historyRequested(const QString& historyId);
 // The unresolved count changed, for the rail's badge.
 void unresolvedChanged(int count);
protected:
 void showEvent(QShowEvent*)override;
private:
 void load(std::function<void(Error)> done={});
 void populate();
 void select(const QString& id);
 void showDetails(const QJsonObject& entry);
 void runStep(const QString& step,std::function<void(Error,QJsonObject)> done={});
 void openClear(bool resolvedOnly);
 void finishClear(bool accepted,std::function<void(Error,QJsonObject)> done={});
 void loadThumbnail(const QString& path);
 void feedback(const QString& text,bool error);
 QString timeText(const QString& iso)const;
 UiSession& session_;
 QComboBox* kind_=nullptr;QLineEdit* search_=nullptr;QCheckBox* openOnly_=nullptr;QPushButton* clear_=nullptr;
 QTreeWidget* table_=nullptr;QLabel *empty_=nullptr,*pageText_=nullptr;QPushButton *newer_=nullptr,*older_=nullptr;
 QWidget* details_=nullptr;QLabel *detailTitle_=nullptr,*detailMessage_=nullptr,*resolution_=nullptr,*feedback_=nullptr;QGridLayout* facts_=nullptr;
 QHBoxLayout* steps_=nullptr;QPlainTextEdit* technical_=nullptr;QToolButton* technicalToggle_=nullptr;QPushButton* revealPackage_=nullptr;
 QTimer* searchDelay_=nullptr;QPointer<QMessageBox> confirm_;bool clearResolvedOnly_=false;
 QJsonArray entries_;QJsonObject selected_;QString wanted_,lastRevision_,packagePath_;
 int offset_=0,total_=0,unresolved_=0;bool loading_=false,reloadPending_=false,stepRunning_=false;
 static constexpr int pageSize=50;
 QCache<QString,QPixmap> thumbnails_{64};QSet<QString> thumbnailRequested_;
};
}
