// Loads a built Nautilus or Caja plugin the way the file manager does and checks
// its menu entry (#1105). Linked to the extension library, as the file manager
// is, so the unlinked plugin's symbols resolve. Not installed.
// Usage: putgd-<fm>-check <module.so> [--activate]
// --activate also chooses the entry, which starts /usr/bin/putgd-launcher.
#include "fm-api.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition, ...) do { if(!(condition)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); return 1; } } while(0)

typedef struct { GTypeModule parent; } CheckModule;
typedef struct { GTypeModuleClass parent; } CheckModuleClass;
G_DEFINE_TYPE(CheckModule, check_module, G_TYPE_TYPE_MODULE)
static gboolean check_module_load(GTypeModule *module) { (void)module; return TRUE; }
static void check_module_unload(GTypeModule *module) { (void)module; }
static void check_module_class_init(CheckModuleClass *klass) {
  G_TYPE_MODULE_CLASS(klass)->load = check_module_load;
  G_TYPE_MODULE_CLASS(klass)->unload = check_module_unload;
}
static void check_module_init(CheckModule *self) { (void)self; }

// A file with a fixed URI and activation URI (NULL for none).
typedef struct { GObject parent; const char *uri, *activation; } CheckFile;
typedef struct { GObjectClass parent; } CheckFileClass;
static void check_file_info_init(FmFileInfoInterface *iface);
G_DEFINE_TYPE_WITH_CODE(CheckFile, check_file, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE(FM_TYPE_FILE_INFO, check_file_info_init))
static char *check_file_uri(FmFileInfo *file) { return g_strdup(((CheckFile *)file)->uri); }
static char *check_file_activation(FmFileInfo *file) { return g_strdup(((CheckFile *)file)->activation); }
static void check_file_info_init(FmFileInfoInterface *iface) {
  iface->get_uri = check_file_uri;
  iface->get_activation_uri = check_file_activation;
}
static void check_file_class_init(CheckFileClass *klass) { (void)klass; }
static void check_file_init(CheckFile *self) { (void)self; }
static gpointer check_file(const char *uri, const char *activation) {
  CheckFile *file = g_object_new(check_file_get_type(), NULL);
  file->uri = uri; file->activation = activation;
  return file;
}

int main(int argc, char **argv) {
  CHECK(argc == 2 || (argc == 3 && !strcmp(argv[2], "--activate")), "usage: %s <module.so> [--activate]", argv[0]);
  void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  CHECK(module, "dlopen: %s", dlerror());
  void (*initialize)(GTypeModule *) = (void (*)(GTypeModule *))dlsym(module, FM_MODULE_SYMBOL("initialize"));
  void (*shutdown)(void) = (void (*)(void))dlsym(module, FM_MODULE_SYMBOL("shutdown"));
  void (*list_types)(const GType **, int *) = (void (*)(const GType **, int *))dlsym(module, FM_MODULE_SYMBOL("list_types"));
  CHECK(initialize && shutdown && list_types, "module entry points missing");
  GTypeModule *types_module = g_object_new(check_module_get_type(), NULL);
  g_type_module_use(types_module);
  initialize(types_module);
  const GType *types = NULL; int count = 0;
  list_types(&types, &count);
  CHECK(count == 1 && g_type_is_a(types[0], FM_TYPE_MENU_PROVIDER), "expected one menu provider type, got %d", count);
  FmMenuProvider *provider = g_object_new(types[0], NULL);
  CHECK(!fm_menu_provider_get_file_items(provider, NULL), "an empty selection offered items");
  // The second file has no activation URI, so its plain URI goes to the launcher.
  GList *files = g_list_append(g_list_append(NULL, check_file("recent:///a", "file:///tmp/a%20b.txt")), check_file("sftp://host/dir", NULL));
  GList *items = fm_menu_provider_get_file_items(provider, files);
  CHECK(g_list_length(items) == 1, "expected one menu item, got %u", g_list_length(items));
  char *name = NULL, *label = NULL, *icon = NULL;
  g_object_get(items->data, "name", &name, "label", &label, "icon", &icon, NULL);
  CHECK(!g_strcmp0(label, PUTGD_FM_LABEL) && !g_strcmp0(icon, PUTGD_FM_ICON), "menu item label %s, icon %s", label, icon);
  if(argc == 3) fm_menu_item_activate(items->data);
  printf("{\"type\":\"%s\",\"item\":\"%s\",\"label\":\"%s\",\"icon\":\"%s\",\"activated\":%s}\n",
         g_type_name(types[0]), name, label, icon, argc == 3 ? "true" : "false");
  g_free(name); g_free(label); g_free(icon);
  g_list_free_full(items, g_object_unref);
  g_list_free_full(files, g_object_unref);
  g_object_unref(provider);
  shutdown();
  return 0;
}
