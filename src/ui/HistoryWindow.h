#pragma once
#include "HistoryModel.h"
#include "RetainedWindow.h"
#include <atomic>
#include <memory>
#include <functional>
#include <QPointer>
class QTextDocument;class QGridLayout;class QCheckBox;class QDialog;class QSplitter;class QListWidget;class QVBoxLayout;class QScrollArea;class QStackedWidget;class QResizeEvent;class QListView;class QLabel;class QPushButton;class QPlainTextEdit;class QTimer;class QLineEdit;class QComboBox;class QProgressBar;class QToolButton;
namespace putgd {
class FileDialog;class QrCodeWindow;namespace Ui { class ElidedLabel; }
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
 // failureFacts: a compact failure's first detail facts, shown between its label and time.
 static void metadataDocument(QTextDocument&,const QModelIndex&,int width,bool createdColumn,bool bounded=false,bool compact=false,bool separated=false,int failureFacts=0);
 // ui.history: drive the open page as its controls do. Actions: inspect,
 // retryLoading, copyDeletionDetails, clearFilters (scope "filters" or "all"), select (ids), clearSelection,
 // openDetails, closeDetails, expandPreview, closePreview, showSavedCopy and
 // copySavedPath, dismissSavedCopy, cancelAll, confirmCancelAll and keepUploading. The reply includes copyJob and
 // savedCopyVisible alongside the page state (historyState).
 void command(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply);
 QJsonObject historyState()const;
 // history.qr show (#1428): the resident refreshed the link and asks for its
 // window; params historyId, url. The reply is the window's state.
 void showQrCode(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply);
 // Why Copy text (a retained local PNG or JPEG, as Edit again) or Show QR code
 // (the link Copy link would copy) cannot apply to this record; empty when it can.
 static QString copyTextUnavailable(const QJsonObject& record);
 static QString qrUnavailable(const QJsonObject& record);
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
 QJsonObject copyJob_;bool savedCopyDismissed_=false;bool rowMenuOpen_=false;
 QString savedPath_;QJsonObject savedFolderResult_;QWidget* savedCopy_=nullptr;Ui::ElidedLabel* savedLabel_=nullptr;
 QGridLayout* actions_=nullptr;QWidget* browserAction_=nullptr;QLabel* previewStatus_=nullptr;QPushButton* retryPreview_=nullptr;QHash<QString,Error> previewErrors_;
 // #190: ask the resident for a selected image's or video's missing preview,
 // once the selection has settled, one request at a time.
 void requestPreview();bool previewMissing(const QJsonObject& detail)const;
 void updateSummary();void updateInspectorLayout();void setGridMode(bool);void applyFilters();void showRowMenu(const QModelIndex&,const QPoint&,bool overflow=false,bool collective=false);void primaryAction(const QModelIndex&);void operationAction(const QString&);void copyHistoryLink(const QJsonObject& record);void updateActivity();void updateActivityLayout();void exportLocal(bool);void editAgain(const QJsonObject& record);static bool editable(const QJsonObject& record);void saveDiagnostics(const QString& operationId,std::function<void(Error,QJsonObject)> done={});bool diagnosticsAvailable(int row)const;
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
 void updateHeading();bool previewRetry_=false;Ui::ElidedLabel* heading_=nullptr;
 UiSession& session_;HistoryModel* model_=nullptr;QListView*table_=nullptr;QSplitter* split_=nullptr;QScrollArea* inspectorScroll_=nullptr;QStackedWidget*list_=nullptr;QLabel *status_=nullptr,*count_=nullptr,*preview_=nullptr,*empty_=nullptr,*title_=nullptr,*outcome_=nullptr,*activeLabel_=nullptr;QPlainTextEdit *details_=nullptr;QWidget*summary_=nullptr;QLabel*capabilityHint_=nullptr;QPushButton*changeLocalCopies_=nullptr;QLineEdit*link_=nullptr;QLabel* linkExpiry_=nullptr;QLabel* browserFeedback_=nullptr;QWidget* previousLink_=nullptr;QLineEdit* previousUrl_=nullptr;QComboBox*items_=nullptr;QProgressBar*progress_=nullptr;QWidget* activeBanner_=nullptr;QToolButton *grid_=nullptr;QCheckBox*showCancelled_=nullptr;QPushButton *copy_=nullptr,*open_=nullptr,*share_=nullptr,*revoke_=nullptr,*inspectLink_=nullptr,*retry_=nullptr,*cancel_=nullptr,*copyImage_=nullptr,*saveAs_=nullptr,*editAgain_=nullptr,*saveDiagnostics_=nullptr;
 QString unlockLinkItemId_;
 // Copy text and Show QR code (#1428), beside Edit again and Copy link.
 QPushButton *copyText_=nullptr,*showQr_=nullptr;QPointer<QrCodeWindow> qrWindow_;QTimer* textPoll_=nullptr;QString textOperation_;
 void copyText(const QJsonObject& record);void requestQrCode(const QJsonObject& record);void pollText();
 QStringList restoreSelection_;QString selectedItemId_,sharingId_,sharingFeedback_,deletionId_,activeId_;QJsonObject currentDetail_;bool restoringSelection_=false,deleteAccepting_=false,sharingFeedbackError_=false,sharingStopped_=false,sharingAccepting_=false,sharingPollActive_=false,deletionPollActive_=false,awaitingInitialization_=false,previewLoading_=false;
 QTimer *sharingTimer_=nullptr,*deletionTimer_=nullptr,*searchTimer_=nullptr,*previewRequestTimer_=nullptr;QString previewRequestId_,previewCandidate_;QSet<QString> previewRefused_;quint64 selectionRequest_=0;std::shared_ptr<std::atomic_bool> previewCancelled_;QString pendingPreviewPath_,pixelDimensions_;
 // The item and preview file the inspector shows now, kept across reloads.
 QString shownItemId_,shownPreviewPath_;
 FileDialog* chooser_=nullptr;
 QToolButton *filtersToggle_=nullptr,*clearFilters_=nullptr,*detailsBack_=nullptr,*detailsClose_=nullptr;QWidget *selectionBar_=nullptr,*emptyState_=nullptr;QLabel *selectionCount_=nullptr,*selectionLinkHint_=nullptr,*emptyBody_=nullptr;QPushButton *copyLinks_=nullptr,*deleteSelected_=nullptr,*resetView_=nullptr,*retryLoading_=nullptr,*remove_=nullptr;bool compact_=false,detailsOpen_=false,detailsFromGallery_=false,restoreDetailsOpen_=false,restoreScroll_=false;int galleryScroll_=0,listScroll_=0;
 QListWidget* categories_=nullptr;QWidget *footerBar_=nullptr,*footerRow_=nullptr;QWidget* columns_=nullptr;bool restoringCategory_=false;
 // The page heading's count note (P1), the pager's position (P2) and the sidebar's totals (P9).
 void updateCountNote();void updatePager();void updateCategoryCounts();QString countNote()const;QJsonObject pageState()const;
 void goToPage(int page);int pageCount()const;int pagerTotal_=0;
 // Details (P6): wanted stays as chosen (and across sessions) in the wide list;
 // the gallery and compact window open the pane on request.
 bool detailsWanted_=true;void setDetailsWanted(bool);void toggleDetails();bool paneVisible()const;void closeDetails();
 QToolButton *detailsToggle_=nullptr,*selectionDetailsClose_=nullptr,*selectionClear_=nullptr;QLabel *selectionNote_=nullptr,*pagePosition_=nullptr;
 // Filters as chips (P8): each opens its own editor; the panel stays for every filter at once.
 QAction* allFilters_=nullptr;QWidget* filterChips_=nullptr;void updateFilterChips();std::function<void(const QString&,QWidget*)> editFilter_;
 struct FilterEntry {QString key,label,value;};QList<FilterEntry> filterEntries()const;void setFilter(const QString& key,const QString& value);
 // Whether the selection bar replaces the header's heading and tools (P7).
 bool selectionHeader()const;
 // Runs once the selected file's detail arrives (true) or fails (false).
 std::function<void(bool)> afterDetail_;std::function<void()> fitFilters_;
};
}
