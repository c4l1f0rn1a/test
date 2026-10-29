#ifndef SIPPHONE_ENGINE_H
#define SIPPHONE_ENGINE_H

/*
 * Thin wrapper over pjsua-lib. PJSIP calls back from its own worker
 * threads; the engine re-posts every event onto the GLib main loop, so
 * all EngineCallbacks run in the GTK thread and may touch widgets.
 *
 * All engine_* functions must be called from the thread that called
 * engine_init() (the GTK main thread).
 */

#include <glib.h>
#include "config.h"

typedef enum {
    CALL_IDLE,
    CALL_CALLING,      /* outgoing INVITE sent */
    CALL_INCOMING,     /* incoming, not answered yet */
    CALL_EARLY,        /* 18x received/sent */
    CALL_CONNECTING,   /* answered, waiting for ACK */
    CALL_CONFIRMED,    /* talking */
    CALL_DISCONNECTED
} CallState;

typedef struct {
    /* code is the SIP status of the last REGISTER (0 = not tried) */
    void (*reg_state)(gboolean registered, int code, const char *reason,
                      gpointer user);
    /* remote: human-readable ("John (101)"), number: user part ("101") */
    void (*incoming)(int call_id, const char *remote, const char *number,
                     gpointer user);
    void (*call_state)(int call_id, CallState state, int code,
                       const char *reason, const char *remote,
                       const char *number, gpointer user);
    void (*hold_state)(int call_id, gboolean local_hold, gboolean remote_hold,
                       gpointer user);
} EngineCallbacks;

typedef struct {
    int log_level;          /* 0..6, PJSIP console log level */
    gboolean null_audio;    /* no sound card (testing, headless) */
} EngineOptions;

/* Starts PJSIP. Call engine_set_account() afterwards to register. */
gboolean engine_init(const Config *cfg, const EngineOptions *opts,
                     const EngineCallbacks *cb, gpointer user,
                     char **error);
void engine_shutdown(void);

/* (Re)create the SIP account from cfg. Removes the previous one. */
gboolean engine_set_account(const Config *cfg, char **error);

/* number: "101", "101@host", "sip:101@host" */
int engine_call(const char *number, char **error);
gboolean engine_answer(int call_id);
gboolean engine_hangup(int call_id);
void engine_hangup_all(void);
gboolean engine_set_hold(int call_id, gboolean hold);
gboolean engine_send_dtmf(int call_id, char digit);

void engine_set_mic_level(double level);   /* 0.0 .. 2.0, 1.0 = normal */
void engine_set_spk_level(double level);
void engine_set_mic_mute(gboolean mute);

#endif
