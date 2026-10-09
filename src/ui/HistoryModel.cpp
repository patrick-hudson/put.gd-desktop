#include "HistoryModel.h"
#include "VisualStyle.h"
#include <QDateTime>
#include <QApplication>
#include <QStyle>
#include <QPointer>
namespace putgd {
HistoryModel::HistoryModel(UiSession& history,QObject* parent):QAbstractTableModel(parent),history_(history){}
int HistoryModel::rowCount(const QModelIndex& parent)const{return parent.isValid()?0:rows_.size();}int HistoryModel::columnCount(const QModelIndex& parent)const{return parent.isValid()?0:7;}
QJsonObject HistoryModel::record(int row)const{return row>=0&&row<rows_.size()?rows_[row].toObject():QJsonObject{};}
bool HistoryModel::linkAvailable(int row)const{return record(row)["linkAvailable"].toBool();}
QVariant HistoryModel::data(const QModelIndex& index,int role)const{if(!index.isValid())return {};auto r=record(index.row());if(role==Qt::DecorationRole&&index.column()==0)return VisualStyle::icon(r["category"].toString());if(role==Qt::ToolTipRole)return data(index,Qt::DisplayRole);if(role!=Qt::DisplayRole)return {};switch(index.column()){case 0:return r["name"].toString();case 1:{auto date=QDateTime::fromString(r["created"].toString(),Qt::ISODateWithMs);return date.isValid()?date.toLocalTime().toString("MMM d, yyyy · HH:mm"):r["created"].toString();}case 2:return r["mime"].toString();case 3:return r["profileId"].toString();case 4:return r["uploadStatus"].toString();case 5:return r["local"].toObject()["status"].toString();case 6:return r["remoteStatus"].toString();}return {};}
QVariant HistoryModel::headerData(int section,Qt::Orientation orientation,int role)const{if(orientation!=Qt::Horizontal||role!=Qt::DisplayRole)return {};return QStringList{"Name","Date","Content type","Original destination","Upload","Local copy","Remote deletion"}.value(section);}
void HistoryModel::setQuery(const UiHistoryQuery& query){query_=query;query_.limit=qBound(1,query.limit,200);query_.offset=qMax(0,query.offset);refresh();}
void HistoryModel::refresh(){const auto seq=++request_;if(loadInFlight_){loadPending_=true;return;}loadInFlight_=true;history_.request("history.list",{{"limit",query_.limit},{"offset",query_.offset},{"search",query_.search},{"category",query_.category},{"mime",query_.mime},{"profileId",query_.profileId},{"uploadStatus",query_.uploadStatus},{"localStatus",query_.localStatus},{"remoteStatus",query_.remoteStatus},{"from",query_.from},{"to",query_.to}},this,[g=QPointer<HistoryModel>(this),seq](Error e,QJsonObject data){if(!g)return;g->loadInFlight_=false;if(g->loadPending_){g->loadPending_=false;g->refresh();return;}if(g->request_!=seq)return;if(e){emit g->error(e.code+": "+e.message);return;}g->beginResetModel();g->rows_=data["records"].toArray();g->total_=data["total"].toInteger();g->endResetModel();emit g->loaded();});}
}
