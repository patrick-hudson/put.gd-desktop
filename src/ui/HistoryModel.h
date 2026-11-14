#pragma once
#include "UiSession.h"
#include <QAbstractListModel>
#include <QCache>
#include <QDateTime>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QSet>
namespace putgd {
struct UiHistoryQuery { int limit=50,offset=0;QString search,category,mime,profileId,uploadStatus,localStatus,remoteStatus,from,to; };
class HistoryModel final:public QAbstractListModel {
 Q_OBJECT
public:
 enum Role { RecordRole=Qt::UserRole+1, SubtitleRole, DayRole, ThumbnailRole, ActiveRole, FailedRole, ProgressRole, PrimaryActionRole,
  // The History list's Destination and Created columns (2026-10-26).
  OutputRole, CreatedRole,
  // Day sections: SectionRole is the section a row belongs to ("In progress",
  // "Today", "Saturday, November 7 · continued"); DayRole is the same text on
  // the row that starts a section and empty on the others. DayCountRole is
  // that section's stored files within the query ("14 items"), empty while
  // unknown. CreatedTipRole is the full timestamp behind the time column.
  SectionRole, DayCountRole, CreatedTipRole };
 explicit HistoryModel(UiSession&,QObject* parent=nullptr);
 int rowCount(const QModelIndex& parent={})const override;
 QVariant data(const QModelIndex&,int role=Qt::DisplayRole)const override;
 void setQuery(const UiHistoryQuery&);void refresh();QJsonObject record(int)const;bool linkAvailable(int)const;
 bool busy()const{return queryBusy_;}
 qint64 total()const{return total_;}const UiHistoryQuery& query()const{return query_;}
 // Stored files the page holds (history.list records, not grouped rows).
 int filesReturned()const{return records_.size();}
 // Failed uploads: no day sections; the time column carries the date.
 bool daySections()const{return query_.uploadStatus!="failed";}
 // history.counts for the page shown: totals per category (all, images,
 // videos, text+other, failed) ignoring the search and filters, empty until
 // known; and the stored files of each day on the page within the query.
 QJsonObject categoryTotals()const{return categoryTotals_;}
 QJsonArray dayCounts()const;
 // One long date form for the details pane and tooltips; the time column's text.
 static QString longTimestamp(const QDateTime&);
 static QString timeColumn(const QDateTime&,bool withDate);
 void setShowCancelled(bool);bool showCancelled()const{return showCancelled_;}
 QJsonArray activeOperations()const;static int operationProgress(const QJsonObject&);
 void releaseThumbnails();
 // KB to hold a page's largest thumbnails, 32 bits a pixel once loaded. A view repaints
 // every visible tile in order, and an LRU cache smaller than that misses on all of them.
 static int thumbnailCacheCost(int rows){return qMax(8192,rows*(360*220*4/1024+1));}
 // Reads a preview again once it has been made (#190) at a path whose earlier read failed.
 void reloadThumbnail(const QString& path);
 // A video poster's length (#112): the PNG text key the helper writes, and m:ss.
 static constexpr const char* durationKey="putgd-duration-ms";
 static QString duration(qint64 milliseconds);
 static QString fileSize(qint64);static QString friendlyAction(const QString&);static QString statusText(const QJsonObject&);
 static QString remoteStateText(const QJsonObject&);
 static bool localCopyUnavailable(const QJsonObject&);
 // A failure that stored no file, such as an image too large to edit. There is
 // nothing to upload, save or copy again; only a new capture can follow.
 static bool storedNothing(const QJsonObject&);
 // Short failure label: what failed, not the error message.
 static QString failureLabel(const QJsonObject&);
 // What a row (a capture group) can do now. The inline action, the inspector,
 // the row menu and the selection strip all ask these, so none of them offers
 // a command the resident would refuse for this row's state and receipts.
 static bool failed(const QJsonObject&);static bool retryUpload(const QJsonObject&);
 // #1496: a row whose files are all uploaded and some of whose wanted links were not
 // confirmed or not made. Its label is "Finish share link", or "Finish share links"
 // when the link-only retry may make more than one; empty when it does not apply.
 static bool finishShareLink(const QJsonObject&);static QString finishShareLabel(const QJsonObject&);
 static bool retryLocalSave(const QJsonObject&);static bool copyLocalResult(const QJsonObject&);
 // One stored file whose original remote target delete.prepare can accept.
 static bool remoteDeletable(const QJsonObject& file);
signals:void activityChanged();void busyChanged(bool);void loaded();void error(const QString&);void countsChanged();
private:
 void rebuild();void loadThumbnail(const QString&)const;void nextThumbnail()const;bool scanDestinations()const;
 // One counts request per change of the page's files (never per progress update).
 void requestCounts();QString section(int row)const;QString destination(int row)const;
 // What the row offers and whether it failed: fixed until the next rebuild.
 struct RowFacts {bool active=false,failed=false;QString primary;bool operator==(const RowFacts&)const=default;};
 static RowFacts rowFacts(const QJsonObject&);
 QString dayLabel(int row)const;
 // Per row, kept with rows_: the parsed local creation time and facts that
 // every paint asks for many times (#1064).
 UiSession& history_;UiHistoryQuery query_;QList<QJsonObject> rows_;QList<QDateTime> createdAt_;QList<RowFacts> facts_;mutable QStringList dayLabels_;mutable QDate dayLabelsFor_;bool multipleDestinations_=false;QJsonArray records_,operations_;qint64 total_=0;quint64 request_=0;
 QJsonObject categoryTotals_;QHash<QDate,qint64> dayCounts_;QDate continued_;QString countsKey_;QElapsedTimer countsAge_;quint64 countsRequest_=0;
 bool queryBusy_=false,loadInFlight_=false,loadPending_=false,showCancelled_=false;
 mutable QCache<QString,QImage> thumbnails_{8192};mutable QStringList thumbnailQueue_;mutable QSet<QString> thumbnailRequested_;mutable bool thumbnailLoading_=false;mutable quint64 thumbnailGeneration_=0;qint64 timesMinute_=0;
};
}
