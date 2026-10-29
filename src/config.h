#ifndef SIPPHONE_CONFIG_H
#define SIPPHONE_CONFIG_H

#include <glib.h>

typedef struct {
    /* [account] */
    char *display_name;
    char *username;     /* SIP user / extension, e.g. 101 */
    char *domain;       /* SIP server (registrar), e.g. pbx.local */
    char *auth_user;    /* login, if different from username */
    char *password;
    char *proxy;        /* optional outbound proxy */
    char *transport;    /* udp | tcp | tls */
    char *srtp;         /* disabled | optional | mandatory */

    /* [network] */
    char *stun;
    int local_port;     /* 0 = any */

    /* [audio] */
    double mic_level;
    double spk_level;

    /* [general] */
    gboolean start_minimized;
    char *last_number;
} Config;

void config_load(Config *cfg);
gboolean config_save(const Config *cfg, GError **error);
void config_clear(Config *cfg);
char *config_path(void);
gboolean config_account_ready(const Config *cfg);

#endif
