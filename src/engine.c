#include "engine.h"

#include <string.h>
#include <pjsua-lib/pjsua.h>

#ifndef SIPPHONE_VERSION
#define SIPPHONE_VERSION "dev"
#endif

typedef enum { EV_REG, EV_INCOMING, EV_CALL_STATE, EV_HOLD } EventType;

typedef struct {
    EventType type;
    int id;              /* account id for EV_REG, call id otherwise */
    int state;
    int code;
    gboolean flag1;
    gboolean flag2;
    char *text;
    char *remote;
    char *number;
} Event;

typedef struct {
    pjmedia_port *port;
    pjsua_conf_port_id slot;
    int call_id;         /* call the tone belongs to, -1 = stopped */
} Tone;

static struct {
    gboolean started;
    EngineCallbacks cb;
    gpointer user;

    pjsua_acc_id acc_id;       /* configured SIP account */
    pjsua_acc_id local_acc;    /* for direct sip:user@ip calls */
    char *domain;
    char *tp_param;            /* "" or ";transport=tcp" etc. */

    pj_pool_t *pool;
    Tone ring;                 /* incoming call */
    Tone ringback;             /* outgoing call, remote is ringing */

    double mic_level;
    gboolean mic_mute;
} eng = {
    .acc_id = PJSUA_INVALID_ID,
    .local_acc = PJSUA_INVALID_ID,
    .ring = { .slot = PJSUA_INVALID_ID, .call_id = -1 },
    .ringback = { .slot = PJSUA_INVALID_ID, .call_id = -1 },
    .mic_level = 1.0,
};

/* ---------- helpers ---------- */

static char *pj_error(const char *what, pj_status_t status)
{
    char buf[PJ_ERR_MSG_SIZE];
    pj_strerror(status, buf, sizeof(buf));
    return g_strdup_printf("%s: %s", what, buf);
}

static pj_str_t str(const char *s)
{
    return pj_str((char *)s);
}

/* "\"John\" <sip:101@pbx>" -> remote "John (101)", number "101" */
static void parse_remote(const pj_str_t *info, char **remote, char **number)
{
    char *raw = g_strndup(info->ptr, info->slen);
    char *name = NULL, *user = NULL, *p, *end;

    p = strchr(raw, '"');
    if (p) {
        end = strchr(p + 1, '"');
        if (end)
            name = g_strndup(p + 1, end - p - 1);
    } else if ((p = strchr(raw, '<')) && p > raw) {
        name = g_strstrip(g_strndup(raw, p - raw));
    }

    p = strstr(raw, "sip:");
    if (p) {
        p += 4;
    } else if ((p = strstr(raw, "sips:"))) {
        p += 5;
    }
    if (p) {
        end = p + strcspn(p, "@>;");
        user = g_strndup(p, end - p);
    }

    if (name && *name && user && strcmp(name, user) != 0)
        *remote = g_strdup_printf("%s (%s)", name, user);
    else if (user && *user)
        *remote = g_strdup(user);
    else
        *remote = g_strdup(raw);
    *number = g_strdup(user && *user ? user : raw);

    g_free(name);
    g_free(user);
    g_free(raw);
}

/* ---------- events: PJSIP thread -> GTK main loop ---------- */

static gboolean dispatch(gpointer data)
{
    Event *ev = data;

    switch (ev->type) {
    case EV_REG:
        /* Ignore late callbacks from an account we already replaced. */
        if (ev->id == eng.acc_id && eng.cb.reg_state)
            eng.cb.reg_state(ev->flag1, ev->code, ev->text, eng.user);
        break;
    case EV_INCOMING:
        if (eng.cb.incoming)
            eng.cb.incoming(ev->id, ev->remote, ev->number, eng.user);
        break;
    case EV_CALL_STATE:
        if (eng.cb.call_state)
            eng.cb.call_state(ev->id, ev->state, ev->code, ev->text,
                              ev->remote, ev->number, eng.user);
        break;
    case EV_HOLD:
        if (eng.cb.hold_state)
            eng.cb.hold_state(ev->id, ev->flag1, ev->flag2, eng.user);
        break;
    }

    g_free(ev->text);
    g_free(ev->remote);
    g_free(ev->number);
    g_free(ev);
    return G_SOURCE_REMOVE;
}

