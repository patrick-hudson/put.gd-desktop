#pragma once
#include "app/HistoryService.h"
#include <QAbstractTableModel>
namespace putgd {
class HistoryModel final:public QAbstractTableModel {
 Q_OBJECT
public:explicit HistoryModel(HistoryService&,QObject* parent=nullptr);int rowCount(const QModelIndex& parent={})const override;int columnCount(const QModelIndex& parent={})const override;QVariant data(const QModelIndex&,int role=Qt::DisplayRole)const override;QVariant headerData(int,Qt::Orientation,int role)const override;void setQuery(const HistoryQuery&);void refresh();QJsonObject record(int)const;bool linkAvailable(int)const;qint64 total()const{return total_;}const HistoryQuery& query()const{return query_;}
signals:void loaded();void error(const QString&);
private:HistoryService& history_;HistoryQuery query_;QJsonArray rows_;qint64 total_=0;quint64 request_=0;bool loadInFlight_=false,loadPending_=false;
};
}
