/*
 * Zeitgeber fuer den Flipper Zero
 *
 *  - Stoppuhr (mit Rundenzeiten)
 *  - Timer (einfacher Countdown)
 *  - Intervall-Timer: Vorlauf, Belastung / Pause (zwei Zeiten, z.B. 60 s / 30 s), Runden
 *  - Signal waehlbar: Vibration, Piepen oder Beides (LED zeigt zusaetzlich die Phase)
 *
 * Einstellungen werden auf der SD-Karte gespeichert (apps_data/interval_timer).
 */

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "IntervalTimer"

#define SETTINGS_PATH  APP_DATA_PATH("settings.bin")
#define SETTINGS_MAGIC 0x49545633UL

#define PREP_MIN   0
#define PREP_MAX   60
#define WORK_MIN   5
#define WORK_MAX   3600
#define REST_MIN   0
#define REST_MAX   3600
#define ROUNDS_MIN 1
#define ROUNDS_MAX 99
#define TIMER_MIN  5
#define TIMER_MAX  3600

/* ---------- Typen ---------- */

typedef enum {
    AlarmVibro,
    AlarmSound,
    AlarmBoth,
    AlarmCount,
} AlarmMode;

static const char* alarm_names[AlarmCount] = {"Vibration", "Piepen", "Beides"};

typedef enum {
    MenuStopwatch,
    MenuTimer,
    MenuInterval,
    MenuAlarm,
    MenuCount,
} MenuItem;

typedef enum {
    FieldPrep,
    FieldWork,
    FieldRest,
    FieldRounds,
    FieldCount,
} Field;

typedef enum {
    ScreenMenu,
    ScreenStopwatch,
    ScreenTimerSetup,
    ScreenIntervalSetup,
    ScreenRun,
} Screen;

typedef enum {
    PhasePrep,
    PhaseWork,
    PhaseRest,
    PhaseDone,
} Phase;

typedef enum {
    SigPrep,
    SigWork,
    SigRest,
    SigTick,
    SigDone,
    SigTest,
    SigCount,
} Signal;

typedef enum {
    EventTypeInput,
    EventTypeTick,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} AppEvent;

typedef struct {
    uint32_t magic;
    uint16_t prep;
    uint16_t work;
    uint16_t rest;
    uint16_t rounds;
    uint16_t timer;
    uint16_t alarm;
} Settings;

typedef struct {
    uint16_t prep;
    uint16_t work;
    uint16_t rest;
    uint16_t rounds;
} RunParams;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* queue;
    FuriMutex* mutex;
    FuriTimer* timer;
    NotificationApp* notifications;

    Settings settings;
    Screen screen;
    uint8_t menu_index;
    Field field;

    /* Ablauf (Timer und Intervall) */
    RunParams run;
    bool is_timer;
    Phase phase;
    uint16_t remaining;
    uint16_t phase_total;
    uint16_t round;
    bool paused;

    /* Stoppuhr */
    bool sw_running;
    uint32_t sw_start_tick;
    uint32_t sw_accum_ms;
    uint32_t sw_lap_total_ms;
    uint32_t sw_lap_last_ms;
    uint32_t sw_lap_prev_ms;
    uint16_t sw_lap_count;

    bool exit;
} App;

/* ---------- Signale: [Ereignis][Vibration | Piepen | Beides] ---------- */

#define LED_OFF   &message_red_0, &message_green_0, &message_blue_0
#define LED_BLUE  &message_red_0, &message_green_0, &message_blue_255
#define LED_GREEN &message_red_0, &message_green_255, &message_blue_0
#define LED_RED   &message_red_255, &message_green_0, &message_blue_0
#define LED_CYAN  &message_red_0, &message_green_255, &message_blue_255

/* Vorlauf: kurz */
static const NotificationSequence seq_prep_vibro = {
    LED_BLUE, &message_vibro_on, &message_delay_100, &message_vibro_off, NULL};
static const NotificationSequence seq_prep_sound = {
    LED_BLUE, &message_note_c5, &message_delay_100, &message_sound_off, NULL};
static const NotificationSequence seq_prep_both = {
    LED_BLUE, &message_vibro_on, &message_note_c5, &message_delay_100, &message_vibro_off,
    &message_sound_off, NULL};

/* Belastung / Start: lang */
static const NotificationSequence seq_work_vibro = {
    LED_GREEN, &message_vibro_on, &message_delay_250, &message_vibro_off, NULL};
