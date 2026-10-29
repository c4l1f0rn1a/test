#include "config.h"

#include <string.h>
#include <glib/gstdio.h>

#define APP_DIR  "sipphone"
#define APP_FILE "sipphone.ini"

char *config_path(void)
{
    /* g_get_user_config_dir() honours XDG_CONFIG_HOME, which the AppImage
     * runtime points at "<name>.AppImage.config" in portable mode. */
    return g_build_filename(g_get_user_config_dir(), APP_DIR, APP_FILE, NULL);
}

static char *get_str(GKeyFile *kf, const char *group, const char *key,
                     const char *def)
{
    char *v = g_key_file_get_string(kf, group, key, NULL);
    if (!v)
        return g_strdup(def);
    g_strstrip(v);
    return v;
}

static int get_int(GKeyFile *kf, const char *group, const char *key, int def)
{
    GError *err = NULL;
    int v = g_key_file_get_integer(kf, group, key, &err);
    if (err) {
        g_error_free(err);
        return def;
    }
    return v;
}

static double get_double(GKeyFile *kf, const char *group, const char *key,
                         double def)
{
    GError *err = NULL;
    double v = g_key_file_get_double(kf, group, key, &err);
    if (err) {
        g_error_free(err);
        return def;
    }
    return CLAMP(v, 0.0, 2.0);
}

static gboolean get_bool(GKeyFile *kf, const char *group, const char *key,
                         gboolean def)
{
    GError *err = NULL;
    gboolean v = g_key_file_get_boolean(kf, group, key, &err);
    if (err) {
        g_error_free(err);
        return def;
    }
    return v;
}

void config_load(Config *cfg)
{
    GKeyFile *kf = g_key_file_new();
    char *path = config_path();

    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);

    cfg->display_name = get_str(kf, "account", "display_name", "");
    cfg->username     = get_str(kf, "account", "username", "");
    cfg->domain       = get_str(kf, "account", "domain", "");
    cfg->auth_user    = get_str(kf, "account", "auth_user", "");
    cfg->password     = get_str(kf, "account", "password", "");
    cfg->proxy        = get_str(kf, "account", "proxy", "");
    cfg->transport    = get_str(kf, "account", "transport", "udp");
    cfg->srtp         = get_str(kf, "account", "srtp", "disabled");

    cfg->stun       = get_str(kf, "network", "stun", "");
    cfg->local_port = get_int(kf, "network", "local_port", 0);

    cfg->mic_level = get_double(kf, "audio", "mic_level", 1.0);
    cfg->spk_level = get_double(kf, "audio", "spk_level", 1.0);

    cfg->start_minimized = get_bool(kf, "general", "start_minimized", FALSE);
    cfg->last_number     = get_str(kf, "general", "last_number", "");

    g_free(path);
    g_key_file_free(kf);
}

gboolean config_save(const Config *cfg, GError **error)
{
    GKeyFile *kf = g_key_file_new();
    char *path = config_path();
    char *dir = g_path_get_dirname(path);
    char *data;
    gsize len;
    gboolean ok;

    g_key_file_set_string(kf, "account", "display_name", cfg->display_name);
    g_key_file_set_string(kf, "account", "username", cfg->username);
    g_key_file_set_string(kf, "account", "domain", cfg->domain);
    g_key_file_set_string(kf, "account", "auth_user", cfg->auth_user);
    g_key_file_set_string(kf, "account", "password", cfg->password);
    g_key_file_set_string(kf, "account", "proxy", cfg->proxy);
    g_key_file_set_string(kf, "account", "transport", cfg->transport);
    g_key_file_set_string(kf, "account", "srtp", cfg->srtp);

    g_key_file_set_string(kf, "network", "stun", cfg->stun);
    g_key_file_set_integer(kf, "network", "local_port", cfg->local_port);

    g_key_file_set_double(kf, "audio", "mic_level", cfg->mic_level);
    g_key_file_set_double(kf, "audio", "spk_level", cfg->spk_level);

    g_key_file_set_boolean(kf, "general", "start_minimized",
                           cfg->start_minimized);
    g_key_file_set_string(kf, "general", "last_number", cfg->last_number);

    data = g_key_file_to_data(kf, &len, NULL);

    /* The file holds the SIP password, so keep it private to the user. */
    g_mkdir_with_parents(dir, 0700);
    ok = g_file_set_contents_full(path, data, len,
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);

    g_free(data);
    g_free(dir);
    g_free(path);
    g_key_file_free(kf);
    return ok;
}

void config_clear(Config *cfg)
{
    g_free(cfg->display_name);
    g_free(cfg->username);
    g_free(cfg->domain);
    g_free(cfg->auth_user);
    g_free(cfg->password);
    g_free(cfg->proxy);
    g_free(cfg->transport);
    g_free(cfg->srtp);
    g_free(cfg->stun);
    g_free(cfg->last_number);
    memset(cfg, 0, sizeof(*cfg));
}

gboolean config_account_ready(const Config *cfg)
{
    return cfg->username && *cfg->username && cfg->domain && *cfg->domain;
}
