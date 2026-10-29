#include "ui.h"

#include <signal.h>
#include <string.h>
#include <glib-unix.h>
#include <gtk/gtk.h>

#include "config.h"
#include "engine.h"

#ifdef HAVE_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>
#endif

#ifndef SIPPHONE_VERSION
#define SIPPHONE_VERSION "dev"
#endif

#define APP_ID   "io.github.c4l1f0rn1a.SipPhone"
#define APP_NAME "SipPhone"
#define APP_ICON "call-start"

typedef struct {
    GtkApplication *gtkapp;
    Config cfg;
    EngineOptions opts;
    gboolean engine_tried;
    gboolean engine_ok;

    GtkWidget *window;
    GtkWidget *number_entry;
    GtkWidget *call_label;
    GtkWidget *call_button;
    GtkWidget *hangup_button;
    GtkWidget *mute_button;
    GtkWidget *hold_button;
    GtkWidget *mic_scale;
    GtkWidget *spk_scale;
    GtkWidget *status_label;
    GtkWidget *incoming_win;

    int call_id;               /* -1 = no call */
    CallState call_state;
    char *remote;
    char *remote_number;
    gint64 connected_at;       /* monotonic usec, 0 = not connected */
    guint timer_id;
    guint clear_id;

    gboolean registered;
    gboolean tray_connected;
#ifdef HAVE_APPINDICATOR
    AppIndicator *indicator;
    GtkWidget *tray_toggle_item;
#endif
} App;

static void dial(App *a, const char *number);

/* ---------- small helpers ---------- */

static void set_status(App *a, const char *color, const char *text)
{
    char *esc = g_markup_escape_text(text, -1);
    char *markup = g_strdup_printf("<span foreground='%s'>●</span> %s",
                                   color, esc);
    gtk_label_set_markup(GTK_LABEL(a->status_label), markup);
    gtk_widget_set_tooltip_text(a->status_label, text);
    g_free(markup);
    g_free(esc);
}

static char *format_duration(gint64 usec)
{
    gint64 s = usec / G_USEC_PER_SEC;
    if (s >= 3600)
        return g_strdup_printf("%d:%02d:%02d", (int)(s / 3600),
                               (int)(s / 60 % 60), (int)(s % 60));
    return g_strdup_printf("%02d:%02d", (int)(s / 60), (int)(s % 60));
}

static const char *disconnect_text(int code)
{
    switch (code) {
    case 200: return "Звонок завершён";
    case 403: return "Запрещено";
    case 404: return "Номер не найден";
    case 408: return "Нет ответа";
    case 480: return "Недоступен";
    case 486: return "Занято";
    case 487: return "Отменён";
    case 488: return "Нет общих кодеков";
    case 503: return "Сервис недоступен";
    case 600: return "Занято";
    case 603: return "Отклонён";
    default:  return NULL;
    }
}

static void update_tray(App *a)
{
#ifdef HAVE_APPINDICATOR
    if (!a->indicator)
        return;
    if (a->call_id >= 0 && a->call_state == CALL_INCOMING)
        app_indicator_set_status(a->indicator, APP_INDICATOR_STATUS_ATTENTION);
    else
        app_indicator_set_status(a->indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_icon_full(a->indicator,
                                a->registered ? "call-start" : "call-stop",
                                a->registered ? "В сети" : "Не в сети");
    gtk_menu_item_set_label(GTK_MENU_ITEM(a->tray_toggle_item),
                            gtk_widget_get_visible(a->window)
                            ? "Скрыть окно" : "Показать окно");
#else
    (void)a;
#endif
}

static void present_window(App *a)
{
    gtk_window_present(GTK_WINDOW(a->window));
    update_tray(a);
}

/* ---------- call display ---------- */

static void update_controls(App *a)
{
    gboolean in_call = a->call_id >= 0;
    gboolean incoming = in_call && a->call_state == CALL_INCOMING;
    gboolean talking = in_call && a->call_state == CALL_CONFIRMED;

    gtk_button_set_label(GTK_BUTTON(a->call_button),
                         incoming ? "Ответить" : "Вызов");
    gtk_widget_set_sensitive(a->call_button,
                             a->engine_ok && (!in_call || incoming));
    gtk_widget_set_sensitive(a->hangup_button, in_call);
    gtk_widget_set_sensitive(a->hold_button, talking);
    gtk_widget_set_sensitive(a->mute_button, in_call);
    update_tray(a);
}

static void update_call_label(App *a)
{
    char *text = NULL;
    const char *who = a->remote ? a->remote : "";

    if (a->call_id < 0)
        return;

    switch (a->call_state) {
    case CALL_CALLING:
        text = g_strdup_printf("Вызов: %s…", who);
        break;
    case CALL_EARLY:
        text = g_strdup_printf("Звонит у %s…", who);
        break;
    case CALL_INCOMING:
        text = g_strdup_printf("Входящий: %s", who);
        break;
    case CALL_CONNECTING:
        text = g_strdup_printf("Соединение с %s…", who);
        break;
    case CALL_CONFIRMED: {
        char *dur = format_duration(g_get_monotonic_time() - a->connected_at);
        gboolean held = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(a->hold_button));
        text = g_strdup_printf("%s %s  %s", held ? "На удержании:" : "Разговор:",
                               who, dur);
        g_free(dur);
        break;
    }
    default:
        break;
    }
    if (text) {
        gtk_label_set_text(GTK_LABEL(a->call_label), text);
        g_free(text);
    }
}

