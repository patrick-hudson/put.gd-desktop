#pragma once
#include "UiSession.h"
#include <QSet>
#include "RetainedWindow.h"
class QTreeWidget;class QStackedWidget;class QLabel;
namespace putgd {
class RecentOperations final:public RetainedWindow {
 Q_OBJECT
public:explicit RecentOperations(UiSession&,QWidget* parent=nullptr);void refresh();
 QJsonObject saveViewState()const;bool restoreViewState(const QJsonObject&);
signals:void viewStateChanged();
private:UiSession& operations_;QString selected_,selectedItem_;bool restoring_=false;QSet<QString> collapsed_;QTreeWidget* tree_;QStackedWidget* list_;QLabel *status_,*empty_,*count_;quint64 request_=0;bool loadInFlight_=false,loadPending_=false;
};
}
