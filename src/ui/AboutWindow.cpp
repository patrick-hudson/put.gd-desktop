#include "AboutWindow.h"
#include "Theme.h"
#include "Ui.h"
#include "Fonts.h"
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
#include <QMimeData>
#include <QVector>
#include <QTextCursor>
#include <QAccessible>
#include <QTimer>
#include <QPointer>
#include <QtMath>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWindow>

namespace putgd {
namespace {
// Selectable facts keep original copy text while their display follows the grid width.
class BuildValue final:public QTextEdit {
public:
 explicit BuildValue(const QString& value,bool package=false):original_(value){QString display;if(package){sourceOffsets_.append(0);const auto append=[&](QChar c,int source){display+=c;sourceOffsets_.append(source);};for(int i=0;i<value.size();++i){const auto c=value[i];if(c=='+')append(QChar(0x200b),i);append(c,i+1);if(c=='~')append(QChar(0x200b),i+1);else if(i+1<value.size()&&value[i+1]!='+')append(QChar(0x2060),i+1);}}else display=value;setPlainText(display);setReadOnly(true);setFrameShape(QFrame::NoFrame);setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);setWordWrapMode(package?QTextOption::WordWrap:QTextOption::WrapAnywhere);document()->setDocumentMargin(0);QSizePolicy policy(QSizePolicy::Expanding,QSizePolicy::Preferred);policy.setHeightForWidth(true);setSizePolicy(policy);}
 bool hasHeightForWidth()const override{return true;}
 int heightForWidth(int width)const override{QTextDocument text;text.setDocumentMargin(0);text.setDefaultFont(font());text.setDefaultTextOption(document()->defaultTextOption());text.setPlainText(toPlainText());text.setTextWidth(qMax(1,width-2*frameWidth()));return qCeil(text.size().height())+2*frameWidth();}
 QSize sizeHint()const override{const int width=fontMetrics().horizontalAdvance(toPlainText())+2*frameWidth()+1;return {width,heightForWidth(width)};}
 QSize minimumSizeHint()const override{return {fontMetrics().averageCharWidth()*8,fontMetrics().height()};}
protected:
 QMimeData* createMimeDataFromSelection()const override{if(sourceOffsets_.isEmpty())return QTextEdit::createMimeDataFromSelection();const auto cursor=textCursor();const int start=sourceOffsets_.value(cursor.selectionStart()),end=sourceOffsets_.value(cursor.selectionEnd());auto* data=new QMimeData;data->setText(original_.mid(start,end-start));return data;}
 void resizeEvent(QResizeEvent* event)override{QTextEdit::resizeEvent(event);if(event->size().width()!=event->oldSize().width())updateGeometry();}
private:QString original_;QVector<int> sourceOffsets_;
};
// The facts scroll only when the screen can't hold them; otherwise they ask for their full height.
class BuildFacts final:public QScrollArea {
public:
 explicit BuildFacts(QWidget* card,QWidget* parent):QScrollArea(parent){setObjectName("aboutBuildScroll");setWidget(card);setWidgetResizable(true);setFrameShape(QFrame::NoFrame);setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);viewport()->setAutoFillBackground(false);QSizePolicy policy(QSizePolicy::Preferred,QSizePolicy::Preferred);policy.setHeightForWidth(true);setSizePolicy(policy);}
 bool hasHeightForWidth()const override{return true;}
 int heightForWidth(int width)const override{return qMin(maximumHeight(),qMax(widget()->layout()->totalHeightForWidth(width),widget()->minimumSizeHint().height()));}
 QSize sizeHint()const override{return {widget()->sizeHint().width(),heightForWidth(qMax(width(),widget()->minimumSizeHint().width()))};}
 QSize minimumSizeHint()const override{return {widget()->minimumSizeHint().width(),3*fontMetrics().height()};}
};
// The top of the window is the moment the app exists for: a dimmed desktop with
// one region selected, and put.gd's identity in the lit selection. Drawn from the
// theme tokens, so it follows the scheme; it says nothing a screen reader needs.
class AboutScene final:public QWidget {
public:
 explicit AboutScene(QWidget* parent):QWidget(parent){setObjectName("aboutScene");connect(&Theme::Notifier::instance(),&Theme::Notifier::changed,this,qOverload<>(&QWidget::update));}
 void select(QWidget* selection){selection_=selection;}
 static int panelHeight(){return Ui::scaled(18);}
protected:
 void paintEvent(QPaintEvent*)override{
  QPainter p(this);p.setRenderHint(QPainter::Antialiasing);const auto& c=Theme::current();const qreal u=Ui::scaled(1000)/1000.0;
  p.fillRect(rect(),c.well);if(!selection_)return;
  const QRectF lit=selection_->geometry();const qreal floor=height()-panelHeight();
  p.save();p.setClipRect(QRectF(0,0,width(),floor));
  drawWindow(p,QRectF(lit.left()-74*u,lit.top()+12*u,136*u,floor-lit.top()),false);
  drawWindow(p,QRectF(lit.right()-34*u,lit.top()-6*u,150*u,floor-lit.top()+24*u),true);
  p.restore();
  // The desktop's panel: a launcher and two tasks, then the tray with put.gd lit in it.
  p.fillRect(QRectF(0,floor,width(),panelHeight()),c.surface);p.fillRect(QRectF(0,floor,width(),1),c.surfaceRaised);p.setPen(Qt::NoPen);
  const qreal pip=6*u,mid=floor+panelHeight()/2.0;
  p.setBrush(c.surfaceRaised);p.drawRoundedRect(QRectF(12*u,mid-pip/2,pip,pip),1.5*u,1.5*u);for(const qreal x:{26.0,62.0})p.drawRoundedRect(QRectF(x*u,mid-pip/2,30*u,pip),1.5*u,1.5*u);
  p.drawRoundedRect(QRectF(width()-40*u,mid-pip/2,28*u,pip),1.5*u,1.5*u);p.drawEllipse(QRectF(width()-54*u,mid-pip/2,pip,pip));p.setBrush(c.accent);p.drawEllipse(QRectF(width()-66*u,mid-pip/2,pip,pip));
  // The selection: lit, ringed, with the region picker's four grab corners.
  p.setRenderHint(QPainter::Antialiasing,false);p.fillRect(lit,c.bg);
  const qreal ring=qMax<qreal>(1,qRound(u)),grip=qRound(3*u),arm=qRound(16*u);const QRectF box=lit.adjusted(-ring,-ring,ring,ring);
  p.fillRect(QRectF(box.left(),box.top(),box.width(),ring),c.accent);p.fillRect(QRectF(box.left(),box.bottom()-ring,box.width(),ring),c.accent);p.fillRect(QRectF(box.left(),box.top(),ring,box.height()),c.accent);p.fillRect(QRectF(box.right()-ring,box.top(),ring,box.height()),c.accent);
  for(const qreal x:{box.left()-grip+ring,box.right()-ring})for(const qreal y:{box.top()-grip+ring,box.bottom()-ring}){const bool left=x<box.center().x(),top=y<box.center().y();p.fillRect(QRectF(left?x:x+grip-arm,y,arm,grip),c.accent);p.fillRect(QRectF(x,top?y:y+grip-arm,grip,arm),c.accent);}
 }
 void mousePressEvent(QMouseEvent* event)override{if(event->button()==Qt::LeftButton&&Ui::WindowHeader::customFrame()&&window()->windowHandle()){window()->windowHandle()->startSystemMove();event->accept();return;}QWidget::mousePressEvent(event);}
private:
 // A window on the dimmed desktop: a title strip and quiet lines of content, or a picture.
 static void drawWindow(QPainter& p,const QRectF& frame,bool picture){
  const auto& c=Theme::current();const qreal u=Ui::scaled(1000)/1000.0,strip=14*u;p.save();p.setPen(Qt::NoPen);QPainterPath shape;shape.addRoundedRect(frame,5*u,5*u);p.setClipPath(shape,Qt::IntersectClip);
  p.fillRect(frame,c.surface);p.fillRect(QRectF(frame.left(),frame.top(),frame.width(),strip),c.surfaceRaised);p.setBrush(c.textDisabled);
  for(int i=1;i<=2;++i)p.drawEllipse(QRectF(frame.right()-i*10*u,frame.top()+5*u,4*u,4*u));
  const QRectF inner=frame.adjusted(9*u,strip+9*u,-9*u,-9*u);p.setBrush(c.surfaceRaised);
  if(picture){p.setBrush(Ui::mix(c.surfaceRaised,c.accent,0.1));p.drawRoundedRect(inner,3*u,3*u);p.setClipRect(inner,Qt::IntersectClip);p.setBrush(Ui::mix(c.surfaceRaised,c.accent,0.42));p.drawEllipse(QPointF(inner.left()+72*u,inner.top()+26*u),9*u,9*u);QPainterPath hills;hills.moveTo(inner.left(),inner.bottom());hills.lineTo(inner.left()+40*u,inner.top()+52*u);hills.lineTo(inner.left()+70*u,inner.top()+72*u);hills.lineTo(inner.left()+100*u,inner.top()+44*u);hills.lineTo(inner.right(),inner.top()+66*u);hills.lineTo(inner.right(),inner.bottom());hills.closeSubpath();p.setBrush(Ui::mix(c.surfaceRaised,c.accent,0.22));p.drawPath(hills);}
  else for(int i=0;i<5;++i)p.drawRoundedRect(QRectF(inner.left(),inner.top()+i*11*u,inner.width()*(i%3==2?0.62:0.86),5*u),2*u,2*u);
  p.restore();
 }
 QPointer<QWidget> selection_;
};
// Manifest keys are camelCase; show them as sentence-case captions.
QString caption(const QString& key){QString text;for(const auto c:key){if(c.isUpper()&&!text.isEmpty())text+=' ';text+=text.isEmpty()?c.toUpper():c.toLower();}return text;}
}
AboutWindow::AboutWindow(const QJsonObject& residentBuild,QWidget* parent):QDialog(parent),details_(QString::fromUtf8(QJsonDocument(residentBuild).toJson(QJsonDocument::Indented))){
 Fonts::load();
 Ui::apply(*qApp);setWindowTitle("About put.gd");setWindowIcon(Ui::icon("brand"));setWindowModality(Qt::NonModal);const auto* display=screen()?screen():QGuiApplication::primaryScreen();setFixedWidth(display?qMin(Ui::scaled(480),display->availableGeometry().width()-2*Ui::space(Ui::Space::xl)):Ui::scaled(480));
 // Without the desktop's frame the window draws its own edge, so it stands off a dark window behind it.
 auto* root=new QVBoxLayout(this);const int edge=Ui::WindowHeader::customFrame()?1:0;root->setContentsMargins(edge,edge,edge,edge);root->setSpacing(0);
 auto* scene=new AboutScene(this);auto* sceneColumn=new QVBoxLayout(scene);sceneColumn->setContentsMargins(0,0,0,0);sceneColumn->setSpacing(0);root->addWidget(scene);
 auto* header=Ui::WindowHeader::install(this,sceneColumn,nullptr,Ui::WindowHeader::Density::Compact);header->setProperty("uiChromePlane",true);header->setCloseOnly();header->findChild<Ui::ElidedLabel*>("brandName")->setText("About put.gd");
 // The selection sits in the desktop with room to see what it was drawn over.
 auto* placed=new QHBoxLayout;const int gutter=qMax(Ui::space(Ui::Space::l),(width()-Ui::scaled(368))/2),above=Ui::space(Ui::Space::l);placed->setContentsMargins(gutter,above,gutter,above+Ui::space(Ui::Space::xs)+AboutScene::panelHeight());sceneColumn->addLayout(placed);
 auto* content=new QWidget(scene);content->setObjectName("aboutContent");scene->select(content);auto* row=new QHBoxLayout(content);const int inset=Ui::space(Ui::Space::l);row->setContentsMargins(inset,inset,inset,inset);row->setSpacing(inset);placed->addWidget(content);
 auto* icon=new QLabel(content);icon->setObjectName("qt_msgboxex_icon_label");const int side=Ui::scaled(72);icon->setFixedSize(side,side);icon->setPixmap(Ui::icon("brand").pixmap(side,side));row->addWidget(icon,0,Qt::AlignVCenter);
 auto* identity=new QWidget(content);identity->setObjectName("aboutIdentity");auto* column=new QVBoxLayout(identity);column->setContentsMargins(0,0,0,0);column->setSpacing(Ui::space(Ui::Space::xs));row->addWidget(identity,1,Qt::AlignVCenter);
 auto* name=Ui::label("put.gd","wordmark",identity);name->setObjectName("qt_msgbox_label");name->setMinimumHeight(name->fontMetrics().height());column->addWidget(name);
 auto* info=Ui::label("Screen capture and file sharing","secondary",identity);info->setObjectName("qt_msgbox_informativelabel");info->setWordWrap(true);info->setMinimumHeight(info->fontMetrics().height());column->addWidget(info);
 auto* version=Ui::label(residentBuild["version"].toString().section('-',0,0).section('+',0,0),"binding",identity);version->setObjectName("aboutVersion");version->setToolTip(residentBuild["version"].toString());version->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);version->setMinimumHeight(version->fontMetrics().height());column->addWidget(version);
 auto* body=new QWidget(this);body->setObjectName("aboutBody");auto* bodyColumn=new QVBoxLayout(body);const int margin=Ui::space(Ui::Space::xl);bodyColumn->setContentsMargins(margin,Ui::space(Ui::Space::m),margin,margin);bodyColumn->setSpacing(Ui::space(Ui::Space::s));root->addWidget(body);
 // The disclosure and its copy action share a line above the facts they belong to.
 auto* detailsRow=new QWidget(body);detailsRow->setObjectName("aboutDetailsRow");auto* detailsLine=new QHBoxLayout(detailsRow);detailsLine->setContentsMargins(0,0,0,0);detailsLine->setSpacing(Ui::space(Ui::Space::s));bodyColumn->addWidget(detailsRow);
 auto* toggle=new QToolButton(detailsRow);toggle->setText("Build details");toggle->setObjectName("buildDetails");Ui::disclosure(toggle);detailsLine->addWidget(toggle,0,Qt::AlignLeft);detailsLine->addStretch();
 auto* copy=new QPushButton("Copy build details",detailsRow);copy->setObjectName("copyBuildDetails");copy->setAutoDefault(false);Ui::role(copy,"textAction");copy->setProperty("uiFlush",true);copy->hide();copy->setMinimumWidth(copy->sizeHint().width());detailsLine->addWidget(copy);auto* copied=new QTimer(copy);copied->setSingleShot(true);copied->setInterval(2000);connect(copied,&QTimer::timeout,copy,[copy]{copy->setText("Copy build details");copy->setAccessibleName("Copy build details");QAccessibleEvent event(copy,QAccessible::NameChanged);QAccessible::updateAccessibility(&event);});connect(copy,&QPushButton::clicked,this,[this,copy,copied]{qApp->clipboard()->setText(details_);copy->setText("Copied");copy->setAccessibleName("Copied");QAccessibleEvent event(copy,QAccessible::NameChanged);QAccessible::updateAccessibility(&event);copied->start();});
 // Expanded facts: a two-column key/value card, values wrapping anywhere.
 auto* details=new Ui::Surface("surface",{},nullptr);details->setObjectName("aboutBuildDetails");details->setProperty("preferenceCard",true);auto* facts=new QGridLayout(details);const int factInset=Ui::space(Ui::Space::m);facts->setContentsMargins(factInset,factInset,factInset,factInset);facts->setHorizontalSpacing(Ui::space(Ui::Space::m));facts->setVerticalSpacing(Ui::space(Ui::Space::s));facts->setColumnStretch(1,1);
 const auto add=[&](const QString& label,const QString& value,const QString& object){auto* key=Ui::label(label,"meta",details);key->setObjectName(object+"Caption");const bool unknown=value.isEmpty()||value.compare("unknown",Qt::CaseInsensitive)==0;const bool hash=object=="buildCommit"||object=="build_gitTree";auto* text=new BuildValue(unknown?QString("Unknown"):hash?value.left(12):value,object=="build_packageVersion");text->setToolTip(value);text->setAccessibleDescription(value);text->setParent(details);text->setObjectName(object);Ui::role(text,!unknown&&hash?"mono":"secondary");text->viewport()->setAutoFillBackground(false);text->setAccessibleName(label);const int row=facts->rowCount();facts->addWidget(key,row,0,Qt::AlignLeft|Qt::AlignTop);facts->addWidget(text,row,1);};
 auto* fullVersion=new BuildValue(residentBuild["version"].toString());fullVersion->setParent(this);fullVersion->setObjectName("buildVersion");fullVersion->hide();add("Commit",residentBuild["gitCommit"].toString(residentBuild["commit"].toString("Unknown")),"buildCommit");add("Build state",residentBuild["dirty"].isBool()?(residentBuild["dirty"].toBool()?"Modified source tree":"Clean source tree"):"Unknown","buildState");for(auto it=residentBuild.begin();it!=residentBuild.end();++it){if(QStringList{"version","gitCommit","commit","dirty"}.contains(it.key()))continue;const auto value=it.value();const auto text=value.isObject()?QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)):value.isArray()?QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)):value.toVariant().toString();add(caption(it.key()),text,"build_"+it.key());}
 auto* scroll=new BuildFacts(details,body);bodyColumn->addWidget(scroll);scroll->hide();
 auto* browser=new Ui::BrowserFeedback(body);browser->setObjectName("aboutWebsiteFeedback");bodyColumn->addWidget(browser);
 auto* buttons=new QWidget(body);buttons->setObjectName("aboutActions");auto* actions=new QHBoxLayout(buttons);actions->setContentsMargins(0,0,0,0);actions->setSpacing(Ui::space(Ui::Space::s));bodyColumn->addWidget(buttons);
 auto* website=new QPushButton(Ui::icon("open-external"),"put.gd website",buttons);website->setObjectName("putgdWebsite");website->setAutoDefault(false);Ui::role(website,"link");website->setProperty("uiFlush",true);actions->addWidget(website);connect(website,&QPushButton::clicked,this,[browser]{const QUrl url("https://put.gd");host::openLink(url,browser->opening(url));});actions->addStretch();
 auto* close=new QPushButton("Close",buttons);close->setObjectName("closeAbout");Ui::role(close,"lead");close->setDefault(true);actions->addWidget(close);connect(close,&QPushButton::clicked,this,&QDialog::accept);
 header->chromeAfter(close);
 connect(toggle,&QToolButton::toggled,this,[this,scroll,copy](bool expanded){scroll->setVisible(expanded);copy->setVisible(expanded);fitHeight();});
}
// Grow to the content, up to a readable maximum inside the screen; the facts scroll only beyond it.
int AboutWindow::maximumFitHeight()const{const auto* display=screen()?screen():QGuiApplication::primaryScreen();const int screenRoom=display?display->availableGeometry().height()-2*Ui::space(Ui::Space::xl):Ui::scaled(600);return qMin(Ui::scaled(600),screenRoom);}
void AboutWindow::showEvent(QShowEvent* event){fitHeight();QDialog::showEvent(event);}
void AboutWindow::paintEvent(QPaintEvent*){if(!Ui::WindowHeader::customFrame())return;QPainter p(this);const auto& c=Theme::current();p.setPen(Ui::mix(c.borderStrong,c.bg,0.45));p.drawRect(rect().adjusted(0,0,-1,-1));}
void AboutWindow::fitHeight(){layout()->setSizeConstraint(QLayout::SetNoConstraint);setMinimumHeight(0);setMaximumHeight(maximumFitHeight());auto* facts=findChild<QScrollArea*>("aboutBuildScroll");
 // A child shown or hidden only marks its own layout; forget the body's cached height before measuring.
 facts->parentWidget()->layout()->invalidate();facts->parentWidget()->updateGeometry();facts->setMaximumHeight(QWIDGETSIZE_MAX);facts->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);layout()->activate();const int natural=layout()->totalHeightForWidth(width()),limit=maximumFitHeight();if(natural>limit){facts->setMaximumHeight(qMax(facts->minimumSizeHint().height(),facts->heightForWidth(facts->width())-(natural-limit)));facts->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);layout()->invalidate();layout()->activate();}setFixedHeight(qMin(layout()->totalHeightForWidth(width()),limit));}
}