static gboolean on_tick(gpointer data)
{
    update_call_label(data);
    return G_SOURCE_CONTINUE;
}

static gboolean on_clear_label(gpointer data)
{
    App *a = data;
    a->clear_id = 0;
    if (a->call_id < 0)
        gtk_label_set_text(GTK_LABEL(a->call_label), "");
    return G_SOURCE_REMOVE;
}

static void set_toggle_silently(GtkWidget *toggle, gboolean active,
                                GCallback handler, gpointer data)
{
    g_signal_handlers_block_by_func(toggle, handler, data);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), active);
    g_signal_handlers_unblock_by_func(toggle, handler, data);
}

static void on_mute_toggled(GtkToggleButton *b, App *a);
static void on_hold_toggled(GtkToggleButton *b, App *a);

static void close_incoming(App *a)
{
    if (a->incoming_win) {
        gtk_widget_destroy(a->incoming_win);
        a->incoming_win = NULL;
    }
    g_application_withdraw_notification(G_APPLICATION(a->gtkapp), "incoming");
    gtk_window_set_urgency_hint(GTK_WINDOW(a->window), FALSE);
}

static void call_finished(App *a, int code, const char *reason)
{
    const char *what = disconnect_text(code);
    char *dur = NULL, *text;

    if (a->timer_id) {
        g_source_remove(a->timer_id);
        a->timer_id = 0;
    }
    if (a->connected_at)
        dur = format_duration(g_get_monotonic_time() - a->connected_at);

    if (!what)
        what = reason && *reason ? reason : "Звонок завершён";
    text = dur ? g_strdup_printf("%s  %s", what, dur) : g_strdup(what);
    gtk_label_set_text(GTK_LABEL(a->call_label), text);
    g_free(text);
    g_free(dur);

    /* Leave the remote number in the field for a quick call back. */
    if (a->remote_number && *a->remote_number)
        gtk_entry_set_text(GTK_ENTRY(a->number_entry), a->remote_number);

    close_incoming(a);
    a->call_id = -1;
    a->call_state = CALL_IDLE;
    a->connected_at = 0;
    g_clear_pointer(&a->remote, g_free);
    g_clear_pointer(&a->remote_number, g_free);

    set_toggle_silently(a->hold_button, FALSE, G_CALLBACK(on_hold_toggled), a);
    set_toggle_silently(a->mute_button, FALSE, G_CALLBACK(on_mute_toggled), a);
    engine_set_mic_mute(FALSE);

    if (a->clear_id)
        g_source_remove(a->clear_id);
    a->clear_id = g_timeout_add_seconds(5, on_clear_label, a);
    update_controls(a);
}

/* ---------- incoming call popup ---------- */

static void on_incoming_answer(GtkButton *b, App *a)
{
    (void)b;
    if (a->call_id >= 0)
        engine_answer(a->call_id);
    close_incoming(a);
}

static void on_incoming_reject(GtkButton *b, App *a)
{
    (void)b;
    if (a->call_id >= 0)
        engine_hangup(a->call_id);
    close_incoming(a);
}

static gboolean on_incoming_delete(GtkWidget *w, GdkEvent *e, App *a)
{
    (void)w;
    (void)e;
    on_incoming_reject(NULL, a);
    return TRUE;
}

static void show_incoming(App *a)
{
    GtkWidget *win, *box, *title, *who, *buttons, *answer, *reject;
    GNotification *n;
    char *markup;

    close_incoming(a);

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Входящий звонок");
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(win), APP_ICON);
    gtk_window_set_urgency_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_application(GTK_WINDOW(win), a->gtkapp);
    g_signal_connect(win, "delete-event", G_CALLBACK(on_incoming_delete), a);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);
    gtk_container_add(GTK_CONTAINER(win), box);

    title = gtk_label_new("Входящий звонок");
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    who = gtk_label_new(NULL);
    markup = g_markup_printf_escaped(
        "<span size='x-large' weight='bold'>%s</span>",
        a->remote ? a->remote : "");
    gtk_label_set_markup(GTK_LABEL(who), markup);
    g_free(markup);
    gtk_box_pack_start(GTK_BOX(box), who, FALSE, FALSE, 0);

    buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
    answer = gtk_button_new_with_label("Ответить");
    reject = gtk_button_new_with_label("Отклонить");
    gtk_style_context_add_class(gtk_widget_get_style_context(answer),
                                "call-button");
    gtk_style_context_add_class(gtk_widget_get_style_context(reject),
                                "hangup-button");
    g_signal_connect(answer, "clicked", G_CALLBACK(on_incoming_answer), a);
    g_signal_connect(reject, "clicked", G_CALLBACK(on_incoming_reject), a);
    gtk_box_pack_start(GTK_BOX(buttons), answer, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), reject, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);

    a->incoming_win = win;
    gtk_widget_show_all(win);
    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(answer);
    gtk_window_set_urgency_hint(GTK_WINDOW(a->window), TRUE);

    /* Desktop notification as well, for when the popup is hidden. */
    n = g_notification_new("Входящий звонок");
    g_notification_set_body(n, a->remote ? a->remote : "");
    g_notification_set_priority(n, G_NOTIFICATION_PRIORITY_URGENT);
    g_notification_add_button_with_target(n, "Ответить", "app.answer", "i",
                                          a->call_id);
    g_notification_add_button_with_target(n, "Отклонить", "app.reject", "i",
                                          a->call_id);
    g_application_send_notification(G_APPLICATION(a->gtkapp), "incoming", n);
    g_object_unref(n);
}

