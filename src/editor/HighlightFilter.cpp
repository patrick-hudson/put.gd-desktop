#include "HighlightFilter.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>

namespace putgd::editor::highlight {
namespace {
// sRGB byte to linear light, once.
const std::array<double,256>& linearTable(){
 static const auto table=[]{std::array<double,256> t{};for(int i=0;i<256;++i){const double c=i/255.0;t[size_t(i)]=c<=0.04045?c/12.92:std::pow((c+0.055)/1.055,2.4);}return t;}();
 return table;
}
int toSrgb(double linear){
 linear=std::clamp(linear,0.0,1.0);
 const double c=linear<=0.0031308?linear*12.92:1.055*std::pow(linear,1/2.4)-0.055;
 return int(std::lround(std::clamp(c,0.0,1.0)*255));
}
// Linear light to sRGB bytes through a table, for the per-pixel wash.
const std::vector<uint8_t>& srgbTable(){
 static const auto table=[]{std::vector<uint8_t> t(4097);for(int i=0;i<=4096;++i)t[size_t(i)]=uint8_t(toSrgb(i/4096.0));return t;}();
 return table;
}
int toSrgbFast(double linear){return srgbTable()[size_t(std::clamp(int(std::lround(linear*4096)),0,4096))];}
constexpr double washChroma=0.6;  // how far a washed pixel's colour moves toward the marker
constexpr int neutralChroma=24;   // markers with less channel spread than this are neutral
constexpr double washShade=0.5,washShadeRatio=0.6,washLighten=1.6;  // neutral washes: Y scaled by washShade (toe) or Y+0.05 by washShadeRatio; Y+0.05 by washLighten
int distance(QRgb a,QRgb b){
 const int dr=qRed(a)-qRed(b),dg=qGreen(a)-qGreen(b),db=qBlue(a)-qBlue(b);
 return int(std::lround(std::sqrt(double(dr*dr+dg*dg+db*db))));
}
int quantize(QRgb p){return ((qRed(p)>>3)<<10)|((qGreen(p)>>3)<<5)|(qBlue(p)>>3);}
bool inside(const QList<QRect>& rects,int x,int y){for(const auto& r:rects)if(r.contains(x,y))return true;return false;}
// The finished colour for every pixel luminance (12-bit, linear) on one page:
// the pixel keeps the contrast it had with its page, now against the band,
// as the first point on the band-to-black ramp that reaches it.
constexpr double kneeLow=0.04,kneeHigh=0.12;  // log contrast: below `kneeLow` is paper, above `kneeHigh` is kept exactly
std::vector<QRgb> transferTable(QRgb band,double pageLuminance){
 std::array<QRgb,256> ramp{};std::array<double,256> rampLuminance{};
 for(int a=0;a<256;++a){ramp[size_t(a)]=qRgb((qRed(band)*(255-a)+127)/255,(qGreen(band)*(255-a)+127)/255,(qBlue(band)*(255-a)+127)/255);rampLuminance[size_t(a)]=luminance(ramp[size_t(a)]);}
 const double paperL=rampLuminance[0];
 std::vector<QRgb> table(size_t(transferSteps+1));
 for(int i=0;i<=transferSteps;++i){
  const double l=double(i)/transferSteps;
  // Contrast with the page, with a knee so page noise, gradients and the
  // luminance quantisation stay paper and faint content eases in.
  double s=std::log((std::max(l,pageLuminance)+0.05)/(std::min(l,pageLuminance)+0.05));
  if(s<kneeLow)s=0;else if(s<kneeHigh){const double t=(s-kneeLow)/(kneeHigh-kneeLow);s*=t*t*(3-2*t);}
  const double wanted=(paperL+0.05)/std::exp(s)-0.05;  // ramp luminance that gives that contrast on the paper
  // Ramp luminance falls with `a`; the first entry at or below `wanted`.
  int lo=0,hi=255;
  while(lo<hi){const int mid=(lo+hi)/2;if(rampLuminance[size_t(mid)]<=wanted+1e-9)hi=mid;else lo=mid+1;}
  table[size_t(i)]=ramp[size_t(lo)];
 }
 return table;
}


struct Sampler {
 // Per quantized colour: count and channel sums, cleared through `touched`.
 std::vector<int> count=std::vector<int>(32768,0);std::vector<qint64> sum=std::vector<qint64>(32768*3,0);std::vector<int> touched;
 void clear(){for(const int k:touched){count[size_t(k)]=0;sum[size_t(k)*3]=sum[size_t(k)*3+1]=sum[size_t(k)*3+2]=0;}touched.clear();}
 void add(QRgb p){const int k=quantize(p);if(!count[size_t(k)]++)touched.push_back(k);sum[size_t(k)*3]+=qRed(p);sum[size_t(k)*3+1]+=qGreen(p);sum[size_t(k)*3+2]+=qBlue(p);}
 // The most common colour, as the mean of its members, and its share.
 bool mode(QRgb& color,double& share) const {
  int best=-1;for(const int k:touched)if(best<0||count[size_t(k)]>count[size_t(best)])best=k;
  if(best<0)return false;
  const auto n=qint64(count[size_t(best)]);qint64 total=0;for(const int k:touched)total+=count[size_t(k)];
  color=qRgb(int(sum[size_t(best)*3]/n),int(sum[size_t(best)*3+1]/n),int(sum[size_t(best)*3+2]/n));share=double(n)/double(total);return true;
 }
};

struct Grid {
 const Plan& plan;
 // Candidate backgrounds per block: its own and its eight neighbours', once.
 std::vector<std::array<int,9>> candidates;std::vector<int> counts;
 explicit Grid(const Plan& p):plan(p),candidates(size_t(p.columns*p.rows)),counts(size_t(p.columns*p.rows),0){
  for(int by=0;by<plan.rows;++by)for(int bx=0;bx<plan.columns;++bx){
   auto& out=candidates[size_t(by*plan.columns+bx)];int n=0;
   for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
    const int x=bx+dx,y=by+dy;if(x<0||y<0||x>=plan.columns||y>=plan.rows)continue;
    const int k=plan.blockBackground[size_t(y*plan.columns+x)];bool seen=false;for(int i=0;i<n;++i)seen=seen||out[size_t(i)]==k;
    if(!seen)out[size_t(n++)]=k;
   }
   counts[size_t(by*plan.columns+bx)]=n;
  }
 }
 int block(int sx,int sy) const {
  const int bx=std::clamp((sx-plan.area.left())/blockSize,0,plan.columns-1),by=std::clamp((sy-plan.area.top())/blockSize,0,plan.rows-1);
  return by*plan.columns+bx;
 }
 // The nearest candidate background to `p` and the squared distance to it.
 int nearest(QRgb p,int b,int& squared) const {
  const auto& c=candidates[size_t(b)];const int n=counts[size_t(b)];
  auto sq=[&](int k){const QRgb bg=plan.backgrounds[size_t(k)];const int dr=qRed(p)-qRed(bg),dg=qGreen(p)-qGreen(bg),db=qBlue(p)-qBlue(bg);return dr*dr+dg*dg+db*db;};
  int best=c[0];squared=sq(best);
  for(int i=1;i<n&&squared>0;++i){const int e=sq(c[size_t(i)]);if(e<squared){squared=e;best=c[size_t(i)];}}
  return best;
 }
};
constexpr int inkSquared=inkDistance*inkDistance;

QRgb pixelAt(const QImage& source,const QList<QRect>& redactions,int x,int y){return inside(redactions,x,y)?qRgb(0,0,0):reinterpret_cast<const QRgb*>(source.constScanLine(y))[x];}

// Ink mask of `box` from a plan: 1 where a pixel is at least half ink.
struct Mask {
 QRect box;int width=0,height=0;std::vector<uint8_t> bits;
 bool at(int x,int y) const {return bits[size_t(y)*size_t(width)+size_t(x)]!=0;}
};
bool inkMask(const QImage& source,const QRect& box,const QList<QRect>& redactions,Mask& mask){
 // Redacted pixels are never ink: a snapped box must not trace what a redaction hides.
 const auto plan_=plan(source,box,redactions,QColor(250,204,21));
 if(plan_.mode!=Plan::Mode::Paper)return false;
 const Grid grid{plan_};mask.box=box;mask.width=box.width();mask.height=box.height();mask.bits.assign(size_t(box.width())*size_t(box.height()),0);
 // The redactions rasterized once, so the pixel loop does not scan the list.
 std::vector<uint8_t> redacted(mask.bits.size(),0);
 for(const auto& r:redactions){const auto cut=r.intersected(box);for(int y=cut.top();y<=cut.bottom();++y)std::fill_n(redacted.begin()+ptrdiff_t((y-box.top())*box.width()+(cut.left()-box.left())),cut.width(),uint8_t(1));}
 for(int y=0;y<box.height();++y){
  const auto* line=reinterpret_cast<const QRgb*>(source.constScanLine(box.top()+y))+box.left();
  const int blockRow=grid.block(box.left(),box.top()+y);const auto* cut=redacted.data()+size_t(y)*size_t(box.width());
  for(int x=0;x<box.width();++x){if(cut[x])continue;int d2=0;grid.nearest(line[x],blockRow+(x/blockSize),d2);if(d2*4>=inkSquared)mask.bits[size_t(y)*size_t(box.width())+size_t(x)]=1;}
 }
 return true;
}

struct Line {int top,bottom;};
// Text lines from the row projection of an ink mask. A line starts where the
// fill passes 8 % of the busiest row and grows over sparse neighbouring rows
// (ascenders, descenders) by at most a quarter of its height; gaps under a
// third of the typical line merge (dots and accents).
std::vector<Line> linesOf(const Mask& mask){
 std::vector<int> rows(size_t(mask.height),0);int peak=0;
 for(int y=0;y<mask.height;++y){int n=0;for(int x=0;x<mask.width;++x)n+=mask.at(x,y);rows[size_t(y)]=n;peak=std::max(peak,n);}
 if(peak<2)return {};
 const int strong=std::max(2,int(std::lround(0.08*peak))),weak=std::max(1,int(std::lround(0.032*peak)));
 std::vector<Line> bands;int start=-1;
 for(int y=0;y<=mask.height;++y){
  const bool on=y<mask.height&&rows[size_t(y)]>=strong;
  if(on&&start<0)start=y;
  if(!on&&start>=0){bands.push_back({start,y-1});start=-1;}
 }
 if(bands.empty())return {};
 // Dense bands run from the cap line to the baseline. Accents and dots a
 // pixel or two above merge; the leading between lines is wider than that.
 std::vector<int> heights;for(const auto& b:bands)heights.push_back(b.bottom-b.top+1);
 std::nth_element(heights.begin(),heights.begin()+heights.size()/2,heights.end());const int median=heights[heights.size()/2];
 std::vector<Line> lines{bands[0]};
 for(size_t i=1;i<bands.size();++i){
  auto& last=lines.back();
  if(bands[i].top-last.bottom-1<0.15*median)last.bottom=std::max(last.bottom,bands[i].bottom);else lines.push_back(bands[i]);
 }
 // Sparse rows next to a line (ascenders, descenders) join it, by at most a
 // quarter of its height and never into the neighbouring line.
 for(size_t i=0;i<lines.size();++i){
  auto& line=lines[i];const int limit=std::max(1,int(std::lround(0.25*(line.bottom-line.top+1))));
  const int floor=i?lines[i-1].bottom+1:0,ceiling=i+1<lines.size()?lines[i+1].top-1:mask.height-1;
  for(int k=0;k<limit&&line.top>floor&&rows[size_t(line.top-1)]>=weak;++k)--line.top;
  for(int k=0;k<limit&&line.bottom<ceiling&&rows[size_t(line.bottom+1)]>=weak;++k)++line.bottom;
 }
 return lines;
}
int padFor(const std::vector<Line>& lines){
 std::vector<int> heights;for(const auto& l:lines)heights.push_back(l.bottom-l.top+1);
 std::nth_element(heights.begin(),heights.begin()+heights.size()/2,heights.end());
 return std::max(2,int(std::lround(0.2*heights[heights.size()/2])));
}
// Column extent of ink within the rows [top, bottom] of a mask.
bool columnExtent(const Mask& mask,int top,int bottom,int& left,int& right){
 left=mask.width;right=-1;
 for(int y=top;y<=bottom;++y)for(int x=0;x<mask.width;++x)if(mask.at(x,y)){left=std::min(left,x);right=std::max(right,x);}
 return right>=left;
}
// Glyph boxes of one line as 4-connected components, left to right.
std::vector<QRect> glyphs(const Mask& mask,int top,int bottom){
 std::vector<uint8_t> seen(mask.bits.size(),0);std::vector<QRect> boxes;std::deque<std::pair<int,int>> queue;
 for(int sy=top;sy<=bottom;++sy)for(int sx=0;sx<mask.width;++sx){
  const size_t i=size_t(sy)*size_t(mask.width)+size_t(sx);if(!mask.bits[i]||seen[i])continue;
  int x0=sx,x1=sx,y0=sy,y1=sy;seen[i]=1;queue.push_back({sx,sy});
  while(!queue.empty()){
   const auto [x,y]=queue.front();queue.pop_front();x0=std::min(x0,x);x1=std::max(x1,x);y0=std::min(y0,y);y1=std::max(y1,y);
   const int nx[4]={x-1,x+1,x,x},ny[4]={y,y,y-1,y+1};
   for(int k=0;k<4;++k){
    if(nx[k]<0||nx[k]>=mask.width||ny[k]<top||ny[k]>bottom)continue;
    const size_t j=size_t(ny[k])*size_t(mask.width)+size_t(nx[k]);if(mask.bits[j]&&!seen[j]){seen[j]=1;queue.push_back({nx[k],ny[k]});}
   }
  }
  boxes.push_back(QRect(QPoint(x0,y0),QPoint(x1,y1)));
 }
 std::sort(boxes.begin(),boxes.end(),[](const QRect& a,const QRect& b){return a.left()<b.left();});
 return boxes;
}
// Words of a line: glyphs merged across gaps narrower than a space, taken as
// 0.22 of the line height (letter gaps are under a tenth, a space a quarter).
// Tight bold text can still merge two words; Shift-click and drag cover that.
std::vector<QRect> words(const std::vector<QRect>& glyphs,int lineHeight){
 if(glyphs.empty())return {};
 const double threshold=std::max(2.5,0.22*lineHeight);
 std::vector<QRect> out{glyphs[0]};int reach=glyphs[0].right();
 for(size_t i=1;i<glyphs.size();++i){
  if(glyphs[i].left()-reach-1>=threshold)out.push_back(glyphs[i]);else out.back()=out.back().united(glyphs[i]);
  reach=std::max(reach,glyphs[i].right());
 }
 return out;
}
QRect wordOrLine(const QImage& source,const QPoint& point,bool wholeLine,const QList<QRect>& redactions){
 if(!source.rect().contains(point)||source.depth()!=32||inside(redactions,point.x(),point.y()))return {};
 // The line's rows come from a window around the point; its words from a
 // band of those rows wide enough for any screen line.
 const QRect window=QRect(point.x()-200,point.y()-40,400,80).intersected(source.rect());
 Mask mask;if(!inkMask(source,window,redactions,mask))return {};
 const auto lines=linesOf(mask);const int py=point.y()-window.top();
 const Line* line=nullptr;for(const auto& l:lines)if(l.top<=py&&py<=l.bottom)line=&l;
 if(!line)return {};
 const int height=line->bottom-line->top+1,pad=padFor(lines);
 // The band keeps a line of paper above and below so the background estimate holds; glyphs come from the line's rows only.
 const int lineTop=window.top()+line->top;
 const QRect band=QRect(point.x()-1500,lineTop-height,3000,3*height).intersected(source.rect());
 Mask bandMask;if(!inkMask(source,band,redactions,bandMask))return {};
 const auto parts=words(glyphs(bandMask,lineTop-band.top(),lineTop-band.top()+height-1),height);if(parts.empty())return {};
 const int px=point.x()-band.left(),slack=std::max(2,int(std::lround(0.3*height)));
 int hit=-1;for(size_t i=0;i<parts.size();++i)if(parts[i].left()-slack<=px&&px<=parts[i].right()+slack)hit=int(i);
 if(hit<0)return {};
 size_t first=size_t(hit),last=size_t(hit);
 if(wholeLine){
  // A line runs across word gaps and stops at a column gap, wider than two and a half line heights.
  const double columnGap=2.5*height;
  while(first>0&&parts[first].left()-parts[first-1].right()-1<columnGap)--first;
  while(last+1<parts.size()&&parts[last+1].left()-parts[last].right()-1<columnGap)++last;
 }
 QRect all=parts[first];for(size_t i=first;i<=last;++i)all=all.united(parts[i]);
 return QRect(band.left()+all.left()-pad,lineTop-pad,all.width()+2*pad,height+2*pad).intersected(source.rect());
}
}

