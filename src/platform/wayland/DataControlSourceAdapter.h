#pragma once
#include "app/OperationService.h"
#include "platform/kde/DataControlClipboard.h"
#include <QObject>
namespace putgd {
// A Wayland desktop with no native adapter (COSMIC, Budgie's labwc session). The
// clipboard goes through data-control when the compositor offers it, and fails
// with source_unsupported when it doesn't. Capture and shortcuts stay unavailable.
class DataControlSourceAdapter final:public QObject,public SourceAdapter {
 Q_OBJECT
public:
 explicit DataControlSourceAdapter(QObject* parent=nullptr);
 quint64 clipboardRevision()const override;
 std::optional<quint64> lastCopyRevision()const override;
 QJsonObject capabilities()const override;
 PreparedAcquisition prepareAcquisition(const QString&) override;
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>) override;
 void cancelAcquisition() override;
 void copy(const CopyPayload&,std::function<void(Error)>) override;
 void invalidateCopyEligibility() override;
signals:
 void capabilitiesChanged();
private:
 Error unavailable()const;
 DataControlClipboard clipboard_;
 // The compositor's first selection event moves the connection's revision. It
 // says nothing about the clipboard, so a cold start's upload, which may read
 // the revision first, must not see it as the user copying something.
 quint64 connectedRevision_=0;bool connected_=false;
};
}
