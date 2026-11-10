#define _GNU_SOURCE
// Private-compositor fixture: present real lock surfaces, then unlock on TERM.
#include "ext-session-lock-v1-client-protocol.h"
#include <wayland-client.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static volatile sig_atomic_t stop;
static struct ext_session_lock_manager_v1* manager;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct wl_output* outputs[16];static int count,frames,secured;
static void stopped(int signal){(void)signal;stop=1;}
static void ready(void){if(secured&&frames==count){puts("locked");fflush(stdout);}}
static void global(void* data,struct wl_registry* registry,uint32_t name,const char* interface,uint32_t version){
 (void)data;(void)version;
 if(!strcmp(interface,"ext_session_lock_manager_v1"))manager=wl_registry_bind(registry,name,&ext_session_lock_manager_v1_interface,1);
 else if(!strcmp(interface,"wl_compositor"))compositor=wl_registry_bind(registry,name,&wl_compositor_interface,1);
 else if(!strcmp(interface,"wl_shm"))shm=wl_registry_bind(registry,name,&wl_shm_interface,1);
 else if(!strcmp(interface,"wl_output")&&count<16)outputs[count++]=wl_registry_bind(registry,name,&wl_output_interface,1);
}
static void removed(void* data,struct wl_registry* registry,uint32_t name){(void)data;(void)registry;(void)name;}
static void locked(void* data,struct ext_session_lock_v1* lock){(void)data;(void)lock;secured=1;ready();}
static void finished(void* data,struct ext_session_lock_v1* lock){(void)data;(void)lock;stop=1;}
static void frame(void* data,struct wl_callback* callback,uint32_t time){(void)data;(void)time;wl_callback_destroy(callback);++frames;ready();}
static void configured(void* data,struct ext_session_lock_surface_v1* lock,uint32_t serial,uint32_t width,uint32_t height){
 struct wl_surface* surface=data;ext_session_lock_surface_v1_ack_configure(lock,serial);
 int fd=memfd_create("private-lock",MFD_CLOEXEC);if(fd<0||ftruncate(fd,(off_t)width*height*4)<0){stop=1;return;}
 struct wl_shm_pool* pool=wl_shm_create_pool(shm,fd,width*height*4);close(fd);
 struct wl_buffer* buffer=wl_shm_pool_create_buffer(pool,0,width,height,width*4,WL_SHM_FORMAT_XRGB8888);wl_shm_pool_destroy(pool);
 wl_surface_attach(surface,buffer,0,0);wl_surface_damage(surface,0,0,width,height);
 static const struct wl_callback_listener listener={frame};wl_callback_add_listener(wl_surface_frame(surface),&listener,NULL);wl_surface_commit(surface);
}
int main(void){
 signal(SIGTERM,stopped);signal(SIGINT,stopped);
 struct wl_display* display=wl_display_connect(NULL);if(!display)return 1;
 struct wl_registry* registry=wl_display_get_registry(display);
 const struct wl_registry_listener globals={global,removed};wl_registry_add_listener(registry,&globals,NULL);
 if(wl_display_roundtrip(display)<0||!manager||!compositor||!shm||!count)return 2;
 struct ext_session_lock_v1* lock=ext_session_lock_manager_v1_lock(manager);
 const struct ext_session_lock_v1_listener listener={locked,finished};ext_session_lock_v1_add_listener(lock,&listener,NULL);
 for(int i=0;i<count;++i){struct wl_surface* surface=wl_compositor_create_surface(compositor);
  struct ext_session_lock_surface_v1* secure=ext_session_lock_v1_get_lock_surface(lock,surface,outputs[i]);
  static const struct ext_session_lock_surface_v1_listener configure={configured};ext_session_lock_surface_v1_add_listener(secure,&configure,surface);}
 wl_display_flush(display);
 while(!stop){struct pollfd fd={wl_display_get_fd(display),POLLIN,0};if(poll(&fd,1,100)>0&&wl_display_dispatch(display)<0)return 3;wl_display_flush(display);}
 ext_session_lock_v1_unlock_and_destroy(lock);wl_display_roundtrip(display);
 ext_session_lock_manager_v1_destroy(manager);wl_registry_destroy(registry);wl_display_disconnect(display);return 0;
}
