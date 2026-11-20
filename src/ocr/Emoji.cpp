#include "Emoji.h"
#include <QtEndian>
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>
#include <new>
#include <queue>
#include <set>
#include <tuple>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace putgd::ocr {
namespace {
// A pixel is colour when its channels differ by this much, and part of a colour
// core only inside a 2x2 block of colour pixels: the one-pixel colour fringes
// of subpixel-rendered text never are.
constexpr int chromaMinimum=32;
// A channel this far from the background is ink.
constexpr int inkMinimum=28;
// A glyph's background is the commonest colour around it; below this share of
// the pixels around it the glyph is not on a flat background (a photo, a
// gradient), and text with emoji always is.
constexpr double flatShare=0.35;
// How far a box may grow past its colour core, as a share of the core's larger side.
constexpr double growShare=0.3;
quint32 u32(const uint8_t* p){return qFromLittleEndian<quint32>(p);}
// Large working buffers come straight from the kernel and go straight back. A
// large block that went through malloc would, once freed, raise glibc's mmap
// threshold for the rest of the job, and Tesseract's own buffers after this
// pass would then be served from the heap, which keeps what it is given.
template<class T> struct Pages {
 using value_type=T;
 static constexpr size_t large=64*1024;
 Pages()=default;
 template<class U> Pages(const Pages<U>&){}
 T* allocate(size_t n){
  const size_t bytes=n*sizeof(T);
  if(bytes<large)return static_cast<T*>(::operator new(bytes));
  void* p=::mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(p==MAP_FAILED)throw std::bad_alloc();
  return static_cast<T*>(p);
 }
 void deallocate(T* p,size_t n){const size_t bytes=n*sizeof(T);if(bytes<large)::operator delete(p);else ::munmap(p,bytes);}
 template<class U> bool operator==(const Pages<U>&)const{return true;}
};
template<class T> using Buffer=std::vector<T,Pages<T>>;
// The summed absolute difference of two byte runs whose length is a multiple of 16.
int differences(const uint8_t* a,const uint8_t* b,int bytes){
#if defined(__SSE2__)
 __m128i sum=_mm_setzero_si128();
 for(int i=0;i<bytes;i+=16)sum=_mm_add_epi64(sum,_mm_sad_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+i)),_mm_loadu_si128(reinterpret_cast<const __m128i*>(b+i))));
 return _mm_cvtsi128_si32(sum)+_mm_cvtsi128_si32(_mm_unpackhi_epi64(sum,sum));
#else
 int sum=0;for(int i=0;i<bytes;++i)sum+=std::abs(int(a[i])-int(b[i]));
 return sum;
#endif
}
quint16 u16(const uint8_t* p){return qFromLittleEndian<quint16>(p);}

struct Box {
 int x0=0,y0=0,x1=-1,y1=-1;  // inclusive
 int width()const{return x1-x0+1;}
 int height()const{return y1-y0+1;}
 int side()const{return std::max(width(),height());}
 bool operator<(const Box& o)const{return std::tie(x0,y0,x1,y1)<std::tie(o.x0,o.y0,o.x1,o.y1);}
 bool operator==(const Box& o)const{return x0==o.x0&&y0==o.y0&&x1==o.x1&&y1==o.y1;}
};
struct Colour { double r=255,g=255,b=255; };

bool colour(const uint8_t* p){return std::max({p[0],p[1],p[2]})-std::min({p[0],p[1],p[2]})>=chromaMinimum;}