double luminance(QRgb p){const auto& t=linearTable();return 0.2126*t[size_t(qRed(p))]+0.7152*t[size_t(qGreen(p))]+0.0722*t[size_t(qBlue(p))];}

QRgb atLuminance(const QColor& color,double target){
 const auto& t=linearTable();const QRgb p=color.rgb();
 double r=t[size_t(qRed(p))],g=t[size_t(qGreen(p))],b=t[size_t(qBlue(p))];
 const double l=0.2126*r+0.7152*g+0.0722*b;
 if(l<1e-4){r=g=b=1;}
 const double scale=target/(0.2126*r+0.7152*g+0.0722*b);
 return qRgb(toSrgb(r*scale),toSrgb(g*scale),toSrgb(b*scale));
}

// `color` mixed with white, as little as needed, until its luminance reaches `floor`.
QRgb whiteMix(QRgb color,double floor){
 for(int w=0;w<255;++w){
  const QRgb q=qRgb(((255-w)*qRed(color)+255*w+127)/255,((255-w)*qGreen(color)+255*w+127)/255,((255-w)*qBlue(color)+255*w+127)/255);
  if(luminance(q)>=floor)return q;
 }
 return qRgb(255,255,255);
}
// The marker's pigment: dark markers lifted with white to the floor, pale ones
// dimmed to the ceiling (hue kept), so dark lettering reads on it and it shows
// on white. Black is a mid grey, so it marks rather than redacts.
QRgb pigmentOf(const QColor& marker){
 const QRgb m=marker.rgb();
 if(std::max({qRed(m),qGreen(m),qBlue(m)})<=blackMarker)return qRgb(149,149,149);
 const double l=luminance(m);
 if(l<pigmentFloor)return whiteMix(m,pigmentFloor);
 if(l>pigmentCeiling)return atLuminance(marker,pigmentCeiling);
 return m;
}
QRgb bandOf(QRgb pigment,QRgb page){
 const QRgb paper=whiteMix(page,pageFloor);
 return qRgb((qRed(paper)*qRed(pigment)+127)/255,(qGreen(paper)*qGreen(pigment)+127)/255,(qBlue(paper)*qBlue(pigment)+127)/255);
}

