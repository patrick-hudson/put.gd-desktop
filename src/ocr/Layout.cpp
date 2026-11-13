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
    const int cut=(first+gap+1)/2;
    result.bands.append({start,cut,seam});start=cut;seam=false;
   }else{
    result.bands.append({start,limit,seam});
    start=std::max(start+1,limit-std::min(2*lineRows,bandRows/4));seam=true;
   }
  }
  return result;
 }
}
QList<Band> strips(const Profile& profile,int width,int height,int parts){
 QList<Band> out;const int lineRows=profile.lineHeight>0?profile.lineHeight:16;
 const auto blank=[&](int y){return y>=0&&y<height&&y<profile.rowInk.size()&&profile.blank(y,width);};
 int start=0;
 for(int k=1;k<parts&&height>0;++k){
  const int target=int(qint64(height)*k/parts),reach=std::max(1,height/(2*parts));
  int found=-1;
  for(int d=0;d<=reach&&found<0;++d){if(blank(target-d))found=target-d;else if(blank(target+d))found=target+d;}
  if(found<0)continue;
  int first=found,last=found;while(blank(first-1))--first;while(blank(last+1))++last;
  const int cut=(first+last+1)/2;
  if(cut-start<4*lineRows||height-cut<4*lineRows)continue;
  out.append({start,cut,false});start=cut;
 }
 out.append({start,height,false});
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