static const NotificationSequence seq_work_sound = {
    LED_GREEN, &message_note_g6, &message_delay_250, &message_sound_off, NULL};
static const NotificationSequence seq_work_both = {
    LED_GREEN, &message_vibro_on, &message_note_g6, &message_delay_250, &message_vibro_off,
    &message_sound_off, NULL};

/* Pause: doppelt kurz */
static const NotificationSequence seq_rest_vibro = {
    LED_RED, &message_vibro_on, &message_delay_100, &message_vibro_off, &message_delay_100,
    &message_vibro_on, &message_delay_100, &message_vibro_off, NULL};
static const NotificationSequence seq_rest_sound = {
    LED_RED, &message_note_c5, &message_delay_100, &message_sound_off, &message_delay_100,
    &message_note_c5, &message_delay_100, &message_sound_off, NULL};
static const NotificationSequence seq_rest_both = {
    LED_RED, &message_vibro_on, &message_note_c5, &message_delay_100, &message_vibro_off,
    &message_sound_off, &message_delay_100, &message_vibro_on, &message_note_c5,
    &message_delay_100, &message_vibro_off, &message_sound_off, NULL};

/* Countdown in den letzten 3 Sekunden */
static const NotificationSequence seq_tick_vibro = {
    &message_vibro_on, &message_delay_100, &message_vibro_off, NULL};
static const NotificationSequence seq_tick_sound = {
    &message_note_c6, &message_delay_100, &message_sound_off, NULL};
static const NotificationSequence seq_tick_both = {
    &message_vibro_on, &message_note_c6, &message_delay_100, &message_vibro_off,
    &message_sound_off, NULL};

/* Fertig */
static const NotificationSequence seq_done_vibro = {
    LED_CYAN, &message_vibro_on, &message_delay_250, &message_vibro_off, &message_delay_100,
    &message_vibro_on, &message_delay_250, &message_vibro_off, &message_delay_100,
    &message_vibro_on, &message_delay_500, &message_vibro_off, NULL};
static const NotificationSequence seq_done_sound = {
    LED_CYAN, &message_note_c6, &message_delay_250, &message_note_e6, &message_delay_250,
    &message_note_g6, &message_delay_500, &message_sound_off, NULL};
static const NotificationSequence seq_done_both = {
    LED_CYAN, &message_vibro_on, &message_note_c6, &message_delay_250, &message_note_e6,
    &message_delay_250, &message_note_g6, &message_delay_500, &message_vibro_off,
    &message_sound_off, NULL};

/* Vorschau bei der Signalwahl (ohne LED) */
static const NotificationSequence seq_test_vibro = {
    &message_vibro_on, &message_delay_250, &message_vibro_off, NULL};
static const NotificationSequence seq_test_sound = {
    &message_note_c6, &message_delay_250, &message_sound_off, NULL};
static const NotificationSequence seq_test_both = {
    &message_vibro_on, &message_note_c6, &message_delay_250, &message_vibro_off,
    &message_sound_off, NULL};

static const NotificationSequence seq_off = {
    LED_OFF, &message_vibro_off, &message_sound_off, NULL};

static const NotificationSequence* const signals[SigCount][AlarmCount] = {
    [SigPrep] = {&seq_prep_vibro, &seq_prep_sound, &seq_prep_both},
    [SigWork] = {&seq_work_vibro, &seq_work_sound, &seq_work_both},
    [SigRest] = {&seq_rest_vibro, &seq_rest_sound, &seq_rest_both},
    [SigTick] = {&seq_tick_vibro, &seq_tick_sound, &seq_tick_both},
    [SigDone] = {&seq_done_vibro, &seq_done_sound, &seq_done_both},
    [SigTest] = {&seq_test_vibro, &seq_test_sound, &seq_test_both},
};

static void signal_play(App* app, Signal sig) {
    uint16_t mode = app->settings.alarm < AlarmCount ? app->settings.alarm : AlarmBoth;
    notification_message(app->notifications, signals[sig][mode]);
}

static void keep_awake(App* app, bool enable) {
    notification_message(
        app->notifications,
        enable ? &sequence_display_backlight_enforce_on :
                 &sequence_display_backlight_enforce_auto);
}

/* ---------- Einstellungen laden / speichern ---------- */

