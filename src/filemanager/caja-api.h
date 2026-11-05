// Caja extensions-2.0: the Nautilus 2 API, whose menu callbacks also get the window.
#pragma once
#include <libcaja-extension/caja-extension-types.h>
#include <libcaja-extension/caja-file-info.h>
#include <libcaja-extension/caja-menu-provider.h>

typedef CajaMenuProvider FmMenuProvider;
typedef CajaMenuProviderIface FmMenuProviderInterface;
typedef CajaMenuItem FmMenuItem;
typedef CajaFileInfo FmFileInfo;
typedef CajaFileInfoIface FmFileInfoInterface;
#define FM_TYPE_MENU_PROVIDER CAJA_TYPE_MENU_PROVIDER
#define FM_TYPE_FILE_INFO CAJA_TYPE_FILE_INFO
#define FM_MODULE(name) caja_module_##name
#define FM_MODULE_SYMBOL(name) "caja_module_" name
#define FM_GET_FILE_ITEMS_PARAMETERS FmMenuProvider *provider, GtkWidget *window G_GNUC_UNUSED, GList *files
#define fm_menu_provider_get_file_items(provider, files) caja_menu_provider_get_file_items(provider, NULL, files)
#define fm_menu_item_new caja_menu_item_new
#define fm_menu_item_activate caja_menu_item_activate
#define fm_file_info_get_activation_uri caja_file_info_get_activation_uri
#define fm_file_info_get_uri caja_file_info_get_uri
