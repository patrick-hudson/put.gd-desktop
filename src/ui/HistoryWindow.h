#pragma once
#include "HistoryModel.h"
#include <QWidget>
#include <atomic>
#include <memory>
class QTableView;class QLabel;class QPushButton;class QPlainTextEdit;class QTimer;
namespace putgd {
class HistoryWindow final:public QWidget {
public:HistoryWindow(HistoryService&,OperationService&,QWidget* parent=nullptr);void refresh();
protected:
 void hideEvent(QHideEvent*) override;
 void showEvent(QShowEvent*) override;
private:
 std::shared_ptr<std::atomic_bool> previewCancelled_;
 QString pendingPreviewPath_;
 void startPreview();
 void selectionChanged();void prepareDelete();void pollDeletion();QStringList selectedIds()const;
 HistoryService& history_;OperationService& operations_;HistoryModel* model_;QTableView* table_;QLabel *status_,*count_,*preview_,*empty_;QPlainTextEdit *details_,*summary_;QPushButton *copy_,*open_;QString deletionId_;QTimer* deletionTimer_=nullptr;quint64 selectionRequest_=0;bool deletionPollActive_=false,awaitingInitialization_=false,previewLoading_=false;
};
}
