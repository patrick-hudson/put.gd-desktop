#pragma once
#include "UiSession.h"
#include <QAbstractListModel>
#include <QCache>
#include <QImage>
#include <QJsonArray>
#include <QSet>
namespace putgd {
struct UiHistoryQuery { int limit=50,offset=0;QString search,category,mime,profileId,uploadStatus,localStatus,remoteStatus,from,to; };
class HistoryModel final:public QAbstractListModel {
 Q_OBJECT
public:
 enum Role { RecordRole=Qt::UserRole+1, SubtitleRole, DayRole, ThumbnailRole, ActiveRole, FailedRole, ProgressRole, PrimaryActionRole };
 explicit HistoryModel(UiSession&,QObject* parent=nullptr);
 int rowCount(const QModelIndex& parent={})const override;
 QVariant data(const QModelIndex&,int role=Qt::DisplayRole)const override;
 void setQuery(const UiHistoryQuery&);void refresh();QJsonObject record(int)const;bool linkAvailable(int)const;
 qint64 total()const{return total_;}const UiHistoryQuery& query()const{return query_;}
 void setShowCancelled(bool);bool showCancelled()const{return showCancelled_;}int hiddenCancelled()const{return hiddenCancelled_;}
 void releaseThumbnails();
 static QString fileSize(qint64);static QString friendlyAction(const QString&);static QString statusText(const QJsonObject&);
signals:void loaded();void error(const QString&);
private:
 void rebuild();void loadThumbnail(const QString&)const;void nextThumbnail()const;
 UiSession& history_;UiHistoryQuery query_;QList<QJsonObject> rows_;QJsonArray records_,operations_;qint64 total_=0;quint64 request_=0;
 bool loadInFlight_=false,loadPending_=false,showCancelled_=false;int hiddenCancelled_=0;
 mutable QCache<QString,QImage> thumbnails_{8192};mutable QStringList thumbnailQueue_;mutable QSet<QString> thumbnailRequested_;mutable bool thumbnailLoading_=false;mutable quint64 thumbnailGeneration_=0;
};
}
