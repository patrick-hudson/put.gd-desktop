#include "AboutWindow.h"
#include "VisualStyle.h"
#include "Graphite.h"
#include "platform/HostOpen.h"
#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonDocument>
#include <QPushButton>
#include <QTextEdit>
#include <QTextDocument>
#include <QTextOption>
#include <QResizeEvent>
#include <QClipboard>
#include <QTimer>
#include <QPointer>
#include <QtMath>

namespace putgd {
namespace {
class BuildValue final:public QTextEdit {
public:
 explicit BuildValue(const QString& value){setPlainText(value);setReadOnly(true);setFrameShape(QFrame::NoFrame);setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);setWordWrapMode(QTextOption::WrapAnywhere);document()->setDocumentMargin(0);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);}
 QSize sizeHint()const override{return {fontMetrics().horizontalAdvance(toPlainText()),fontMetrics().height()};}
 QSize minimumSizeHint()const override{return {0,fontMetrics().height()};}
protected:
 void resizeEvent(QResizeEvent* event)override{QTextEdit::resizeEvent(event);document()->setTextWidth(viewport()->width());const int height=qCeil(document()->size().height())+2*frameWidth();if(minimumHeight()!=height){setFixedHeight(height);updateGeometry();QTimer::singleShot(0,window(),[owner=QPointer<QWidget>(window())]{if(owner)owner->adjustSize();});}}
};
}
AboutWindow::AboutWindow(const QJsonObject& residentBuild,QWidget* parent):QDialog(parent),details_(QString::fromUtf8(QJsonDocument(residentBuild).toJson(QJsonDocument::Indented))){
 VisualStyle::apply(*qApp);setWindowTitle("About put.gd");setWindowIcon(VisualStyle::icon("brand"));setWindowModality(Qt::NonModal);setFixedWidth(Graphite::scaled(440));
 auto* root=new QVBoxLayout(this);const int margin=Graphite::space(Graphite::Space::xl);root->setContentsMargins(margin,margin,margin,margin);root->setSpacing(margin);
 auto* content=new QWidget(this);content->setObjectName("aboutContent");auto* row=new QHBoxLayout(content);row->setContentsMargins(0,0,0,0);row->setSpacing(Graphite::space(Graphite::Space::l));
 auto* icon=new QLabel(content);icon->setObjectName("qt_msgboxex_icon_label");const int side=Graphite::scaled(64);icon->setFixedSize(side,side);icon->setPixmap(VisualStyle::icon("brand").pixmap(side,side));row->addWidget(icon,0,Qt::AlignTop);
 auto* identity=new QWidget(content);identity->setObjectName("aboutIdentity");auto* column=new QVBoxLayout(identity);column->setContentsMargins(0,0,0,0);column->setSpacing(Graphite::space(Graphite::Space::s));row->addWidget(identity,1);
 auto* name=Graphite::label("put.gd","title",identity);name->setObjectName("qt_msgbox_label");column->addWidget(name);
 auto* info=Graphite::label("Screen capture and file sharing","secondary",identity);info->setObjectName("qt_msgbox_informativelabel");info->setWordWrap(true);column->addWidget(info);
 auto* version=Graphite::label(residentBuild["version"].toString().section('-',0,0).section('+',0,0),"binding",identity);version->setObjectName("aboutVersion");version->setToolTip(residentBuild["version"].toString());version->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);column->addWidget(version);
 auto* website=new QPushButton(VisualStyle::icon("open-external"),"put.gd website",identity);website->setObjectName("putgdWebsite");VisualStyle::role(website,"link");website->setProperty("graphiteFlush",true);column->addWidget(website,0,Qt::AlignLeft);
 auto* browser=new Graphite::BrowserFeedback(identity);browser->setObjectName("aboutWebsiteFeedback");column->addWidget(browser);connect(website,&QPushButton::clicked,this,[browser]{const QUrl url("https://put.gd");host::openLink(url,browser->opening(url));});
 auto* details=new Graphite::Surface("surface",identity);details->setObjectName("aboutBuildDetails");auto* facts=new QVBoxLayout(details);const int inset=Graphite::space(Graphite::Space::m);facts->setContentsMargins(inset,inset,inset,inset);facts->setSpacing(Graphite::space(Graphite::Space::s));
 const auto add=[&](const QString& label,const QString& value,const QString& object){auto* caption=Graphite::label(label,"meta",details);facts->addWidget(caption);auto* text=new BuildValue(value);text->setObjectName(object);VisualStyle::role(text,"mono");facts->addWidget(text);};
 add("Version",residentBuild["version"].toString(),"buildVersion");add("Commit",residentBuild["gitCommit"].toString(residentBuild["commit"].toString("Unknown")),"buildCommit");add("Build state",residentBuild["dirty"].isBool()?(residentBuild["dirty"].toBool()?"Modified source tree":"Clean source tree"):"Unknown","buildState");column->addWidget(details);details->hide();root->addWidget(content);
 auto* buttons=new QWidget(this);buttons->setObjectName("aboutActions");auto* actions=new QHBoxLayout(buttons);actions->setContentsMargins(0,0,0,0);actions->setSpacing(Graphite::space(Graphite::Space::s));
 auto* copy=new QPushButton("Copy build details",buttons);copy->setObjectName("copyBuildDetails");VisualStyle::role(copy,"quiet");copy->hide();actions->addWidget(copy);connect(copy,&QPushButton::clicked,this,[this]{qApp->clipboard()->setText(details_);});actions->addStretch();
 auto* toggle=new QPushButton("Build details",buttons);toggle->setObjectName("buildDetails");toggle->setCheckable(true);VisualStyle::role(toggle,"quiet");actions->addWidget(toggle);
 auto* close=new QPushButton("Close",buttons);close->setObjectName("closeAbout");VisualStyle::role(close,"lead");close->setDefault(true);actions->addWidget(close);connect(close,&QPushButton::clicked,this,&QDialog::accept);root->addWidget(buttons);
 connect(toggle,&QPushButton::toggled,this,[this,toggle,details,copy](bool expanded){details->setVisible(expanded);copy->setVisible(expanded);toggle->setText(expanded?"Hide build details":"Build details");layout()->activate();adjustSize();});
}
}