// Connected colour cores of an area, streamed row by row: runs of core pixels,
// joined to the runs above them (8-connected) by union-find.
struct Seeds {
 Buffer<int> parent;
 Buffer<Box> box;
 Buffer<int> pixels;
 int root(int a){while(parent[a]!=a){parent[a]=parent[parent[a]];a=parent[a];}return a;}
 void join(int a,int b){
  a=root(a);b=root(b);if(a==b)return;if(a>b)std::swap(a,b);
  parent[b]=a;auto& k=box[a];const auto& o=box[b];
  k={std::min(k.x0,o.x0),std::min(k.y0,o.y0),std::max(k.x1,o.x1),std::max(k.y1,o.y1)};pixels[a]+=pixels[b];
 }
};
// isColour says which pixels count; the whole image uses colour(), a flat
// coloured region its own (colour, and not the region's colour). Stops, with
// what it has, when stop says so or there are more than `limit` runs. Lets
// rows go behind it only for the whole image (`release`).
template<class IsColour>
bool scanSeeds(const ColourView& image,const Box& area,const IsColour& isColour,int limit,const std::function<bool()>& stop,bool release,Buffer<Box>& out){
 const int w=area.width(),h=area.height();
 Buffer<uint8_t> above(w),below(w),blockAbove(w,0),block(w,0),core(w);
 const auto saturate=[&](int y,Buffer<uint8_t>& row){const auto* p=image.row(area.y0+y)+3*area.x0;for(int x=0;x<w;++x)row[x]=isColour(p+3*x);};
 struct Run { int x0,x1,label; };
 Buffer<Run> previous,current;Seeds seeds;bool complete=true;int released=0;
 saturate(0,above);
 for(int y=0;y<h;++y){
  if(stop&&y%64==0&&stop()){complete=false;break;}
  if(y+1<h)saturate(y+1,below);else std::fill(below.begin(),below.end(),0);
  for(int x=0;x+1<w;++x)block[x]=above[x]&above[x+1]&below[x]&below[x+1];
  block[w-1]=0;
  for(int x=0;x<w;++x)core[x]=blockAbove[x]|block[x]|(x>0?blockAbove[x-1]|block[x-1]:0);
  current.clear();
  for(int x=0;x<w;){
   if(!core[x]){++x;continue;}
   const int start=x;while(x<w&&core[x])++x;
   const int label=int(seeds.parent.size());
   seeds.parent.push_back(label);seeds.box.push_back({area.x0+start,area.y0+y,area.x0+x-1,area.y0+y});seeds.pixels.push_back(x-start);
   current.push_back({start,x-1,label});
  }
  // Two pointers over the runs above: 8-connected when they overlap or touch diagonally.
  for(size_t i=0,j=0;i<current.size();++i){
   while(j<previous.size()&&previous[j].x1<current[i].x0-1)++j;
   for(size_t k=j;k<previous.size()&&previous[k].x0<=current[i].x1+1;++k)seeds.join(current[i].label,previous[k].label);
  }
  std::swap(previous,current);std::swap(above,below);std::swap(blockAbove,block);
  if(int(seeds.parent.size())>limit){complete=false;break;}
  // Rows above y are finished with once y+1 has been read.
  if(release&&image.done&&y-released>=64){image.done(area.y0+released,area.y0+y);released=y;}
 }
 if(release&&image.done)image.done(area.y0+released,area.y0+h);
 for(int i=0;i<int(seeds.parent.size());++i)if(seeds.parent[i]==i&&seeds.pixels[i]>=4)out.push_back(seeds.box[i]);
 return complete;
}

// Seeds inside one glyph (a face and its tears, a cone and its confetti) become
// one: the larger seeds first, a smaller one joining a group whose box, grown by
// 15% of its side, holds it. A spatial grid keeps it near linear.
Buffer<Box> group(Buffer<Box> seeds,int width,int height,const std::function<bool()>& stop,bool& cut){
 std::sort(seeds.begin(),seeds.end(),[](const Box& a,const Box& b){return qint64(a.width())*a.height()>qint64(b.width())*b.height();});
 constexpr int cell=64;
 const int columns=width/cell+1,rows=height/cell+1;
 std::vector<std::vector<int>> grid(size_t(columns)*rows);
 const auto at=[&](int gx,int gy)->std::vector<int>*{return gx<0||gy<0||gx>=columns||gy>=rows?nullptr:&grid[size_t(gy)*columns+gx];};
 Buffer<Box> groups;
 const auto enter=[&](int index){const auto& g=groups[size_t(index)];for(int gy=g.y0/cell;gy<=g.y1/cell;++gy)for(int gx=g.x0/cell;gx<=g.x1/cell;++gx)if(auto* c=at(gx,gy))c->push_back(index);};
 for(size_t n=0;n<seeds.size();++n){
  if(stop&&n%4096==0&&stop()){cut=true;break;}
  const auto& s=seeds[n];
  const int cx=(s.x0+s.x1)/2/cell,cy=(s.y0+s.y1)/2/cell;int joined=-1;
  for(int gy=cy-1;gy<=cy+1&&joined<0;++gy)for(int gx=cx-1;gx<=cx+1&&joined<0;++gx){
   const auto* found=at(gx,gy);if(!found)continue;
   for(const int index:*found){
    const auto& g=groups[size_t(index)];const int m=std::max(1,int(0.15*g.side()));
    if(s.x0>=g.x0-m&&s.x1<=g.x1+m&&s.y0>=g.y0-m&&s.y1<=g.y1+m){joined=index;break;}
   }
  }
  // The group takes the part's box too, so a part across a blank row or
  // column is still inside the glyph's box.
  if(joined>=0){auto& g=groups[size_t(joined)];g={std::min(g.x0,s.x0),std::min(g.y0,s.y0),std::max(g.x1,s.x1),std::max(g.y1,s.y1)};enter(joined);continue;}
  groups.push_back(s);enter(int(groups.size())-1);
 }
 return groups;
}