// How far a light neutral marker's wash may scale Y+0.05 over `area`: every
// pixel counts here, one bright pixel the sampling grid missed would clip.
double washLightenFor(const QImage& source,const QRect& area,const QList<QRect>& redactions,const QColor& marker){
 const QRgb m=marker.rgb();
 if(std::max({qRed(m),qGreen(m),qBlue(m)})-std::min({qRed(m),qGreen(m),qBlue(m)})>neutralChroma||luminance(m)<0.5)return 1.0;
 double brightest=0;
 for(int y=area.top();y<=area.bottom();++y)for(int x=area.left();x<=area.right();++x)brightest=std::max(brightest,luminance(pixelAt(source,redactions,x,y)));
 return std::min(washLighten,1.05/(brightest+0.05));
}

Plan plan(const QImage& source,const QRect& requested,const QList<QRect>& redactions,const QColor& marker){
 Plan plan;const auto area=requested.intersected(source.rect());
 if(area.isEmpty()||source.depth()!=32)return plan;
 plan.area=area;plan.columns=(area.width()+blockSize-1)/blockSize;plan.rows=(area.height()+blockSize-1)/blockSize;
 // Sample on a grid so a 4K box costs about half a million reads.
 const int step=std::clamp(int(std::ceil(std::sqrt(double(area.width())*area.height()/500000.0))),1,4);
 const int blocks=plan.columns*plan.rows;
 std::vector<QRgb> blockColor(size_t(blocks),0);std::vector<uint8_t> confident(size_t(blocks),0);
 Sampler sampler;int confidentBlocks=0;
 for(int by=0;by<plan.rows;++by)for(int bx=0;bx<plan.columns;++bx){
  sampler.clear();
  // Every block samples a full cell inside the box: a partial last row or
  // column slides back over its neighbour, so a one-pixel strip of an icon
  // or a separator cannot pass for a background of its own.
  const int x0=std::max(area.left(),std::min(area.left()+bx*blockSize,area.right()-blockSize+1)),y0=std::max(area.top(),std::min(area.top()+by*blockSize,area.bottom()-blockSize+1));
  const int x1=std::min(area.right(),x0+blockSize-1),y1=std::min(area.bottom(),y0+blockSize-1);
  for(int y=y0;y<=y1;y+=step)for(int x=x0;x<=x1;x+=step)sampler.add(pixelAt(source,redactions,x,y));
  QRgb color=0;double share=0;
  if(sampler.mode(color,share)){blockColor[size_t(by*plan.columns+bx)]=color;if(share>=0.5){confident[size_t(by*plan.columns+bx)]=1;++confidentBlocks;}}
 }
 plan.fallback=marker.rgb();
 if(confidentBlocks*2<blocks){
  // No dominant background: a photo or gradient. A wash of the marker over
  // everything that keeps the picture's detail. A white marker may only
  // lighten as far as the brightest pixel allows, so nothing clips.
  plan.mode=Plan::Mode::Wash;plan.lighten=washLightenFor(source,area,redactions,marker);
  return plan;
 }
 // Blocks without a dominant colour (dense text, icons) borrow the nearest
 // confident block's, by breadth-first search over the grid.
 std::deque<int> queue;for(int b=0;b<blocks;++b)if(confident[size_t(b)])queue.push_back(b);
 std::vector<uint8_t> filled=confident;
 while(!queue.empty()){
  const int b=queue.front();queue.pop_front();const int bx=b%plan.columns,by=b/plan.columns;
  const int nx[4]={bx-1,bx+1,bx,bx},ny[4]={by,by,by-1,by+1};
  for(int k=0;k<4;++k){
   if(nx[k]<0||ny[k]<0||nx[k]>=plan.columns||ny[k]>=plan.rows)continue;
   const int n=ny[k]*plan.columns+nx[k];if(filled[size_t(n)])continue;
   filled[size_t(n)]=1;blockColor[size_t(n)]=blockColor[size_t(b)];queue.push_back(n);
  }
 }
 // Distinct backgrounds, merged within a small distance.
 plan.blockBackground.assign(size_t(blocks),0);
 for(int b=0;b<blocks;++b){
  int index=-1;for(size_t i=0;i<plan.backgrounds.size()&&index<0;++i)if(distance(blockColor[size_t(b)],plan.backgrounds[i])<12)index=int(i);
  if(index<0){
   if(int(plan.backgrounds.size())>=maximumBackgrounds){
    // A tiled pattern, not a page: too many backgrounds for a paper plan.
    plan.backgrounds.clear();plan.blockBackground.clear();plan.mode=Plan::Mode::Wash;plan.lighten=washLightenFor(source,area,redactions,marker);
    return plan;
   }
   index=int(plan.backgrounds.size());plan.backgrounds.push_back(blockColor[size_t(b)]);
  }
  plan.blockBackground[size_t(b)]=index;
 }
 plan.mode=Plan::Mode::Paper;const QRgb pigment=pigmentOf(marker);
 for(const QRgb bg:plan.backgrounds){const QRgb band=bandOf(pigment,bg);plan.band.push_back(band);plan.transfer.push_back(transferTable(band,luminance(bg)));}
 return plan;
}