static void post(Event *ev)
{
    g_idle_add(dispatch, ev);
}

/* ---------- tones ---------- */

static pj_status_t tone_create(Tone *t, const pjmedia_tone_desc *tones,
                               unsigned count)
{
    pjsua_media_config mcfg;
    pj_status_t st;

    pjsua_media_config_default(&mcfg);
    st = pjmedia_tonegen_create(eng.pool, mcfg.clock_rate, 1,
                                mcfg.clock_rate * 20 / 1000, 16,
                                PJMEDIA_TONEGEN_LOOP, &t->port);
    if (st != PJ_SUCCESS)
        return st;
    st = pjmedia_tonegen_play(t->port, count, tones, PJMEDIA_TONEGEN_LOOP);
    if (st != PJ_SUCCESS)
        return st;
    return pjsua_conf_add_port(eng.pool, t->port, &t->slot);
}

/* Tones are started/stopped from both the PJSIP and the GTK thread. */
static GMutex tone_lock;

static void tone_start(Tone *t, int call_id)
{
    g_mutex_lock(&tone_lock);
    if (t->slot != PJSUA_INVALID_ID && t->call_id == -1) {
        t->call_id = call_id;
        pjmedia_tonegen_rewind(t->port);
        pjsua_conf_connect(t->slot, 0);
    }
    g_mutex_unlock(&tone_lock);
}

static void tone_stop(Tone *t, int call_id)
{
    g_mutex_lock(&tone_lock);
    if (t->slot != PJSUA_INVALID_ID && t->call_id != -1 &&
        (call_id == -1 || t->call_id == call_id)) {
        t->call_id = -1;
        pjsua_conf_disconnect(t->slot, 0);
    }
    g_mutex_unlock(&tone_lock);
}

static void tone_destroy(Tone *t)
{
    if (t->slot != PJSUA_INVALID_ID)
        pjsua_conf_remove_port(t->slot);
    if (t->port)
        pjmedia_port_destroy(t->port);
    t->slot = PJSUA_INVALID_ID;
    t->port = NULL;
    t->call_id = -1;
}

static void tones_init(void)
{
    /* Incoming: two short bursts, then a pause. */
    static const pjmedia_tone_desc ring[] = {
        { .freq1 = 440, .freq2 = 480, .on_msec = 400, .off_msec = 200 },
        { .freq1 = 440, .freq2 = 480, .on_msec = 400, .off_msec = 2000 },
    };
    /* Ringback as heard in most of Europe/CIS: 425 Hz, 1 s on, 4 s off. */
    static const pjmedia_tone_desc ringback[] = {
        { .freq1 = 425, .on_msec = 1000, .off_msec = 4000 },
    };

    if (tone_create(&eng.ring, ring, G_N_ELEMENTS(ring)) != PJ_SUCCESS)
        eng.ring.slot = PJSUA_INVALID_ID;
    if (tone_create(&eng.ringback, ringback, G_N_ELEMENTS(ringback))
            != PJ_SUCCESS)
        eng.ringback.slot = PJSUA_INVALID_ID;
}

/* ---------- PJSIP callbacks (worker thread) ---------- */

static void on_reg_state2(pjsua_acc_id acc_id, pjsua_reg_info *info)
{
    pjsua_acc_info ai;
    Event *ev;

    PJ_UNUSED_ARG(info);
    if (pjsua_acc_get_info(acc_id, &ai) != PJ_SUCCESS)
        return;

    ev = g_new0(Event, 1);
    ev->type = EV_REG;
    ev->id = acc_id;
    ev->code = ai.status;
    ev->flag1 = ai.status / 100 == 2 && ai.expires > 0;
    ev->text = g_strndup(ai.status_text.ptr, ai.status_text.slen);
    post(ev);
}