// The commonest colour (to 4 bits a channel) around a box, outside its colour
// core, and that colour's share of those pixels.
Colour background(const ColourView& image,const Box& b,double& share){
 const int s=b.side();
 const int x0=std::max(0,b.x0-s),y0=std::max(0,b.y0-s),x1=std::min(image.width-1,b.x1+s),y1=std::min(image.height-1,b.y1+s);
 const auto around=[&](auto&& each){
  for(int y=y0;y<=y1;++y){
   const auto* row=image.row(y);
   for(int x=x0;x<=x1;++x){const auto* p=row+3*x;if(!(x>=b.x0&&x<=b.x1&&y>=b.y0&&y<=b.y1&&colour(p)))each(p,(p[0]>>4)<<8|(p[1]>>4)<<4|(p[2]>>4));}
  }
 };
 thread_local std::vector<int> count(4096,0);qint64 total=0;int mode=0;
 around([&](const uint8_t*,int key){if(++count[key]>count[mode])mode=key;++total;});
 share=total>0?double(count[mode])/double(total):0;
 qint64 sum[3]={0,0,0},n=0;
 around([&](const uint8_t* p,int key){count[key]=0;if(key==mode){sum[0]+=p[0];sum[1]+=p[1];sum[2]+=p[2];++n;}});
 count[mode]=0;
 if(n==0)return {};
 return {double(sum[0])/n,double(sum[1])/n,double(sum[2])/n};
}
bool ink(const uint8_t* p,const Colour& bg){return std::abs(p[0]-bg.r)>inkMinimum||std::abs(p[1]-bg.g)>inkMinimum||std::abs(p[2]-bg.b)>inkMinimum;}
// The commonest colour (to 4 bits a channel) inside a box, from every other
// pixel of every other row, and its share of them.
Colour regionColour(const ColourView& image,const Box& b,double& share){
 thread_local std::vector<int> count(4096,0);qint64 total=0;int mode=0;
 const auto each=[&](auto&& visit){for(int y=b.y0;y<=b.y1;y+=2){const auto* row=image.row(y);for(int x=b.x0;x<=b.x1;x+=2){const auto* p=row+3*x;visit(p,(p[0]>>4)<<8|(p[1]>>4)<<4|(p[2]>>4));}}};
 each([&](const uint8_t*,int key){if(++count[key]>count[mode])mode=key;++total;});
 share=total>0?double(count[mode])/double(total):0;
 qint64 sum[3]={0,0,0},n=0;
 each([&](const uint8_t* p,int key){count[key]=0;if(key==mode){sum[0]+=p[0];sum[1]+=p[1];sum[2]+=p[2];++n;}});
 count[mode]=0;
 if(n==0)return {};
 return {double(sum[0])/n,double(sum[1])/n,double(sum[2])/n};
}
// Grows a colour core to the ink touching it, one row or column at a time,
// at most growShare of its side in any direction.
Box grow(const ColourView& image,Box b,const Colour& bg){
 const int limit=int(growShare*b.side())+2;
 const int lx0=std::max(0,b.x0-limit),ly0=std::max(0,b.y0-limit),lx1=std::min(image.width-1,b.x1+limit),ly1=std::min(image.height-1,b.y1+limit);
 const auto column=[&](int x){for(int y=b.y0;y<=b.y1;++y)if(ink(image.row(y)+3*x,bg))return true;return false;};
 const auto row=[&](int y){const auto* r=image.row(y);for(int x=b.x0;x<=b.x1;++x)if(ink(r+3*x,bg))return true;return false;};
 for(bool changed=true;changed;){
  changed=false;
  if(b.x0>lx0&&column(b.x0-1)){--b.x0;changed=true;}
  if(b.x1<lx1&&column(b.x1+1)){++b.x1;changed=true;}
  if(b.y0>ly0&&row(b.y0-1)){--b.y0;changed=true;}
  if(b.y1<ly1&&row(b.y1+1)){++b.y1;changed=true;}
 }
 return b;
}