static void settings_defaults(Settings* s) {
    s->magic = SETTINGS_MAGIC;
    s->prep = 5;
    s->work = 60;
    s->rest = 30;
    s->rounds = 5;
    s->timer = 180;
    s->alarm = AlarmBoth;
}

static bool settings_valid(const Settings* s) {
    return s->magic == SETTINGS_MAGIC && s->prep <= PREP_MAX && s->work >= WORK_MIN &&
           s->work <= WORK_MAX && s->rest <= REST_MAX && s->rounds >= ROUNDS_MIN &&
           s->rounds <= ROUNDS_MAX && s->timer >= TIMER_MIN && s->timer <= TIMER_MAX &&
           s->alarm < AlarmCount;
}

static void settings_load(Settings* s) {
    settings_defaults(s);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    Settings tmp;
    if(storage_file_open(file, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(storage_file_read(file, &tmp, sizeof(tmp)) == sizeof(tmp) && settings_valid(&tmp)) {
            *s = tmp;
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void settings_save(const Settings* s) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, s, sizeof(*s));
        storage_file_close(file);
    } else {
        FURI_LOG_W(TAG, "Einstellungen konnten nicht gespeichert werden");
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

/* ---------- Hilfsfunktionen ---------- */

static uint32_t total_seconds(const RunParams* r) {
    uint32_t total = (uint32_t)r->prep + (uint32_t)r->rounds * r->work;
    if(r->rounds > 1) total += (uint32_t)(r->rounds - 1) * r->rest;
    return total;
}

static void format_time(char* buf, size_t size, uint32_t secs) {
    uint32_t h = secs / 3600;
    uint32_t m = (secs / 60) % 60;
    uint32_t s = secs % 60;
    if(h > 0) {
        snprintf(buf, size, "%lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    } else {
        snprintf(buf, size, "%02lu:%02lu", (unsigned long)m, (unsigned long)s);
    }
}

static void format_stopwatch(char* buf, size_t size, uint32_t ms) {
    uint32_t total_s = ms / 1000;
    uint32_t tenth = (ms % 1000) / 100;
    uint32_t h = total_s / 3600;
    uint32_t m = (total_s / 60) % 60;
    uint32_t s = total_s % 60;
    if(h > 0) {
        snprintf(buf, size, "%lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    } else {
        snprintf(
            buf,
            size,
            "%02lu:%02lu.%lu",
            (unsigned long)m,
            (unsigned long)s,
            (unsigned long)tenth);
    }
}

static uint16_t* field_value(App* app, Field field, int32_t* min, int32_t* max) {
    switch(field) {
    case FieldPrep:
        *min = PREP_MIN;
        *max = PREP_MAX;
        return &app->settings.prep;
    case FieldWork:
        *min = WORK_MIN;
        *max = WORK_MAX;
        return &app->settings.work;
    case FieldRest:
        *min = REST_MIN;
        *max = REST_MAX;
        return &app->settings.rest;
    default:
        *min = ROUNDS_MIN;
        *max = ROUNDS_MAX;
        return &app->settings.rounds;
    }
}

static void adjust_field(App* app, int32_t direction, bool repeat) {
    int32_t min, max;
    uint16_t* value = field_value(app, app->field, &min, &max);

    int32_t step;
    if(app->field == FieldRounds) {
        step = repeat ? 5 : 1;
    } else {
        step = repeat ? 30 : 5;
    }

    int32_t v = (int32_t)*value + direction * step;
    if(v < min) v = min;
    if(v > max) v = max;
    *value = (uint16_t)v;
}

static void adjust_timer(App* app, int32_t seconds) {
    int32_t v = (int32_t)app->settings.timer + seconds;
    if(v < TIMER_MIN) v = TIMER_MIN;
    if(v > TIMER_MAX) v = TIMER_MAX;
    app->settings.timer = (uint16_t)v;
}

static void change_alarm(App* app, int32_t direction) {
    app->settings.alarm = (app->settings.alarm + AlarmCount + direction) % AlarmCount;
    signal_play(app, SigTest);
}

static uint32_t stopwatch_elapsed(const App* app) {
    uint32_t ms = app->sw_accum_ms;
    if(app->sw_running) ms += furi_get_tick() - app->sw_start_tick;
    return ms;
}

/* ---------- Ablaufsteuerung (Timer / Intervall) ---------- */

static void enter_phase(App* app, Phase phase) {
    app->phase = phase;
    app->paused = false;

    switch(phase) {
    case PhasePrep:
        app->remaining = app->run.prep;
        signal_play(app, SigPrep);
        break;
    case PhaseWork:
        app->remaining = app->run.work;
        if(!app->is_timer) signal_play(app, SigWork);
        break;
    case PhaseRest:
        app->remaining = app->run.rest;
        signal_play(app, SigRest);
        break;
    case PhaseDone:
        app->remaining = 0;
        furi_timer_stop(app->timer);
        signal_play(app, SigDone);
        break;
    }
    app->phase_total = app->remaining;
}

static void advance_phase(App* app) {
    switch(app->phase) {
    case PhasePrep:
        app->round = 1;
        enter_phase(app, PhaseWork);
        break;
    case PhaseWork:
        if(app->round >= app->run.rounds) {
            enter_phase(app, PhaseDone);
        } else if(app->run.rest > 0) {
            enter_phase(app, PhaseRest);
        } else {
            app->round++;
            enter_phase(app, PhaseWork);
        }
        break;
    case PhaseRest:
        app->round++;
        enter_phase(app, PhaseWork);
        break;
    case PhaseDone:
        break;
    }
}

static void start_run(App* app, bool is_timer) {
    settings_save(&app->settings);

    app->is_timer = is_timer;
    if(is_timer) {
        app->run = (RunParams){.prep = 0, .work = app->settings.timer, .rest = 0, .rounds = 1};
    } else {
        app->run = (RunParams){
            .prep = app->settings.prep,
            .work = app->settings.work,
            .rest = app->settings.rest,
            .rounds = app->settings.rounds};
    }

    app->screen = ScreenRun;
    app->round = 1;
    keep_awake(app, true);

    if(app->run.prep > 0) {
        enter_phase(app, PhasePrep);
    } else {
        enter_phase(app, PhaseWork);
    }

    if(app->phase != PhaseDone) {
        furi_timer_start(app->timer, furi_kernel_get_tick_frequency());
    }
}

static void leave_to(App* app, Screen screen) {
    furi_timer_stop(app->timer);
    app->screen = screen;
    app->paused = false;
    app->sw_running = false;
    notification_message(app->notifications, &seq_off);
    keep_awake(app, false);
    settings_save(&app->settings);
}

static void handle_tick(App* app) {
    if(app->screen != ScreenRun || app->paused || app->phase == PhaseDone) return;

    if(app->remaining > 0) app->remaining--;

    if(app->remaining == 0) {
        advance_phase(app);
    } else if(app->remaining <= 3) {
        signal_play(app, SigTick);
    }
}

/* ---------- Stoppuhr ---------- */

static void stopwatch_reset(App* app) {
    app->sw_running = false;
    app->sw_accum_ms = 0;
    app->sw_lap_total_ms = 0;
    app->sw_lap_last_ms = 0;
    app->sw_lap_prev_ms = 0;
    app->sw_lap_count = 0;
}

static void stopwatch_toggle(App* app) {
    if(app->sw_running) {
        app->sw_accum_ms = stopwatch_elapsed(app);
        app->sw_running = false;
        furi_timer_stop(app->timer);
        keep_awake(app, false);
    } else {
        app->sw_start_tick = furi_get_tick();
        app->sw_running = true;
        furi_timer_start(app->timer, furi_kernel_get_tick_frequency() / 10);
        keep_awake(app, true);
    }
}

static void stopwatch_lap(App* app) {
    if(!app->sw_running) return;
    uint32_t now = stopwatch_elapsed(app);
    app->sw_lap_prev_ms = app->sw_lap_last_ms;
    app->sw_lap_last_ms = now - app->sw_lap_total_ms;
    app->sw_lap_total_ms = now;
    if(app->sw_lap_count < 999) app->sw_lap_count++;
}

/* ---------- Eingabe ---------- */

static void handle_input_menu(App* app, const InputEvent* event) {
    bool is_short = event->type == InputTypeShort;
    bool is_repeat = event->type == InputTypeRepeat;
    if(!is_short && !is_repeat) return;

    switch(event->key) {
    case InputKeyUp:
        app->menu_index = (app->menu_index + MenuCount - 1) % MenuCount;
        break;
    case InputKeyDown:
        app->menu_index = (app->menu_index + 1) % MenuCount;
        break;
    case InputKeyLeft:
        if(is_short && app->menu_index == MenuAlarm) change_alarm(app, -1);
        break;
    case InputKeyRight:
        if(is_short && app->menu_index == MenuAlarm) change_alarm(app, +1);
        break;
    case InputKeyOk:
        if(!is_short) break;
        switch(app->menu_index) {
        case MenuStopwatch:
            stopwatch_reset(app);
            app->screen = ScreenStopwatch;
            break;
        case MenuTimer:
            app->screen = ScreenTimerSetup;
            break;
        case MenuInterval:
            app->screen = ScreenIntervalSetup;
            break;
        default:
            change_alarm(app, +1);
            break;
        }
        break;
    case InputKeyBack:
        if(is_short) app->exit = true;
        break;
    default:
        break;
    }
}

static void handle_input_stopwatch(App* app, const InputEvent* event) {
    if(event->type != InputTypeShort) return;

    switch(event->key) {
    case InputKeyOk:
        stopwatch_toggle(app);
        break;
    case InputKeyRight:
        stopwatch_lap(app);
        break;
    case InputKeyLeft:
        if(!app->sw_running) stopwatch_reset(app);
        break;
    case InputKeyBack:
        leave_to(app, ScreenMenu);
        break;
    default:
        break;
    }
}

static void handle_input_timer_setup(App* app, const InputEvent* event) {
    bool is_short = event->type == InputTypeShort;
    bool is_repeat = event->type == InputTypeRepeat;
    if(!is_short && !is_repeat) return;

    switch(event->key) {
    case InputKeyLeft:
        adjust_timer(app, is_repeat ? -15 : -5);
        break;
    case InputKeyRight:
        adjust_timer(app, is_repeat ? +15 : +5);
        break;
    case InputKeyDown:
        adjust_timer(app, is_repeat ? -300 : -60);
        break;
    case InputKeyUp:
        adjust_timer(app, is_repeat ? +300 : +60);
        break;
    case InputKeyOk:
        if(is_short) start_run(app, true);
        break;
    case InputKeyBack:
        if(is_short) leave_to(app, ScreenMenu);
        break;
    default:
        break;
    }
}

static void handle_input_interval_setup(App* app, const InputEvent* event) {
    bool is_short = event->type == InputTypeShort;
    bool is_repeat = event->type == InputTypeRepeat;
    if(!is_short && !is_repeat) return;

    switch(event->key) {
    case InputKeyUp:
        app->field = (app->field + FieldCount - 1) % FieldCount;
        break;
    case InputKeyDown:
        app->field = (app->field + 1) % FieldCount;
        break;
    case InputKeyLeft:
        adjust_field(app, -1, is_repeat);
        break;
    case InputKeyRight:
        adjust_field(app, +1, is_repeat);
        break;
    case InputKeyOk:
        if(is_short) start_run(app, false);
        break;
    case InputKeyBack:
        if(is_short) leave_to(app, ScreenMenu);
        break;
    default:
        break;
    }
}

static void handle_input_run(App* app, const InputEvent* event) {
    if(event->type != InputTypeShort) return;

    Screen back = app->is_timer ? ScreenTimerSetup : ScreenIntervalSetup;

    switch(event->key) {
    case InputKeyOk:
        if(app->phase == PhaseDone) {
            leave_to(app, back);
        } else {
            app->paused = !app->paused;
        }
        break;
    case InputKeyRight: /* naechste Phase ueberspringen */
        if(app->phase != PhaseDone && !app->is_timer) advance_phase(app);
        break;
    case InputKeyLeft: /* aktuelle Phase neu starten */
        if(app->phase != PhaseDone) app->remaining = app->phase_total;
        break;
    case InputKeyBack:
        leave_to(app, back);
        break;
    default:
        break;
    }
}

/* ---------- Zeichnen ---------- */

static void draw_menu(Canvas* canvas, App* app) {
    static const char* labels[MenuCount] = {"Stoppuhr", "Timer", "Intervall-Timer", "Signal"};

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "Zeitgeber");

    canvas_set_font(canvas, FontSecondary);
    for(int i = 0; i < MenuCount; i++) {
        int y = 12 + i * 12;
        bool selected = (i == (int)app->menu_index);

        if(selected) {
            canvas_draw_box(canvas, 0, y, 128, 12);
            canvas_set_color(canvas, ColorWhite);
        }

        canvas_draw_str_aligned(canvas, 4, y + 10, AlignLeft, AlignBottom, labels[i]);
        if(i == MenuAlarm) {
            uint16_t mode = app->settings.alarm < AlarmCount ? app->settings.alarm : AlarmBoth;
            canvas_draw_str_aligned(canvas, 100, y + 10, AlignCenter, AlignBottom, alarm_names[mode]);
            if(selected) {
                canvas_draw_str_aligned(canvas, 66, y + 10, AlignLeft, AlignBottom, "<");
                canvas_draw_str_aligned(canvas, 124, y + 10, AlignRight, AlignBottom, ">");
            }
        }
        if(selected) canvas_set_color(canvas, ColorBlack);
    }
}

static void draw_stopwatch(Canvas* canvas, App* app) {
    char buf[48];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas, 64, 0, AlignCenter, AlignTop, app->sw_running ? "STOPPUHR" : "STOPPUHR (Stopp)");

    format_stopwatch(buf, sizeof(buf), stopwatch_elapsed(app));
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 27, AlignCenter, AlignCenter, buf);

    canvas_set_font(canvas, FontSecondary);
    if(app->sw_lap_count == 0) {
        canvas_draw_str_aligned(canvas, 64, 51, AlignCenter, AlignBottom, "OK Start/Stopp  > Runde");
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, "< Reset (wenn gestoppt)");
    } else {
        char t[24];
        format_stopwatch(t, sizeof(t), app->sw_lap_last_ms);
        snprintf(buf, sizeof(buf), "R%u  %s", (unsigned)app->sw_lap_count, t);
        canvas_draw_str_aligned(canvas, 4, 51, AlignLeft, AlignBottom, buf);
        if(app->sw_lap_count > 1) {
            format_stopwatch(t, sizeof(t), app->sw_lap_prev_ms);
            snprintf(buf, sizeof(buf), "R%u  %s", (unsigned)(app->sw_lap_count - 1), t);
            canvas_draw_str_aligned(canvas, 4, 62, AlignLeft, AlignBottom, buf);
        }
    }
}

static void draw_timer_setup(Canvas* canvas, App* app) {
    char buf[16];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "Timer");

    format_time(buf, sizeof(buf), app->settings.timer);
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, buf);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 51, AlignCenter, AlignBottom, "</>: 5 s   ^/v: 1 min");
    canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, "OK = Start");
}