/* ---------- engine callbacks (GTK thread) ---------- */

static void on_reg_state(gboolean registered, int code, const char *reason,
                         gpointer user)
{
    App *a = user;
    char *text;

    a->registered = registered;
    if (!config_account_ready(&a->cfg)) {
        set_status(a, "#888888", "Аккаунт не настроен");
    } else if (registered) {
        text = g_strdup_printf("%s@%s", a->cfg.username, a->cfg.domain);
        set_status(a, "#2e9d48", text);
        g_free(text);
    } else if (code == 0) {
        set_status(a, "#e0a020", "Регистрация…");
    } else if (code / 100 == 2) {
        set_status(a, "#888888", "Не зарегистрирован");
    } else {
        text = g_strdup_printf("Ошибка регистрации: %d %s", code,
                               reason ? reason : "");
        set_status(a, "#d93a3a", text);
        g_free(text);
    }
    update_tray(a);
}

static void on_incoming(int call_id, const char *remote, const char *number,
                        gpointer user)
{
    App *a = user;

    if (a->call_id >= 0) {      /* engine already rejects these; be safe */
        engine_hangup(call_id);
        return;
    }
    if (a->clear_id) {
        g_source_remove(a->clear_id);
        a->clear_id = 0;
    }
    a->call_id = call_id;
    a->call_state = CALL_INCOMING;
    g_free(a->remote);
    a->remote = g_strdup(remote);
    g_free(a->remote_number);
    a->remote_number = g_strdup(number);

    update_call_label(a);
    update_controls(a);
    show_incoming(a);
}

static void on_call_state(int call_id, CallState state, int code,
                          const char *reason, const char *remote,
                          const char *number, gpointer user)
{
    App *a = user;

    if (call_id != a->call_id)
        return;

    g_free(a->remote);
    a->remote = g_strdup(remote);
    g_free(a->remote_number);
    a->remote_number = g_strdup(number);

    if (state == CALL_DISCONNECTED) {
        call_finished(a, code, reason);
        return;
    }

    a->call_state = state;
    if (state != CALL_INCOMING)
        close_incoming(a);
    if (state == CALL_CONFIRMED && !a->connected_at) {
        a->connected_at = g_get_monotonic_time();
        a->timer_id = g_timeout_add_seconds(1, on_tick, a);
    }
    update_call_label(a);
    update_controls(a);
}

static void on_hold_state(int call_id, gboolean local_hold,
                          gboolean remote_hold, gpointer user)
{
    App *a = user;

    (void)remote_hold;
    if (call_id != a->call_id)
        return;
    set_toggle_silently(a->hold_button, local_hold,
                        G_CALLBACK(on_hold_toggled), a);
    update_call_label(a);
}

/* ---------- main window handlers ---------- */

static void dial(App *a, const char *number)
{
    char *error = NULL;
    int call_id;

    if (!a->engine_ok || a->call_id >= 0)
        return;

    call_id = engine_call(number, &error);
    if (call_id < 0) {
        gtk_label_set_text(GTK_LABEL(a->call_label), error);
        g_free(error);
        return;
    }

    g_free(a->cfg.last_number);
    a->cfg.last_number = g_strdup(number);

    if (a->clear_id) {
        g_source_remove(a->clear_id);
        a->clear_id = 0;
    }
    a->call_id = call_id;
    a->call_state = CALL_CALLING;
    g_free(a->remote);
    a->remote = g_strdup(number);
    g_free(a->remote_number);
    a->remote_number = g_strdup(number);
    update_call_label(a);
    update_controls(a);
}

static void on_call_clicked(GtkButton *b, App *a)
{
    (void)b;
    if (a->call_id >= 0) {
        if (a->call_state == CALL_INCOMING) {
            engine_answer(a->call_id);
            close_incoming(a);
        }
        return;
    }
    dial(a, gtk_entry_get_text(GTK_ENTRY(a->number_entry)));
}

static void on_hangup_clicked(GtkButton *b, App *a)
{
    (void)b;
    if (a->call_id >= 0)
        engine_hangup(a->call_id);
}