// Area-averaging weights from m pixels to n: for each output pixel, the input
// pixels it covers and how much of each.
struct Taps { std::vector<int> first,count;std::vector<float> weight; };
const Taps& taps(int n,int m){
 thread_local std::map<std::pair<int,int>,Taps> cache;
 auto& t=cache[{n,m}];
 if(!t.first.empty()||n<=0)return t;
 for(int i=0;i<n;++i){
  const double start=double(i)*m/n,end=double(i+1)*m/n;
  const int a=int(start),b=std::min(m,int(std::ceil(end)));
  t.first.push_back(a);t.count.push_back(b-a);
  for(int j=a;j<b;++j)t.weight.push_back(float((std::min(end,double(j+1))-std::max(start,double(j)))*n/m));
 }
 return t;
}
// Resamples a w x h image of `channels` floats to tw x th.
Buffer<float> resample(const float* in,int w,int h,int channels,int tw,int th){
 const auto& tx=taps(tw,w);const auto& ty=taps(th,h);
 Buffer<float> across(size_t(h)*tw*channels,0.f),out(size_t(th)*tw*channels,0.f);
 for(int y=0;y<h;++y)for(int i=0,k=0;i<tw;++i)for(int c=0;c<tx.count[i];++c,++k){
  const float wgt=tx.weight[k];const float* p=in+(size_t(y)*w+tx.first[i]+c)*channels;float* o=&across[(size_t(y)*tw+i)*channels];
  for(int ch=0;ch<channels;++ch)o[ch]+=wgt*p[ch];
 }
 for(int j=0,k=0;j<th;++j)for(int c=0;c<ty.count[j];++c,++k){
  const float wgt=ty.weight[k];const float* row=&across[size_t(ty.first[j]+c)*tw*channels];float* o=&out[size_t(j)*tw*channels];
  for(int i=0;i<tw*channels;++i)o[i]+=wgt*row[i];
 }
 return out;
}
// The image inside a box as floats 0 to 255, with the background outside the image.
Buffer<float> crop(const ColourView& image,const Box& b,const Colour& bg){
 Buffer<float> out(size_t(b.width())*b.height()*3);
 for(int y=b.y0,i=0;y<=b.y1;++y)for(int x=b.x0;x<=b.x1;++x,i+=3){
  if(x<0||y<0||x>=image.width||y>=image.height){out[i]=float(bg.r);out[i+1]=float(bg.g);out[i+2]=float(bg.b);continue;}
  const auto* p=image.row(y)+3*x;out[i]=p[0];out[i+1]=p[1];out[i+2]=p[2];
 }
 return out;
}

// The share of a box's ink that is colour.
double colourShare(const ColourView& image,const Box& b,const Colour& bg){
 qint64 inked=0,coloured=0;
 for(int y=std::max(0,b.y0);y<=std::min(image.height-1,b.y1);++y){
  const auto* row=image.row(y);
  for(int x=std::max(0,b.x0);x<=std::min(image.width-1,b.x1);++x){const auto* p=row+3*x;if(!ink(p,bg))continue;++inked;coloured+=colour(p);}
 }
 return inked>0?double(coloured)/double(inked):0;
}

