// putgd-clipboard-keeper: keeps put.gd's last clipboard result alive after the
// resident quits, on wlroots compositors with no clipboard manager.
//
// The resident passes the exact types and bytes as an anonymous memory file on
// standard input (ClipboardKeeperFormat.h). Nothing reaches the disk. The keeper
// waits for the resident's selection to go away; if nobody restores it within a
// moment, it offers the same content itself, and exits once anything else owns
// the clipboard. A clipboard manager that restores the selection first leaves
// the keeper with nothing to do.
//
// C, with only libc and libwayland-client: an AppImage runs this from memory on
// the host's libraries, because the image's mount ends with the resident.
#include "ClipboardKeeperFormat.h"
#include "ext-data-control-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include <wayland-client.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
 // Time a clipboard manager gets to restore the selection (clipman starts two
 // processes to do it). A slower one still wins: it cancels the keeper.
 graceMs=300,
 // The resident releases the selection right after starting the keeper.
 ownerExitMs=10000,
 // A paste that stops reading.
 stallMs=5000,
 maxTransfers=32,
 chunkBytes=65536,
};

struct item { const char* name; const unsigned char* data; size_t size; };
struct transfer { int fd; const struct item* item; size_t offset; int64_t progressed; };
struct offer { struct ext_data_control_offer_v1* proxy; bool ours; };
enum state { connecting, ownerPresent, waitingForManager, keeping };

static struct item items[PUTGD_KEEPER_MAX_TYPES];
static unsigned itemCount;
static const char* token;
static struct transfer transfers[maxTransfers];
static int transferCount;
static struct wl_seat* seat;
static struct ext_data_control_manager_v1* manager;
static struct ext_data_control_device_v1* device;
static uint32_t wlrName, wlrVersion;
static struct offer* current;
static enum state state=connecting;
static int64_t deadline;
static bool finished;

static int64_t nowMs(void)
{
 struct timespec t;
 clock_gettime(CLOCK_MONOTONIC,&t);
 return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}

static bool parse(const unsigned char* bytes,size_t size)
{
 const size_t headerBytes=PUTGD_KEEPER_MAGIC_BYTES+sizeof(uint32_t);
 if(size<headerBytes||memcmp(bytes,PUTGD_KEEPER_MAGIC,PUTGD_KEEPER_MAGIC_BYTES)!=0)return false;
 uint32_t count;
 memcpy(&count,bytes+PUTGD_KEEPER_MAGIC_BYTES,sizeof count);
 if(count==0||count>PUTGD_KEEPER_MAX_TYPES)return false;
 size_t at=headerBytes;
 for(uint32_t i=0;i<count;++i){
  uint32_t sizes[2];
  uint64_t dataBytes;
  if(size-at<sizeof sizes+sizeof dataBytes)return false;
  memcpy(sizes,bytes+at,sizeof sizes);
  memcpy(&dataBytes,bytes+at+sizeof sizes,sizeof dataBytes);
  at+=sizeof sizes+sizeof dataBytes;
  if(sizes[0]==0||size-at<sizes[0]||bytes[at+sizes[0]-1]!=0)return false;
  items[i].name=(const char*)bytes+at;
  at+=sizes[0];
  if(size-at<dataBytes)return false;
  items[i].data=bytes+at;
  items[i].size=(size_t)dataBytes;
  at+=(size_t)dataBytes;
  if(strncmp(items[i].name,PUTGD_KEEPER_TOKEN_PREFIX,strlen(PUTGD_KEEPER_TOKEN_PREFIX))==0)token=items[i].name;
 }
 itemCount=count;
 return at==size&&token!=NULL;
}

static void dropOffer(struct offer* offer)
{
 if(!offer)return;
 ext_data_control_offer_v1_destroy(offer->proxy);
 free(offer);
}

// Decides what the current selection means while the keeper holds nothing.
static void evaluate(void)
{
 if(state==connecting||state==keeping)return;
 if(!current){
  // Started after the resident left, or the resident has just released it.
  if(state!=waitingForManager){state=waitingForManager;deadline=nowMs()+graceMs;}
 }else if(current->ours){
  if(state!=ownerPresent){state=ownerPresent;deadline=nowMs()+ownerExitMs;}
 }else finished=true; // Another owner: a new copy, or a manager's restored one.
}