static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id,
                             pjsip_rx_data *rdata)
{
    pjsua_call_info ci;
    Event *ev;

    PJ_UNUSED_ARG(acc_id);
    PJ_UNUSED_ARG(rdata);

    /* One call at a time for now: second caller gets "busy". */
    if (pjsua_call_get_count() > 1) {
        pjsua_call_answer(call_id, PJSIP_SC_BUSY_HERE, NULL, NULL);
        return;
    }

    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS)
        return;

    pjsua_call_answer(call_id, PJSIP_SC_RINGING, NULL, NULL);
    tone_start(&eng.ring, call_id);

    ev = g_new0(Event, 1);
    ev->type = EV_INCOMING;
    ev->id = call_id;
    parse_remote(&ci.remote_info, &ev->remote, &ev->number);
    post(ev);
}

static void on_call_state(pjsua_call_id call_id, pjsip_event *e)
{
    pjsua_call_info ci;
    CallState state;
    Event *ev;

    PJ_UNUSED_ARG(e);
    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS)
        return;

    switch (ci.state) {
    case PJSIP_INV_STATE_CALLING:
        state = CALL_CALLING;
        break;
    case PJSIP_INV_STATE_INCOMING:
        state = CALL_INCOMING;
        break;
    case PJSIP_INV_STATE_EARLY:
        state = ci.role == PJSIP_ROLE_UAS ? CALL_INCOMING : CALL_EARLY;
        break;
    case PJSIP_INV_STATE_CONNECTING:
        state = CALL_CONNECTING;
        break;
    case PJSIP_INV_STATE_CONFIRMED:
        state = CALL_CONFIRMED;
        break;
    case PJSIP_INV_STATE_DISCONNECTED:
        state = CALL_DISCONNECTED;
        break;
    default:
        state = CALL_IDLE;
        break;
    }

    if (state != CALL_INCOMING)
        tone_stop(&eng.ring, call_id);

    /* Remote is ringing and sends no early media: play our own ringback. */
    if (state == CALL_EARLY && ci.last_status == PJSIP_SC_RINGING &&
        ci.media_status != PJSUA_CALL_MEDIA_ACTIVE)
        tone_start(&eng.ringback, call_id);
    else if (state != CALL_EARLY)
        tone_stop(&eng.ringback, call_id);

    ev = g_new0(Event, 1);
    ev->type = EV_CALL_STATE;
    ev->id = call_id;
    ev->state = state;
    ev->code = ci.last_status;
    ev->text = g_strndup(ci.last_status_text.ptr, ci.last_status_text.slen);
    parse_remote(&ci.remote_info, &ev->remote, &ev->number);
    post(ev);
}

static void on_call_media_state(pjsua_call_id call_id)
{
    pjsua_call_info ci;
    Event *ev;

    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS)
        return;

    if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE ||
        ci.media_status == PJSUA_CALL_MEDIA_REMOTE_HOLD) {
        pjsua_conf_port_id slot = pjsua_call_get_conf_port(call_id);

        /* Early media (183) replaces our generated ringback. */
        tone_stop(&eng.ringback, call_id);
        if (slot != PJSUA_INVALID_ID) {
            pjsua_conf_connect(slot, 0);
            pjsua_conf_connect(0, slot);
        }
    }

    ev = g_new0(Event, 1);
    ev->type = EV_HOLD;
    ev->id = call_id;
    ev->flag1 = ci.media_status == PJSUA_CALL_MEDIA_LOCAL_HOLD;
    ev->flag2 = ci.media_status == PJSUA_CALL_MEDIA_REMOTE_HOLD;
    post(ev);
}

/* ---------- public API ---------- */