class Matcher {
public:
 Matcher(const ColourView& image,const EmojiTemplates& templates,const EmojiOptions& options):image_(image),t_(templates),o_(options){}
 struct Fit { double distance=9;int index=-1;Box box; };
 // A template's mean colour distance from bg (0 to 1) over its opaque pixels.
 double contrast(int t,const Colour& bg)const{
  const int n=t_.side()*t_.side();const uint8_t* p=t_.pixels(t);double sum=0,weight=0;
  for(int i=0;i<n;++i){
   const double a=p[4*i+3]/255.0;
   sum+=std::max({std::abs(p[4*i]-a*bg.r),std::abs(p[4*i+1]-a*bg.g),std::abs(p[4*i+2]-a*bg.b)})/255;weight+=a;
  }
  return weight>0?sum/weight:0;
 }
 // Every template's coarse pixels laid on one background, as RGB bytes padded
 // with zeros to whole 16-byte blocks, so the first pass only subtracts bytes.
 // Kept for the last two backgrounds met (a page has one or two), about 2 MiB each.
 int laidBytes()const{return (t_.coarseSide()*t_.coarseSide()*3+15)/16*16;}
 const Buffer<uint8_t>& laidOn(const Colour& bg)const{
  const int key=(int(bg.r)>>2)<<12|(int(bg.g)>>2)<<6|(int(bg.b)>>2);
  for(auto& laid:laid_)if(laid.key==key)return laid.rgb;
  auto& laid=laid_[nextLaid_];nextLaid_=(nextLaid_+1)%int(laid_.size());laid.key=key;
  const int n=t_.coarseSide()*t_.coarseSide(),stride=laidBytes();laid.rgb.assign(size_t(t_.count())*stride,0);
  const auto under=undersOf(bg);
  for(int t=0;t<t_.count();++t){
   const uint8_t* p=t_.coarse(t);uint8_t* o=laid.rgb.data()+size_t(t)*stride;
   for(int i=0;i<n;++i){const int a=p[4*i+3];for(int c=0;c<3;++c)o[3*i+c]=uint8_t(std::min(255,p[4*i+c]+under[c][a]));}
  }
  return laid.rgb;
 }
 // (255 - a) * bg / 255 for each alpha a: what a template pixel lets through.
 static std::array<std::array<int,256>,3> undersOf(const Colour& bg){
  std::array<std::array<int,256>,3> under{};
  for(int a=0;a<256;++a){under[0][a]=int(std::lround((255-a)*bg.r/255));under[1][a]=int(std::lround((255-a)*bg.g/255));under[2][a]=int(std::lround((255-a)*bg.b/255));}
  return under;
 }
 // The templates closest to the box stretched to their size, laid on bg:
 // first every template whose colour share is near the glyph's at the coarse
 // size, then the closest of those at the full size. Summed absolute
 // differences, abandoned once over the worst one kept. Returns the closest
 // full-size template's mean difference (0 to 1).
 double shortlist(const Box& b,const Colour& bg,double share,int keep,std::set<int>& out)const{
  if(keep<1)return 9;
  const auto c=crop(image_,b,bg);
  const auto target=[&](int side){
   const auto small=resample(c.data(),b.width(),b.height(),3,side,side);
   Buffer<uint8_t> want(small.size());for(size_t i=0;i<small.size();++i)want[i]=uint8_t(std::clamp(std::lround(small[i]),0L,255L));
   return want;
  };
  using Kept=std::priority_queue<std::pair<int,int>>;  // largest kept sum on top
  const auto take=[](Kept& best,int sum,int t,int count){best.push({sum,t});if(int(best.size())>count)best.pop();};
  // Coarse: bytes against the templates laid on bg.
  const int stride=laidBytes(),coarseKeep=std::max(1,o_.coarseShortlist);auto small=target(t_.coarseSide());small.resize(size_t(stride),0);const auto& laid=laidOn(bg);
  Kept coarse;
  for(int t=0;t<t_.count();++t){
   if(std::abs(t_.colour(t)-share)>o_.colourShare)continue;
   const int sum=differences(laid.data()+size_t(t)*stride,small.data(),stride);
   if(int(coarse.size())<coarseKeep||sum<coarse.top().first)take(coarse,sum,t,coarseKeep);
  }
  // Full size, for those.
  const int fs=t_.side(),fn=fs*fs;const auto big=target(fs);const auto under=undersOf(bg);
  Kept full;
  while(!coarse.empty()){
   const int t=coarse.top().second;coarse.pop();
   const uint8_t* p=t_.pixels(t);const int bound=int(full.size())<keep?INT_MAX:full.top().first;int sum=0;
   for(int i=0;i<fn&&sum<bound;++i){
    const int a=p[4*i+3];
    sum+=std::abs(p[4*i]+under[0][a]-big[3*i])+std::abs(p[4*i+1]+under[1][a]-big[3*i+1])+std::abs(p[4*i+2]+under[2][a]-big[3*i+2]);
   }
   if(sum<bound)take(full,sum,t,keep);
  }
  int closest=INT_MAX;
  while(!full.empty()){closest=std::min(closest,full.top().first);out.insert(full.top().second);full.pop();}
  return closest==INT_MAX?9:double(closest)/(double(fn)*3*255);
 }
 // The template drawn at the box's own size on bg against the image, mean
 // absolute difference (0 to 1) over the pixels either of them inks.
 double native(int t,const Box& b,const Colour& bg)const{
  const int w=b.width(),h=b.height(),s=t_.side();
  if(w<4||h<4)return 9;
  // Each template's pixels as floats once per glyph.
  auto& master=masters_[t];
  if(master.empty()){master.resize(size_t(s)*s*4);const uint8_t* p=t_.pixels(t);for(int i=0;i<s*s*4;++i)master[i]=p[i];}
  const auto drawn=resample(master.data(),s,s,4,w,h);
  double sum=0,weights=0;const double bgc[3]={bg.r,bg.g,bg.b};
  for(int y=0,i=0;y<h;++y)for(int x=0;x<w;++x,++i){
   const int ix=b.x0+x,iy=b.y0+y;const bool inside=ix>=0&&iy>=0&&ix<image_.width&&iy<image_.height;
   const uint8_t* q=inside?image_.row(iy)+3*ix:nullptr;
   const double a=drawn[4*i+3]/255.0;double diff=0;bool inked=false;
   for(int ch=0;ch<3;++ch){
    const double have=inside?q[ch]:bgc[ch];
    diff+=std::abs(drawn[4*i+ch]+(1-a)*bgc[ch]-have);
    inked|=std::abs(have-bgc[ch])>0.1*255;
   }
   const double weight=std::max(a,inked?1.0:0.0);
   sum+=weight*diff/3;weights+=weight;
  }
  return weights>0?sum/weights/255:9;
 }
 // The boxes a template could stand in: the grown box, and the box its own
 // aspect gives from the box's height (anchored left, right or centred) or
 // from its width (top, bottom or centred).
 std::vector<Box> shapes(int t,const Box& b)const{
  const double aspect=t_.aspect(t);
  const int pw=std::max(4,int(std::lround(aspect*b.height()))),ph=std::max(4,int(std::lround(b.width()/aspect)));
  // The grown box itself only when it has about the template's shape.
  std::vector<Box> out;
  if(std::abs(std::log(double(b.width())/b.height()/aspect))<0.2)out.push_back(b);
  for(const int x0:{b.x0,b.x1-pw+1,(b.x0+b.x1+1)/2-pw/2})out.push_back({x0,b.y0,x0+pw-1,b.y1});
  for(const int y0:{b.y0,b.y1-ph+1,(b.y0+b.y1+1)/2-ph/2})out.push_back({b.x0,y0,b.x1,y0+ph-1});
  std::sort(out.begin(),out.end());out.erase(std::unique(out.begin(),out.end()),out.end());
  return out;
 }
 Fit best(const Box& b,const Colour& bg)const{
  // The float copies are kept for one glyph only, so a scan's memory stays small.
  masters_.clear();
  const double share=colourShare(image_,b,bg);
  std::set<int> list;double stretched=shortlist(b,bg,share,o_.shortlist,list);
  // A box much wider than tall can hold a glued letter or a second glyph: its
  // two ends, squared, are shortlisted too.
  if(b.width()>1.15*b.height()){
   const Box left{b.x0,b.y0,b.x0+b.height()-1,b.y1},right{b.x1-b.height()+1,b.y0,b.x1,b.y1};
   stretched=std::min(stretched,shortlist(left,bg,colourShare(image_,left,bg),std::max(1,o_.shortlist/2),list));
   stretched=std::min(stretched,shortlist(right,bg,colourShare(image_,right,bg),std::max(1,o_.shortlist/2),list));
  }
  lastStretched_=stretched;
  // Nothing close even stretched: not worth drawing each at the glyph's size.
  if(stretched>o_.stretchedDistance)return {};
  std::vector<Fit> fits;
  for(const int t:list){
   Fit mine;
   for(const auto& shape:shapes(t,b)){const double d=native(t,shape,bg);if(d<mine.distance)mine={d,t,shape};}
   if(mine.index>=0)fits.push_back(mine);
  }
  // The closest four, one pixel on each edge either way around their best shape.
  std::sort(fits.begin(),fits.end(),[](const Fit& a,const Fit& c){return a.distance<c.distance||(a.distance==c.distance&&a.index<c.index);});
  if(fits.size()>4)fits.resize(4);
  Fit fit;
  for(auto mine:fits){
   const Box centre=mine.box;
   for(int edge=0;edge<4;++edge)for(const int step:{-1,1}){
    Box moved=centre;(edge==0?moved.x0:edge==1?moved.y0:edge==2?moved.x1:moved.y1)+=step;
    const double d=native(mine.index,moved,bg);if(d<mine.distance)mine={d,mine.index,moved};
   }
   if(mine.distance<fit.distance||(mine.distance==fit.distance&&mine.index<fit.index))fit=mine;
  }
  return fit;
 }
private:
 const ColourView& image_;
 const EmojiTemplates& t_;
 const EmojiOptions& o_;
 mutable std::map<int,std::vector<float>> masters_;
 struct Laid { int key=-1;Buffer<uint8_t> rgb; };
 mutable std::array<Laid,2> laid_;
 mutable int nextLaid_=0;
public:
 mutable double lastStretched_=9;   // measurements: the closest stretched template of the last glyph
};
}

