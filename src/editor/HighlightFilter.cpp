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
int distance(QRgb a,QRgb b){
 const int dr=qRed(a)-qRed(b),dg=qGreen(a)-qGreen(b),db=qBlue(a)-qBlue(b);
 return int(std::lround(std::sqrt(double(dr*dr+dg*dg+db*db))));
}
int quantize(QRgb p){return ((qRed(p)>>3)<<10)|((qGreen(p)>>3)<<5)|(qBlue(p)>>3);}
QRgb multiply(QRgb p,QRgb m){return qRgba(qRed(p)*qRed(m)/255,qGreen(p)*qGreen(m)/255,qBlue(p)*qBlue(m)/255,qAlpha(p));}
QRgb screen(QRgb p,QRgb s){return qRgba(255-(255-qRed(p))*(255-qRed(s))/255,255-(255-qGreen(p))*(255-qGreen(s))/255,255-(255-qBlue(p))*(255-qBlue(s))/255,qAlpha(p));}
bool inside(const QList<QRect>& rects,int x,int y){for(const auto& r:rects)if(r.contains(x,y))return true;return false;}

// A marker on a light page: the hue as chosen, lifted to a pastel when it is
// too dark to leave black text readable and dimmed when it is nearly white.
QRgb lightMarker(const QColor& marker){return atLuminance(marker,std::clamp(luminance(marker.rgb()),0.3,0.7));}

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
bool inkMask(const QImage& source,const QRect& box,Mask& mask){
 const auto plan_=plan(source,box,{},QColor(250,204,21));
 if(plan_.mode!=Plan::Mode::Unmix)return false;
 const Grid grid{plan_};mask.box=box;mask.width=box.width();mask.height=box.height();mask.bits.assign(size_t(box.width())*size_t(box.height()),0);
 for(int y=0;y<box.height();++y){
  const auto* line=reinterpret_cast<const QRgb*>(source.constScanLine(box.top()+y))+box.left();
  const int blockRow=grid.block(box.left(),box.top()+y);
  for(int x=0;x<box.width();++x){int d2=0;grid.nearest(line[x],blockRow+(x/blockSize),d2);if(d2*4>=inkSquared)mask.bits[size_t(y)*size_t(box.width())+size_t(x)]=1;}
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
QRect wordOrLine(const QImage& source,const QPoint& point,bool wholeLine){
 if(!source.rect().contains(point)||source.depth()!=32)return {};
 // The line's rows come from a window around the point; its words from a
 // band of those rows wide enough for any screen line.
 const QRect window=QRect(point.x()-200,point.y()-40,400,80).intersected(source.rect());
 Mask mask;if(!inkMask(source,window,mask))return {};
 const auto lines=linesOf(mask);const int py=point.y()-window.top();
 const Line* line=nullptr;for(const auto& l:lines)if(l.top<=py&&py<=l.bottom)line=&l;
 if(!line)return {};
 const int height=line->bottom-line->top+1,pad=padFor(lines);
 // The band keeps a line of paper above and below so the background estimate holds; glyphs come from the line's rows only.
 const int lineTop=window.top()+line->top;
 const QRect band=QRect(point.x()-1500,lineTop-height,3000,3*height).intersected(source.rect());
 Mask bandMask;if(!inkMask(source,band,bandMask))return {};
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

Plan plan(const QImage& source,const QRect& requested,const QList<QRect>& redactions,const QColor& marker){
 Plan plan;const auto area=requested.intersected(source.rect());
 if(area.isEmpty()||source.depth()!=32)return plan;
 plan.area=area;plan.columns=(area.width()+blockSize-1)/blockSize;plan.rows=(area.height()+blockSize-1)/blockSize;
 // Sample on a grid so a 4K box costs about half a million reads.
 const int step=std::clamp(int(std::ceil(std::sqrt(double(area.width())*area.height()/500000.0))),1,4);
 const int blocks=plan.columns*plan.rows;
 std::vector<QRgb> blockColor(size_t(blocks),0);std::vector<uint8_t> confident(size_t(blocks),0);
 Sampler sampler;double luminanceSum=0;qint64 samples=0;int confidentBlocks=0;
 for(int by=0;by<plan.rows;++by)for(int bx=0;bx<plan.columns;++bx){
  sampler.clear();
  const int x0=area.left()+bx*blockSize,y0=area.top()+by*blockSize,x1=std::min(area.right(),x0+blockSize-1),y1=std::min(area.bottom(),y0+blockSize-1);
  for(int y=y0;y<=y1;y+=step)for(int x=x0;x<=x1;x+=step){const QRgb p=pixelAt(source,redactions,x,y);sampler.add(p);luminanceSum+=luminance(p);++samples;}
  QRgb color=0;double share=0;
  if(sampler.mode(color,share)){blockColor[size_t(by*plan.columns+bx)]=color;if(share>=0.5){confident[size_t(by*plan.columns+bx)]=1;++confidentBlocks;}}
 }
 const double meanLuminance=samples?luminanceSum/double(samples):0;
 if(confidentBlocks*2<blocks){
  // No dominant background: a photo or gradient. Multiply on light content,
  // screen a dim marker on dark content, where multiply would vanish.
  plan.mode=meanLuminance>0.35?Plan::Mode::Multiply:Plan::Mode::Screen;
  plan.fallback=meanLuminance>0.35?lightMarker(marker):atLuminance(marker,whiteFloor);
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
  if(index<0){index=int(plan.backgrounds.size());plan.backgrounds.push_back(blockColor[size_t(b)]);}
  plan.blockBackground[size_t(b)]=index;
 }
 plan.mode=Plan::Mode::Unmix;
 // Brightest ink per background, from the samples that are all ink.
 std::vector<std::vector<double>> inkLuminance(plan.backgrounds.size());
 const Grid grid{plan};
 for(int by=0;by<plan.rows;++by)for(int bx=0;bx<plan.columns;++bx){
  const int b=by*plan.columns+bx;
  const int x0=area.left()+bx*blockSize,y0=area.top()+by*blockSize,x1=std::min(area.right(),x0+blockSize-1),y1=std::min(area.bottom(),y0+blockSize-1);
  for(int y=y0;y<=y1;y+=step)for(int x=x0;x<=x1;x+=step){
   const QRgb p=pixelAt(source,redactions,x,y);int d2=0;const int k=grid.nearest(p,b,d2);
   if(d2>=inkSquared)inkLuminance[size_t(k)].push_back(luminance(p));
  }
 }
 for(size_t i=0;i<plan.backgrounds.size();++i){
  const QRgb bg=plan.backgrounds[i];const double lb=luminance(bg);
  if(lb>=0.3){plan.tinted.push_back(multiply(bg,lightMarker(marker)));continue;}
  double cap=whiteFloor;
  auto& inks=inkLuminance[i];
  if(inks.size()>=8){std::nth_element(inks.begin(),inks.begin()+qsizetype(inks.size()*98/100),inks.end());cap=std::min(cap,(inks[inks.size()*98/100]+0.05)/4.5-0.05);}
  plan.tinted.push_back(atLuminance(marker,std::max(cap,lb+visibleLift)));
 }
 return plan;
}

void apply(QImage& image,const QRect& requested,const Plan& plan){
 const auto target=requested.intersected(image.rect());
 if(plan.isNull()||target.isEmpty()||image.depth()!=32)return;
 if(plan.mode!=Plan::Mode::Unmix){
  for(int y=target.top();y<=target.bottom();++y){auto* line=reinterpret_cast<QRgb*>(image.scanLine(y));for(int x=target.left();x<=target.right();++x)line[x]=plan.mode==Plan::Mode::Multiply?multiply(line[x],plan.fallback):screen(line[x],plan.fallback);}
  return;
 }
 const Grid grid{plan};
 // Shifts per background, precomputed; coverage scales them per pixel.
 std::vector<std::array<int,3>> shift;for(size_t i=0;i<plan.backgrounds.size();++i)shift.push_back({qRed(plan.tinted[i])-qRed(plan.backgrounds[i]),qGreen(plan.tinted[i])-qGreen(plan.backgrounds[i]),qBlue(plan.tinted[i])-qBlue(plan.backgrounds[i])});
 const double sx=double(plan.area.width())/requested.width(),sy=double(plan.area.height())/requested.height();
 const bool unscaled=sx==1.0&&sy==1.0;
 for(int y=target.top();y<=target.bottom();++y){
  auto* line=reinterpret_cast<QRgb*>(image.scanLine(y));const int srcY=plan.area.top()+(unscaled?y-requested.top():int((y-requested.top())*sy));
  const int blockRow=grid.block(plan.area.left(),srcY);
  for(int x=target.left();x<=target.right();++x){
   const int srcX=unscaled?x-requested.left():int((x-requested.left())*sx);
   int d2=0;const int k=grid.nearest(line[x],blockRow+std::min(srcX/blockSize,plan.columns-1),d2);
   if(d2>=inkSquared)continue;
   const int keep=d2?inkDistance-int(std::lround(std::sqrt(double(d2)))):inkDistance;  // background share, in 1/inkDistance
   const QRgb p=line[x];
   line[x]=qRgba(std::clamp(qRed(p)+shift[size_t(k)][0]*keep/inkDistance,0,255),std::clamp(qGreen(p)+shift[size_t(k)][1]*keep/inkDistance,0,255),std::clamp(qBlue(p)+shift[size_t(k)][2]*keep/inkDistance,0,255),qAlpha(p));
  }
 }
}

QRect snapToText(const QImage& source,const QRect& requested){
 const auto box=requested.intersected(source.rect());
 if(box.isEmpty()||source.depth()!=32)return requested;
 // Look a little beyond the box so a drag through the middle of a line
 // still snaps to the whole line height.
 const auto window=box.adjusted(-8,-24,8,24).intersected(source.rect());
 if(qint64(window.width())*window.height()>maximumSnapPixels)return requested;
 Mask mask;if(!inkMask(source,window,mask))return requested;
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
QRect wordAt(const QImage& source,const QPoint& point){return wordOrLine(source,point,false);}
QRect lineAt(const QImage& source,const QPoint& point){return wordOrLine(source,point,true);}
}
