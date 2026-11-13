#include "Layout.h"
#include <QPointF>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace putgd::ocr {
namespace {
// Ink must differ from the background by at least this much.
constexpr int inkContrast=24;
// A text height Tesseract's LSTM reads well: line runs of about this many rows.
constexpr int targetLineRows=24;
QRect bounds(const Quad& q){
 int left=q[0].x(),right=q[0].x(),top=q[0].y(),bottom=q[0].y();
 for(const auto& p:q){left=std::min(left,p.x());right=std::max(right,p.x());top=std::min(top,p.y());bottom=std::max(bottom,p.y());}
 return QRect(QPoint(left,top),QPoint(right,bottom));
}
int otsu(const std::array<qint64,256>& histogram){
 qint64 total=0;double sum=0;
 for(int i=0;i<256;++i){total+=histogram[i];sum+=double(i)*histogram[i];}
 if(total==0)return 127;
 double sumBelow=0,best=-1;qint64 below=0;int threshold=127;
 for(int t=0;t<256;++t){
  below+=histogram[t];if(below==0)continue;
  const auto above=total-below;if(above==0)break;
  sumBelow+=double(t)*histogram[t];
  const double meanBelow=sumBelow/below,meanAbove=(sum-sumBelow)/above;
  const double between=double(below)*double(above)*(meanBelow-meanAbove)*(meanBelow-meanAbove);
  if(between>best){best=between;threshold=t;}
 }
 return threshold;
}
// A code's area grows by its quiet zone, roughly, before it is filled.
Quad grown(const Quad& q){
 QPointF centre;for(const auto& p:q)centre+=QPointF(p);centre/=4;
 Quad out;
 for(int i=0;i<4;++i){
  const QPointF d=QPointF(q[i])-centre;const double length=std::hypot(d.x(),d.y());
  const double grow=length>0?(length*0.12+3)/length:0;
  out[i]=(QPointF(q[i])+d*grow).toPoint();
 }
 return out;
}
void fill(GreyImage& image,const std::array<QPointF,4>& corners,uint8_t value){
 double top=corners[0].y(),bottom=top;
 for(const auto& p:corners){top=std::min(top,p.y());bottom=std::max(bottom,p.y());}
 const int first=std::max(0,int(std::floor(top))),last=std::min(image.height-1,int(std::ceil(bottom)));
 for(int y=first;y<=last;++y){
  const double scan=y+0.5;double crossings[4];int count=0;
  for(int i=0;i<4;++i){
   const auto& a=corners[i];const auto& b=corners[(i+1)%4];
   if((a.y()<=scan&&b.y()>scan)||(b.y()<=scan&&a.y()>scan))crossings[count++]=a.x()+(scan-a.y())*(b.x()-a.x())/(b.y()-a.y());
  }
  for(int i=1;i<count;++i)for(int j=i;j>0&&crossings[j-1]>crossings[j];--j)std::swap(crossings[j-1],crossings[j]);
  auto* row=image.row(y);
  for(int i=0;i+1<count;i+=2){
   const int from=std::max(0,int(std::ceil(crossings[i]-0.5))),to=std::min(image.width-1,int(std::floor(crossings[i+1]-0.5)));
   for(int x=from;x<=to;++x)row[x]=value;
  }
 }
}
}
namespace {
// Where to cut inside a run of rows [first, last] that a blank test passed:
// the middle of its longest stretch of least-inked rows. A "blank" row may
// still hold a few pixels of a descender or an ascender (up to width/256), and
// a cut through it clips that glyph in the strip on each side ("sample" read
// as "samnle", "beside" as "heside").
int quietestRow(const Profile& profile,int first,int last){
 int least=profile.rowInk.at(first);for(int y=first+1;y<=last;++y)least=std::min(least,profile.rowInk.at(y));
 int best=first,bestLength=0;
 for(int y=first;y<=last;){
  if(profile.rowInk.at(y)!=least){++y;continue;}
  int end=y;while(end+1<=last&&profile.rowInk.at(end+1)==least)++end;
  if(end-y+1>bestLength){bestLength=end-y+1;best=(y+end+1)/2;}
  y=end+1;
 }
 return best;
}
}
Profile measure(const GreyView& image,const QList<Quad>& codes){
 Profile profile;profile.rowInk.fill(0,image.height);
 QList<QRect> excluded;for(const auto& q:codes)excluded.append(bounds(grown(q)).intersected(QRect(0,0,image.width,image.height)));
 // Which pixels of a row lie in a code: the rectangles covering that row,
 // marked once per row, so each pixel costs one lookup however many codes.
 std::vector<uint8_t> mask(size_t(std::max(0,image.width)));
 const auto rowMask=[&](int y)->const uint8_t*{
  bool any=false;
  for(const auto& r:excluded)if(y>=r.top()&&y<=r.bottom()){
   if(!any){std::fill(mask.begin(),mask.end(),uint8_t(0));any=true;}
   std::fill(mask.begin()+r.left(),mask.begin()+r.right()+1,uint8_t(1));
  }
  return any?mask.data():nullptr;
 };
 std::array<qint64,256> histogram{};
 for(int y=0;y<image.height;++y){
  const auto* row=image.row(y);const auto* inCode=rowMask(y);
  for(int x=0;x<image.width;++x)if(!inCode||!inCode[x])++histogram[row[x]];
 }
 const int threshold=otsu(histogram);
 qint64 dark=0,light=0;double darkSum=0,lightSum=0;
 for(int i=0;i<256;++i){if(i<=threshold){dark+=histogram[i];darkSum+=double(i)*histogram[i];}else{light+=histogram[i];lightSum+=double(i)*histogram[i];}}
 profile.dark=dark>light;
 profile.background=profile.dark?(dark?int(std::lround(darkSum/dark)):0):(light?int(std::lround(lightSum/light)):255);
 const bool inkIsLight=profile.dark;
 for(int y=0;y<image.height;++y){
  const auto* row=image.row(y);const auto* inCode=rowMask(y);int ink=0;
  for(int x=0;x<image.width;++x){
   const int p=row[x];
   if((inkIsLight?p>threshold:p<=threshold)&&std::abs(p-profile.background)>=inkContrast&&(!inCode||!inCode[x]))++ink;
  }
  profile.rowInk[y]=ink;
 }
 QList<int> runs;int run=0;
 for(int y=0;y<=image.height;++y){
  if(y<image.height&&!profile.blank(y,image.width)){++run;continue;}
  if(run>=3&&run<=std::max(3,image.height/3))runs.append(run);
  run=0;
 }
 if(!runs.isEmpty()){std::nth_element(runs.begin(),runs.begin()+runs.size()/2,runs.end());profile.lineHeight=runs[runs.size()/2];}
 return profile;
}
int preferredScale(int lineHeight,int maximumScale){
 if(lineHeight<=0)return std::min(2,maximumScale);
 return std::clamp((targetLineRows+lineHeight-1)/lineHeight,1,maximumScale);
}
Plan plan(int width,int height,const Profile& profile,const PlanLimits& limits){
 Plan result;
 const qint64 pixels=qint64(width)*height;
 if(width<1||height<1||pixels>maximumInputPixels){result.error="input_excessive";return result;}
 double scale=limits.forcedScale>0?limits.forcedScale:preferredScale(profile.lineHeight,limits.maximumScale);
 if(double(pixels)*scale*scale>double(limits.scaledPixels))scale=std::max(1.0,std::sqrt(double(limits.scaledPixels)/double(pixels)));
 const int lineRows=profile.lineHeight>0?profile.lineHeight:16;
 for(;;){
  const qint64 scaledWidth=qint64(std::ceil(width*scale))+2*limits.border;
  const qint64 scaledHeight=qint64(std::ceil(height*scale))+2*limits.border;
  const qint64 bandPixels=limits.bandPixels>0?limits.bandPixels:std::clamp<qint64>(scaledWidth*scaledHeight/(2*std::max(1,limits.threads)),1000000,2000000);
  result.scale=scale;result.bands.clear();
  if(scaledWidth*scaledHeight<=limits.singlePassPixels){result.bands.append({0,height,false});return result;}
  const auto sourceRows=[&](qint64 pixels){const qint64 rows=pixels/scaledWidth-2*limits.border;return rows>0?int(std::floor(rows/scale)):0;};
  const int largestRows=sourceRows(bandPixelLimit);
  const int bandRows=std::min(largestRows,std::max(sourceRows(bandPixels),8*lineRows));
  if(largestRows<4*lineRows){
   if(scale>1){scale=1;continue;}
   result.error="input_excessive";result.bands.clear();return result;
  }
  int start=0;bool seam=false;
  while(start<height){
   const int limit=start+bandRows;
   if(limit>=height){result.bands.append({start,height,seam});break;}
   int gap=-1;
   for(int y=limit-1;y>=start+bandRows*3/4;--y)if(profile.blank(y,width)){gap=y;break;}
   if(gap>=0){
    int first=gap;while(first-1>=start+bandRows*3/4&&profile.blank(first-1,width))--first;
    const int cut=quietestRow(profile,first,gap);
    result.bands.append({start,cut,seam});start=cut;seam=false;
   }else{
    result.bands.append({start,limit,seam});
    start=std::max(start+1,limit-std::min(2*lineRows,bandRows/4));seam=true;
   }
  }
  return result;
 }
}
namespace {
// The line pitch of a row profile: its period, the first peak of its
// autocorrelation after it has fallen below a third of its height. 0 when the
// profile has no clear period (no text lines, or too few).
int linePitch(const QList<int>& ink,int height){
 const int longest=std::min(200,height/4);
 if(longest<6)return 0;
 double mean=0;for(int y=0;y<height;++y)mean+=ink.at(y);mean/=height;
 std::vector<double> d(size_t(height),0.0);for(int y=0;y<height;++y)d[size_t(y)]=ink.at(y)-mean;
 const auto at=[&](int lag){double c=0;for(int y=0;y+lag<height;++y)c+=d[size_t(y)]*d[size_t(y+lag)];return c/(height-lag);};
 const double zero=at(0);if(zero<=0)return 0;
 bool fell=false;int best=0;double peak=0;
 for(int lag=3;lag<=longest;++lag){
  const double c=at(lag);
  if(!fell){fell=c<zero/3;continue;}
  if(c>=zero/3&&c>peak){peak=c;best=lag;}
  else if(best>0)break;
 }
 return best;
}
}
QList<Band> strips(const Profile& profile,int width,int height,int parts){
 QList<Band> out;
 const auto blank=[&](int y){return y>=0&&y<height&&y<profile.rowInk.size()&&profile.blank(y,width);};
 const auto ink=[&](int y){return profile.rowInk.at(y);};
 // Between two lines with no blank row (a window border or a rule down the
 // page, or lines set without leading, inks every row): within one line pitch
 // of the target, the row with the least ink is between two lines, where only
 // descenders and ascenders reach; taken when it holds at most a quarter of
 // the ink of the rows around it, so a strip never starts inside a line.
 int pitch=-1;
 const auto betweenLines=[&](int target){
  if(pitch<0)pitch=height<=profile.rowInk.size()?linePitch(profile.rowInk,height):0;
  if(pitch<6)return -1;
  const int from=std::max(1,target-pitch/2-1),to=std::min(height-2,target+pitch/2+1);
  int low=-1;for(int y=from;y<=to;++y)if(low<0||ink(y)<ink(low))low=y;
  if(low<0)return -1;
  int last=low;while(last+1<=to&&ink(last+1)==ink(low))++last;
  QList<int> around;for(int y=std::max(0,target-2*pitch);y<std::min(height,target+2*pitch);++y)around.append(ink(y));
  std::nth_element(around.begin(),around.begin()+around.size()/2,around.end());
  return 4*ink(low)<=around.at(around.size()/2)?(low+last)/2:-1;
 };
 const int lineRows=profile.lineHeight>0?profile.lineHeight:16;
 int start=0;
 for(int k=1;k<parts&&height>0;++k){
  const int target=int(qint64(height)*k/parts),reach=std::max(1,height/(2*parts));
  int found=-1;
  for(int d=0;d<=reach&&found<0;++d){if(blank(target-d))found=target-d;else if(blank(target+d))found=target+d;}
  int cut=-1;
  if(found>=0){
   int first=found,last=found;while(blank(first-1))--first;while(blank(last+1))++last;
   cut=quietestRow(profile,first,last);
  }else cut=betweenLines(target);
  if(cut<0)continue;
  const int shortest=4*(profile.lineHeight>0?lineRows:std::max(lineRows,pitch));
  if(cut-start<shortest||height-cut<shortest)continue;
  out.append({start,cut,false});start=cut;
 }
 out.append({start,height,false});
 return out;
}
namespace {
// The activity grid of panels(): two counts per 4x4 cell, one from horizontal
// steps (for gutters down the page) and one from vertical steps (across it).
struct Activity {
 static constexpr int cell=4;
 int columns=0,rows=0;
 std::vector<uint16_t> down,across;
 uint16_t& at(std::vector<uint16_t>& grid,int x,int y){return grid[size_t(y)*size_t(columns)+size_t(x)];}
 uint16_t at(const std::vector<uint16_t>& grid,int x,int y)const{return grid[size_t(y)*size_t(columns)+size_t(x)];}
 qint64 sum(const std::vector<uint16_t>& grid,int x0,int y0,int x1,int y1)const{
  qint64 n=0;for(int y=y0;y<y1;++y){const auto* row=grid.data()+size_t(y)*size_t(columns);for(int x=x0;x<x1;++x)n+=row[x];}return n;
 }
};
Activity activity(const GreyView& image,const QList<Quad>& codes,int longRun){
 Activity a;a.columns=(image.width+Activity::cell-1)/Activity::cell;a.rows=(image.height+Activity::cell-1)/Activity::cell;
 a.down.assign(size_t(a.columns)*size_t(a.rows),0);a.across.assign(a.down.size(),0);
 QList<QRect> excluded;for(const auto& q:codes)excluded.append(bounds(grown(q)).intersected(QRect(0,0,image.width,image.height)));
 constexpr int step=32;
 // Vertical runs of horizontal steps: a run as long as longRun is a straight
 // edge (a window border), not text, and is not counted.
 std::vector<int> run(size_t(std::max(0,image.width)),0);
 const auto flushDown=[&](int x,int end){
  const int length=run[size_t(x)];run[size_t(x)]=0;
  if(length<=0||length>=longRun)return;
  for(int y=end-length;y<end;++y){auto& c=a.at(a.down,x/Activity::cell,y/Activity::cell);if(c<65535)++c;}
 };
 std::vector<uint8_t> mask(size_t(std::max(0,image.width)));
 for(int y=0;y+1<image.height;++y){
  bool masked=false;
  for(const auto& r:excluded)if(y>=r.top()&&y<=r.bottom()){if(!masked){std::fill(mask.begin(),mask.end(),uint8_t(0));masked=true;}std::fill(mask.begin()+r.left(),mask.begin()+r.right()+1,uint8_t(1));}
  const auto* p=image.row(y);const auto* q=image.row(y+1);int horizontal=0;
  for(int x=0;x+1<image.width;++x){
   const bool skip=masked&&mask[size_t(x)];
   if(!skip&&std::abs(int(p[x+1])-int(p[x]))>=step)++run[size_t(x)];else flushDown(x,y);
   // Horizontal runs of vertical steps: a long one is a rule or an underline.
   if(!skip&&std::abs(int(q[x])-int(p[x]))>=step){++horizontal;continue;}
   if(horizontal>0&&horizontal<longRun)for(int i=x-horizontal;i<x;++i){auto& c=a.at(a.across,i/Activity::cell,y/Activity::cell);if(c<65535)++c;}
   horizontal=0;
  }
  if(horizontal>0&&horizontal<longRun)for(int i=image.width-1-horizontal;i<image.width-1;++i){auto& c=a.at(a.across,i/Activity::cell,y/Activity::cell);if(c<65535)++c;}
 }
 for(int x=0;x<image.width;++x)flushDown(x,image.height-1);
 return a;
}
struct CellRect { int x0=0,y0=0,x1=0,y1=0; int width()const{return x1-x0;} int height()const{return y1-y0;} };
// The widest run of empty projection entries strictly inside [0, size), at
// least minimum long, with activity on both sides. Returns its middle or -1.
int gutter(const std::vector<qint64>& projection,int minimum,qint64 noise,int* from=nullptr,int* to=nullptr){
 const int size=int(projection.size());int best=-1,bestLength=0;
 int first=0;while(first<size&&projection[size_t(first)]<=noise)++first;
 int last=size-1;while(last>=0&&projection[size_t(last)]<=noise)--last;
 for(int i=first;i<=last;){
  if(projection[size_t(i)]>noise){++i;continue;}
  int j=i;while(j<=last&&projection[size_t(j)]<=noise)++j;
  if(j-i>=minimum&&j-i>bestLength){bestLength=j-i;best=(i+j)/2;if(from)*from=i;if(to)*to=j;}
  i=j;
 }
 return best;
}
void cut(const Activity& a,const CellRect& r,int down,int across,int minimumSide,int depth,QList<CellRect>& out){
 if(depth<10&&r.width()>=2*minimumSide){
  std::vector<qint64> columns(size_t(r.width()));
  for(int x=r.x0;x<r.x1;++x)columns[size_t(x-r.x0)]=a.sum(a.down,x,r.y0,x+1,r.y1);
  int from=0,to=0;int at=gutter(columns,down,r.height()/16,&from,&to);
  // Not a gutter where a text line runs straight across it: on a tall page a
  // single long line past the others is under the noise floor, and its tail
  // was cut into panels at its word spaces. Two rows of cells in a row, each
  // active over nine tenths of the gutter's width, are such a line; a bar of
  // icons or a title across a screen has gaps and is not.
  if(at>=0){
   int run=0;
   for(int y=r.y0;y<r.y1&&at>=0;++y){
    int active=0;for(int x=r.x0+from;x<r.x0+to;++x)if(a.at(a.down,x,y)||a.at(a.across,x,y))++active;
    run=10*active>=9*(to-from)?run+1:0;
    if(run>=2)at=-1;
   }
  }
  if(at>=minimumSide&&r.width()-at>=minimumSide){
   cut(a,{r.x0,r.y0,r.x0+at,r.y1},down,across,minimumSide,depth+1,out);
   cut(a,{r.x0+at,r.y0,r.x1,r.y1},down,across,minimumSide,depth+1,out);
   return;
  }
 }
 if(depth<10&&r.height()>=2*minimumSide){
  std::vector<qint64> rows(size_t(r.height()));
  for(int y=r.y0;y<r.y1;++y)rows[size_t(y-r.y0)]=a.sum(a.across,r.x0,y,r.x1,y+1);
  const int at=gutter(rows,across,r.width()/16);
  if(at>=minimumSide&&r.height()-at>=minimumSide){
   QList<CellRect> top,bottom;
   cut(a,{r.x0,r.y0,r.x1,r.y0+at},down,across,minimumSide,depth+1,top);
   cut(a,{r.x0,r.y0+at,r.x1,r.y1},down,across,minimumSide,depth+1,bottom);
   // Kept only when it lets a part be cut down the page.
   if(top.size()>1||bottom.size()>1){out.append(top);out.append(bottom);return;}
  }
 }
 out.append(r);
}
}
QList<QRect> panels(const GreyView& image,const QList<Quad>& codes,int lineHeight){
 const QRect whole(0,0,image.width,image.height);
 if(!image.valid())return {whole};
 const int line=lineHeight>0?lineHeight:16;
 const auto a=activity(image,codes,std::max(48,4*line));
 const int cell=Activity::cell;
 const int down=std::max(6,(3*line+cell-1)/cell),across=std::max(3,(3*line/2+cell-1)/cell),minimumSide=std::max(4,(4*line+cell-1)/cell);
 QList<CellRect> leaves;cut(a,{0,0,a.columns,a.rows},down,across,minimumSide,0,leaves);
 if(leaves.size()<2)return {whole};
 QList<QRect> out;const int margin=std::max(4,line/2);
 for(const auto& r:leaves){
  // Trimmed to its activity; a panel with less than one short line of it is empty.
  int x0=r.x1,y0=r.y1,x1=r.x0,y1=r.y0;qint64 total=0;
  for(int y=r.y0;y<r.y1;++y)for(int x=r.x0;x<r.x1;++x){
   const qint64 n=qint64(a.down[size_t(y)*size_t(a.columns)+size_t(x)])+a.across[size_t(y)*size_t(a.columns)+size_t(x)];
   if(n>0){total+=n;x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x+1);y1=std::max(y1,y+1);}
  }
  if(total<qint64(line)*4)continue;
  out.append(QRect(QPoint(std::max(0,x0*cell-margin),std::max(0,y0*cell-margin)),QPoint(std::min(image.width-1,x1*cell+margin-1),std::min(image.height-1,y1*cell+margin-1))));
 }
 if(out.size()<2)return {whole};
 return out;
}
GreyImage bandImage(const GreyView& source,const Band& band,double scale,int border,const QList<Quad>& codes,int background,bool invert){
 GreyImage out;
 const int innerWidth=int(std::ceil(source.width*scale)),innerHeight=int(std::ceil((band.bottom-band.top)*scale));
 out.width=innerWidth+2*border;out.height=innerHeight+2*border;
 const uint8_t fillValue=uint8_t(std::clamp(background,0,255));
 out.pixels=QByteArray(qsizetype(out.width)*out.height,char(fillValue));
 if(scale==1){
  for(int y=0;y<innerHeight;++y)std::copy_n(source.row(band.top+y),innerWidth,out.row(y+border)+border);
 }else{
  // Bilinear, from the unscaled input; the whole scaled image never exists.
  const auto columns=static_cast<size_t>(innerWidth);
  std::vector<int> x0(columns),x1(columns),wx(columns);
  for(int x=0;x<innerWidth;++x){
   const double sx=std::clamp((x+0.5)/scale-0.5,0.0,double(source.width-1));
   x0[size_t(x)]=int(sx);x1[size_t(x)]=std::min(x0[size_t(x)]+1,source.width-1);wx[size_t(x)]=int(std::lround((sx-x0[size_t(x)])*256));
  }
  const int* c0=x0.data();const int* c1=x1.data();const int* cw=wx.data();
  auto* base=reinterpret_cast<uint8_t*>(out.pixels.data());
  for(int y=0;y<innerHeight;++y){
   const double sy=std::clamp(band.top+(y+0.5)/scale-0.5,double(band.top),double(band.bottom-1));
   const int y0=int(sy),y1=std::min(y0+1,band.bottom-1),wy=int(std::lround((sy-y0)*256));
   const uint8_t* a=source.row(y0);const uint8_t* b=source.row(y1);uint8_t* row=base+qsizetype(y+border)*out.width+border;
   for(int x=0;x<innerWidth;++x){
    const int top=a[c0[x]]*(256-cw[x])+a[c1[x]]*cw[x];
    const int bottom=b[c0[x]]*(256-cw[x])+b[c1[x]]*cw[x];
    row[x]=uint8_t((top*(256-wy)+bottom*wy+32768)>>16);
   }
  }
 }
 for(const auto& code:codes){
  const auto g=grown(code);std::array<QPointF,4> corners;
  for(int i=0;i<4;++i)corners[i]=QPointF(g[i].x()*scale+border,(g[i].y()-band.top)*scale+border);
  fill(out,corners,fillValue);
 }
 if(invert){auto* p=reinterpret_cast<uint8_t*>(out.pixels.data());for(qsizetype i=0;i<out.pixels.size();++i)p[i]=uint8_t(255-p[i]);}
 return out;
}
}