static void set_codec_priorities(void)
{
    pj_str_t id;

    /* Wideband first, then the G.711 pair every PBX understands. */
    pjsua_codec_set_priority((id = str("opus/48000"), &id),
                             PJMEDIA_CODEC_PRIO_HIGHEST);
    pjsua_codec_set_priority((id = str("G722/16000"), &id),
                             PJMEDIA_CODEC_PRIO_NEXT_HIGHER);
    pjsua_codec_set_priority((id = str("PCMA/8000"), &id),
                             PJMEDIA_CODEC_PRIO_NORMAL + 1);
    pjsua_codec_set_priority((id = str("PCMU/8000"), &id),
                             PJMEDIA_CODEC_PRIO_NORMAL);
    /* Speex is only built for its echo canceller; do not offer it. */
    pjsua_codec_set_priority((id = str("speex"), &id),
                             PJMEDIA_CODEC_PRIO_DISABLED);
}

gboolean engine_init(const Config *cfg, const EngineOptions *opts,
                     const EngineCallbacks *cb, gpointer user, char **error)
{
    pjsua_config ua_cfg;
    pjsua_logging_config log_cfg;
    pjsua_media_config media_cfg;
    pjsua_transport_config tp_cfg;
    pjsua_transport_id udp_id;
    pj_status_t st;

    eng.cb = *cb;
    eng.user = user;

    st = pjsua_create();
    if (st != PJ_SUCCESS) {
        *error = pj_error("pjsua_create", st);
        return FALSE;
    }

    pjsua_config_default(&ua_cfg);
    ua_cfg.user_agent = str("SipPhone/" SIPPHONE_VERSION);
    ua_cfg.cb.on_reg_state2 = on_reg_state2;
    ua_cfg.cb.on_incoming_call = on_incoming_call;
    ua_cfg.cb.on_call_state = on_call_state;
    ua_cfg.cb.on_call_media_state = on_call_media_state;
    if (cfg->stun && *cfg->stun) {
        ua_cfg.stun_srv_cnt = 1;
        ua_cfg.stun_srv[0] = str(cfg->stun);
    }

    pjsua_logging_config_default(&log_cfg);
    log_cfg.level = opts->log_level;
    log_cfg.console_level = opts->log_level;

    pjsua_media_config_default(&media_cfg);
    media_cfg.no_vad = PJ_TRUE;
    /* Release the sound card shortly after a call so other apps get it. */
    media_cfg.snd_auto_close_time = 1;

    st = pjsua_init(&ua_cfg, &log_cfg, &media_cfg);
    if (st != PJ_SUCCESS) {
        *error = pj_error("pjsua_init", st);
        pjsua_destroy();
        return FALSE;
    }

    if (opts->null_audio)
        pjsua_set_null_snd_dev();

    pjsua_transport_config_default(&tp_cfg);
    tp_cfg.port = cfg->local_port;
    st = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &tp_cfg, &udp_id);
    if (st != PJ_SUCCESS) {
        *error = pj_error("UDP transport", st);
        pjsua_destroy();
        return FALSE;
    }
    /* TCP and TLS are optional: a busy port should not stop the app. */
    pjsua_transport_create(PJSIP_TRANSPORT_TCP, &tp_cfg, NULL);
    tp_cfg.port = 0;
    pjsua_transport_create(PJSIP_TRANSPORT_TLS, &tp_cfg, NULL);

    st = pjsua_start();
    if (st != PJ_SUCCESS) {
        *error = pj_error("pjsua_start", st);
        pjsua_destroy();
        return FALSE;
    }
    eng.started = TRUE;

    pjsua_acc_add_local(udp_id, PJ_FALSE, &eng.local_acc);
    set_codec_priorities();

    eng.pool = pjsua_pool_create("sipphone", 1024, 1024);
    tones_init();

    engine_set_mic_level(cfg->mic_level);
    engine_set_spk_level(cfg->spk_level);
    return TRUE;
}

void engine_shutdown(void)
{
    if (!eng.started)
        return;
    eng.started = FALSE;
    memset(&eng.cb, 0, sizeof(eng.cb));
    pjsua_call_hangup_all();
    /* Tone ports live in our pool: detach them before the pool goes. */
    tone_destroy(&eng.ring);
    tone_destroy(&eng.ringback);
    if (eng.pool)
        pj_pool_release(eng.pool);
    eng.pool = NULL;
    pjsua_destroy();
    eng.acc_id = PJSUA_INVALID_ID;
    eng.local_acc = PJSUA_INVALID_ID;
    g_clear_pointer(&eng.domain, g_free);
    g_clear_pointer(&eng.tp_param, g_free);
}

