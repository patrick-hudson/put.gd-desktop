#include "KdeKeyboardMap.h"
#include <xkbcommon/xkbcommon.h>
#include <wayland-client-protocol.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <vector>
namespace putgd {
xkb_keymap* readKdeKeyboardMap(xkb_context* context,uint32_t format,int fd,uint32_t size){
 struct Descriptor {int fd;~Descriptor(){if(fd>=0)close(fd);}} descriptor{fd};
 // Bound transient text as well as retained compiled state. Never mmap an
 // externally supplied, potentially truncated FD or print keymap contents.
 constexpr uint32_t maximumBytes=1024*1024;
 struct stat metadata{};
 if(!context||fd<0||format!=WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1||size<2||size>maximumBytes||fstat(fd,&metadata)<0||!S_ISREG(metadata.st_mode)||metadata.st_size<size)return nullptr;
 std::vector<char> text(size);
 size_t offset=0;
 while(offset<size){const auto count=pread(fd,text.data()+offset,size-offset,off_t(offset));if(count<0&&errno==EINTR)continue;if(count<=0)return nullptr;offset+=size_t(count);}
 if(text.back()!=0)return nullptr;
 // Interior NULs cannot silently discard a trailing malformed map.
 for(size_t i=0;i+1<text.size();++i)if(text[i]==0)return nullptr;
 return xkb_keymap_new_from_string(context,text.data(),XKB_KEYMAP_FORMAT_TEXT_V1,XKB_KEYMAP_COMPILE_NO_FLAGS);
}
}