void apply(QImage& image,const QRect& requested,const Plan& plan){
 const auto target=requested.intersected(image.rect());
 if(plan.isNull()||target.isEmpty()||image.depth()!=32)return;
 if(plan.mode==Plan::Mode::Wash){
  // A tint that keeps every pixel's luminance: the colour moves toward the
  // marker in linear light and is scaled back to its own brightness, so text
  // on a photo or a coloured label keeps its exact contrast under the wash.
  // A neutral marker (black, white, grey) has no chroma to lend, so it moves
  // the luminance instead: a black marker shades the region (luminance
  // halved, so dark detail survives), a white one lightens it (Y+0.05 scaled
  // within the region's headroom, so WCAG contrast ratios are kept).
  const QRgb m=plan.fallback;const int chroma=std::max({qRed(m),qGreen(m),qBlue(m)})-std::min({qRed(m),qGreen(m),qBlue(m)});
  const double shade=chroma>neutralChroma?1.0:luminance(m)<0.5?washShade:plan.lighten;
  const auto& lin=linearTable();const double mr=lin[size_t(qRed(m))],mg=lin[size_t(qGreen(m))],mb=lin[size_t(qBlue(m))];
  for(int y=target.top();y<=target.bottom();++y){
   auto* line=reinterpret_cast<QRgb*>(image.scanLine(y));
   for(int x=target.left();x<=target.right();++x){
    const QRgb p=line[x];const double r=lin[size_t(qRed(p))],g=lin[size_t(qGreen(p))],b=lin[size_t(qBlue(p))];
    const double y=0.2126*r+0.7152*g+0.0722*b;
    // Shading: contrast ratios kept above a toe that keeps dark detail; lightening: ratios kept, clipped at white.
    const double l=shade<1.0?std::max(y*shade,(y+0.05)*washShadeRatio-0.05):std::min(1.0,(y+0.05)*shade-0.05);
    double tr=r+(mr-r)*washChroma,tg=g+(mg-g)*washChroma,tb=b+(mb-b)*washChroma;
    const double tl=0.2126*tr+0.7152*tg+0.0722*tb;if(tl<1e-6)continue;
    const double scale=l/tl;tr*=scale;tg*=scale;tb*=scale;
    // A channel over white: pull the colour toward the grey of the same luminance until it fits.
    const double top=std::max({tr,tg,tb});
    if(top>1.0){const double k=(1.0-l)/(top-l);tr=l+(tr-l)*k;tg=l+(tg-l)*k;tb=l+(tb-l)*k;}
    line[x]=qRgba(toSrgbFast(tr),toSrgbFast(tg),toSrgbFast(tb),qAlpha(p));
   }
  }
  return;
 }
 const Grid grid{plan};const auto& linear=linearTable();
 const double sx=double(plan.area.width())/requested.width(),sy=double(plan.area.height())/requested.height();
 const bool unscaled=sx==1.0&&sy==1.0;
 for(int y=target.top();y<=target.bottom();++y){
  auto* line=reinterpret_cast<QRgb*>(image.scanLine(y));const int srcY=plan.area.top()+(unscaled?y-requested.top():int((y-requested.top())*sy));
  const int blockRow=grid.block(plan.area.left(),srcY);
  for(int x=target.left();x<=target.right();++x){
   const int srcX=unscaled?x-requested.left():int((x-requested.left())*sx);
   const QRgb p=line[x];int d2=0;const int k=grid.nearest(p,blockRow+std::min(srcX/blockSize,plan.columns-1),d2);
   // The pixel's luminance picks its colour on this page's transfer table.
   const double l=0.2126*linear[size_t(qRed(p))]+0.7152*linear[size_t(qGreen(p))]+0.0722*linear[size_t(qBlue(p))];
   const QRgb out=plan.transfer[size_t(k)][size_t(std::clamp(int(l*transferSteps+0.5),0,transferSteps))];
   line[x]=qRgba(qRed(out),qGreen(out),qBlue(out),qAlpha(p));
  }
 }
}