bool EmojiTemplates::load(const uint8_t* data,qsizetype bytes){
 *this={};
 if(!data||bytes<48||std::memcmp(data,"PGEM",4)!=0||u32(data+4)!=1||u32(data+36)!=0||u32(data+40)!=0||u32(data+44)!=0)return false;
 const qint64 count=u32(data+8),side=u32(data+12),coarse=u32(data+16),stringOffset=u32(data+20),stringBytes=u32(data+24),coarseOffset=u32(data+28),pixelOffset=u32(data+32);
 if(count<=0||count>100000||side<4||side>64||coarse<2||coarse>side)return false;
 if(stringOffset!=48+12*count||stringOffset+stringBytes>coarseOffset||coarseOffset+count*coarse*coarse*4>pixelOffset||pixelOffset+count*side*side*4!=bytes)return false;
 for(qint64 i=0;i<count;++i){
  const auto* entry=data+48+12*i;
  if(entry[4]==0||qint64(u32(entry))+entry[4]>stringBytes||u16(entry+6)==0||entry[9]>1||u16(entry+10)!=0)return false;
 }
 data_=data;entries_=data+48;strings_=data+stringOffset;coarse_=data+coarseOffset;pixels_=data+pixelOffset;
 count_=int(count);side_=int(side);coarseSide_=int(coarse);stringBytes_=stringBytes;bytes_=bytes;
 return true;
}
void EmojiTemplates::release()const{
 if(!mapped_||!data_)return;
 // Whole pages inside the data only; they are the executable's, so dropping
 // them loses nothing.
 const qint64 page=::sysconf(_SC_PAGESIZE);if(page<=0)return;
 const auto start=reinterpret_cast<quintptr>(data_),end=start+quintptr(bytes_);
 const quintptr first=(start+quintptr(page)-1)/quintptr(page)*quintptr(page),last=end/quintptr(page)*quintptr(page);
 if(last>first)::madvise(reinterpret_cast<void*>(first),size_t(last-first),MADV_DONTNEED);
}
QString EmojiTemplates::text(int index)const{const auto* e=entries_+12*index;return QString::fromUtf8(reinterpret_cast<const char*>(strings_+u32(e)),e[4]);}
int EmojiTemplates::style(int index)const{return entries_[12*index+5];}
double EmojiTemplates::aspect(int index)const{return u16(entries_+12*index+6)/1024.0;}
double EmojiTemplates::colour(int index)const{return entries_[12*index+8]/255.0;}
bool EmojiTemplates::shape(int index)const{return entries_[12*index+9]&1;}