static void on_entry_activate(GtkEntry *e, App *a)
{
    (void)e;
    on_call_clicked(NULL, a);
}

static void on_entry_icon(GtkEntry *e, GtkEntryIconPosition pos,
                          GdkEvent *ev, App *a)
{
    (void)pos;
    (void)ev;
    (void)a;
    gtk_entry_set_text(e, "");
}

/* Every key typed or clicked during a call also goes out as DTMF. */
static void on_entry_insert(GtkEditable *ed, const char *text, int len,
                            int *pos, App *a)
{
    int i;

    (void)ed;
    (void)pos;
    if (a->call_id < 0 || a->call_state != CALL_CONFIRMED)
        return;
    if (len < 0)
        len = strlen(text);
    for (i = 0; i < len; i++)
        if (strchr("0123456789*#ABCDabcd", text[i]))
            engine_send_dtmf(a->call_id, text[i]);
}

static void on_digit_clicked(GtkButton *b, App *a)
{
    const char *digit = g_object_get_data(G_OBJECT(b), "digit");
    int pos = -1;

    gtk_editable_set_position(GTK_EDITABLE(a->number_entry), -1);
    pos = gtk_editable_get_position(GTK_EDITABLE(a->number_entry));
    gtk_editable_insert_text(GTK_EDITABLE(a->number_entry), digit, 1, &pos);
    gtk_editable_set_position(GTK_EDITABLE(a->number_entry), pos);
}

static void on_mute_toggled(GtkToggleButton *b, App *a)
{
    (void)a;
    engine_set_mic_mute(gtk_toggle_button_get_active(b));
}

static void on_hold_toggled(GtkToggleButton *b, App *a)
{
    gboolean want = gtk_toggle_button_get_active(b);

    if (a->call_id < 0 || !engine_set_hold(a->call_id, want))
        set_toggle_silently(GTK_WIDGET(b), !want,
                            G_CALLBACK(on_hold_toggled), a);
    update_call_label(a);
}

static void on_mic_changed(GtkRange *r, App *a)
{
    a->cfg.mic_level = gtk_range_get_value(r) / 100.0;
    engine_set_mic_level(a->cfg.mic_level);
}

static void on_spk_changed(GtkRange *r, App *a)
{
    a->cfg.spk_level = gtk_range_get_value(r) / 100.0;
    engine_set_spk_level(a->cfg.spk_level);
}

static gboolean on_window_delete(GtkWidget *w, GdkEvent *e, App *a)
{
    (void)e;
    /* With a tray icon, closing the window only hides it (like MicroSIP). */
    if (a->tray_connected) {
        gtk_widget_hide(w);
        update_tray(a);
        return TRUE;
    }
    g_action_group_activate_action(G_ACTION_GROUP(a->gtkapp), "quit", NULL);
    return TRUE;
}

/* ---------- settings dialog ---------- */

static GtkWidget *add_row(GtkWidget *grid, int row, const char *label,
                          GtkWidget *w)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 1.0);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
    return w;
}

static GtkWidget *entry_with(const char *text, const char *placeholder)
{
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), text ? text : "");
    if (placeholder)
        gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    return e;
}

static void take_entry(char **field, GtkWidget *entry)
{
    char *v = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
    g_free(*field);
    *field = v;
}

