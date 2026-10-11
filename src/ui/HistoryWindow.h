#pragma once
#include "HistoryModel.h"
#include "RetainedWindow.h"
#include <atomic>
#include <memory>
class QSplitter;class QScrollArea;class QStackedWidget;class QResizeEvent;class QTableView;class QLabel;class QPushButton;class QPlainTextEdit;class QTimer;
namespace putgd {
class HistoryWindow final:public RetainedWindow {
 Q_OBJECT
public:HistoryWindow(UiSession&,QWidget* parent=nullptr);void refresh();
 QJsonObject saveViewState()const;bool restoreViewState(const QJsonObject&);void releasePreview();
signals:void viewStateChanged();
protected:
 void hideEvent(QHideEvent*) override;
 void showEvent(QShowEvent*) override;
 void resizeEvent(QResizeEvent*) override;
private:
 std::shared_ptr<std::atomic_bool> previewCancelled_;
 QString pendingPreviewPath_;
 void startPreview();
 void updateInspectorLayout();
 void selectionChanged();void prepareDelete();void pollDeletion();QStringList selectedIds()const;
 UiSession& session_;QStringList restoreSelection_;bool restoringSelection_=false,deleteAccepting_=false;HistoryModel* model_;QTableView* table_;QSplitter* split_=nullptr;QScrollArea* inspectorScroll_=nullptr;QStackedWidget* list_;QLabel *status_,*count_,*preview_,*empty_;QPlainTextEdit *details_,*summary_;QPushButton *copy_,*open_;QString deletionId_;QTimer* deletionTimer_=nullptr;quint64 selectionRequest_=0;bool deletionPollActive_=false,awaitingInitialization_=false,previewLoading_=false;
};
}