/* Display names go inside quotes in the From header. */
static char *sanitize_display(const char *s)
{
    char *out = g_strdup(s ? s : "");
    g_strdelimit(out, "\"\\<>\r\n", ' ');
    return g_strstrip(out);
}

gboolean engine_set_account(const Config *cfg, char **error)
{
    pjsua_acc_config acc;
    char *display, *id, *reg_uri, *proxy = NULL;
    const char *login;
    pj_status_t st;

    if (eng.acc_id != PJSUA_INVALID_ID) {
        pjsua_acc_del(eng.acc_id);
        eng.acc_id = PJSUA_INVALID_ID;
    }
    g_clear_pointer(&eng.domain, g_free);
    g_clear_pointer(&eng.tp_param, g_free);

    if (!config_account_ready(cfg)) {
        if (eng.cb.reg_state)
            eng.cb.reg_state(FALSE, 0, NULL, eng.user);
        return TRUE;
    }

    eng.domain = g_strdup(cfg->domain);
    eng.tp_param = g_strcmp0(cfg->transport, "udp") == 0 || !*cfg->transport
                 ? g_strdup("")
                 : g_strdup_printf(";transport=%s", cfg->transport);

    display = sanitize_display(cfg->display_name);
    id = *display
       ? g_strdup_printf("\"%s\" <sip:%s@%s>", display, cfg->username,
                         cfg->domain)
       : g_strdup_printf("<sip:%s@%s>", cfg->username, cfg->domain);
    reg_uri = g_strdup_printf("sip:%s%s", cfg->domain, eng.tp_param);
    login = *cfg->auth_user ? cfg->auth_user : cfg->username;

    pjsua_acc_config_default(&acc);
    acc.id = str(id);
    acc.reg_uri = str(reg_uri);
    acc.reg_timeout = 300;
    acc.cred_count = 1;
    acc.cred_info[0].realm = str("*");
    acc.cred_info[0].scheme = str("digest");
    acc.cred_info[0].username = str(login);
    acc.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
    acc.cred_info[0].data = str(cfg->password);

    if (*cfg->proxy) {
        proxy = g_str_has_prefix(cfg->proxy, "sip:") ||
                g_str_has_prefix(cfg->proxy, "sips:")
              ? g_strdup(cfg->proxy)
              : g_strdup_printf("sip:%s;lr%s", cfg->proxy, eng.tp_param);
        acc.proxy_cnt = 1;
        acc.proxy[0] = str(proxy);
    }

    if (g_strcmp0(cfg->srtp, "mandatory") == 0)
        acc.use_srtp = PJMEDIA_SRTP_MANDATORY;
    else if (g_strcmp0(cfg->srtp, "optional") == 0)
        acc.use_srtp = PJMEDIA_SRTP_OPTIONAL;
    else
        acc.use_srtp = PJMEDIA_SRTP_DISABLED;
    acc.srtp_secure_signaling = 0;

    /* pjsua_acc_add() copies the config, so the strings can go. */
    st = pjsua_acc_add(&acc, PJ_TRUE, &eng.acc_id);

    g_free(display);
    g_free(id);
    g_free(reg_uri);
    g_free(proxy);

    if (st != PJ_SUCCESS) {
        eng.acc_id = PJSUA_INVALID_ID;
        *error = pj_error("Не удалось создать аккаунт", st);
        return FALSE;
    }
    return TRUE;
}

/* "+7 (999) 123-45-67" -> "+79991234567"; anything else is left as is. */
static char *normalize_number(const char *number)
{
    char *n = g_strstrip(g_strdup(number));
    const char *p;
    GString *out;

    for (p = n; *p; p++)
        if (!strchr("0123456789+*#-() ", *p))
            return n;

    out = g_string_new(NULL);
    for (p = n; *p; p++)
        if (!strchr("-() ", *p))
            g_string_append_c(out, *p);
    g_free(n);
    return g_string_free(out, FALSE);
}