EmojiScan findEmoji(const ColourView& image,const EmojiTemplates& templates,const EmojiOptions& options,const std::function<bool()>& stop){
 EmojiScan scan;
 if(!image.valid()||templates.count()==0)return scan;
 constexpr int runLimit=1<<20;
 Buffer<Box> found0;
 if(!scanSeeds(image,{0,0,image.width-1,image.height-1},colour,runLimit,stop,true,found0))scan.cut=true;
 // A flat coloured region (a chat bubble, a coloured panel) is one large
 // core that would swallow the emoji on it: inside it, colour means colour
 // that is not the region's own.
 Buffer<Box> seeds;
 for(const auto& seed:found0){
  if(seed.side()<=options.maximumSize){seeds.push_back(seed);continue;}
  if(scan.cut||(stop&&stop())){scan.cut=true;continue;}
  // Flat as a chat bubble is: three quarters or more of it one colour.
  double share=0;const auto flat=regionColour(image,seed,share);
  if(share<0.75)continue;
  const auto inside=[&flat](const uint8_t* p){return colour(p)&&ink(p,flat);};
  if(!scanSeeds(image,seed,inside,runLimit,stop,false,seeds))scan.cut=true;
  if(image.done)image.done(seed.y0,seed.y1+1);
 }
 struct Candidate { Box box;Colour bg; };
 Buffer<Candidate> candidates;
 // Rows read around a glyph go again once it is done with, so the colour
 // plane never comes back whole.
 const auto letGo=[&](const Box& b,int margin){if(image.done)image.done(std::max(0,b.y0-margin),std::min(image.height,b.y1+margin+1));};
 for(const auto& g:group(std::move(seeds),image.width,image.height,stop,scan.cut)){
  if(scan.cut&&stop&&stop())break;
  if(g.side()>options.maximumSize||g.side()<2)continue;
  double share=0;const auto bg=background(image,g,share);
  if(share<flatShare){letGo(g,g.side());continue;}
  const auto box=grow(image,g,bg);
  letGo(g,g.side());
  if(box.side()<options.minimumSize||box.side()>options.maximumSize)continue;
  if(int(candidates.size())>=options.maximumCandidates){scan.cut=true;break;}
  candidates.push_back({box,bg});
 }
 Matcher matcher(image,templates,options);
 struct Found { Box box;double distance;int index; };
 Buffer<Found> found;
 for(const auto& candidate:candidates){
  if(stop&&stop()){scan.cut=true;break;}
  ++scan.candidates;
  if(options.keepCandidates){
   const Box& b=candidate.box;qint64 coloured=0;
   for(int y=b.y0;y<=b.y1;++y)for(int x=b.x0;x<=b.x1;++x)coloured+=colour(image.row(y)+3*x);
   scan.examined.append({QRect(QPoint(b.x0,b.y0),QPoint(b.x1,b.y1)),double(coloured)/(double(b.width())*b.height()),9});
  }
  // A box wide enough for glyphs that touch: what is left on either side of
  // a named emoji, if it could hold another, is matched again.
  std::vector<Box> pending{candidate.box};int named=0;bool first=true;
  while(!pending.empty()&&named<4){
   const Box rest=pending.back();pending.pop_back();
   const auto fit=matcher.best(rest,candidate.bg);
   if(options.keepCandidates&&first){scan.examined.last().distance=fit.distance;scan.examined.last().stretched=matcher.lastStretched_;}
   first=false;
   if(fit.index<0||fit.distance>options.distance||fit.box.side()<options.minimumSize)continue;
   if(fit.distance>options.relativeDistance*matcher.contrast(fit.index,candidate.bg))continue;
   found.push_back({fit.box,fit.distance,fit.index});++named;
   for(const Box side:{Box{rest.x0,rest.y0,fit.box.x0-1,rest.y1},Box{fit.box.x1+1,rest.y0,rest.x1,rest.y1}})
    if(side.width()>=0.6*side.height()&&side.side()>=options.minimumSize)pending.push_back(side);
  }
  letGo(candidate.box,candidate.box.side());
 }
 // One glyph reached from two seeds: the closer fit stays.
 std::sort(found.begin(),found.end(),[](const Found& a,const Found& b){return a.distance<b.distance;});
 Buffer<Found> kept;
 for(const auto& f:found){
  bool overlaps=false;
  for(const auto& k:kept){
   const int ix=std::min(f.box.x1,k.box.x1)-std::max(f.box.x0,k.box.x0)+1,iy=std::min(f.box.y1,k.box.y1)-std::max(f.box.y0,k.box.y0)+1;
   if(ix>0&&iy>0&&qint64(ix)*iy>0.3*std::min(qint64(f.box.width())*f.box.height(),qint64(k.box.width())*k.box.height())){overlaps=true;break;}
  }
  if(!overlaps)kept.push_back(f);
 }
 // Top to bottom, then left to right: a fixed key, so the order is a strict one.
 std::sort(kept.begin(),kept.end(),[](const Found& a,const Found& b){return std::tie(a.box.y0,a.box.x0)<std::tie(b.box.y0,b.box.x0);});
 // The colour rows read again for the glyphs go too.
 if(image.done)image.done(0,image.height);
 for(const auto& k:kept){
  if(templates.shape(k.index))continue;
  const QRect rect(QPoint(std::max(0,k.box.x0),std::max(0,k.box.y0)),QPoint(std::min(image.width-1,k.box.x1),std::min(image.height-1,k.box.y1)));
  scan.hits.append({templates.text(k.index),rect,k.distance,templates.style(k.index)});
 }
 return scan;
}
}