QRect snapToText(const QImage& source,const QRect& requested,const QList<QRect>& redactions){
 const auto box=requested.intersected(source.rect());
 if(box.isEmpty()||source.depth()!=32)return requested;
 // Look a little beyond the box so a drag through the middle of a line
 // still snaps to the whole line height.
 const auto window=box.adjusted(-8,-24,8,24).intersected(source.rect());
 if(qint64(window.width())*window.height()>maximumSnapPixels)return requested;
 Mask mask;if(!inkMask(source,window,redactions,mask))return requested;
 const auto all=linesOf(mask);std::vector<Line> lines;
 const int boxTop=box.top()-window.top(),boxBottom=box.bottom()-window.top();
 for(const auto& l:all)if(l.bottom>=boxTop&&l.top<=boxBottom)lines.push_back(l);
 if(lines.empty())return requested;
 const int pad=padFor(all);int left=0,right=0;
 // Columns are trimmed to the ink inside the dragged width only.
 Mask slice=mask;slice.width=box.width();slice.height=mask.height;slice.bits.assign(size_t(slice.width)*size_t(slice.height),0);
 const int boxLeft=box.left()-window.left();
 for(int y=0;y<mask.height;++y)for(int x=0;x<box.width();++x)slice.bits[size_t(y)*size_t(slice.width)+size_t(x)]=mask.bits[size_t(y)*size_t(mask.width)+size_t(boxLeft+x)];
 if(!columnExtent(slice,lines.front().top,lines.back().bottom,left,right))return requested;
 return QRect(QPoint(box.left()+left-pad,window.top()+lines.front().top-pad),QPoint(box.left()+right+pad,window.top()+lines.back().bottom+pad)).intersected(source.rect());
}
QRect wordAt(const QImage& source,const QPoint& point,const QList<QRect>& redactions){return wordOrLine(source,point,false,redactions);}
QRect lineAt(const QImage& source,const QPoint& point,const QList<QRect>& redactions){return wordOrLine(source,point,true,redactions);}
}