static void show_settings(App *a)
{
    GtkWidget *dlg, *area, *grid, *note;
    GtkWidget *display, *user, *domain, *login, *pass, *proxy;
    GtkWidget *transport, *srtp, *stun, *port, *minimized;
    int row = 0;

    dlg = gtk_dialog_new_with_buttons("Настройки", GTK_WINDOW(a->window),
                                      GTK_DIALOG_MODAL |
                                      GTK_DIALOG_DESTROY_WITH_PARENT,
                                      "Отмена", GTK_RESPONSE_CANCEL,
                                      "Сохранить", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));

    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_box_pack_start(GTK_BOX(area), grid, TRUE, TRUE, 0);

    display = add_row(grid, row++, "Имя",
                      entry_with(a->cfg.display_name, "Иван Петров"));
    user = add_row(grid, row++, "Номер *",
                   entry_with(a->cfg.username, "101"));
    domain = add_row(grid, row++, "Сервер *",
                     entry_with(a->cfg.domain, "pbx.example.com"));
    login = add_row(grid, row++, "Логин",
                    entry_with(a->cfg.auth_user, "если отличается от номера"));
    pass = add_row(grid, row++, "Пароль", entry_with(a->cfg.password, NULL));
    gtk_entry_set_visibility(GTK_ENTRY(pass), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(pass), GTK_INPUT_PURPOSE_PASSWORD);
    proxy = add_row(grid, row++, "Прокси",
                    entry_with(a->cfg.proxy, "необязательно"));

    transport = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(transport), "udp", "UDP");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(transport), "tcp", "TCP");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(transport), "tls", "TLS");
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(transport),
                                     a->cfg.transport))
        gtk_combo_box_set_active(GTK_COMBO_BOX(transport), 0);
    add_row(grid, row++, "Транспорт", transport);

    srtp = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(srtp), "disabled",
                              "Выключено");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(srtp), "optional",
                              "Если поддерживается");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(srtp), "mandatory",
                              "Обязательно");
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(srtp), a->cfg.srtp))
        gtk_combo_box_set_active(GTK_COMBO_BOX(srtp), 0);
    add_row(grid, row++, "Шифрование (SRTP)", srtp);

    stun = add_row(grid, row++, "STUN-сервер",
                   entry_with(a->cfg.stun, "stun.example.com:3478"));
    port = gtk_spin_button_new_with_range(0, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port), a->cfg.local_port);
    gtk_widget_set_tooltip_text(port, "0 — выбрать автоматически");
    add_row(grid, row++, "Локальный порт", port);

    minimized = gtk_check_button_new_with_label("Запускать свёрнутым в трей");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(minimized),
                                 a->cfg.start_minimized);
    gtk_grid_attach(GTK_GRID(grid), minimized, 1, row++, 1, 1);

    note = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(note),
        "<small>STUN и локальный порт применяются после перезапуска.</small>");
    gtk_label_set_xalign(GTK_LABEL(note), 0.0);
    gtk_grid_attach(GTK_GRID(grid), note, 0, row++, 2, 1);

    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        GError *gerr = NULL;
        char *error = NULL;

        take_entry(&a->cfg.display_name, display);
        take_entry(&a->cfg.username, user);
        take_entry(&a->cfg.domain, domain);
        take_entry(&a->cfg.auth_user, login);
        g_free(a->cfg.password);
        a->cfg.password = g_strdup(gtk_entry_get_text(GTK_ENTRY(pass)));
        take_entry(&a->cfg.proxy, proxy);
        g_free(a->cfg.transport);
        a->cfg.transport = g_strdup(
            gtk_combo_box_get_active_id(GTK_COMBO_BOX(transport)));
        g_free(a->cfg.srtp);
        a->cfg.srtp = g_strdup(
            gtk_combo_box_get_active_id(GTK_COMBO_BOX(srtp)));
        take_entry(&a->cfg.stun, stun);
        a->cfg.local_port =
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(port));
        a->cfg.start_minimized =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(minimized));

        if (!config_save(&a->cfg, &gerr)) {
            set_status(a, "#d93a3a", gerr->message);
            g_error_free(gerr);
        }
        if (a->engine_ok) {
            a->registered = FALSE;
            if (config_account_ready(&a->cfg))
                set_status(a, "#e0a020", "Регистрация…");
            if (!engine_set_account(&a->cfg, &error)) {
                set_status(a, "#d93a3a", error);
                g_free(error);
            }
        }
    }
    gtk_widget_destroy(dlg);
}

/* ---------- actions ---------- */

static void act_settings(GSimpleAction *ac, GVariant *p, gpointer data)
{
    (void)ac;
    (void)p;
    present_window(data);
    show_settings(data);
}

static void act_about(GSimpleAction *ac, GVariant *p, gpointer data)
{
    App *a = data;
    (void)ac;
    (void)p;
    gtk_show_about_dialog(GTK_WINDOW(a->window),
                          "program-name", APP_NAME,
                          "version", SIPPHONE_VERSION,
                          "comments", "Лёгкий SIP-телефон на PJSIP и GTK",
                          "logo-icon-name", APP_ICON,
                          "license-type", GTK_LICENSE_GPL_2_0,
                          NULL);
}

static void act_show(GSimpleAction *ac, GVariant *p, gpointer data)
{
    App *a = data;
    (void)ac;
    (void)p;
    if (gtk_widget_get_visible(a->window) && a->tray_connected)
        gtk_widget_hide(a->window);
    else
        gtk_window_present(GTK_WINDOW(a->window));
    update_tray(a);
}

static void act_answer(GSimpleAction *ac, GVariant *p, gpointer data)
{
    App *a = data;
    (void)ac;
    if (a->call_id == g_variant_get_int32(p) &&
        a->call_state == CALL_INCOMING) {
        engine_answer(a->call_id);
        close_incoming(a);
        present_window(a);
    }
}

static void act_reject(GSimpleAction *ac, GVariant *p, gpointer data)
{
    App *a = data;
    (void)ac;
    if (a->call_id == g_variant_get_int32(p))
        engine_hangup(a->call_id);
}

static void act_quit(GSimpleAction *ac, GVariant *p, gpointer data)
{
    App *a = data;
    (void)ac;
    (void)p;
    g_application_quit(G_APPLICATION(a->gtkapp));
}

static const GActionEntry app_actions[] = {
    { .name = "settings", .activate = act_settings },
    { .name = "about",    .activate = act_about },
    { .name = "show",     .activate = act_show },
    { .name = "answer",   .activate = act_answer, .parameter_type = "i" },
    { .name = "reject",   .activate = act_reject, .parameter_type = "i" },
    { .name = "quit",     .activate = act_quit },
};

