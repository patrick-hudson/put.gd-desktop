#include "AboutWindow.h"
#include "VisualStyle.h"
#include "Graphite.h"
#include "platform/HostOpen.h"
#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonArray>
#include <QPushButton>
#include <QToolButton>
#include <QTextEdit>
#include <QGridLayout>
#include <QScrollArea>
#include <QScreen>
#include <QGuiApplication>
#include <QTextDocument>
#include <QTextOption>
#include <QResizeEvent>
#include <QClipboard>
#include <QAccessible>
#include <QTimer>
#include <QPointer>
#include <QtMath>

namespace putgd {
namespace {
// Selectable, wrap-anywhere values whose height follows the width the grid gives them.
class BuildValue final:public QTextEdit {
public:
 explicit BuildValue(const QString& value){setPlainText(value);setReadOnly(true);setFrameShape(QFrame::NoFrame);setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);setWordWrapMode(QTextOption::WrapAnywhere);document()->setDocumentMargin(0);QSizePolicy policy(QSizePolicy::Expanding,QSizePolicy::Preferred);policy.setHeightForWidth(true);setSizePolicy(policy);}
 bool hasHeightForWidth()const override{return true;}
 int heightForWidth(int width)const override{QTextDocument text;text.setDocumentMargin(0);text.setDefaultFont(font());text.setDefaultTextOption(document()->defaultTextOption());text.setPlainText(toPlainText());text.setTextWidth(qMax(1,width-2*frameWidth()));return qCeil(text.size().height())+2*frameWidth();}
 QSize sizeHint()const override{const int width=fontMetrics().horizontalAdvance(toPlainText())+2*frameWidth()+1;return {width,heightForWidth(width)};}
 QSize minimumSizeHint()const override{return {fontMetrics().averageCharWidth()*8,fontMetrics().height()};}
protected:
 void resizeEvent(QResizeEvent* event)override{QTextEdit::resizeEvent(event);if(event->size().width()!=event->oldSize().width())updateGeometry();}
};
// The facts scroll only when the screen can't hold them; otherwise they ask for their full height.
class BuildFacts final:public QScrollArea {
public:
 explicit BuildFacts(QWidget* card,QWidget* parent):QScrollArea(parent){setObjectName("aboutBuildScroll");setWidget(card);setWidgetResizable(true);setFrameShape(QFrame::NoFrame);setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);viewport()->setAutoFillBackground(false);QSizePolicy policy(QSizePolicy::Preferred,QSizePolicy::Preferred);policy.setHeightForWidth(true);setSizePolicy(policy);}
 bool hasHeightForWidth()const override{return true;}
 int heightForWidth(int width)const override{return qMin(maximumHeight(),widget()->layout()->totalHeightForWidth(width));}
 QSize sizeHint()const override{return {widget()->sizeHint().width(),heightForWidth(qMax(width(),widget()->minimumSizeHint().width()))};}
 QSize minimumSizeHint()const override{return {widget()->minimumSizeHint().width(),3*fontMetrics().height()};}
};
// Manifest keys are camelCase; show them as sentence-case captions.
QString caption(const QString& key){QString text;for(const auto c:key){if(c.isUpper()&&!text.isEmpty())text+=' ';text+=text.isEmpty()?c.toUpper():c.toLower();}return text;}
}
AboutWindow::AboutWindow(const QJsonObject& residentBuild,QWidget* parent):QDialog(parent),details_(QString::fromUtf8(QJsonDocument(residentBuild).toJson(QJsonDocument::Indented))){
 VisualStyle::apply(*qApp);setWindowTitle("About put.gd");setWindowIcon(VisualStyle::icon("brand"));setWindowModality(Qt::NonModal);const auto* display=screen()?screen():QGuiApplication::primaryScreen();setFixedWidth(display?qMin(Graphite::scaled(440),display->availableGeometry().width()-2*Graphite::space(Graphite::Space::xl)):Graphite::scaled(440));
 auto* root=new QVBoxLayout(this);const int margin=Graphite::space(Graphite::Space::xl);root->setContentsMargins(margin,margin,margin,margin);root->setSpacing(margin);
 auto* content=new QWidget(this);content->setObjectName("aboutContent");auto* row=new QHBoxLayout(content);row->setContentsMargins(0,0,0,0);row->setSpacing(Graphite::space(Graphite::Space::l));
 auto* icon=new QLabel(content);icon->setObjectName("qt_msgboxex_icon_label");const int side=Graphite::scaled(64);icon->setFixedSize(side,side);icon->setPixmap(VisualStyle::icon("brand").pixmap(side,side));row->addWidget(icon,0,Qt::AlignTop);
 auto* identity=new QWidget(content);identity->setObjectName("aboutIdentity");auto* column=new QVBoxLayout(identity);column->setContentsMargins(0,0,0,0);column->setSpacing(Graphite::space(Graphite::Space::s));row->addWidget(identity,1);
 auto* name=Graphite::label("put.gd","title",identity);name->setObjectName("qt_msgbox_label");column->addWidget(name);
 auto* info=Graphite::label("Screen capture and file sharing","secondary",identity);info->setObjectName("qt_msgbox_informativelabel");info->setWordWrap(true);column->addWidget(info);
 auto* version=Graphite::label(residentBuild["version"].toString().section('-',0,0).section('+',0,0),"binding",identity);version->setObjectName("aboutVersion");version->setToolTip(residentBuild["version"].toString());version->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);column->addWidget(version);
 auto* website=new QPushButton(VisualStyle::icon("open-external"),"put.gd website",identity);website->setObjectName("putgdWebsite");website->setAutoDefault(false);VisualStyle::role(website,"link");website->setProperty("graphiteFlush",true);column->addWidget(website,0,Qt::AlignLeft);
 auto* browser=new Graphite::BrowserFeedback(identity);browser->setObjectName("aboutWebsiteFeedback");column->addWidget(browser);connect(website,&QPushButton::clicked,this,[browser]{const QUrl url("https://put.gd");host::openLink(url,browser->opening(url));});
 auto* toggle=new QToolButton(identity);toggle->setText("Build details");toggle->setObjectName("buildDetails");Graphite::disclosure(toggle);column->addSpacing(Graphite::space(Graphite::Space::m)-column->spacing());column->addWidget(toggle,0,Qt::AlignLeft);
 // Expanded facts: a two-column key/value card on the text column, values wrapping anywhere.
 auto* details=new Graphite::Surface("surface",{},nullptr);details->setObjectName("aboutBuildDetails");details->setProperty("preferenceCard",true);auto* facts=new QGridLayout(details);const int inset=Graphite::space(Graphite::Space::m);facts->setContentsMargins(inset,inset,inset,inset);facts->setHorizontalSpacing(Graphite::space(Graphite::Space::m));facts->setVerticalSpacing(Graphite::space(Graphite::Space::s));facts->setColumnStretch(1,1);
 const auto add=[&](const QString& label,const QString& value,const QString& object){auto* key=Graphite::label(label,"meta",details);key->setObjectName(object+"Caption");const bool unknown=value.isEmpty()||value.compare("unknown",Qt::CaseInsensitive)==0;const bool hash=object=="buildCommit"||object=="build_gitTree";auto* text=new BuildValue(unknown?QString("Unknown"):hash?value.left(12):value);text->setToolTip(value);text->setAccessibleDescription(value);text->setParent(details);text->setObjectName(object);VisualStyle::role(text,!unknown&&hash?"mono":"secondary");text->viewport()->setAutoFillBackground(false);text->setAccessibleName(label);const int row=facts->rowCount();facts->addWidget(key,row,0,Qt::AlignLeft|Qt::AlignTop);facts->addWidget(text,row,1);};
 auto* fullVersion=new BuildValue(residentBuild["version"].toString());fullVersion->setParent(this);fullVersion->setObjectName("buildVersion");fullVersion->hide();add("Commit",residentBuild["gitCommit"].toString(residentBuild["commit"].toString("Unknown")),"buildCommit");add("Build state",residentBuild["dirty"].isBool()?(residentBuild["dirty"].toBool()?"Modified source tree":"Clean source tree"):"Unknown","buildState");for(auto it=residentBuild.begin();it!=residentBuild.end();++it){if(QStringList{"version","gitCommit","commit","dirty"}.contains(it.key()))continue;const auto value=it.value();const auto text=value.isObject()?QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)):value.isArray()?QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)):value.toVariant().toString();add(caption(it.key()),text,"build_"+it.key());}
 auto* scroll=new BuildFacts(details,identity);column->addWidget(scroll);scroll->hide();root->addWidget(content);
 auto* buttons=new QWidget(this);buttons->setObjectName("aboutActions");auto* actions=new QHBoxLayout(buttons);actions->setContentsMargins(0,0,0,0);actions->setSpacing(Graphite::space(Graphite::Space::s));
 auto* copy=new QPushButton("Copy build details",buttons);copy->setObjectName("copyBuildDetails");copy->setAutoDefault(false);VisualStyle::role(copy,"quiet");copy->setProperty("graphiteFlush",true);copy->hide();copy->setMinimumWidth(copy->sizeHint().width());actions->addWidget(copy);auto* copied=new QTimer(copy);copied->setSingleShot(true);copied->setInterval(2000);connect(copied,&QTimer::timeout,copy,[copy]{copy->setText("Copy build details");copy->setAccessibleName("Copy build details");QAccessibleEvent event(copy,QAccessible::NameChanged);QAccessible::updateAccessibility(&event);});connect(copy,&QPushButton::clicked,this,[this,copy,copied]{qApp->clipboard()->setText(details_);copy->setText("Copied");copy->setAccessibleName("Copied");QAccessibleEvent event(copy,QAccessible::NameChanged);QAccessible::updateAccessibility(&event);copied->start();});actions->addStretch();
 auto* close=new QPushButton("Close",buttons);close->setObjectName("closeAbout");VisualStyle::role(close,"lead");close->setDefault(true);actions->addWidget(close);connect(close,&QPushButton::clicked,this,&QDialog::accept);root->addWidget(buttons);
 connect(toggle,&QToolButton::toggled,this,[this,scroll,copy](bool expanded){scroll->setVisible(expanded);copy->setVisible(expanded);fitHeight();});
}
// Grow to the content, up to a readable maximum inside the screen; the facts scroll only beyond it.
int AboutWindow::maximumFitHeight()const{const auto* display=screen()?screen():QGuiApplication::primaryScreen();const int screenRoom=display?display->availableGeometry().height()-2*Graphite::space(Graphite::Space::xl):Graphite::scaled(600);return qMin(Graphite::scaled(600),screenRoom);}
void AboutWindow::fitHeight(){layout()->setSizeConstraint(QLayout::SetNoConstraint);setMaximumHeight(maximumFitHeight());auto* facts=findChild<QScrollArea*>("aboutBuildScroll");facts->setMaximumHeight(QWIDGETSIZE_MAX);facts->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);layout()->activate();const int natural=layout()->totalHeightForWidth(width()),limit=maximumFitHeight();if(natural>limit){facts->setMaximumHeight(qMax(facts->minimumSizeHint().height(),facts->heightForWidth(facts->width())-(natural-limit)));facts->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);layout()->invalidate();layout()->activate();}resize(width(),qMin(layout()->totalHeightForWidth(width()),limit));}
}
