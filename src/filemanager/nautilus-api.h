// Nautilus extension API 4 (Nautilus 43+): menus come from the model, without a window.
#pragma once
#include <nautilus-extension.h>

typedef NautilusMenuProvider FmMenuProvider;
typedef NautilusMenuProviderInterface FmMenuProviderInterface;
typedef NautilusMenuItem FmMenuItem;
typedef NautilusFileInfo FmFileInfo;
typedef NautilusFileInfoInterface FmFileInfoInterface;
#define FM_TYPE_MENU_PROVIDER NAUTILUS_TYPE_MENU_PROVIDER
#define FM_TYPE_FILE_INFO NAUTILUS_TYPE_FILE_INFO
#define FM_MODULE(name) nautilus_module_##name
#define FM_MODULE_SYMBOL(name) "nautilus_module_" name
#define FM_GET_FILE_ITEMS_PARAMETERS FmMenuProvider *provider, GList *files
#define fm_menu_provider_get_file_items(provider, files) nautilus_menu_provider_get_file_items(provider, files)
#define fm_menu_item_new nautilus_menu_item_new
#define fm_menu_item_activate nautilus_menu_item_activate
#define fm_file_info_get_activation_uri nautilus_file_info_get_activation_uri
#define fm_file_info_get_uri nautilus_file_info_get_uri