/* ---------- building the window ---------- */

static const char *css =
    ".call-button, .hangup-button { background-image: none; color: #ffffff;"
    "  font-weight: bold; min-height: 36px; text-shadow: none; }"
    ".call-button label, .hangup-button label { color: #ffffff; }"
    ".call-button { background-color: #2e9d48; border-color: #23803a; }"
    ".call-button:hover { background-color: #36b153; }"
    ".hangup-button { background-color: #d93a3a; border-color: #b52d2d; }"
    ".hangup-button:hover { background-color: #e64b4b; }"
    ".call-button:disabled, .hangup-button:disabled { opacity: 0.45; }"
    ".dialpad button { min-height: 38px; }"
    ".number-entry { font-size: 15pt; min-height: 36px; }"
    ".call-info { font-weight: bold; }";

static GtkWidget *make_dialpad(App *a)
{
    static const char *keys[12][2] = {
        { "1", "" },    { "2", "ABC" }, { "3", "DEF" },
        { "4", "GHI" }, { "5", "JKL" }, { "6", "MNO" },
        { "7", "PQRS" },{ "8", "TUV" }, { "9", "WXYZ" },
        { "*", "" },    { "0", "+" },   { "#", "" },
    };
    GtkWidget *grid = gtk_grid_new();
    int i;

    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(grid), "dialpad");

    for (i = 0; i < 12; i++) {
        GtkWidget *btn = gtk_button_new();
        GtkWidget *lbl = gtk_label_new(NULL);
        char *markup = g_markup_printf_escaped(
            "<span size='large' weight='bold'>%s</span>"
            "<span size='x-small'>%s%s</span>",
            keys[i][0], *keys[i][1] ? "  " : "", keys[i][1]);

        gtk_label_set_markup(GTK_LABEL(lbl), markup);
        g_free(markup);
        gtk_container_add(GTK_CONTAINER(btn), lbl);
        gtk_widget_set_can_focus(btn, FALSE);
        g_object_set_data(G_OBJECT(btn), "digit", (gpointer)keys[i][0]);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_digit_clicked), a);
        gtk_grid_attach(GTK_GRID(grid), btn, i % 3, i / 3, 1, 1);
    }
    return grid;
}

static GtkWidget *make_level_row(const char *icon, double value,
                                 GtkWidget **scale_out)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                0, 200, 5);

    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_scale_add_mark(GTK_SCALE(scale), 100, GTK_POS_BOTTOM, NULL);
    gtk_range_set_value(GTK_RANGE(scale), value * 100.0);
    gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), scale, TRUE, TRUE, 0);
    *scale_out = scale;
    return row;
}

static void build_window(App *a)
{
    GtkWidget *win, *box, *top, *menu_btn, *title, *buttons, *toggles;
    GtkCssProvider *provider;
    GMenu *menu;

    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    win = gtk_application_window_new(a->gtkapp);
    a->window = win;
    gtk_window_set_title(GTK_WINDOW(win), APP_NAME);
    gtk_window_set_icon_name(GTK_WINDOW(win), APP_ICON);
    gtk_window_set_default_size(GTK_WINDOW(win), 280, -1);
    g_signal_connect(win, "delete-event", G_CALLBACK(on_window_delete), a);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_container_add(GTK_CONTAINER(win), box);

    /* Top row: menu button. */
    top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    title = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(top), title, TRUE, TRUE, 0);
    menu = g_menu_new();
    g_menu_append(menu, "Настройки…", "app.settings");
    g_menu_append(menu, "О программе", "app.about");
    g_menu_append(menu, "Выход", "app.quit");
    menu_btn = gtk_menu_button_new();
    gtk_button_set_image(GTK_BUTTON(menu_btn),
                         gtk_image_new_from_icon_name("open-menu-symbolic",
                                                      GTK_ICON_SIZE_BUTTON));
    gtk_button_set_relief(GTK_BUTTON(menu_btn), GTK_RELIEF_NONE);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn),
                                   G_MENU_MODEL(menu));
    g_object_unref(menu);
    gtk_box_pack_end(GTK_BOX(top), menu_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

    /* Number field. */
    a->number_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(a->number_entry),
                                   "Номер или sip:адрес");
    gtk_entry_set_input_purpose(GTK_ENTRY(a->number_entry),
                                GTK_INPUT_PURPOSE_PHONE);
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(a->number_entry),
                                      GTK_ENTRY_ICON_SECONDARY,
                                      "edit-clear-symbolic");
    gtk_entry_set_text(GTK_ENTRY(a->number_entry), a->cfg.last_number);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(a->number_entry), "number-entry");
    g_signal_connect(a->number_entry, "activate",
                     G_CALLBACK(on_entry_activate), a);
    g_signal_connect(a->number_entry, "icon-press",
                     G_CALLBACK(on_entry_icon), a);
    g_signal_connect(a->number_entry, "insert-text",
                     G_CALLBACK(on_entry_insert), a);
    gtk_box_pack_start(GTK_BOX(box), a->number_entry, FALSE, FALSE, 0);

    /* Current call info. */
    a->call_label = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(a->call_label), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(a->call_label),
                                "call-info");
    gtk_box_pack_start(GTK_BOX(box), a->call_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), make_dialpad(a), TRUE, TRUE, 0);

    /* Call / hang up. */
    buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
    a->call_button = gtk_button_new_with_label("Вызов");
    a->hangup_button = gtk_button_new_with_label("Сброс");
    gtk_style_context_add_class(gtk_widget_get_style_context(a->call_button),
                                "call-button");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(a->hangup_button), "hangup-button");
    g_signal_connect(a->call_button, "clicked",
                     G_CALLBACK(on_call_clicked), a);
    g_signal_connect(a->hangup_button, "clicked",
                     G_CALLBACK(on_hangup_clicked), a);
    gtk_box_pack_start(GTK_BOX(buttons), a->call_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), a->hangup_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);

    /* Mute / hold. */
    toggles = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(toggles), TRUE);
    a->mute_button = gtk_toggle_button_new_with_label("Без звука");
    a->hold_button = gtk_toggle_button_new_with_label("Удержание");
    g_signal_connect(a->mute_button, "toggled",
                     G_CALLBACK(on_mute_toggled), a);
    g_signal_connect(a->hold_button, "toggled",
                     G_CALLBACK(on_hold_toggled), a);
    gtk_box_pack_start(GTK_BOX(toggles), a->mute_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(toggles), a->hold_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), toggles, FALSE, FALSE, 0);

    /* Levels. */
    gtk_box_pack_start(GTK_BOX(box),
                       make_level_row("audio-input-microphone-symbolic",
                                      a->cfg.mic_level, &a->mic_scale),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       make_level_row("audio-volume-high-symbolic",
                                      a->cfg.spk_level, &a->spk_scale),
                       FALSE, FALSE, 0);
    g_signal_connect(a->mic_scale, "value-changed",
                     G_CALLBACK(on_mic_changed), a);
    g_signal_connect(a->spk_scale, "value-changed",
                     G_CALLBACK(on_spk_changed), a);

    /* Registration status. */
    gtk_box_pack_start(GTK_BOX(box),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);
    a->status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(a->status_label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(a->status_label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(box), a->status_label, FALSE, FALSE, 0);

    gtk_widget_show_all(box);
}