static void offerType(void* data,struct ext_data_control_offer_v1* proxy,const char* type)
{
 (void)proxy;
 struct offer* offer=data;
 if(strcmp(type,token)==0)offer->ours=true;
}
static const struct ext_data_control_offer_v1_listener offerListener={offerType};

static void dataOffer(void* data,struct ext_data_control_device_v1* d,struct ext_data_control_offer_v1* proxy)
{
 (void)data;(void)d;
 struct offer* offer=calloc(1,sizeof *offer);
 if(!offer)exit(1);
 offer->proxy=proxy;
 ext_data_control_offer_v1_add_listener(proxy,&offerListener,offer);
}

static void selection(void* data,struct ext_data_control_device_v1* d,struct ext_data_control_offer_v1* proxy)
{
 (void)data;(void)d;
 struct offer* next=proxy?ext_data_control_offer_v1_get_user_data(proxy):NULL;
 if(next!=current)dropOffer(current);
 current=next;
 evaluate();
}

static void deviceFinished(void* data,struct ext_data_control_device_v1* d)
{
 (void)data;(void)d;
 exit(0);
}

static void primarySelection(void* data,struct ext_data_control_device_v1* d,struct ext_data_control_offer_v1* proxy)
{
 (void)data;(void)d;
 struct offer* offer=proxy?ext_data_control_offer_v1_get_user_data(proxy):NULL;
 if(offer!=current)dropOffer(offer);
}
static const struct ext_data_control_device_v1_listener deviceListener={dataOffer,selection,deviceFinished,primarySelection};

static void sourceSend(void* data,struct ext_data_control_source_v1* source,const char* type,int fd)
{
 (void)data;(void)source;
 const struct item* found=NULL;
 for(unsigned i=0;i<itemCount&&!found;++i)if(strcmp(items[i].name,type)==0)found=&items[i];
 const int flags=fcntl(fd,F_GETFL);
 if(!found||transferCount==maxTransfers||flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){close(fd);return;}
 transfers[transferCount++]=(struct transfer){fd,found,0,nowMs()};
}

static void sourceCancelled(void* data,struct ext_data_control_source_v1* source)
{
 (void)data;
 ext_data_control_source_v1_destroy(source);
 finished=true; // Pastes already under way still finish.
}
static const struct ext_data_control_source_v1_listener sourceListener={sourceSend,sourceCancelled};

static void takeSelection(void)
{
 struct ext_data_control_source_v1* source=ext_data_control_manager_v1_create_data_source(manager);
 ext_data_control_source_v1_add_listener(source,&sourceListener,NULL);
 for(unsigned i=0;i<itemCount;++i)ext_data_control_source_v1_offer(source,items[i].name);
 ext_data_control_device_v1_set_selection(device,source);
 state=keeping;
}

static void global(void* data,struct wl_registry* registry,uint32_t name,const char* interface,uint32_t version)
{
 (void)data;
 if(strcmp(interface,ext_data_control_manager_v1_interface.name)==0&&!manager)
  manager=wl_registry_bind(registry,name,&ext_data_control_manager_v1_interface,1);
 else if(strcmp(interface,zwlr_data_control_manager_v1_interface.name)==0){wlrName=name;wlrVersion=version;}
 else if(strcmp(interface,wl_seat_interface.name)==0&&!seat)
  seat=wl_registry_bind(registry,name,&wl_seat_interface,1);
}
static void globalRemoved(void* data,struct wl_registry* registry,uint32_t name)
{
 (void)data;(void)registry;(void)name;
}
static const struct wl_registry_listener registryListener={global,globalRemoved};

static void removeTransfer(int index)
{
 close(transfers[index].fd);
 transfers[index]=transfers[--transferCount];
}