static void draw_interval_setup(Canvas* canvas, App* app) {
    static const char* labels[FieldCount] = {"Vorlauf", "Belastung", "Pause", "Runden"};
    char buf[16];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "Intervall-Timer");

    canvas_set_font(canvas, FontSecondary);
    for(int i = 0; i < FieldCount; i++) {
        int y = 13 + i * 10;
        bool selected = (i == (int)app->field);

        int32_t min, max;
        uint16_t value = *field_value(app, (Field)i, &min, &max);
        if(i == FieldRounds) {
            snprintf(buf, sizeof(buf), "%u", (unsigned)value);
        } else {
            format_time(buf, sizeof(buf), value);
        }

        if(selected) {
            canvas_draw_box(canvas, 0, y, 128, 10);
            canvas_set_color(canvas, ColorWhite);
        }

        canvas_draw_str_aligned(canvas, 4, y + 9, AlignLeft, AlignBottom, labels[i]);
        canvas_draw_str_aligned(canvas, 100, y + 9, AlignCenter, AlignBottom, buf);
        if(selected) {
            canvas_draw_str_aligned(canvas, 76, y + 9, AlignLeft, AlignBottom, "<");
            canvas_draw_str_aligned(canvas, 124, y + 9, AlignRight, AlignBottom, ">");
            canvas_set_color(canvas, ColorBlack);
        }
    }

    RunParams r = {
        .prep = app->settings.prep,
        .work = app->settings.work,
        .rest = app->settings.rest,
        .rounds = app->settings.rounds};
    canvas_draw_str_aligned(canvas, 2, 63, AlignLeft, AlignBottom, "OK=Start");
    format_time(buf, sizeof(buf), total_seconds(&r));
    char total[24];
    snprintf(total, sizeof(total), "Gesamt %s", buf);
    canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, total);
}