#ifdef HAVE_APPINDICATOR
static void on_tray_connection(AppIndicator *ind, gboolean connected, App *a)
{
    (void)ind;
    a->tray_connected = connected;
}

static void on_tray_item(GtkMenuItem *item, gpointer action)
{
    GApplication *gapp = g_application_get_default();
    (void)item;
    g_action_group_activate_action(G_ACTION_GROUP(gapp), action, NULL);
}

static void build_tray(App *a)
{
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item;

    a->tray_toggle_item = gtk_menu_item_new_with_label("Показать окно");
    g_signal_connect(a->tray_toggle_item, "activate",
                     G_CALLBACK(on_tray_item), "show");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), a->tray_toggle_item);

    item = gtk_menu_item_new_with_label("Настройки…");
    g_signal_connect(item, "activate", G_CALLBACK(on_tray_item), "settings");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    item = gtk_menu_item_new_with_label("Выход");
    g_signal_connect(item, "activate", G_CALLBACK(on_tray_item), "quit");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_widget_show_all(menu);

    a->indicator = app_indicator_new(APP_ID, "call-stop",
                                     APP_INDICATOR_CATEGORY_COMMUNICATIONS);
    app_indicator_set_title(a->indicator, APP_NAME);
    app_indicator_set_attention_icon_full(a->indicator, "call-start",
                                          "Входящий звонок");
    app_indicator_set_menu(a->indicator, GTK_MENU(menu));
    /* Middle click toggles the window. */
    app_indicator_set_secondary_activate_target(a->indicator,
                                                a->tray_toggle_item);
    g_signal_connect(a->indicator, "connection-changed",
                     G_CALLBACK(on_tray_connection), a);
    app_indicator_set_status(a->indicator, APP_INDICATOR_STATUS_ACTIVE);
}
#endif

/* ---------- application lifecycle ---------- */

static void start_engine(App *a)
{
    static const EngineCallbacks cb = {
        .reg_state = on_reg_state,
        .incoming = on_incoming,
        .call_state = on_call_state,
        .hold_state = on_hold_state,
    };
    char *error = NULL;

    a->engine_tried = TRUE;
    if (config_account_ready(&a->cfg))
        set_status(a, "#e0a020", "Регистрация…");

    if (!engine_init(&a->cfg, &a->opts, &cb, a, &error)) {
        char *text = g_strdup_printf("Ошибка запуска: %s", error);
        set_status(a, "#d93a3a", text);
        g_free(text);
    } else {
        a->engine_ok = TRUE;
        if (!engine_set_account(&a->cfg, &error))
            set_status(a, "#d93a3a", error);
    }
    g_free(error);
    update_controls(a);
}

