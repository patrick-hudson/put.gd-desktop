#pragma once
#include "HistoryModel.h"
#include "RetainedWindow.h"
#include <atomic>
#include <memory>
class QSplitter;class QScrollArea;class QStackedWidget;class QResizeEvent;class QListView;class QLabel;class QPushButton;class QPlainTextEdit;class QTimer;class QLineEdit;class QComboBox;class QProgressBar;class QToolButton;
namespace putgd {
class HistoryWindow final:public RetainedWindow {
 Q_OBJECT
public:HistoryWindow(UiSession&,QWidget* parent=nullptr);void refresh();QJsonObject saveViewState()const;bool restoreViewState(const QJsonObject&);void releasePreview();
signals:void viewStateChanged();
protected:void hideEvent(QHideEvent*) override;void showEvent(QShowEvent*) override;void resizeEvent(QResizeEvent*) override;
private:
 void startPreview();void updateSummary();void updateInspectorLayout();void setGridMode(bool);void applyFilters();void showRowMenu(const QModelIndex&,const QPoint&);void primaryAction(const QModelIndex&);void operationAction(const QString&);void updateActivity();void exportLocal(bool);
 void changeSharing(const QString&);void pollSharing();void selectionChanged();void loadSelectedItem();void prepareDelete();void pollDeletion();QStringList selectedIds()const;QJsonObject selectedRecord()const;QString destinationLabel(const QJsonObject&)const;
 UiSession& session_;HistoryModel* model_;QListView* table_;QSplitter* split_=nullptr;QScrollArea* inspectorScroll_=nullptr;QStackedWidget* list_;QLabel *status_,*count_,*preview_,*empty_,*title_,*outcome_,*cancelled_,*activeLabel_;QPlainTextEdit *details_;QLabel* summary_;QLineEdit* link_;QComboBox* items_;QProgressBar* progress_;QWidget* activeBanner_;QToolButton *grid_,*showCancelled_;QPushButton *copy_,*open_,*share_,*revoke_,*inspectLink_,*retry_,*cancel_,*copyImage_,*saveAs_;
 QStringList restoreSelection_;QString selectedItemId_,sharingId_,sharingFeedback_,deletionId_,activeId_;QJsonObject currentDetail_;bool restoringSelection_=false,deleteAccepting_=false,sharingFeedbackError_=false,sharingStopped_=false,sharingAccepting_=false,sharingPollActive_=false,deletionPollActive_=false,awaitingInitialization_=false,previewLoading_=false;
 QTimer *sharingTimer_=nullptr,*deletionTimer_=nullptr,*searchTimer_=nullptr;quint64 selectionRequest_=0;std::shared_ptr<std::atomic_bool> previewCancelled_;QString pendingPreviewPath_,pixelDimensions_;
};
}
