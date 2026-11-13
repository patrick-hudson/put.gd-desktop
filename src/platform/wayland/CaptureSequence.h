#pragma once
#include "core/SourceAdapter.h"
#include "platform/Geometry.h"
#include <QPointer>
#include <QVariantMap>
#include <functional>
#include <memory>
namespace putgd {
// Steps of a capture shared by the Wayland adapters that read outputs in the
// resident (Plasma, Budgie, Hyprland). Each adapter befriends this struct; the
// steps use its generation_, unchanged(), lockGate(), captureScreens() and
// remembering().
struct CaptureSequence {
 // After a live selection: the delay's lock check (#1363), then the frozen
 // layout, then the pixels of the outputs the selection touches. The helper
 // reports geometry only after compositor unmap confirmation and normal exit,
 // and the selection may have finished before this continuation runs. Pixel
 // transfer remains in the resident's own connection.
 template<class Adapter>
 static std::function<void(QRectF,Error)> afterSelection(QPointer<Adapter> guard,QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,quint64 generation,int delay,bool remember,std::function<void(PayloadResult)> cb){
  return [guard,frozen,revision,limits,generation,delay,remember,cb=std::move(cb)](QRectF selection,Error error)mutable{
   if(!guard||guard->generation_!=generation){cb({{},{"cancelled","Desktop capture cancelled",{}}});return;}
   if(error){cb({{},std::move(error)});return;}
   guard->lockGate(delay,[guard,frozen,revision,limits,generation,selection,remember,cb=std::move(cb)](Error locked)mutable{
    if(!guard||guard->generation_!=generation){cb({{},{"cancelled","Desktop capture cancelled",{}}});return;}
    if(locked){cb({{},std::move(locked)});return;}
    if(!guard->unchanged(frozen,revision)){cb({{},{"display_changed","Display layout changed during region selection",{}}});return;}
    guard->captureScreens(frozen,revision,selection,limits,guard->remembering(selection,frozen,std::move(cb),remember));
   });
  };
 }
 // One output's image and, from KWin, its metadata.
 using FrameReply=std::function<void(QImage,QVariantMap,Error)>;
 // Reads outputs one at a time in the given order, each against the image budget
 // the earlier outputs left (#1444). frames: the outputs to read; frozen: the
 // whole layout, which must not change. read(frame,remaining,reply) fetches one
 // output; accept(frame,image,metadata,remaining) checks the image against the
 // frozen output and the budget left and stores it in frame, or says why not.
 template<class Adapter,class Read,class Accept>
 static void readFrames(QPointer<Adapter> guard,QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,Read read,Accept accept,std::function<void(QList<CaptureFrame>,Error)> cb){
  struct Batch {QList<CaptureFrame> frames,frozenLayout;qint64 bytes=0,pixels=0;int index=0;std::function<void(QList<CaptureFrame>,Error)> reply;std::function<void()> next;};
  auto batch=std::make_shared<Batch>();batch->frames=std::move(frames);batch->frozenLayout=std::move(frozen);batch->reply=std::move(cb);const auto generation=guard->generation_;std::weak_ptr<Batch> weak=batch;
  batch->next=[guard,batchWeak=weak,revision,limits,generation,read,accept]{auto batch=batchWeak.lock();if(!batch)return;
   auto fail=[batch](Error error){auto cb=std::move(batch->reply);batch->next={};if(cb)cb({},error);};
   if(!guard||generation!=guard->generation_){fail({"cancelled","Desktop capture cancelled",{}});return;}
   if(!guard->unchanged(batch->frozenLayout,revision)){fail({"display_changed","Display layout changed during acquisition",{}});return;}
   if(batch->index==batch->frames.size()){auto frames=std::move(batch->frames);auto reply=std::move(batch->reply);batch->next={};reply(std::move(frames),{});return;}
   const auto index=batch->index;
   // Each output's budget is what the earlier outputs left.
   ResourceLimits remaining=limits;remaining.imagePixels=limits.imagePixels-batch->pixels;remaining.imageBytes=limits.imageBytes-batch->bytes;
   read(batch->frames[index],remaining,FrameReply([guard,batch,index,revision,remaining,generation,accept](QImage image,QVariantMap metadata,Error error){
    const auto reject=[batch](Error failure){auto cb=std::move(batch->reply);batch->next={};if(cb)cb({},failure);};
    if(!guard||generation!=guard->generation_){reject({"cancelled","Desktop capture cancelled",{}});return;}
    if(error){reject(error);return;}
    if(!guard->unchanged(batch->frozenLayout,revision)){reject({"display_changed","Display layout changed during acquisition",{}});return;}
    const auto pixels=qint64(image.width())*image.height();const auto bytes=image.sizeInBytes();
    if(auto refused=accept(batch->frames[index],std::move(image),metadata,remaining)){reject(std::move(refused));return;}
    batch->pixels+=pixels;batch->bytes+=bytes;
    ++batch->index;auto next=batch->next;next();
   }));
  };
  // The first native request retains batch until its callback; next is weak to
  // avoid a self-owning function cycle across cancellation and adapter shutdown.
  auto next=batch->next;next();
 }
};
}