int engine_call(const char *number, char **error)
{
    char *n = normalize_number(number);
    char *uri;
    pjsua_acc_id acc;
    pjsua_call_setting opt;
    pjsua_call_id call_id = PJSUA_INVALID_ID;
    pj_str_t uri_str;
    pj_status_t st;

    if (!*n) {
        g_free(n);
        *error = g_strdup("Введите номер");
        return -1;
    }

    if (g_str_has_prefix(n, "sip:") || g_str_has_prefix(n, "sips:"))
        uri = g_strdup(n);
    else if (strchr(n, '@'))
        uri = g_strdup_printf("sip:%s%s", n, eng.tp_param ? eng.tp_param : "");
    else if (eng.domain)
        uri = g_strdup_printf("sip:%s@%s%s", n, eng.domain, eng.tp_param);
    else {
        g_free(n);
        *error = g_strdup("Аккаунт не настроен");
        return -1;
    }
    g_free(n);

    if (pjsua_verify_sip_url(uri) != PJ_SUCCESS) {
        *error = g_strdup_printf("Неверный адрес: %s", uri);
        g_free(uri);
        return -1;
    }

    acc = eng.acc_id != PJSUA_INVALID_ID ? eng.acc_id : eng.local_acc;

    pjsua_call_setting_default(&opt);
    opt.aud_cnt = 1;
    opt.vid_cnt = 0;

    uri_str = str(uri);
    st = pjsua_call_make_call(acc, &uri_str, &opt, NULL, NULL, &call_id);
    g_free(uri);

    if (st != PJ_SUCCESS) {
        *error = pj_error("Не удалось позвонить", st);
        return -1;
    }
    return call_id;
}

gboolean engine_answer(int call_id)
{
    return pjsua_call_answer(call_id, PJSIP_SC_OK, NULL, NULL) == PJ_SUCCESS;
}

gboolean engine_hangup(int call_id)
{
    tone_stop(&eng.ring, call_id);
    tone_stop(&eng.ringback, call_id);
    return pjsua_call_hangup(call_id, 0, NULL, NULL) == PJ_SUCCESS;
}

void engine_hangup_all(void)
{
    if (eng.started)
        pjsua_call_hangup_all();
}

gboolean engine_set_hold(int call_id, gboolean hold)
{
    pj_status_t st = hold
        ? pjsua_call_set_hold(call_id, NULL)
        : pjsua_call_reinvite(call_id, PJSUA_CALL_UNHOLD, NULL);
    return st == PJ_SUCCESS;
}

gboolean engine_send_dtmf(int call_id, char digit)
{
    pjsua_call_send_dtmf_param p;
    char buf[2] = { digit, 0 };

    pjsua_call_send_dtmf_param_default(&p);
    p.digits = str(buf);
    p.method = PJSUA_DTMF_METHOD_RFC2833;
    if (pjsua_call_send_dtmf(call_id, &p) == PJ_SUCCESS)
        return TRUE;

    /* Remote did not negotiate telephone-event: fall back to SIP INFO. */
    p.method = PJSUA_DTMF_METHOD_SIP_INFO;
    return pjsua_call_send_dtmf(call_id, &p) == PJ_SUCCESS;
}

/* Conference slot 0 is the sound device: rx = microphone, tx = speaker. */
void engine_set_mic_level(double level)
{
    eng.mic_level = level;
    if (eng.started)
        pjsua_conf_adjust_rx_level(0, eng.mic_mute ? 0.0f : (float)level);
}

void engine_set_spk_level(double level)
{
    if (eng.started)
        pjsua_conf_adjust_tx_level(0, (float)level);
}

void engine_set_mic_mute(gboolean mute)
{
    eng.mic_mute = mute;
    engine_set_mic_level(eng.mic_level);
}