static gboolean show_if_no_tray(gpointer data)
{
    App *a = data;
    if (!a->tray_connected)
        present_window(a);
    return G_SOURCE_REMOVE;
}

/* "tel:+7999", "callto://101", "sip:101@pbx" -> something engine_call takes */
static char *uri_to_number(const char *arg)
{
    const char *p = arg;
    char *unescaped, *cut;

    if (g_ascii_strncasecmp(p, "callto://", 9) == 0)
        p += 9;
    else if (g_ascii_strncasecmp(p, "callto:", 7) == 0)
        p += 7;
    else if (g_ascii_strncasecmp(p, "tel:", 4) == 0)
        p += 4;

    unescaped = g_uri_unescape_string(p, NULL);
    if (!unescaped)
        unescaped = g_strdup(p);
    /* tel: URIs may carry parameters: "tel:+7999;phone-context=..." */
    if (p != arg && (cut = strchr(unescaped, ';')))
        *cut = '\0';
    return unescaped;
}

static int on_command_line(GApplication *gapp, GApplicationCommandLine *cmd,
                           App *a)
{
    GVariantDict *opts = g_application_command_line_get_options_dict(cmd);
    const char **rest = NULL;
    gboolean minimized = FALSE;

    (void)gapp;
    g_variant_dict_lookup(opts, "minimized", "b", &minimized);
    g_variant_dict_lookup(opts, G_OPTION_REMAINING, "^a&s", &rest);

    if (!a->engine_tried) {
        gboolean null_audio = FALSE;
        int level = 1;

        g_variant_dict_lookup(opts, "null-audio", "b", &null_audio);
        g_variant_dict_lookup(opts, "debug", "i", &level);
        a->opts.null_audio = null_audio;
        a->opts.log_level = CLAMP(level, 0, 6);
        start_engine(a);

        if (minimized || a->cfg.start_minimized)
            g_timeout_add_seconds(2, show_if_no_tray, a);
        else
            present_window(a);
    } else if (!rest || !rest[0]) {
        present_window(a);   /* launched again: just bring the window up */
    }

    if (rest && rest[0]) {
        char *number = uri_to_number(rest[0]);
        gtk_entry_set_text(GTK_ENTRY(a->number_entry), number);
        present_window(a);
        dial(a, number);
        g_free(number);
    }
    g_free(rest);
    return 0;
}

/* SIGTERM/SIGINT (logout, kill, Ctrl+C): hang up and unregister cleanly. */
static gboolean on_unix_signal(gpointer data)
{
    g_application_quit(G_APPLICATION(data));
    return G_SOURCE_CONTINUE;
}

static void on_startup(GApplication *gapp, App *a)
{
    g_unix_signal_add(SIGTERM, on_unix_signal, gapp);
    g_unix_signal_add(SIGINT, on_unix_signal, gapp);
    g_action_map_add_action_entries(G_ACTION_MAP(a->gtkapp), app_actions,
                                    G_N_ELEMENTS(app_actions), a);
    build_window(a);
#ifdef HAVE_APPINDICATOR
    build_tray(a);
#endif
    set_status(a, "#888888", config_account_ready(&a->cfg)
                             ? "Запуск…" : "Аккаунт не настроен");
    a->call_id = -1;
    update_controls(a);
}

static void on_shutdown(GApplication *gapp, App *a)
{
    (void)gapp;
    if (a->timer_id)
        g_source_remove(a->timer_id);
    if (a->clear_id)
        g_source_remove(a->clear_id);
    a->timer_id = a->clear_id = 0;
    engine_shutdown();
    config_save(&a->cfg, NULL);
}

int ui_run(int argc, char **argv)
{
    static const GOptionEntry entries[] = {
        { "debug", 'd', 0, G_OPTION_ARG_INT, NULL,
          "Уровень логов PJSIP в консоли (0-6)", "N" },
        { "null-audio", 0, 0, G_OPTION_ARG_NONE, NULL,
          "Работать без звуковой карты (для тестов)", NULL },
        { "minimized", 'm', 0, G_OPTION_ARG_NONE, NULL,
          "Запуститься свёрнутым в трей", NULL },
        { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, NULL, NULL,
          "[НОМЕР | sip:URI | tel:НОМЕР]" },
        { NULL }
    };
    App a = { .call_id = -1 };
    int status;

    config_load(&a.cfg);

    a.gtkapp = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_application_add_main_option_entries(G_APPLICATION(a.gtkapp), entries);
    g_signal_connect(a.gtkapp, "startup", G_CALLBACK(on_startup), &a);
    g_signal_connect(a.gtkapp, "command-line", G_CALLBACK(on_command_line),
                     &a);
    g_signal_connect(a.gtkapp, "shutdown", G_CALLBACK(on_shutdown), &a);

    status = g_application_run(G_APPLICATION(a.gtkapp), argc, argv);

    g_object_unref(a.gtkapp);
    g_free(a.remote);
    g_free(a.remote_number);
    config_clear(&a.cfg);
    return status;
}
