// "Upload with put.gd" and "Upload encrypted with put.gd" (#2119) in Nautilus and
// Caja (#1105). One source, built once per extension API (fm-api.h). The file
// manager loads this module into its own process for every user, so it only
// builds the two menu items and, when one is chosen, starts the launcher with
// the selection's activation URIs. No I/O,
// IPC, path checks, main-loop sources or timers: the launcher alone decides
// what has a local path. Not linked to the extension library; its symbols
// resolve from the file manager. Built with hidden visibility; only the module
// entry points are exported.
#include "fm-api.h"

#define PUTGD_FM_EXPORT __attribute__((visibility("default")))

typedef struct { GObject parent; } PutgdMenu;
typedef struct { GObjectClass parent; } PutgdMenuClass;

static void putgd_menu_provider_init(FmMenuProviderInterface *iface);
G_DEFINE_DYNAMIC_TYPE_EXTENDED(PutgdMenu, putgd_menu, G_TYPE_OBJECT, 0,
  G_IMPLEMENT_INTERFACE_DYNAMIC(FM_TYPE_MENU_PROVIDER, putgd_menu_provider_init))

static void putgd_menu_class_init(PutgdMenuClass *klass) { (void)klass; }
static void putgd_menu_class_finalize(PutgdMenuClass *klass) { (void)klass; }
static void putgd_menu_init(PutgdMenu *self) { (void)self; }

static void free_argv(gpointer argv, GClosure *closure) { (void)closure; g_strfreev(argv); }

// Detached: without G_SPAWN_DO_NOT_REAP_CHILD GLib reaps through an
// intermediate child, so nothing is left to watch in the file manager.
static void upload(FmMenuItem *item, gpointer argv) {
  (void)item;
  GError *error = NULL;
  if(!g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &error)) {
    g_message("put.gd: cannot start " PUTGD_FM_LAUNCHER ": %s", error->message);
    g_error_free(error);
  }
}

// One item: the launcher's argv, owned by the item's activate handler.
static FmMenuItem *upload_item(GList *files, guint count, gboolean encrypt) {
  char **argv = g_new0(char *, count + 5), **arg = argv;
  *arg++ = g_strdup(PUTGD_FM_LAUNCHER); *arg++ = g_strdup("--upload");
  if(encrypt) *arg++ = g_strdup("--encrypt");
  *arg++ = g_strdup("--");
  // The activation URI is cached on the file; Recent and Starred items name their target.
  for(GList *file = files; file; file = file->next) {
    char *uri = fm_file_info_get_activation_uri(file->data);
    *arg++ = uri ? uri : fm_file_info_get_uri(file->data);
  }
  FmMenuItem *item = encrypt
    ? fm_menu_item_new("PutgdMenu::UploadEncrypted", PUTGD_FM_ENCRYPTED_LABEL, "Upload the selected items as encrypted links with put.gd", PUTGD_FM_ICON)
    : fm_menu_item_new("PutgdMenu::Upload", PUTGD_FM_LABEL, "Upload the selected items with put.gd", PUTGD_FM_ICON);
  g_signal_connect_data(item, "activate", G_CALLBACK(upload), argv, free_argv, 0);
  return item;
}

static GList *get_file_items(FM_GET_FILE_ITEMS_PARAMETERS) {
  (void)provider;
  guint count = g_list_length(files);
  if(!count) return NULL;
  GList *items = g_list_append(NULL, upload_item(files, count, FALSE));
  return g_list_append(items, upload_item(files, count, TRUE));
}

static void putgd_menu_provider_init(FmMenuProviderInterface *iface) { iface->get_file_items = get_file_items; }

static GType putgd_menu_types[1];

PUTGD_FM_EXPORT void FM_MODULE(initialize)(GTypeModule *module) {
  putgd_menu_register_type(module);
  putgd_menu_types[0] = putgd_menu_get_type();
}

PUTGD_FM_EXPORT void FM_MODULE(shutdown)(void) {}

PUTGD_FM_EXPORT void FM_MODULE(list_types)(const GType **types, int *count) {
  *types = putgd_menu_types;
  *count = G_N_ELEMENTS(putgd_menu_types);
}
