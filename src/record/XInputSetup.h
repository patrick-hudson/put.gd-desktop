#pragma once
#include <QString>
#include <cstdint>
#include <cstdlib>
#include <xcb/xcb.h>
#include <xcb/xinput.h>
namespace putgd::record {
// Select raw events on the chosen screen. The caller owns connection lifetime.
template<typename Opcode>
inline QString setupXInput(xcb_connection_t* connection,int screenNumber,std::uint32_t mask,Opcode& opcode,xcb_window_t& root,const QString& refused){
 if(!connection||xcb_connection_has_error(connection))return QStringLiteral("no X server connection");
 const auto* extension=xcb_get_extension_data(connection,&xcb_input_id);
 if(!extension||!extension->present)return QStringLiteral("the X server has no XInput extension");
 opcode=extension->major_opcode;
 auto* version=xcb_input_xi_query_version_reply(connection,xcb_input_xi_query_version(connection,2,1),nullptr);
 const bool raw=version&&(version->major_version>2||(version->major_version==2&&version->minor_version>=1));
 std::free(version);
 if(!raw)return QStringLiteral("the X server has no XInput 2.1");
 auto iterator=xcb_setup_roots_iterator(xcb_get_setup(connection));
 for(int i=0;i<screenNumber&&iterator.rem;++i)xcb_screen_next(&iterator);
 if(!iterator.data)return QStringLiteral("no X screen");
 root=iterator.data->root;
 struct {xcb_input_event_mask_t head;std::uint32_t mask;} select{};
 select.head.deviceid=XCB_INPUT_DEVICE_ALL_MASTER;select.head.mask_len=1;select.mask=mask;
 auto* error=xcb_request_check(connection,xcb_input_xi_select_events_checked(connection,root,1,&select.head));
 if(error){std::free(error);return refused;}
 xcb_flush(connection);
 return {};
}
}
