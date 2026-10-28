#pragma once
#include "HistoryModel.h"
#include "RetainedWindow.h"
#include <atomic>
#include <memory>
#include <functional>
#include <QPointer>
class QTextDocument;class QGridLayout;class QCheckBox;class QDialog;class QSplitter;class QListWidget;class QVBoxLayout;class QScrollArea;class QStackedWidget;class QResizeEvent;class QListView;class QLabel;class QPushButton;class QPlainTextEdit;class QTimer;class QLineEdit;class QComboBox;class QProgressBar;class QToolButton;
namespace putgd {
class FileDialog;namespace Graphite { class ElidedLabel; }
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
 // The compact failure metadata line in row coordinates, shared with tests.
 static QRect failureTextArea(const QSize& row,bool gallery,bool dayHeading);
 // Row metadata, shared by the delegate and document-format regression tests.
 static void metadataDocument(QTextDocument&,const QModelIndex&,int width,bool createdColumn,bool bounded=false,bool compact=false,bool separated=false);
 // ui.history: drive the open page as its controls do. Actions: inspect,
 // retryLoading, copyDeletionDetails, clearFilters (scope "filters" or "all"), select (ids), clearSelection,
 // openDetails, closeDetails, expandPreview, closePreview, showSavedCopy and
 // copySavedPath, dismissSavedCopy, cancelAll, confirmCancelAll and keepUploading. The reply includes copyJob and
 // savedCopyVisible alongside the page state (historyState).
 void command(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply);
 QJsonObject historyState()const;
 void confirmUploads(const QStringList& ids);void showFailedUploads();
 // The remote-deletion confirmation for a delete.prepare reply: each exact
 // target by file name, then the full preparation under Technical details.
 // Cancel is the default and Escape choice; accepting is the named action.
 // names maps history IDs to file names; skipped counts selected files that
 // were not sent because they have no deletable remote target.
 static QDialog* deletionConfirmation(QWidget* parent,const QJsonObject& preparation,const QHash<QString,QString>& names,const QJsonArray& profiles,int skipped=0);
signals:void viewStateChanged();
 void captureSettingsRequested();void localCopySettingsRequested();
 // Edit & upload from another source: "clipboard", or "file" with a path.
 void editRequested(const QString& source,const QString& path);void chooseImageRequested();
protected:bool eventFilter(QObject*,QEvent*) override;void hideEvent(QHideEvent*) override;void showEvent(QShowEvent*) override;void resizeEvent(QResizeEvent*) override;
private:
 void startPreview();void previewFailure(const Error&);void updatePreviewState();void updateActions();
 bool expandPreview();void updateSavedCopy();void showSavedCopy();void copySavedPath();void dismissSavedCopy();
 QPointer<QDialog> expandedPreview_;QPushButton* expandPreview_=nullptr;
 QJsonObject copyJob_;bool savedCopyDismissed_=false;
 QString savedPath_;QJsonObject savedFolderResult_;QWidget* savedCopy_=nullptr;Graphite::ElidedLabel* savedLabel_=nullptr;
 QGridLayout* actions_=nullptr;QWidget* browserAction_=nullptr;QLabel* previewStatus_=nullptr;QPushButton* retryPreview_=nullptr;QHash<QString,Error> previewErrors_;
 // #190: ask the resident for a selected image's or video's missing preview,
 // once the selection has settled, one request at a time.
 void requestPreview();bool previewMissing(const QJsonObject& detail)const;
 void updateSummary();void updateInspectorLayout();void setGridMode(bool);void applyFilters();void showRowMenu(const QModelIndex&,const QPoint&,bool overflow=false,bool collective=false);void primaryAction(const QModelIndex&);void operationAction(const QString&);void updateActivity();void updateActivityLayout();void exportLocal(bool);void editAgain(const QJsonObject& record);static bool editable(const QJsonObject& record);void saveDiagnostics(const QString& operationId,std::function<void(Error,QJsonObject)> done={});bool diagnosticsAvailable(int row)const;
 void changeSharing(const QString&);void pollSharing();void selectionChanged();void loadSelectedItem();void prepareDelete();void prepareDelete(const QStringList& ids,int skipped);void pollDeletion();QStringList selectedIds()const;QJsonObject selectedRecord()const;QString destinationLabel(const QJsonObject&)const;
 // The stored files of the selected rows, and those delete.prepare can take.
 QList<QJsonObject> selectedFiles()const;QStringList deletableIds()const;QStringList selectedLinkIds()const;bool selectionHasLinks()const;void copySelectedLinks();
 // Filters · N, its description and Clear filters follow the applied query.
 void updateFilterSummary();QStringList activeFilters()const;void clearFilters(bool all);
 void updateMultiSummary();void prepareCancelAll();void confirmCancelAll();
 QJsonObject selectionSummaryState_;QPointer<QDialog> cancelAllConfirmation_;QStringList cancelAllIds_;QWidget *singleInspector_=nullptr,*selectionInspector_=nullptr,*selectionSummary_=nullptr;
 void updateSelectionBar();void updateFooterLayout();void openBrowser(const QString& url);
 // Below the width both panes need at this text size, the inspector opens
 // over the list on request (Enter, Show details) instead of squeezing it.
 bool compact()const{return compact_;}void setDetailsOpen(bool);void openSelectedDetails();
 void inspectSelectedDeletion();void deletionAction(const QString&);QJsonObject selectedDeletion_;QHash<QString,QString> deletionNames_;QLabel* deletionStatus_=nullptr;QPushButton *deletionRetry_=nullptr,*deletionCancel_=nullptr;
 void updateHeading();bool previewRetry_=false;Graphite::ElidedLabel* heading_=nullptr;
 UiSession& session_;HistoryModel* model_=nullptr;QListView* table_;QSplitter* split_=nullptr;QScrollArea* inspectorScroll_=nullptr;QStackedWidget* list_;QLabel *status_,*count_,*preview_,*empty_,*title_,*outcome_,*activeLabel_;QPlainTextEdit *details_;QWidget* summary_;QLabel* capabilityHint_;QPushButton* changeLocalCopies_;QLineEdit* link_;QLabel* linkExpiry_=nullptr;QLabel* browserFeedback_=nullptr;QWidget* previousLink_=nullptr;QLineEdit* previousUrl_=nullptr;QComboBox* items_;QProgressBar* progress_;QWidget* activeBanner_=nullptr;QToolButton *grid_;QCheckBox* showCancelled_;QPushButton *copy_,*open_,*share_,*revoke_,*inspectLink_,*retry_,*cancel_,*copyImage_,*saveAs_,*editAgain_,*saveDiagnostics_;
 QStringList restoreSelection_;QString selectedItemId_,sharingId_,sharingFeedback_,deletionId_,activeId_;QJsonObject currentDetail_;bool restoringSelection_=false,deleteAccepting_=false,sharingFeedbackError_=false,sharingStopped_=false,sharingAccepting_=false,sharingPollActive_=false,deletionPollActive_=false,awaitingInitialization_=false,previewLoading_=false;
 QTimer *sharingTimer_=nullptr,*deletionTimer_=nullptr,*searchTimer_=nullptr,*previewRequestTimer_=nullptr;QString previewRequestId_,previewCandidate_;QSet<QString> previewRefused_;quint64 selectionRequest_=0;std::shared_ptr<std::atomic_bool> previewCancelled_;QString pendingPreviewPath_,pixelDimensions_;
 // The item and preview file the inspector shows now, kept across reloads.
 QString shownItemId_,shownPreviewPath_;
 FileDialog* chooser_=nullptr;
 QToolButton *filtersToggle_=nullptr,*clearFilters_=nullptr,*detailsBack_=nullptr,*detailsClose_=nullptr;QWidget *selectionBar_=nullptr,*emptyState_=nullptr;QLabel *selectionCount_=nullptr,*selectionLinkHint_=nullptr,*emptyBody_=nullptr;QPushButton *copyLinks_=nullptr,*deleteSelected_=nullptr,*resetView_=nullptr,*retryLoading_=nullptr,*remove_=nullptr;bool compact_=false,detailsOpen_=false,detailsFromGallery_=false,restoreDetailsOpen_=false,restoreScroll_=false;int galleryScroll_=0,listScroll_=0;
 QListWidget* categories_=nullptr;QWidget *footerBar_=nullptr,*footerRow_=nullptr;QWidget* columns_=nullptr;bool restoringCategory_=false;
 // Runs once the selected file's detail arrives (true) or fails (false).
 std::function<void(bool)> afterDetail_;std::function<void()> fitFilters_;
};
}
