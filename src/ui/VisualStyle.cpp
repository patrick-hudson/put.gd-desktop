#include "VisualStyle.h"
#include "IconArt.h"
#include "Theme.h"
#include "Graphite.h"
#include <QApplication>
#include <QAbstractItemView>
#include <QFrame>
#include <QComboBox>
#include <QStyleOptionComboBox>
#include <QHash>
#include <QIconEngine>
#include <QHeaderView>
#include <QResizeEvent>
#include <QFormLayout>
#include <QLabel>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QScreen>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QDir>
#include <QImage>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <utility>
#include <functional>
#include <memory>
namespace putgd::VisualStyle {
namespace {
// Indicator glyphs painted from the tokens, at 1x and @2x, so they change with
// the scheme. They live in a private temporary folder for this process.
void paintIndicators(const QString& folder, const Theme::Palette& p)
{
    auto save = [&](const QString& name, QSize size, const std::function<void(QPainter&)>& draw) {
        for (int scale : {1, 2}) {
            QImage image(size * scale, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.scale(scale, scale);
            draw(painter);
            painter.end();
            image.save(folder + "/" + name + (scale == 2 ? "@2x.png" : ".png"), "PNG");
        }
    };
    auto stroke = [](QPainter& painter, const QColor& color, qreal width, const QPainterPath& path) {
        painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    };
    // Checked but disabled (#300): a muted tick on the accent-tinted fill.
    for (const bool off : {false, true}) {
        save(off ? "check-off" : "check", {17, 17}, [&](QPainter& painter) {
            QPainterPath tick;
            tick.moveTo(4, 8.5); tick.lineTo(7, 11.5); tick.lineTo(13, 5.5);
            stroke(painter, off ? p.textMuted : p.onAccent, 1.8, tick);
        });
    }
    save("chevron", {12, 12}, [&](QPainter& painter) {
        QPainterPath chevron;
        chevron.moveTo(2, 4); chevron.lineTo(6, 8); chevron.lineTo(10, 4);
        stroke(painter, p.textSecondary, 1.7, chevron);
    });
    // Spin box steps (#233): up and down, and dimmed at a limit or disabled.
    for (const bool up : {true, false}) {
        for (const bool off : {false, true}) {
            save(QString("spin-%1%2").arg(up ? "up" : "down", off ? "-off" : ""), {10, 10}, [&](QPainter& painter) {
                QPainterPath chevron;
                if (up) { chevron.moveTo(2, 6.5); chevron.lineTo(5, 3.5); chevron.lineTo(8, 6.5); }
                else { chevron.moveTo(2, 3.5); chevron.lineTo(5, 6.5); chevron.lineTo(8, 3.5); }
                stroke(painter, off ? p.textDisabled : p.textSecondary, 1.5, chevron);
            });
        }
    }
    for (const bool checked : {false, true}) {
        for (const bool off : {false, true}) {
            save(QString("radio%1%2").arg(checked ? "-checked" : "", off ? "-off" : ""), {18, 18}, [&](QPainter& painter) {
                painter.setPen(QPen(off ? p.border : p.borderStrong, 1));
                painter.setBrush(off ? p.surface : p.surfaceRaised);
                painter.drawEllipse(QPointF(9, 9), 8, 8);
                if (!checked) return;
                painter.setPen(Qt::NoPen);
                painter.setBrush(off ? p.textMuted : p.accent);
                painter.drawEllipse(QPointF(9, 9), 4, 4);
            });
        }
    }
}
// Fit the name column when the viewport changes, never while dragging a section.
// Stretch mode would steal the drag distance from the first column instead.
class ColumnSizer final:public QObject {
public:
 explicit ColumnSizer(QHeaderView* header):QObject(header),header_(header){
  header_->setSectionResizeMode(QHeaderView::Interactive);header_->setStretchLastSection(false);
  header_->viewport()->installEventFilter(this);
 }
protected:
 bool eventFilter(QObject* watched,QEvent* event)override {
  if(event->type()==QEvent::Resize){
   const auto* resize=static_cast<QResizeEvent*>(event);
   if(resize->size().width()!=resize->oldSize().width()&&header_->count()>0){
    int remaining=resize->size().width();
    for(int column=1;column<header_->count();++column)if(!header_->isSectionHidden(column))remaining-=header_->sectionSize(column);
    header_->resizeSection(0,qMax(header_->minimumSectionSize(),remaining));
   }
  }
  return QObject::eventFilter(watched,event);
 }
private:QHeaderView* header_;
};
class ChoiceBox final:public QComboBox {
public:using QComboBox::QComboBox;
protected:
 // Leave wheel input to the containing pane. The open popup handles its own scrolling.
 void wheelEvent(QWheelEvent* event)override {event->ignore();}
};
class StateDelegate final:public QStyledItemDelegate {
public:StateDelegate(QList<int> columns,QObject* parent):QStyledItemDelegate(parent),columns_(std::move(columns)){}
 void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index)const override {
  if(!columns_.contains(index.column())||index.data().toString().isEmpty()){QStyledItemDelegate::paint(painter,option,index);return;}
  auto value=index.data().toString();
  // A local-save cell can contain a path; keep that evidence as ordinary text.
  if(value.contains(' ')||value.contains('/')){QStyledItemDelegate::paint(painter,option,index);return;}
  QStyleOptionViewItem base(option);initStyleOption(&base,index);base.text.clear();base.widget->style()->drawControl(QStyle::CE_ItemViewItem,&base,painter,base.widget);
  const auto& p=Theme::current();QColor foreground=p.textSecondary,background=p.surfaceRaised;
  if(QStringList{"completed","uploaded","copied","saved","removed"}.contains(value)){foreground=p.success;background=p.successSurface;}
  else if(QStringList{"failed","denied","interrupted"}.contains(value)){foreground=p.danger;background=p.dangerSurface;}
  else if(QStringList{"partial","uncertain","pending","running","accepted","superseded","version_retained"}.contains(value)){foreground=p.warning;background=p.warningSurface;}
  value.replace('_',' ');value=value.left(1).toUpper()+value.mid(1);
  painter->save();painter->setRenderHint(QPainter::Antialiasing);auto bounds=option.rect.adjusted(8,5,-8,-5);bounds.setWidth(qMin(bounds.width(),option.fontMetrics.horizontalAdvance(value)+22));painter->setPen(Qt::NoPen);painter->setBrush(background);painter->drawRoundedRect(bounds,5,5);painter->setPen(foreground);painter->drawText(bounds.adjusted(10,0,-8,0),Qt::AlignVCenter|Qt::AlignLeft,option.fontMetrics.elidedText(value,Qt::ElideRight,qMax(0,bounds.width()-18)));painter->restore();
 }
 QSize sizeHint(const QStyleOptionViewItem& option,const QModelIndex& index)const override {auto size=QStyledItemDelegate::sizeHint(option,index);size.setHeight(qMax(36,size.height()));if(columns_.contains(index.column()))size.setWidth(size.width()+26);return size;}
private:QList<int> columns_;
};
}
void configureColumns(QHeaderView* header){new ColumnSizer(header);}
QComboBox* choiceBox(QWidget* parent){return new ChoiceBox(parent);}
void role(QWidget* widget,const char* value){widget->setProperty("visualRole",value);widget->style()->unpolish(widget);widget->style()->polish(widget);}
namespace {
void setFeedback(QLabel* label,const QString& text,const char* tone){label->setProperty("feedbackTone",tone);label->setText(text);label->setVisible(!text.isEmpty());label->style()->unpolish(label);label->style()->polish(label);}
}
void feedback(QLabel* label,const QString& text,bool error){setFeedback(label,text,error?"error":"neutral");}
void success(QLabel* label,const QString& text){setFeedback(label,text,"success");}
QToolButton* helpButton(const QString& title,const QString& description,QWidget* control,QWidget* parent){
 control->setToolTip(description);control->setAccessibleDescription(description);
 auto* button=new QToolButton(parent);button->setObjectName(control->objectName()+"Help");button->setProperty("settingHelpFor",control->objectName());button->setProperty("visualRole","info");button->setIcon(icon("info"));button->setIconSize(QSize(18,18));button->setFixedSize(26,26);button->setFocusPolicy(Qt::StrongFocus);button->setAccessibleName("Help for "+title);button->setAccessibleDescription(description);button->setToolTip(description);
 QObject::connect(button,&QToolButton::clicked,button,[button,title,description]{
  auto* dialog=new QMessageBox(QMessageBox::NoIcon,title,title,QMessageBox::Close,button->window());dialog->setObjectName("settingHelpDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setTextFormat(Qt::PlainText);dialog->setInformativeText(description);dialog->open();
 });return button;
}
QWidget* settingLabel(const QString& title,const QString& description,QWidget* control,QWidget* parent){
 control->setAccessibleName(title);auto* row=new QWidget(parent);auto* layout=new QHBoxLayout(row);layout->setContentsMargins(0,0,0,0);layout->setSpacing(6);
 // A label with a buddy treats & as a mnemonic marker; show titles literally.
 auto* label=new QLabel(QString(title).replace('&',QLatin1String("&&")),row);label->setBuddy(control);layout->addWidget(label);layout->addWidget(helpButton(title,description,control,row));layout->addStretch();return row;
}
QWidget* settingRow(const QString& title,const QString& description,QWidget* control,QWidget* parent){
 control->setAccessibleName(title);auto* row=new QWidget(parent);auto* layout=new QHBoxLayout(row);layout->setContentsMargins(0,0,0,0);layout->setSpacing(6);layout->addWidget(control);layout->addWidget(helpButton(title,description,control,row));layout->addStretch();return row;
}
namespace {
void setPalette(QApplication& app){app.setPalette(Graphite::palette());}
const char* const schemeProperty="themeScheme";
void repolish(QWidget* widget){widget->setProperty(schemeProperty,Theme::name(Theme::scheme()));widget->style()->unpolish(widget);widget->style()->polish(widget);widget->update();}
bool stale(const QWidget* widget){return widget->property(schemeProperty).toString()!=Theme::name(Theme::scheme());}
QList<std::function<void()>>& beforeRepolish(){static QList<std::function<void()>> hooks;return hooks;}
// Marks each widget with the active scheme before Qt first polishes it, and
// brings a widget hidden during a switch up to date before it is shown.
class SchemeMarker final:public QObject {
public:using QObject::QObject;
protected:
 bool eventFilter(QObject* object,QEvent* event)override{
  if((event->type()==QEvent::Polish||event->type()==QEvent::Show)&&object->isWidgetType()){
   auto* widget=static_cast<QWidget*>(object);
   if(event->type()==QEvent::Polish)widget->setProperty(schemeProperty,Theme::name(Theme::scheme()));
   else if(stale(widget))repolish(widget);
  }
  return QObject::eventFilter(object,event);
 }
};
}
QString schemeScoped(const std::function<QString(Theme::Scheme)>& render){
 QString result;
 static const QRegularExpression comments(QStringLiteral("/\\*.*?\\*/"),QRegularExpression::DotMatchesEverythingOption);
 for(const auto scheme:{Theme::Scheme::Dark,Theme::Scheme::Light}){
  const auto mark=QStringLiteral("[%1=\"%2\"]").arg(QLatin1String(schemeProperty),Theme::name(scheme));
  for(const auto& rule:render(scheme).remove(comments).split('}',Qt::SkipEmptyParts)){
   const auto open=rule.indexOf('{');if(open<0)continue;
   QStringList selectors;
   for(auto selector:rule.left(open).split(',')){
    selector=selector.simplified();if(selector.isEmpty())continue;
    // The mark goes on the element the rule styles: the last compound,
    // before any pseudo-state or sub-control.
    const auto last=qMax(selector.lastIndexOf(' '),selector.lastIndexOf('>'))+1;
    const auto pseudo=selector.indexOf(':',last);
    selectors.append(pseudo<0?selector+mark:selector.left(pseudo)+mark+selector.mid(pseudo));
   }
   if(!selectors.isEmpty())result+=selectors.join(", ")+" {"+rule.mid(open+1)+"}\n";
  }
 }
 return result;
}
void followScheme(QApplication& app,std::function<void()> beforeRepolishHook){
 static bool following=false;
 if(beforeRepolishHook)beforeRepolish().append(std::move(beforeRepolishHook));
 if(std::exchange(following,true))return;
 app.installEventFilter(new SchemeMarker(&app));
 for(auto* widget:QApplication::allWidgets())widget->setProperty(schemeProperty,Theme::name(Theme::scheme()));
 QObject::connect(&Theme::Notifier::instance(),&Theme::Notifier::changed,&app,[]{
  for(const auto& hook:beforeRepolish())hook();
  for(auto* widget:QApplication::allWidgets())if(widget->isVisible()&&stale(widget))repolish(widget);
 });
}
void apply(QApplication& app){
 if(app.property("putgdVisualStyle").toBool())return;
 app.setProperty("putgdVisualStyle",true);
 // The Graphite style paints every control from the tokens (2026-10-26); no
 // application stylesheet, so nothing re-parses on a scheme switch.
 Graphite::install(app);
 // A scheme switch restyles every open window and dialog without a restart.
 followScheme(app,[&app]{setPalette(app);});
}
namespace {
// Renders IconArt glyphs in the scheme active when Qt asks for a pixmap, so an
// icon set once on a button follows later scheme switches.
class ThemedIcon final:public QIconEngine {
public:
 explicit ThemedIcon(QString name):name_(std::move(name)){}
 QIconEngine* clone()const override{return new ThemedIcon(name_);}
 QString key()const override{return QStringLiteral("putgd-themed");}
 QSize actualSize(const QSize& size,QIcon::Mode,QIcon::State)override{const int side=qMin(size.width(),size.height());return {side,side};}
 QPixmap pixmap(const QSize& size,QIcon::Mode mode,QIcon::State state)override{return scaledPixmap(size,mode,state,1.0);}
 QPixmap scaledPixmap(const QSize& size,QIcon::Mode mode,QIcon::State,qreal scale)override{
  const int side=qMax(1,qRound(qMin(size.width(),size.height())*scale));
  const auto& p=Theme::current();const bool disabled=mode==QIcon::Disabled;
  const auto key=QStringLiteral("%1/%2/%3/%4").arg(name_,Theme::name(Theme::scheme())).arg(int(disabled)).arg(side);
  static QHash<QString,QPixmap> cache;if(const auto found=cache.constFind(key);found!=cache.cend())return *found;
  const auto art=IconArt::symbol(name_,disabled?p.textDisabled:p.icon,qMax(1,(side+23)/24),disabled?p.textDisabled.darker(150):p.iconSecondary);
  auto pixmap=QPixmap::fromImage(art.scaled(side,side,Qt::KeepAspectRatio,Qt::SmoothTransformation));pixmap.setDevicePixelRatio(scale);
  if(cache.size()>512)cache.clear();cache.insert(key,pixmap);return pixmap;
 }
 void paint(QPainter* painter,const QRect& rect,QIcon::Mode mode,QIcon::State state)override{
  const qreal scale=painter->device()?painter->device()->devicePixelRatioF():1.0;painter->drawPixmap(rect,scaledPixmap(rect.size(),mode,state,scale));
 }
private:QString name_;
};
}
QIcon icon(const QString& name){
 // The brand mark keeps its own colours in every scheme.
 if(name=="brand")return icon(name,Theme::brandAccent());
 return QIcon(new ThemedIcon(name));
}
QIcon icon(const QString& name,const QColor& color,int percent){
 static QHash<QString,QIcon> cache;const auto key=name+color.name(QColor::HexArgb);
 const bool progress=name=="tray"&&percent>=0;
 if(!progress&&cache.contains(key))return cache.value(key);
 QIcon result;
 if(name=="brand") {
  for(int scale:{1,2,4,8}) {
   auto image=IconArt::liftApp(color,scale);image.setDevicePixelRatio(1);
   result.addPixmap(QPixmap::fromImage(image));
  }
 } else for(int scale:{1,2,3})result.addPixmap(QPixmap::fromImage(progress?IconArt::liftTray(color,percent,scale):IconArt::symbol(name,color,scale)));
 if(!progress)cache.insert(key,result);
 return result;
}
QWidget* header(const QString& title,const QString& subtitle,QWidget* parent){auto* widget=new QWidget(parent);auto* layout=new QVBoxLayout(widget);layout->setContentsMargins(0,0,0,4);layout->setSpacing(4);auto* heading=new QLabel(title,widget);role(heading,"title");layout->addWidget(heading);if(!subtitle.isEmpty()){auto* description=new QLabel(subtitle,widget);description->setWordWrap(true);role(description,"subtitle");layout->addWidget(description);}return widget;}
QFrame* card(const QString& title,const QString& subtitle,QWidget* parent){auto* frame=new QFrame(parent);role(frame,"card");frame->setMaximumWidth(Graphite::scaled(880));auto* layout=new QVBoxLayout(frame);layout->setContentsMargins(0,16,0,16);layout->setSpacing(10);auto* heading=new QLabel(title,frame);role(heading,"sectionTitle");layout->addWidget(heading);if(!subtitle.isEmpty()){auto* description=new QLabel(subtitle,frame);description->setWordWrap(true);role(description,"subtitle");layout->addWidget(description);}return frame;}
void styleForm(QFormLayout* form){form->setContentsMargins(0,0,0,0);form->setHorizontalSpacing(16);form->setVerticalSpacing(8);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->setRowWrapPolicy(QFormLayout::DontWrapRows);form->setLabelAlignment(Qt::AlignLeft|Qt::AlignVCenter);}
void decorateView(QAbstractItemView* view,const QList<int>& columns){view->setItemDelegate(new StateDelegate(columns,view));view->setAlternatingRowColors(true);view->setIconSize(QSize(22,22));view->setTextElideMode(Qt::ElideMiddle);}
QString indicators(Theme::Scheme scheme){
    static std::unique_ptr<QTemporaryDir> folder;
    static QHash<int, QString> painted;
    if (const auto found = painted.constFind(int(scheme)); found != painted.cend()) return *found;
    if (!folder) folder = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/putgd-theme-XXXXXX");
    if (!folder->isValid()) return {};
    const auto path = folder->filePath(Theme::name(scheme));
    QDir().mkpath(path);
    paintIndicators(path, Theme::palette(scheme));
    painted.insert(int(scheme), path);
    return path;
}
}