static void draw_run(Canvas* canvas, App* app) {
    char buf[24];

    if(app->phase == PhaseDone) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 14, AlignCenter, AlignCenter, "FERTIG!");
        canvas_set_font(canvas, FontSecondary);
        if(app->is_timer) {
            canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, "Timer abgelaufen");
            format_time(buf, sizeof(buf), app->run.work);
        } else {
            snprintf(buf, sizeof(buf), "%u Runden", (unsigned)app->run.rounds);
            canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, buf);
            format_time(buf, sizeof(buf), total_seconds(&app->run));
        }
        canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignCenter, buf);
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK = Zurueck");
        return;
    }

    const char* title = "";
    switch(app->phase) {
    case PhasePrep:
        title = "VORLAUF";
        break;
    case PhaseWork:
        title = app->is_timer ? "TIMER" : "BELASTUNG";
        break;
    case PhaseRest:
        title = "PAUSE";
        break;
    default:
        break;
    }

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 1, AlignCenter, AlignTop, title);

    format_time(buf, sizeof(buf), app->remaining);
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 29, AlignCenter, AlignCenter, buf);

    /* Fortschrittsbalken der aktuellen Phase */
    canvas_draw_frame(canvas, 4, 42, 120, 6);
    if(app->phase_total > 0) {
        uint32_t elapsed = app->phase_total - app->remaining;
        uint32_t width = (116UL * elapsed) / app->phase_total;
        if(width > 0) canvas_draw_box(canvas, 6, 44, width, 2);
    }

    canvas_set_font(canvas, FontSecondary);
    if(!app->is_timer) {
        if(app->phase == PhasePrep) {
            snprintf(buf, sizeof(buf), "%u Runden", (unsigned)app->run.rounds);
        } else {
            snprintf(
                buf, sizeof(buf), "Runde %u/%u", (unsigned)app->round, (unsigned)app->run.rounds);
        }
        canvas_draw_str_aligned(canvas, 2, 63, AlignLeft, AlignBottom, buf);
    }
    if(app->paused) {
        canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, "ANGEHALTEN");
    }
}

