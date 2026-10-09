#pragma once
#include "UiSession.h"
#include <QJsonArray>
#include <QAbstractTableModel>
namespace putgd {
struct UiHistoryQuery { int limit=50,offset=0;QString search,category,mime,profileId,uploadStatus,localStatus,remoteStatus,from,to; };
class HistoryModel final:public QAbstractTableModel {
 Q_OBJECT
public:explicit HistoryModel(UiSession&,QObject* parent=nullptr);int rowCount(const QModelIndex& parent={})const override;int columnCount(const QModelIndex& parent={})const override;QVariant data(const QModelIndex&,int role=Qt::DisplayRole)const override;QVariant headerData(int,Qt::Orientation,int role)const override;void setQuery(const UiHistoryQuery&);void refresh();QJsonObject record(int)const;bool linkAvailable(int)const;qint64 total()const{return total_;}const UiHistoryQuery& query()const{return query_;}
signals:void loaded();void error(const QString&);
private:UiSession& history_;UiHistoryQuery query_;QJsonArray rows_;qint64 total_=0;quint64 request_=0;bool loadInFlight_=false,loadPending_=false;
};
}
