#pragma once
#include <xcb/xcb.h>
#include <cstdlib>
#include <cstring>
#include <memory>
namespace putgd::x11 {
template<class T>using XReply=std::unique_ptr<T,decltype(&std::free)>;
template<class T>XReply<T> reply(T* value){return XReply<T>(value,&std::free);}
inline xcb_atom_t internAtom(xcb_connection_t* connection,const char* name){
 auto value=reply(xcb_intern_atom_reply(connection,xcb_intern_atom(connection,0,std::strlen(name),name),nullptr));
 return value?value->atom:xcb_atom_t(XCB_ATOM_NONE);
}
}
