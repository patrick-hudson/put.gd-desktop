#pragma once
#include "app/OperationService.h"
#include <QWidget>
class QTreeWidget;class QLabel;
namespace putgd {
class RecentOperations final:public QWidget {
public:explicit RecentOperations(OperationService&,QWidget* parent=nullptr);void refresh();
private:OperationService& operations_;QTreeWidget* tree_;QLabel *status_,*empty_,*count_;quint64 request_=0;bool loadInFlight_=false,loadPending_=false;
};
}