static void draw_callback(Canvas* canvas, void* context) {
    App* app = context;
    if(furi_mutex_acquire(app->mutex, 200) != FuriStatusOk) return;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    switch(app->screen) {
    case ScreenMenu:
        draw_menu(canvas, app);
        break;
    case ScreenStopwatch:
        draw_stopwatch(canvas, app);
        break;
    case ScreenTimerSetup:
        draw_timer_setup(canvas, app);
        break;
    case ScreenIntervalSetup:
        draw_interval_setup(canvas, app);
        break;
    case ScreenRun:
        draw_run(canvas, app);
        break;
    }

    furi_mutex_release(app->mutex);
}

/* ---------- Callbacks fuer Eingabe und Timer ---------- */

static void input_callback(InputEvent* input_event, void* context) {
    App* app = context;
    AppEvent event = {.type = EventTypeInput, .input = *input_event};
    furi_message_queue_put(app->queue, &event, FuriWaitForever);
}

static void timer_callback(void* context) {
    App* app = context;
    AppEvent event = {.type = EventTypeTick};
    furi_message_queue_put(app->queue, &event, 0);
}

/* ---------- Einstiegspunkt ---------- */

int32_t interval_timer_app(void* p) {
    UNUSED(p);

    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));

    settings_load(&app->settings);
    app->screen = ScreenMenu;
    app->field = FieldWork;
    app->phase = PhaseDone;

    app->queue = furi_message_queue_alloc(8, sizeof(AppEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->timer = furi_timer_alloc(timer_callback, FuriTimerTypePeriodic, app);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    AppEvent event;
    while(!app->exit) {
        if(furi_message_queue_get(app->queue, &event, FuriWaitForever) != FuriStatusOk) continue;

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(event.type == EventTypeTick) {
            handle_tick(app);
        } else if(event.type == EventTypeInput) {
            switch(app->screen) {
            case ScreenMenu:
                handle_input_menu(app, &event.input);
                break;
            case ScreenStopwatch:
                handle_input_stopwatch(app, &event.input);
                break;
            case ScreenTimerSetup:
                handle_input_timer_setup(app, &event.input);
                break;
            case ScreenIntervalSetup:
                handle_input_interval_setup(app, &event.input);
                break;
            case ScreenRun:
                handle_input_run(app, &event.input);
                break;
            }
        }
        furi_mutex_release(app->mutex);

        view_port_update(app->view_port);
    }

    /* Aufraeumen */
    furi_timer_stop(app->timer);
    settings_save(&app->settings);
    notification_message(app->notifications, &seq_off);
    keep_awake(app, false);

    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    view_port_free(app->view_port);
    furi_timer_free(app->timer);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->queue);
    free(app);

    return 0;
}
