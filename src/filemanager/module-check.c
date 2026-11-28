// Loads a built Nautilus or Caja plugin the way the file manager does and checks
// its two menu entries (#1105, #2119). Linked to the extension library, as the
// file manager is, so the unlinked plugin's symbols resolve. Not installed.
// Usage: putgd-<fm>-check <module.so> [--activate|--activate-encrypted]
// --activate also chooses Upload with put.gd, --activate-encrypted Upload
// encrypted with put.gd; either starts /usr/bin/putgd-launcher. It then writes the
// resident's marker (#2136) under a private XDG_DATA_HOME and checks that only
// Upload with put.gd is offered while it exists.
#include "fm-api.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
  const int activate = argc == 3 ? (!strcmp(argv[2], "--activate") ? 1 : !strcmp(argv[2], "--activate-encrypted") ? 2 : -1) : 0;
  CHECK(argc == 2 || activate > 0, "usage: %s <module.so> [--activate|--activate-encrypted]", argv[0]);
  // Before GLib first reads it: the plugin's marker lives under this private data home.
  char data_home[] = "/tmp/putgd-fm-check-XXXXXX";
  CHECK(mkdtemp(data_home), "mkdtemp failed");
  setenv("XDG_DATA_HOME", data_home, 1);
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
  CHECK(g_list_length(items) == 2, "expected two menu items, got %u", g_list_length(items));
  // Upload with put.gd, then Upload encrypted with put.gd, each with the same icon.
  const char *labels[] = {PUTGD_FM_LABEL, PUTGD_FM_ENCRYPTED_LABEL}, *names[] = {"PutgdMenu::Upload", "PutgdMenu::UploadEncrypted"};
  GString *json = g_string_new(NULL);
  int index = 0;
  for(GList *entry = items; entry; entry = entry->next, ++index) {
    char *name = NULL, *label = NULL, *icon = NULL;
    g_object_get(entry->data, "name", &name, "label", &label, "icon", &icon, NULL);
    CHECK(!g_strcmp0(name, names[index]) && !g_strcmp0(label, labels[index]) && !g_strcmp0(icon, PUTGD_FM_ICON), "menu item %d: name %s, label %s, icon %s", index, name, label, icon);
    if(activate == index + 1) fm_menu_item_activate(entry->data);
    g_string_append_printf(json, "%s{\"item\":\"%s\",\"label\":\"%s\",\"icon\":\"%s\"}", index ? "," : "", name, label, icon);
    g_free(name); g_free(label); g_free(icon);
  }
  // #2136: with the resident's marker present, the encrypted item is left out.
  char *folder = g_build_filename(data_home, "putgd", "file-manager", NULL), *marker = g_build_filename(data_home, PUTGD_FM_ENCRYPTED_HIDDEN, NULL);
  CHECK(g_mkdir_with_parents(folder, 0700) == 0 && g_file_set_contents(marker, "proton\n", -1, NULL), "cannot write %s", marker);
  GList *hidden = fm_menu_provider_get_file_items(provider, files);
  char *only = NULL;
  if(g_list_length(hidden) == 1) g_object_get(hidden->data, "name", &only, NULL);
  CHECK(!g_strcmp0(only, "PutgdMenu::Upload"), "with the marker: %u items, first %s", g_list_length(hidden), only);
  g_free(only);
  g_list_free_full(hidden, g_object_unref);
  CHECK(unlink(marker) == 0 && rmdir(folder) == 0, "cannot remove %s", marker);
  char *parent = g_path_get_dirname(folder);
  CHECK(rmdir(parent) == 0 && rmdir(data_home) == 0, "cannot remove %s", parent);
  g_free(parent); g_free(folder); g_free(marker);
  printf("{\"type\":\"%s\",\"items\":[%s],\"activated\":%s,\"hidesEncrypted\":true}\n", g_type_name(types[0]), json->str,
         activate == 1 ? "\"plain\"" : activate == 2 ? "\"encrypted\"" : "null");
  g_string_free(json, TRUE);
  g_list_free_full(items, g_object_unref);
  g_list_free_full(files, g_object_unref);
  g_object_unref(provider);
  shutdown();
  return 0;
}
