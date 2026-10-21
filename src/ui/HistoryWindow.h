#pragma once
#include "HistoryModel.h"
#include "RetainedWindow.h"
#include <atomic>
#include <memory>
#include <functional>
class QSplitter;class QScrollArea;class QStackedWidget;class QResizeEvent;class QListView;class QLabel;class QPushButton;class QPlainTextEdit;class QTimer;class QLineEdit;class QComboBox;class QProgressBar;class QToolButton;
namespace putgd {
class FileDialog;
class HistoryWindow final:public RetainedWindow {
 Q_OBJECT
public:HistoryWindow(UiSession&,QWidget* parent=nullptr);void refresh();QJsonObject saveViewState()const;bool restoreViewState(const QJsonObject&);void releasePreview();
 // Selects the failed row for an operation (or a file's history ID) and clicks Save
 // diagnostics for this upload, answering with the saved package (#122).
 void saveDiagnosticsFor(const QString& id,std::function<void(Error,QJsonObject)> reply);
 // #259: Save as… through the desktop's chooser. ui.fileDialog opens it for the
 // selected file (or the row with this history or operation ID), reads its
 // state or cancels it; the reply is the chooser's state.
 void saveAsDialogCommand(const QString& action,const QString& id,std::function<void(Error,QJsonObject)> reply);
 FileDialog& saveAsDialog(){return *chooser_;}
signals:void viewStateChanged();
 // Edit & upload from another source: "clipboard", or "file" with a path.
 void editRequested(const QString& source,const QString& path);void chooseImageRequested();
protected:void hideEvent(QHideEvent*) override;void showEvent(QShowEvent*) override;void resizeEvent(QResizeEvent*) override;
private:
 void startPreview();
 // #190: ask the resident for a selected image's or video's missing preview,
 // once the selection has settled, one request at a time.
 void requestPreview();bool previewMissing(const QJsonObject& detail)const;
 void updateSummary();void updateInspectorLayout();void setGridMode(bool);void applyFilters();void showRowMenu(const QModelIndex&,const QPoint&);void primaryAction(const QModelIndex&);void operationAction(const QString&);void updateActivity();void exportLocal(bool);void editAgain(const QJsonObject& record);static bool editable(const QJsonObject& record);void saveDiagnostics(const QString& operationId,std::function<void(Error,QJsonObject)> done={});bool diagnosticsAvailable(int row)const;
 void changeSharing(const QString&);void pollSharing();void selectionChanged();void loadSelectedItem();void prepareDelete();void pollDeletion();QStringList selectedIds()const;QJsonObject selectedRecord()const;QString destinationLabel(const QJsonObject&)const;
 UiSession& session_;HistoryModel* model_;QListView* table_;QSplitter* split_=nullptr;QScrollArea* inspectorScroll_=nullptr;QStackedWidget* list_;QLabel *status_,*count_,*preview_,*empty_,*title_,*outcome_,*cancelled_,*activeLabel_;QPlainTextEdit *details_;QLabel* summary_;QLineEdit* link_;QLabel* linkExpiry_=nullptr;QComboBox* items_;QProgressBar* progress_;QWidget* activeBanner_;QToolButton *grid_,*showCancelled_;QPushButton *copy_,*open_,*share_,*revoke_,*inspectLink_,*retry_,*cancel_,*copyImage_,*saveAs_,*editAgain_,*saveDiagnostics_;
 QStringList restoreSelection_;QString selectedItemId_,sharingId_,sharingFeedback_,deletionId_,activeId_;QJsonObject currentDetail_;bool restoringSelection_=false,deleteAccepting_=false,sharingFeedbackError_=false,sharingStopped_=false,sharingAccepting_=false,sharingPollActive_=false,deletionPollActive_=false,awaitingInitialization_=false,previewLoading_=false;
 QTimer *sharingTimer_=nullptr,*deletionTimer_=nullptr,*searchTimer_=nullptr,*previewRequestTimer_=nullptr;QString previewRequestId_,previewCandidate_;QSet<QString> previewRefused_;quint64 selectionRequest_=0;std::shared_ptr<std::atomic_bool> previewCancelled_;QString pendingPreviewPath_,pixelDimensions_;
 // The item and preview file the inspector shows now, kept across reloads.
 QString shownItemId_,shownPreviewPath_;
 FileDialog* chooser_=nullptr;
 // Runs once the selected file's detail arrives (true) or fails (false).
 std::function<void(bool)> afterDetail_;
};
}
