#include "TlsCompatibility.h"
#if defined(PUTGD_OPENSSL_GROUP_POLICY)
#include <openssl/conf.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <cstdlib>
#include <memory>

namespace putgd {
namespace {
// Configuration inspection must not replace an earlier transport error.
struct ErrorScope {
    ~ErrorScope() { ERR_clear_error(); }
};
std::string_view commandName(const char* name) {
    const std::string_view value(name);
    const auto dot = value.find('.');
    return dot == value.npos ? value : value.substr(dot + 1);
}
}
bool defaultSystemTlsGroups(const char* configuration) {
    // Unexplained library errors also disable the override; leave them intact.
    if (ERR_peek_error() != 0) return false;
    ErrorScope errors;
    if (!configuration) return false;
    if (!*configuration) return true; // OPENSSL_CONF="" disables configuration.
    std::unique_ptr<CONF, decltype(&NCONF_free)> conf(NCONF_new(nullptr), NCONF_free);
    if (!conf || NCONF_load(conf.get(), configuration, nullptr) != 1 ||
        ERR_peek_error() != 0) return false;
    const auto* initName = NCONF_get_string(conf.get(), nullptr, "openssl_conf");
    if (!initName) return true;
    const auto* init = NCONF_get_section(conf.get(), initName);
    if (!init) return false;
    for (int i = 0; i < sk_CONF_VALUE_num(init); ++i) {
        const auto* module = sk_CONF_VALUE_value(init, i);
        if (commandName(module->name) != "ssl_conf") continue;
        const auto* ssl = NCONF_get_section(conf.get(), module->value);
        if (!ssl) return false;
        for (int j = 0; j < sk_CONF_VALUE_num(ssl); ++j) {
            const auto* policy = sk_CONF_VALUE_value(ssl, j);
            if (std::string_view(policy->name) != "system_default") continue;
            const auto* commands = NCONF_get_section(conf.get(), policy->value);
            if (!commands) return false;
            for (int k = 0; k < sk_CONF_VALUE_num(commands); ++k) {
                const auto name = commandName(sk_CONF_VALUE_value(commands, k)->name);
                if (name == "Groups" || name == "Curves") return false;
            }
        }
    }
    return true;
}
bool defaultSystemTlsGroups() {
    if (const auto* configured = std::getenv("OPENSSL_CONF")) return defaultSystemTlsGroups(configured);
    char* path = CONF_get1_default_config_file();
    const bool defaults = defaultSystemTlsGroups(path);
    OPENSSL_free(path);
    return defaults;
}
}
#else
namespace putgd {
bool defaultSystemTlsGroups(const char*) { return false; }
bool defaultSystemTlsGroups() { return false; }
}
#endif