static void pump(int index)
{
 struct transfer* t=&transfers[index];
 size_t left=t->item->size-t->offset;
 ssize_t written=left?write(t->fd,t->item->data+t->offset,left<chunkBytes?left:chunkBytes):0;
 if(written<0&&(errno==EAGAIN||errno==EINTR))return;
 if(written<0||(size_t)written==left){removeTransfer(index);return;}
 t->offset+=(size_t)written;
 t->progressed=nowMs();
}

static int run(struct wl_display* display)
{
 const int wayland=wl_display_get_fd(display);
 for(;;){
  if(state==waitingForManager&&nowMs()>=deadline)takeSelection();
  else if(state==ownerPresent&&nowMs()>=deadline)return 0;
  if(finished&&transferCount==0)return 0;
  const int64_t now=nowMs();
  for(int i=transferCount-1;i>=0;--i)if(now-transfers[i].progressed>=stallMs)removeTransfer(i);
  while(wl_display_prepare_read(display)!=0)if(wl_display_dispatch_pending(display)<0)return 1;
  if(wl_display_flush(display)<0&&errno!=EAGAIN){wl_display_cancel_read(display);return 1;}
  struct pollfd polled[maxTransfers+1];
  polled[0]=(struct pollfd){.fd=wayland,.events=POLLIN};
  for(int i=0;i<transferCount;++i)polled[i+1]=(struct pollfd){.fd=transfers[i].fd,.events=POLLOUT};
  int64_t wait=-1;
  if(state==waitingForManager||state==ownerPresent)wait=deadline>now?deadline-now:0;
  if(transferCount&&(wait<0||wait>stallMs))wait=stallMs;
  const int ready=poll(polled,(nfds_t)transferCount+1,(int)wait);
  if(ready<0&&errno!=EINTR){wl_display_cancel_read(display);return 1;}
  if(ready>0&&(polled[0].revents&POLLIN)){if(wl_display_read_events(display)<0)return 1;}
  else wl_display_cancel_read(display);
  if(ready>0)for(int i=transferCount-1;i>=0;--i)if(polled[i+1].revents)pump(i);
  if(wl_display_dispatch_pending(display)<0)return 1;
 }
}

int main(void)
{
 prctl(PR_SET_NAME,"putgd-clipboard");
 // Clipboard content never goes to a core file.
 prctl(PR_SET_DUMPABLE,0);
 signal(SIGPIPE,SIG_IGN);
 struct stat info;
 if(fstat(STDIN_FILENO,&info)!=0||!S_ISREG(info.st_mode)||info.st_size<=0||(uint64_t)info.st_size>PUTGD_KEEPER_MAX_BYTES)return 2;
 const size_t size=(size_t)info.st_size;
 const unsigned char* bytes=mmap(NULL,size,PROT_READ,MAP_PRIVATE,STDIN_FILENO,0);
 if(bytes==MAP_FAILED||!parse(bytes,size))return 2;
 // Keep it out of swap where the memory-lock limit allows; a larger result can still swap.
 (void)mlock(bytes,size);
 const int null=open("/dev/null",O_RDONLY|O_CLOEXEC);
 if(null<0||dup2(null,STDIN_FILENO)<0)return 2;
 close(null);

 struct wl_display* display=wl_display_connect(NULL);
 if(!display)return 1;
 struct wl_registry* registry=wl_display_get_registry(display);
 wl_registry_add_listener(registry,&registryListener,NULL);
 if(wl_display_roundtrip(display)<0)return 1;
 // Prefer ext. The wlr protocol has the same requests and events, so it is
 // driven through the ext API, as the resident does.
 if(!manager&&wlrName)manager=wl_registry_bind(registry,wlrName,&zwlr_data_control_manager_v1_interface,wlrVersion<2?wlrVersion:2);
 if(!manager||!seat)return 1;
 device=ext_data_control_manager_v1_get_data_device(manager,seat);
 ext_data_control_device_v1_add_listener(device,&deviceListener,NULL);
 // Mir announces nothing while the selection is empty, so a finished round
 // trip with no event means an empty selection.
 if(wl_display_roundtrip(display)<0)return 1;
 state=ownerPresent;
 deadline=nowMs()+ownerExitMs;
 evaluate();
 return run(display);
}
